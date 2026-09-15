#include "storage.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The persisted conversation still has a broad corruption/resource guard.
   Individual model outputs have no smaller fixed-size truncation point. */
#define STORAGE_LIMIT (128u * 1024u * 1024u)
/* Format 2 raised the conversation bound to 128; format 3 raised the
   per-conversation message bound from 64 to 512. The record layout is
   unchanged across all three versions. Formats 1-3 all decode, so old
   snapshots migrate on their next save; version 4+ is unsupported and fails
   the load closed (writes disabled, backup never tried) so an older build can
   never silently restore stale state over a newer primary. See docs/CHAT.md. */
#define FORMAT_VERSION 3
#define FORMAT_VERSION_MIN 1

static uint32_t checksum(const char *s, size_t n) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ (unsigned char)s[i]) * 16777619u;
    return hash;
}
static void raw(JsonBuf *b, const char *s) { json_buf_append_raw(b, s, strlen(s)); }
static void number(JsonBuf *b, const char *name, double value) {
    char text[128];
    snprintf(text, sizeof text, ",\"%s\":%.17g", name, value);
    raw(b, text);
}
static void string(JsonBuf *b, const char *name, const wchar_t *value) {
    raw(b, ",\""); raw(b, name); raw(b, "\":");
    json_buf_append_json_string(b, value);
}
static bool get_string(const char *s, const char *name, wchar_t *out, size_t cap) {
    char *text = malloc(strlen(s) + 1);
    if (!text) return false;
    bool ok = json_query_string(s, name, text, strlen(s) + 1);
    wchar_t *wide = ok ? json_utf8_to_utf16(text, strlen(text)) : NULL;
    ok = wide && wcslen(wide) < cap;
    if (ok) wcscpy(out, wide);
    free(wide); free(text);
    return ok;
}
static bool get_message_string(const char *s, const char *name,
    ChatMessage *message, bool reasoning, bool required) {
    size_t size=strlen(s)+1;
    char *text=(char *)malloc(size);
    if (!text) return false;
    bool found=json_query_string(s,name,text,size);
    if (!found) { free(text); return !required; }
    wchar_t *wide=json_utf8_to_utf16(text,strlen(text));
    free(text);
    if (!wide) return false;
    bool ok=reasoning ? chat_message_set_reasoning(message,wide) :
        chat_message_set_text(message,wide);
    free(wide);
    return ok;
}
static bool integer(const char *s, const char *name, double min, double max, double *out) {
    return json_query_number(s, name, out) && *out >= min && *out <= max && floor(*out) == *out;
}
/* True when `id` already belongs to a decoded conversation id or to any live
   message decoded before the record now being decoded (conversation `ci`,
   messages before index `j`). Message and conversation identities share the
   one persisted counter, so a collision in either direction is corruption.
   Known scaling risk, deliberate until a future pass justifies an index: the
   scan is quadratic in persisted messages, so a fully loaded maximum store
   (128 x 512 = 65,536 messages) can require roughly 2.15 billion prior-id
   comparisons. */
static bool message_id_taken(const Chat *chat, uint64_t id, int ci, size_t j) {
    for (int i = 0; i <= ci; i++) {
        const ChatConversation *c = &chat->conversations[i];
        if (c->id == id) return true;
        size_t limit = i < ci ? c->message_count : j;
        for (size_t k = 0; k < limit; k++)
            if (c->messages[k].id == id) return true;
    }
    return false;
}
/* True when a conversation id collides with any earlier conversation or with
   any message decoded so far. */
static bool conversation_id_taken(const Chat *chat, uint64_t id, int ci) {
    for (int i = 0; i < ci; i++) {
        const ChatConversation *c = &chat->conversations[i];
        if (c->id == id) return true;
        for (size_t k = 0; k < c->message_count; k++)
            if (c->messages[k].id == id) return true;
    }
    return false;
}
#define NUM(b,obj,field) number(b, #field, (double)(obj)->field)
#define STR(b,obj,field) string(b, #field, (obj)->field)
#define READ_INT(obj,field,min,max) do { if (!integer(line,#field,min,max,&v)) goto bad; (obj)->field = v; } while (0)
#define READ_NUM(obj,field) do { if (!json_query_number(line,#field,&v) || v < -1) goto bad; (obj)->field = v; } while (0)
#define READ_STR(obj,field) do { if (!get_string(line,#field,(obj)->field,sizeof (obj)->field / sizeof(wchar_t))) goto bad; } while (0)

static bool encode(const Chat *chat, JsonBuf *b) {
    json_buf_init(b, 8192);
    /* The emitted version is always the canonical format definition. */
    char header[64];
    snprintf(header, sizeof header,
        "{\"type\":\"settings\",\"version\":%d", FORMAT_VERSION);
    raw(b, header);
    NUM(b, chat, next_id);
    NUM(b, chat, active);
    NUM(b, chat, conversation_count);
    NUM(b, chat, model_history_count);
    NUM(b, chat, window_x);
    NUM(b, chat, window_y);
    NUM(b, chat, window_width);
    NUM(b, chat, window_height);
    NUM(b, chat, maximized);
    NUM(b, chat, sidebar_width);
    STR(b, chat, model);
    STR(b, chat, system_prompt);
    raw(b, "}\n");
    for (int i = 0; i < chat->model_history_count; i++) {
        raw(b, "{\"type\":\"model\""); string(b, "model", chat->model_history[i]); raw(b, "}\n");
    }
    for (int i = 0; i < chat->conversation_count; i++) {
        const ChatConversation *c = &chat->conversations[i];
        raw(b, "{\"type\":\"conversation\"");
        NUM(b, c, id);
        NUM(b, c, created_at);
        NUM(b, c, modified_at);
        NUM(b, c, renamed);
        NUM(b, c, message_count);
        STR(b, c, title);
        STR(b, c, draft);
        raw(b, "}\n");
        for (size_t j = 0; j < c->message_count; j++) {
            const ChatMessage *m = &c->messages[j];
            const ChatGeneration *g = &m->generation;
            raw(b, "{\"type\":\"message\"");
            /* Stable message identity: the persisted counter value this
               message was allocated from. Older builds that do not know the
               field simply ignore it. */
            NUM(b, m, id);
            NUM(b, m, role);
            NUM(b, m, created_at);
            NUM(b, m, modified_at);
            string(b, "text", chat_message_text(m));
            NUM(b, g, state);
            NUM(b, g, started_at);
            NUM(b, g, finished_at);
            NUM(b, g, first_token_at);
            NUM(b, g, ttft_ms);
            NUM(b, g, latency_ms);
            NUM(b, g, prompt_tokens);
            NUM(b, g, completion_tokens);
            NUM(b, g, total_tokens);
            NUM(b, g, cost);
            STR(b, g, requested_model);
            STR(b, g, actual_model);
            STR(b, g, finish_reason);
            STR(b, g, error);
            /* Optional, appended last so a turn without reasoning is byte-for-byte
               the same shape as an older version 1 message line. */
            if (chat_message_reasoning(m)[0])
                string(b, "reasoning", chat_message_reasoning(m));
            if (g->reasoning_ms >= 0) number(b, "reasoning_ms", g->reasoning_ms);
            raw(b, "}\n");
        }
    }
    uint32_t hash = checksum(b->data, b->length);
    raw(b, "{\"type\":\"commit\""); number(b, "checksum", hash); raw(b, "}\n");
    return json_buf_ok(b) && b->length <= STORAGE_LIMIT;
}

static char *next_line(char **cursor) {
    char *line = *cursor;
    char *end = strchr(line, '\n');
    if (!end) return NULL;
    *end = 0; *cursor = end + 1;
    return json_validate(line) ? line : NULL;
}
static bool type_is(const char *line, const char *type) {
    char value[32];
    return line && json_query_string(line, "type", value, sizeof value) && !strcmp(value,type);
}
/* Decode into a separate Chat; never expose a partially loaded snapshot. */
static bool decode(char *data, Chat *chat) {
    size_t length = strlen(data);
    if (!length || data[length - 1] != '\n') return false;
    char *footer = data + length - 1;
    while (footer > data && footer[-1] != '\n') --footer;
    double v;
    size_t declared;
    if (!json_validate(footer) || !type_is(footer,"commit") ||
        !integer(footer,"checksum",0,4294967295.0,&v) ||
        (uint32_t)v != checksum(data,(size_t)(footer-data))) return false;
    char *cursor = data, *line = next_line(&cursor);
    if (!type_is(line,"settings") ||
        !integer(line,"version",FORMAT_VERSION_MIN,FORMAT_VERSION,&v)) return false;
    memset(chat,0,sizeof *chat);
    READ_INT(chat, next_id, 1, (double)CHAT_MAX_ID);
    /* The counter exactly as it was persisted: every persisted identity is
       validated against this ceiling, never against the mutated counter that
       grows while ids are synthesized for older id-less messages. */
    double stored_next_id = (double)chat->next_id;
    READ_INT(chat, active, 0, CHAT_MAX_CONVERSATIONS-1);
    READ_INT(chat, conversation_count, 1, CHAT_MAX_CONVERSATIONS);
    READ_INT(chat, model_history_count, 0, CHAT_MODEL_HISTORY);
    READ_INT(chat, window_x, -100000, 100000);
    READ_INT(chat, window_y, -100000, 100000);
    READ_INT(chat, window_width, 720, 10000);
    READ_INT(chat, window_height, 480, 10000);
    READ_INT(chat, maximized, 0, 1);
    READ_INT(chat, sidebar_width, 160, 360);
    READ_STR(chat, model);
    READ_STR(chat, system_prompt);
    if (chat->active >= chat->conversation_count || !chat->model[0]) goto bad;
    for (int i=0; i<chat->model_history_count; i++) {
        line=next_line(&cursor);
        if (!type_is(line,"model") || !get_string(line,"model",chat->model_history[i],CHAT_MODEL_TEXT)) goto bad;
    }
    for (int i=0; i<chat->conversation_count; i++) {
        ChatConversation *c=&chat->conversations[i];
        line=next_line(&cursor);
        if (!type_is(line,"conversation")) goto bad;
        READ_INT(c, id, 1, stored_next_id);
        READ_INT(c, created_at, 1, 9007199254740991.0);
        READ_INT(c, modified_at, 1, 9007199254740991.0);
        READ_INT(c, renamed, 0, 1);
        READ_INT(c, message_count, 0, CHAT_MAX_MESSAGES);
        /* A declared count is not a live count. Reserve backing storage for
           the declared messages, but let message_count track only slots that
           were actually constructed: each scratch slot is zeroed (fresh
           realloc memory is uninitialized) and marked live before any
           fallible decoding, so the quarantine dispose below can never free a
           garbage overflow pointer and a failed message decode disposes
           exactly the valid partial state. */
        declared = c->message_count;
        c->message_count = 0;
        if (!chat_reserve_messages(c, declared)) goto bad;
        READ_STR(c, title);
        READ_STR(c, draft);
        if (conversation_id_taken(chat, c->id, i)) goto bad;
        for (size_t j=0; j<declared; j++) {
            ChatMessage *m=&c->messages[j]; ChatGeneration *g=&m->generation;
            memset(m, 0, sizeof *m);
            c->message_count = j + 1;   /* live before any fallible decoding */
            line=next_line(&cursor);
            if (!type_is(line,"message")) goto bad;
            /* Stable message identity. A persisted id must be an exact
               integer in [1, stored_next_id] and globally unused; an absent
               id (older version 1 snapshots) is synthesized deterministically
               in file order from the live counter. Persisted ids are never
               re-checked against the mutated counter, so a stored id equal to
               a value this pass already synthesized is out of range and
               rejected. Counter exhaustion is corruption and fails the whole
               snapshot transactionally. */
            {
                JsonFieldKind kind;
                double id_value;
                if (!json_query_field(line,"id",&kind,&id_value)) goto bad;
                if (kind == JSON_FIELD_ABSENT) {
                    if (chat->next_id >= CHAT_MAX_ID) goto bad;
                    m->id = ++chat->next_id;
                } else if (kind == JSON_FIELD_NUMBER &&
                    id_value >= 1.0 && id_value <= stored_next_id &&
                    floor(id_value) == id_value) {
                    m->id = (uint64_t)id_value;
                } else goto bad;
                if (message_id_taken(chat, m->id, i, j)) goto bad;
            }
            READ_INT(m, role, 0, CHAT_ROLE_ERROR);
            READ_INT(m, created_at, 1, 9007199254740991.0);
            READ_INT(m, modified_at, 1, 9007199254740991.0);
            if (!get_message_string(line,"text",m,false,true)) goto bad;
            READ_INT(g, state, 0, CHAT_GENERATION_FAILED);
            READ_INT(g, started_at, 0, 9007199254740991.0);
            READ_INT(g, finished_at, 0, 9007199254740991.0);
            READ_INT(g, first_token_at, 0, 9007199254740991.0);
            READ_NUM(g, ttft_ms);
            READ_NUM(g, latency_ms);
            READ_NUM(g, prompt_tokens);
            READ_NUM(g, completion_tokens);
            READ_NUM(g, total_tokens);
            READ_NUM(g, cost);
            READ_STR(g, requested_model);
            READ_STR(g, actual_model);
            READ_STR(g, finish_reason);
            READ_STR(g, error);
            /* Optional fields: absent in older version 1 snapshots. */
            if (!get_message_string(line,"reasoning",m,true,false)) goto bad;
            g->reasoning_ms=-1;
            if (json_query_number(line,"reasoning_ms",&v) && v>=-1)
                g->reasoning_ms=v;
            if (g->state == CHAT_GENERATION_RUNNING) {
                g->state = CHAT_GENERATION_INTERRUPTED;
                /* End time is unknown after a crash; do not invent latency. */
                wcscpy(g->error,L"Application exited before generation finished.");
            }
        }
    }
    if (cursor != footer) goto bad;
    return true;
bad:
    chat_dispose(chat);
    memset(chat,0,sizeof *chat);
    return false;
}
static bool read_snapshot(const wchar_t *path, Chat *chat, bool *unsupported) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok=GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<=STORAGE_LIMIT;
    char *data=ok ? malloc((size_t)size.QuadPart+1) : NULL;
    DWORD got=0;
    ok=data && ReadFile(file,data,(DWORD)size.QuadPart,&got,NULL) && got==(DWORD)size.QuadPart;
    CloseHandle(file);
    if (ok) {
        data[got]=0;
        double version;
        if (json_query_number(data,"version",&version) &&
            (version<FORMAT_VERSION_MIN || version>FORMAT_VERSION)) *unsupported=true;
        ok=!*unsupported && strlen(data)==got && decode(data,chat);
    }
    free(data);
    return ok;
}

/* Flush the backup before replacing the primary. If this fails, the primary
   is still untouched and save reports failure to the host. */
static bool durable_copy(const wchar_t *source, const wchar_t *target) {
    if (!CopyFileW(source,target,FALSE)) return false;
    HANDLE file=CreateFileW(target,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if (file==INVALID_HANDLE_VALUE) return false;
    bool ok=FlushFileBuffers(file)!=0;
    CloseHandle(file);
    return ok;
}

bool storage_open(ChatStorage *store, const wchar_t *directory) {
    memset(store,0,sizeof *store);
    wchar_t dir[1024];
    if (directory) {
        if (wcslen(directory)>900) return false;
        wcscpy(dir,directory);
    } else {
        DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",dir,900);
        if (!n || n>=900) return false;
        wcscat(dir,L"\\DarkChat");
    }
    if (!CreateDirectoryW(dir,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    swprintf(store->path,1024,L"%ls\\state.jsonl",dir);
    swprintf(store->backup,1024,L"%ls\\state.bak.jsonl",dir);
    swprintf(store->temporary,1024,L"%ls\\state.tmp.jsonl",dir);
    wchar_t lock[1024]; swprintf(lock,1024,L"%ls\\writer.lock",dir);
    store->lock=CreateFileW(lock,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    store->writable=store->lock!=INVALID_HANDLE_VALUE;
    return store->writable;
}
int storage_load(ChatStorage *store, Chat *chat) {
    if (!store->writable) return -1;
    Chat *loaded=calloc(1,sizeof *loaded);
    if (!loaded) { store->writable=false; return -1; }
    const wchar_t *paths[]={store->path,store->backup,store->temporary};
    bool exists=false;
    for (int i=0;i<3;i++) {
        if (GetFileAttributesW(paths[i])!=INVALID_FILE_ATTRIBUTES) exists=true;
        bool unsupported=false;
        bool valid=read_snapshot(paths[i],loaded,&unsupported);
        if (unsupported) {
            chat_dispose(loaded); free(loaded);
            store->writable=false; return -1;
        }
        if (valid) {
            /* A recovered temp may be the only valid copy. Preserve it before
               the next save reuses the temp filename. */
            if (i==2 && !durable_copy(store->temporary,store->backup)) {
                chat_dispose(loaded); free(loaded);
                store->writable=false; return -1;
            }
            chat_dispose(chat);
            *chat=*loaded; free(loaded);
            store->primary_valid=i==0; store->recovered=i!=0;
            return 1;
        }
    }
    chat_dispose(loaded); free(loaded);
    if (exists) { store->writable=false; return -1; }
    return 0;
}
bool storage_save(ChatStorage *store, const Chat *chat) {
    if (!store->writable) return false;
    JsonBuf b;
    bool ok=encode(chat,&b);
    if (!ok) { json_buf_free(&b); return false; }
    /* Copy only the last validated primary. Never rotate a corrupt file over
       the recovery backup, and abort if backup creation fails. */
    if (store->primary_valid && !durable_copy(store->path,store->backup)) {
        json_buf_free(&b); return false;
    }
    HANDLE file=CreateFileW(store->temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD written=0;
    ok=file!=INVALID_HANDLE_VALUE && WriteFile(file,b.data,(DWORD)b.length,&written,NULL) &&
        written==b.length && FlushFileBuffers(file);
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    json_buf_free(&b);
    if (ok) ok=MoveFileExW(store->temporary,store->path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (ok) store->primary_valid=true;
    return ok;
}
void storage_close(ChatStorage *store) {
    if (store->lock && store->lock!=INVALID_HANDLE_VALUE) CloseHandle(store->lock);
    store->lock=NULL; store->writable=false;
}
