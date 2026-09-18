#include "../chat/storage.h"
#include "../chat/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Allocation seam for the decode-failure tests: the storage test links with
   -Wl,--wrap=malloc and -Wl,--wrap=realloc (chat.bat test does), so every
   message-array reservation is interceptable and fresh array memory is
   poisoned. A successful decode therefore cannot rely on zero-filled memory:
   an uninitialized slot would expose 0x5C5C... garbage. Injection is armed
   immediately before the operation under test and reset right after:
   - pass_mallocs / fail_mallocs: skip that many mallocs, then fail that many.
   - fail_malloc_size / fail_big_mallocs: fail the next N mallocs of at least
     that size (targets one message's UTF-16 expansion mid-decode).
   - fail_realloc_index: fail the nth realloc call (the conversation
     message-array reservations are the only reallocs a load performs). */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
static long pass_mallocs, fail_mallocs;
static size_t fail_malloc_size;
static long fail_big_mallocs;
static long fail_realloc_index, realloc_seen;
void *__wrap_malloc(size_t size) {
    if (pass_mallocs > 0) { --pass_mallocs; return __real_malloc(size); }
    if (fail_mallocs > 0) { --fail_mallocs; return NULL; }
    if (fail_big_mallocs > 0 && fail_malloc_size && size >= fail_malloc_size) {
        --fail_big_mallocs; return NULL;
    }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    ++realloc_seen;
    if (fail_realloc_index > 0 && realloc_seen == fail_realloc_index) return NULL;
    void *grown = __real_realloc(pointer, size);
    if (grown && !pointer) memset(grown, 0x5C, size);
    return grown;
}
static void seam_reset(void) {
    pass_mallocs = fail_mallocs = fail_big_mallocs = 0;
    fail_malloc_size = 0;
    fail_realloc_index = 0;
    realloc_seen = 0;
}

/* Compares two generations by every field the format persists. */
static bool same_generation(const ChatGeneration *a, const ChatGeneration *b) {
    return a->state == b->state && a->backend == b->backend &&
        a->started_at == b->started_at && a->finished_at == b->finished_at &&
        a->first_token_at == b->first_token_at &&
        a->ttft_ms == b->ttft_ms && a->latency_ms == b->latency_ms &&
        a->prompt_tokens == b->prompt_tokens &&
        a->completion_tokens == b->completion_tokens &&
        a->total_tokens == b->total_tokens && a->cost == b->cost &&
        a->reasoning_ms == b->reasoning_ms &&
        !wcscmp(a->requested_model, b->requested_model) &&
        !wcscmp(a->actual_model, b->actual_model) &&
        !wcscmp(a->finish_reason, b->finish_reason) &&
        !wcscmp(a->error, b->error);
}
static bool same_message(const ChatMessage *a, const ChatMessage *b) {
    return a->role == b->role && a->id == b->id &&
        a->created_at == b->created_at &&
        a->modified_at == b->modified_at &&
        !wcscmp(chat_message_text(a), chat_message_text(b)) &&
        !wcscmp(chat_message_reasoning(a), chat_message_reasoning(b)) &&
        same_generation(&a->generation, &b->generation);
}
/* Compares every logically persisted field, including the stable message ids.
    View bookkeeping (revisions, expansion) and runtime-only state (status,
    reply counter) are deliberately excluded: they are not part of a snapshot. */
static bool same_chat(const Chat *a, const Chat *b) {
    if (a->next_id != b->next_id || a->active != b->active ||
        a->conversation_count != b->conversation_count ||
        a->model_history_count != b->model_history_count ||
        a->window_x != b->window_x || a->window_y != b->window_y ||
        a->window_width != b->window_width ||
        a->window_height != b->window_height ||
        a->maximized != b->maximized || a->sidebar_width != b->sidebar_width ||
        a->sidebar_collapsed != b->sidebar_collapsed ||
        wcscmp(a->model, b->model) ||
        a->backend != b->backend ||
        wcscmp(a->ollama_model, b->ollama_model) ||
        wcscmp(a->system_prompt, b->system_prompt) ||
        a->provider_routing.sort != b->provider_routing.sort ||
        a->provider_routing.disallow_fallbacks != b->provider_routing.disallow_fallbacks ||
        a->provider_routing.data_collection != b->provider_routing.data_collection ||
        a->provider_routing.zdr != b->provider_routing.zdr) return false;
    for (int i = 0; i < a->model_history_count; i++)
        if (wcscmp(a->model_history[i], b->model_history[i]) ||
            a->model_history_backend[i] != b->model_history_backend[i])
            return false;
    for (int i = 0; i < a->conversation_count; i++) {
        const ChatConversation *ca = &a->conversations[i];
        const ChatConversation *cb = &b->conversations[i];
        if (ca->id != cb->id || ca->created_at != cb->created_at ||
            ca->modified_at != cb->modified_at || ca->renamed != cb->renamed ||
            ca->message_count != cb->message_count ||
            wcscmp(ca->title, cb->title) || wcscmp(ca->draft, cb->draft))
            return false;
        if (ca->message_count && (!ca->messages || !cb->messages)) return false;
        for (size_t j = 0; j < ca->message_count; j++)
            if (!same_message(&ca->messages[j], &cb->messages[j])) return false;
    }
    return true;
}

static void corrupt(const wchar_t *path) {
    FILE *f=_wfopen(path,L"wb");
    if (f) { fputs("{\"version\":1,\"truncated\":",f); fclose(f); }
}
static void remove_store(const ChatStorage *store, const wchar_t *dir) {
    DeleteFileW(store->path); DeleteFileW(store->backup); DeleteFileW(store->temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock);
    RemoveDirectoryW(dir);
}

/* The commit checksum is the same FNV-1a the storage module computes. */
static uint32_t fnv1a(const char *s, size_t n) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ (unsigned char)s[i]) * 16777619u;
    return hash;
}
/* Writes a hand-built snapshot: one record per line, each LF-terminated,
    followed by a commit record whose checksum covers every byte before it. */
static bool write_snapshot(const wchar_t *path, const char *const *lines,
    size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += strlen(lines[i]) + 1;
    char *body = malloc(total);
    FILE *f = body ? _wfopen(path, L"wb") : NULL;
    if (!f) { free(body); return false; }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(lines[i]);
        memcpy(body + used, lines[i], n);
        used += n;
        body[used++] = '\n';
    }
    char commit[96];
    snprintf(commit, sizeof commit, "{\"type\":\"commit\",\"checksum\":%u}",
        fnv1a(body, used));
    bool ok = fwrite(body, 1, used, f) == used &&
        fwrite(commit, 1, strlen(commit), f) == strlen(commit) &&
        fwrite("\n", 1, 1, f) == 1;
    fclose(f);
    free(body);
    return ok;
}
/* One message record; `id_field` is `""` for an older id-less record or
    something like `"\"id\":7,"` for a new-format one. */
static void msg_raw(char *out, size_t cap, const char *id_field) {
    snprintf(out, cap,
        "{\"type\":\"message\",%s\"role\":0,\"created_at\":1000,"
        "\"modified_at\":1000,\"text\":\"m\",\"state\":0,\"started_at\":0,"
        "\"finished_at\":0,\"first_token_at\":0,\"ttft_ms\":-1,\"latency_ms\":-1,"
        "\"prompt_tokens\":-1,\"completion_tokens\":-1,\"total_tokens\":-1,"
        "\"cost\":-1,\"requested_model\":\"\",\"actual_model\":\"\","
        "\"finish_reason\":\"\",\"error\":\"\"}", id_field);
}
/* Builds a fresh one-conversation, two-message snapshot in its own store
    directory, loads it and removes the store. Returns storage_load's result
    (or -2 when building failed). A rejected load leaves `dest` untouched. */
static int load_case(const wchar_t *tag, long long next_id, const char *m0,
    const char *m1, Chat *dest) {
    wchar_t dir[256];
    swprintf(dir, 256, L"build\\storage-id-%ls-%lu", tag, GetCurrentProcessId());
    ChatStorage store;
    if (!storage_open(&store, dir)) return -2;
    char settings[384], conversation[256];
    snprintf(settings, sizeof settings,
        "{\"type\":\"settings\",\"version\":1,\"next_id\":%lld,\"active\":0,"
        "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
        "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
        "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
        "\"system_prompt\":\"\"}", next_id);
    snprintf(conversation, sizeof conversation,
        "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
        "\"modified_at\":1000,\"renamed\":0,\"message_count\":2,"
        "\"title\":\"c\",\"draft\":\"\"}");
    const char *lines[4] = { settings, conversation, m0, m1 };
    int result = write_snapshot(store.path, lines, 4) ?
        storage_load(&store, dest) : -2;
    storage_close(&store);
    remove_store(&store, dir);
    return result;
}
/* Builds a one-conversation, no-message snapshot whose settings line carries
    `provider_fields` verbatim (empty or NULL for an absent-field snapshot),
    loads it and removes the store. */
static int load_routing_case(const char *provider_fields, Chat *dest) {
    wchar_t dir[256];
    swprintf(dir, 256, L"build\\storage-route-%lu", GetCurrentProcessId());
    ChatStorage store;
    if (!storage_open(&store, dir)) return -2;
    if (!provider_fields) provider_fields = "";
    char settings[512], conversation[256];
    snprintf(settings, sizeof settings,
        "{\"type\":\"settings\",\"version\":1,\"next_id\":1,\"active\":0,"
        "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
        "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
        "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
        "\"system_prompt\":\"\"%s%s}", provider_fields[0] ? "," : "",
        provider_fields);
    snprintf(conversation, sizeof conversation,
        "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
        "\"modified_at\":1000,\"renamed\":0,\"message_count\":0,"
        "\"title\":\"c\",\"draft\":\"\"}");
    const char *lines[2] = { settings, conversation };
    int result = write_snapshot(store.path, lines, 2) ?
        storage_load(&store, dest) : -2;
    storage_close(&store);
    remove_store(&store, dir);
    return result;
}
/* Builds a one-conversation, no-message snapshot whose settings line carries
    `backend_fields` verbatim (empty or NULL for an absent-field snapshot),
    loads it and removes the store. */
static int load_backend_case(const char *backend_fields, Chat *dest) {
    wchar_t dir[256];
    swprintf(dir, 256, L"build\\storage-backend-%lu", GetCurrentProcessId());
    ChatStorage store;
    if (!storage_open(&store, dir)) return -2;
    if (!backend_fields) backend_fields = "";
    char settings[512], conversation[256];
    snprintf(settings, sizeof settings,
        "{\"type\":\"settings\",\"version\":1,\"next_id\":1,\"active\":0,"
        "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
        "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
        "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
        "\"system_prompt\":\"\"%s%s}", backend_fields[0] ? "," : "",
        backend_fields);
    snprintf(conversation, sizeof conversation,
        "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
        "\"modified_at\":1000,\"renamed\":0,\"message_count\":0,"
        "\"title\":\"c\",\"draft\":\"\"}");
    const char *lines[2] = { settings, conversation };
    int result = write_snapshot(store.path, lines, 2) ?
        storage_load(&store, dest) : -2;
    storage_close(&store);
    remove_store(&store, dir);
    return result;
}
/* Downgrade simulation: removes the stable id field from every message
    record, recomputes the commit checksum and rewrites the file, so an older
    build could have written it. */
static bool downgrade_strip_ids(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }
    char *data = malloc((size_t)size + 1), *out = malloc((size_t)size + 128);
    bool ok = data && out && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) { free(data); free(out); return false; }
    data[size] = 0;
    size_t used = 0;
    char *body_end = NULL;   /* everything before the commit record */
    char *cursor = data;
    while (cursor < data + size) {
        char *end = strchr(cursor, '\n');
        if (!end) end = data + size;
        size_t len = (size_t)(end - cursor);
        char *line = malloc(len + 1);
        if (!line) { free(data); free(out); return false; }
        memcpy(line, cursor, len);
        line[len] = 0;
        bool commit = strstr(line, "\"type\":\"commit\"") != NULL;
        if (!commit && strstr(line, "\"type\":\"message\"")) {
            char *field = strstr(line, "\"id\":");
            if (field) {
                char *p = field + 5;
                while (*p >= '0' && *p <= '9') ++p;
                if (p != field + 5 && *p == ',') {
                    size_t removed = (size_t)(p + 1 - field);
                    memmove(field, field + removed, strlen(field + removed) + 1);
                    len -= removed;
                }
            }
        }
        memcpy(out + used, line, len);
        used += len;
        out[used++] = '\n';
        free(line);
        if (commit) { body_end = out + used - (len + 1); break; }
        cursor = end + 1;
    }
    if (!body_end) { free(data); free(out); return false; }
    char commit[96];
    snprintf(commit, sizeof commit, "{\"type\":\"commit\",\"checksum\":%u}",
        fnv1a(out, (size_t)(body_end - out)));
    f = _wfopen(path, L"wb");
    ok = f && fwrite(out, 1, (size_t)(body_end - out), f) ==
        (size_t)(body_end - out) &&
        fwrite(commit, 1, strlen(commit), f) == strlen(commit) &&
        fwrite("\n", 1, 1, f) == 1;
    if (f) fclose(f);
    free(data); free(out);
    return ok;
}
static bool read_file_bytes(const wchar_t *path, char **out, size_t *size) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = length > 0 ? malloc((size_t)length) : NULL;
    bool ok = data && fread(data, 1, (size_t)length, f) == (size_t)length;
    fclose(f);
    if (ok) { *out = data; *size = (size_t)length; }
    else free(data);
    return ok;
}
int main(void) {
    wchar_t dir[256]; swprintf(dir,256,L"build\\storage-test-%lu",GetCurrentProcessId());
    char *first_bytes=NULL; size_t first_size=0;
    Chat *chat=calloc(1,sizeof *chat), *loaded=calloc(1,sizeof *loaded);
    CHECK(chat && loaded);
    chat_init(chat); chat_clear(chat);
    ChatStorage store, other;
    CHECK(storage_open(&store,dir));
    CHECK(!storage_open(&other,dir)); storage_close(&other);
    CHECK(storage_load(&store,loaded)==0);
    uint64_t id=chat_active(chat)->id;
    CHECK(chat_rename(chat,L"Renamed \xd83d\xde80"));
    CHECK(chat_active(chat)->id==id);
    wcscpy(chat->system_prompt,L"Be concise.\nUnicode \x03bb");
    wcscpy(chat->conversations[0].draft,L"Unsent draft");
    chat->sidebar_width=280;
    chat->sidebar_collapsed=1;
    int n=chat_begin_response(chat,CHAT_SEND,L"First question"); CHECK(n==1);
    ChatMessage *m=&chat->conversations[0].messages[n];
    wcscpy(m->text,L"Partial \xd83d\xde80\n```c\nint x;\n```");
    wcscpy(m->generation.actual_model,L"actual/model");
    m->generation.ttft_ms=123.5; m->generation.prompt_tokens=42;
    m->generation.completion_tokens=8; m->generation.total_tokens=50; m->generation.cost=0.000123;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->conversations[0].id==id);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
    CHECK(loaded->conversations[0].messages[1].generation.cost==0.000123);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].text,m->text));
    CHECK(!wcscmp(loaded->system_prompt,chat->system_prompt));
    CHECK(loaded->sidebar_width==280 && loaded->model_history_count==1);
    CHECK(loaded->sidebar_collapsed==1);
    CHECK(!wcscmp(loaded->conversations[0].draft,L"Unsent draft"));
    /* A message line without the optional reasoning fields (an older version 1
       snapshot) loads with empty reasoning and unavailable duration. */
    CHECK(!loaded->conversations[0].messages[1].reasoning[0]);
    CHECK(loaded->conversations[0].messages[1].generation.reasoning_ms==-1);
    wcscpy(m->reasoning,L"Checked the options and chose this.");
    m->generation.reasoning_ms=2500.0;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].reasoning,
        L"Checked the options and chose this."));
    CHECK(loaded->conversations[0].messages[1].generation.reasoning_ms==2500.0);
    /* Provider routing is optional and persisted only when non-default. A
       default snapshot carries no provider fields and reloads to defaults. */
    CHECK(loaded->provider_routing.sort==CHAT_PROVIDER_SORT_DEFAULT &&
        !loaded->provider_routing.disallow_fallbacks &&
        loaded->provider_routing.data_collection==CHAT_DATA_COLLECTION_ALLOW &&
        !loaded->provider_routing.zdr);
    chat->provider_routing.sort=CHAT_PROVIDER_SORT_LATENCY;
    chat->provider_routing.disallow_fallbacks=true;
    chat->provider_routing.data_collection=CHAT_DATA_COLLECTION_DENY;
    chat->provider_routing.zdr=true;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->provider_routing.sort==CHAT_PROVIDER_SORT_LATENCY &&
        loaded->provider_routing.disallow_fallbacks &&
        loaded->provider_routing.data_collection==CHAT_DATA_COLLECTION_DENY &&
        loaded->provider_routing.zdr);
    {
        char *routed=NULL; size_t routed_size=0;
        CHECK(read_file_bytes(store.path,&routed,&routed_size));
        CHECK(strstr(routed,"\"provider_sort\":3")!=NULL);
        CHECK(strstr(routed,"\"provider_no_fallbacks\":1")!=NULL);
        CHECK(strstr(routed,"\"provider_data_collection\":1")!=NULL);
        CHECK(strstr(routed,"\"provider_zdr\":1")!=NULL);
        free(routed);
    }
    /* Reset to defaults: the settings line then has the older shape with no
       provider fields at all, and reloading preserves the defaults. */
    chat->provider_routing.sort=CHAT_PROVIDER_SORT_DEFAULT;
    chat->provider_routing.disallow_fallbacks=false;
    chat->provider_routing.data_collection=CHAT_DATA_COLLECTION_ALLOW;
    chat->provider_routing.zdr=false;
    CHECK(storage_save(&store,chat));
    {
        char *plain=NULL; size_t plain_size=0;
        CHECK(read_file_bytes(store.path,&plain,&plain_size));
        CHECK(strstr(plain,"provider_")==NULL);
        free(plain);
    }
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->provider_routing.sort==CHAT_PROVIDER_SORT_DEFAULT &&
        !loaded->provider_routing.disallow_fallbacks &&
        loaded->provider_routing.data_collection==CHAT_DATA_COLLECTION_ALLOW &&
        !loaded->provider_routing.zdr);
    /* Every optional routing field decodes; an absent field defaults, and a
       present but malformed one rejects the snapshot. */
    {
        Chat *dest=calloc(1,sizeof *dest); CHECK(dest);
        CHECK(load_routing_case(
            "\"provider_sort\":1,\"provider_no_fallbacks\":1,"
            "\"provider_data_collection\":1,\"provider_zdr\":1",dest)==1);
        CHECK(dest->provider_routing.sort==CHAT_PROVIDER_SORT_PRICE &&
            dest->provider_routing.disallow_fallbacks &&
            dest->provider_routing.data_collection==CHAT_DATA_COLLECTION_DENY &&
            dest->provider_routing.zdr);
        /* Absent fields are not corruption: the OpenRouter defaults apply. */
        CHECK(load_routing_case(NULL,dest)==1);
        CHECK(dest->provider_routing.sort==CHAT_PROVIDER_SORT_DEFAULT &&
            !dest->provider_routing.disallow_fallbacks &&
            dest->provider_routing.data_collection==CHAT_DATA_COLLECTION_ALLOW &&
            !dest->provider_routing.zdr);
        /* A present but malformed field rejects the whole snapshot. */
        static const char *const malformed[] = {
            "\"provider_sort\":99",              /* out of range */
            "\"provider_sort\":-1",              /* out of range */
            "\"provider_no_fallbacks\":2",       /* out of range */
            "\"provider_data_collection\":-1",   /* out of range */
            "\"provider_zdr\":1.5",              /* not an exact integer */
            "\"provider_sort\":\"price\"",       /* wrong type */
            "\"provider_zdr\":true",             /* wrong type */
            "\"provider_sort\":null"             /* wrong type */
        };
        for (size_t i=0;i<sizeof malformed/sizeof malformed[0];i++) {
            CHECK(load_routing_case(malformed[i],dest)==-1);
            CHECK(load_routing_case(NULL,dest)==1);   /* store stays loadable */
        }
        chat_dispose(dest); free(dest);
    }
    /* The active backend and the per-backend models are additive at the same
       format version: an OpenRouter-only snapshot carries neither field, an
       Ollama selection round-trips, and a malformed backend rejects. */
    {
        char *absent=NULL; size_t absent_size=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&absent,&absent_size));
        CHECK(strstr(absent,"\"backend\"")==NULL);
        CHECK(strstr(absent,"\"ollama_model\"")==NULL);
        free(absent);
    }
    chat->backend=CHAT_BACKEND_OLLAMA;
    wcscpy(chat->ollama_model,L"llama3.2:latest");
    m->generation.backend=CHAT_BACKEND_OLLAMA;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->backend==CHAT_BACKEND_OLLAMA);
    CHECK(!wcscmp(loaded->ollama_model,L"llama3.2:latest"));
    CHECK(loaded->conversations[0].messages[1].generation.backend==CHAT_BACKEND_OLLAMA);
    {
        char *selected=NULL; size_t selected_size=0;
        CHECK(read_file_bytes(store.path,&selected,&selected_size));
        CHECK(strstr(selected,"\"backend\":1")!=NULL);
        CHECK(strstr(selected,"\"ollama_model\":\"llama3.2:latest\"")!=NULL);
        free(selected);
    }
    /* Old-snapshot defaults and present-but-malformed rejection. */
    {
        Chat *dest=calloc(1,sizeof *dest); CHECK(dest);
        CHECK(load_backend_case("\"backend\":1,\"ollama_model\":\"local/m\"",dest)==1);
        CHECK(dest->backend==CHAT_BACKEND_OLLAMA &&
            !wcscmp(dest->ollama_model,L"local/m"));
        CHECK(load_backend_case(NULL,dest)==1);
        CHECK(dest->backend==CHAT_BACKEND_OPENROUTER && !dest->ollama_model[0]);
        static const char *const malformed[]={
            "\"backend\":2","\"backend\":-1","\"backend\":1.5",
            "\"backend\":\"ollama\"",
            "\"ollama_model\":42",                 /* present, wrong JSON type */
            "\"backend\":1",                       /* Ollama with no local model */
            "\"backend\":1,\"ollama_model\":\"\""   /* Ollama with an empty local model */
        };
        for (size_t i=0;i<sizeof malformed/sizeof malformed[0];i++) {
            CHECK(load_backend_case(malformed[i],dest)==-1);
            CHECK(load_backend_case(NULL,dest)==1);   /* store stays loadable */
        }
        chat_dispose(dest); free(dest);
    }
    /* The sidebar collapse preference is additive at format 3: emitted only
       when collapsed, absent means expanded, and a present malformed value
       rejects the snapshot. */
    {
        char *collapsed=NULL; size_t collapsed_size=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&collapsed,&collapsed_size));
        CHECK(strstr(collapsed,"\"sidebar_collapsed\":1")!=NULL);
        free(collapsed);
        chat->sidebar_collapsed=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&collapsed,&collapsed_size));
        CHECK(strstr(collapsed,"\"sidebar_collapsed\"")==NULL);
        free(collapsed);
        CHECK(storage_load(&store,loaded)==1);
        CHECK(loaded->sidebar_collapsed==0);
        /* The reordered settings line: an absent field defaults to expanded. */
        Chat *dest=calloc(1,sizeof *dest); CHECK(dest);
        CHECK(load_backend_case("\"sidebar_collapsed\":1",dest)==1);
        CHECK(dest->sidebar_collapsed==1);
        CHECK(load_backend_case(NULL,dest)==1);
        CHECK(dest->sidebar_collapsed==0);
        static const char *const malformed[]={
            "\"sidebar_collapsed\":2","\"sidebar_collapsed\":-1",
            "\"sidebar_collapsed\":0.5","\"sidebar_collapsed\":true",
            "\"sidebar_collapsed\":\"1\"","\"sidebar_collapsed\":null"
        };
        for (size_t i=0;i<sizeof malformed/sizeof malformed[0];i++) {
            CHECK(load_backend_case(malformed[i],dest)==-1);
            CHECK(load_backend_case(NULL,dest)==1);
        }
        chat_dispose(dest); free(dest);
    }
    /* Backend-tagged history round-trips: one backend's recent models never
       migrate to another backend's tag on load. */
    wcsncpy(chat->model_history[0],L"openrouter/history",CHAT_MODEL_TEXT-1);
    chat->model_history_backend[0]=CHAT_BACKEND_OPENROUTER;
    wcsncpy(chat->model_history[1],L"local/history",CHAT_MODEL_TEXT-1);
    chat->model_history_backend[1]=CHAT_BACKEND_OLLAMA;
    chat->model_history_count=2;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->model_history_count==2);
    CHECK(!wcscmp(loaded->model_history[0],L"openrouter/history") &&
        loaded->model_history_backend[0]==CHAT_BACKEND_OPENROUTER);
    CHECK(!wcscmp(loaded->model_history[1],L"local/history") &&
        loaded->model_history_backend[1]==CHAT_BACKEND_OLLAMA);
    {
        char *tags=NULL; size_t tags_size=0;
        CHECK(read_file_bytes(store.path,&tags,&tags_size));
        CHECK(strstr(tags,"\"model\":\"local/history\",\"backend\":1")!=NULL);
        CHECK(strstr(tags,"\"model\":\"openrouter/history\",\"backend\"")==NULL);
        free(tags);
    }
    chat->model_history_count=0;
    chat->backend=CHAT_BACKEND_OPENROUTER;
    chat->ollama_model[0]=0;
    m->generation.backend=CHAT_BACKEND_OPENROUTER;
    wchar_t *large_reasoning=(wchar_t *)malloc(70001*sizeof(wchar_t));
    CHECK(large_reasoning);
    for (int i=0;i<70000;i++) large_reasoning[i]=L'r';
    large_reasoning[70000]=0;
    CHECK(chat_message_set_reasoning(m,large_reasoning));
    free(large_reasoning);
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(wcslen(chat_message_reasoning(
        &loaded->conversations[0].messages[1]))==70000);
    m->generation.state=CHAT_GENERATION_COMPLETE;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
    HANDLE block=CreateFileW(store.temporary,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,0,NULL);
    CHECK(block!=INVALID_HANDLE_VALUE);
    CHECK(!storage_save(&store,chat)); CloseHandle(block);
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    block=CreateFileW(store.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(block!=INVALID_HANDLE_VALUE);
    CHECK(!storage_save(&store,chat)); CloseHandle(block);
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    corrupt(store.temporary); /* torn temp must not displace valid primary */
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    corrupt(store.path);
    CHECK(storage_load(&store,loaded)==1 && store.recovered);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(storage_save(&store,loaded)); /* must not rotate corrupt primary over backup */
    corrupt(store.path);
    CHECK(storage_load(&store,loaded)==1 && store.recovered);
    CHECK(storage_save(&store,loaded));
    CHECK(CopyFileW(store.path,store.temporary,FALSE));
    DeleteFileW(store.path); DeleteFileW(store.backup);
    CHECK(storage_load(&store,loaded)==1 && store.recovered); /* first-save temp recovery */
    corrupt(store.temporary); corrupt(store.backup);
    CHECK(storage_load(&store,loaded)==-1);
    CHECK(!storage_save(&store,chat)); /* preserve unreadable files */
    storage_close(&store);
    /* An unknown newer version must not be overwritten by an older backup:
        the load fails closed with writes disabled and no backup is tried. */
    CHECK(storage_open(&store,dir));
    CHECK(storage_save(&store,chat)); CHECK(storage_save(&store,chat));
    CHECK(read_file_bytes(store.path,&first_bytes,&first_size));
    CHECK(strstr(first_bytes,"\"version\":3")); /* this build writes format 3 */
    free(first_bytes);
    FILE *future=_wfopen(store.path,L"r+b"); CHECK(future);
    char header[64]={0}; CHECK(fread(header,1,63,future)==63);
    char *version=strstr(header,"\"version\":3"); CHECK(version);
    CHECK(fseek(future,(long)(version-header)+(long)strlen("\"version\":"),SEEK_SET)==0);
    fputc('4',future); fclose(future);
    CHECK(storage_load(&store,loaded)==-1 && !store.writable);
    storage_close(&store);
    DeleteFileW(store.path); DeleteFileW(store.backup); DeleteFileW(store.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    /* A version 1 snapshot produced by the previous build of the same format
       loads unchanged, decodes into reserved dynamic message capacity, and
       migrates: every id-less message gets a deterministic nonzero stable id
       in file order from the persisted counter, and the counter is bumped. */
    {
        wchar_t fdir[256]; swprintf(fdir,256,L"build\\storage-fixture-%lu",GetCurrentProcessId());
        ChatStorage fstore;
        CHECK(storage_open(&fstore,fdir));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.path,FALSE));
        Chat *fixture=calloc(1,sizeof *fixture);
        CHECK(fixture);
        CHECK(storage_load(&fstore,fixture)==1);
        CHECK(fixture->conversation_count==2);
        CHECK(!wcscmp(fixture->conversations[0].title,L"Renamed \xd83d\xde80"));
        CHECK(fixture->conversations[0].message_count==2);
        CHECK(!wcscmp(chat_message_text(&fixture->conversations[0].messages[0]),
            L"First question"));
        CHECK(fixture->conversations[0].messages!=NULL &&
            fixture->conversations[0].message_capacity>=2);
        CHECK(fixture->conversations[1].messages!=NULL &&
            fixture->conversations[1].message_capacity>=2);
        CHECK(!wcscmp(chat_message_text(&fixture->conversations[0].messages[1]),
            L"Regenerated answer with \x03bd math."));
        CHECK(fixture->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
        CHECK(fixture->conversations[0].messages[1].generation.cost==-1);
        CHECK(wcslen(chat_message_reasoning(
            &fixture->conversations[0].messages[1]))==25000);
        CHECK(fixture->conversations[0].messages[1].generation.reasoning_ms==1234.5);
        CHECK(!fixture->conversations[1].messages[1].reasoning[0]);
        CHECK(fixture->conversations[1].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
        CHECK(!wcscmp(fixture->system_prompt,L"Be concise.\nUnicode \x03bb"));
        CHECK(!wcscmp(fixture->conversations[0].draft,L"Unsent draft"));
        CHECK(fixture->provider_routing.sort==CHAT_PROVIDER_SORT_DEFAULT &&
            !fixture->provider_routing.disallow_fallbacks &&
            fixture->provider_routing.data_collection==CHAT_DATA_COLLECTION_ALLOW &&
            !fixture->provider_routing.zdr);
        /* Migration: the fixture stored next_id 1789338462008 and four
           id-less messages, so file order assigns 009, 010, 011, 012 and the
           counter ends exactly at the last synthesized id. */
        CHECK(fixture->conversations[0].messages[0].id==1789338462009);
        CHECK(fixture->conversations[0].messages[1].id==1789338462010);
        CHECK(fixture->conversations[1].messages[0].id==1789338462011);
        CHECK(fixture->conversations[1].messages[1].id==1789338462012);
        CHECK(fixture->next_id==1789338462012);
        /* Saving the migrated state writes the new format and reloading it
           reproduces the same logical content: migration is idempotent. */
        CHECK(storage_save(&fstore,fixture));
        CHECK(storage_load(&fstore,fixture)==1 && !fstore.recovered);
        CHECK(fixture->conversations[0].messages[0].id==1789338462009);
        CHECK(fixture->next_id==1789338462012);
        /* Repeated saves of the loaded state are byte-stable: every message
           now carries its persisted id and nothing else moves. */
        char *first=NULL,*second=NULL; size_t first_size=0,second_size=0;
        CHECK(storage_save(&fstore,fixture));
        CHECK(read_file_bytes(fstore.path,&first,&first_size));
        CHECK(storage_save(&fstore,fixture));
        CHECK(read_file_bytes(fstore.path,&second,&second_size));
        CHECK(first_size==second_size && !memcmp(first,second,first_size));
        free(first); free(second);
        chat_dispose(fixture); free(fixture);
        storage_close(&fstore);
        remove_store(&fstore,fdir);
    }
    /* Format 2 snapshots (the previous build's output, 64-message era) still
        decode: the record layout is unchanged, so a format 2 file loads and
        migrates to format 3 on its next save. */
    {
        wchar_t v2dir[256]; swprintf(v2dir,256,L"build\\storage-v2-%lu",GetCurrentProcessId());
        ChatStorage v2store;
        char settings[384], conversation[256], a[512], b[512];
        CHECK(storage_open(&v2store,v2dir));
        snprintf(settings, sizeof settings,
            "{\"type\":\"settings\",\"version\":2,\"next_id\":100,\"active\":0,"
            "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
            "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
            "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
            "\"system_prompt\":\"\"}");
        snprintf(conversation, sizeof conversation,
            "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
            "\"modified_at\":1000,\"renamed\":0,\"message_count\":2,"
            "\"title\":\"c\",\"draft\":\"\"}");
        msg_raw(a,sizeof a,"\"id\":2,"); msg_raw(b,sizeof b,"\"id\":3,");
        const char *v2_lines[4]={settings,conversation,a,b};
        CHECK(write_snapshot(v2store.path,v2_lines,4));
        CHECK(storage_load(&v2store,loaded)==1 && !v2store.recovered);
        CHECK(loaded->conversations[0].messages[0].id==2);
        CHECK(loaded->conversations[0].messages[1].id==3);
        CHECK(loaded->next_id==100);
        /* The next save emits the canonical format 3 and reloads. */
        CHECK(storage_save(&v2store,loaded));
        char *v2_saved=NULL; size_t v2_saved_size=0;
        CHECK(read_file_bytes(v2store.path,&v2_saved,&v2_saved_size));
        CHECK(v2_saved && strstr(v2_saved,"\"version\":3"));
        free(v2_saved);
        CHECK(storage_load(&v2store,loaded)==1 && !v2store.recovered);
        CHECK(loaded->conversations[0].messages[0].id==2);
        storage_close(&v2store);
        remove_store(&v2store,v2dir);
    }
    /* Format 3 at the shipped bound: a conversation with exactly 512 messages
        (including a promoted overflow answer and reasoning) saves, reloads
        and round-trips with count, order, stable ids and content intact. */
    {
        wchar_t f3dir[256]; swprintf(f3dir,256,L"build\\storage-full512-%lu",GetCurrentProcessId());
        ChatStorage f3store;
        Chat *full=calloc(1,sizeof *full), *back=calloc(1,sizeof *back);
        CHECK(full && back);
        chat_init(full); chat_clear(full);
        ChatConversation *c=&full->conversations[0];
        wchar_t text[64];
        for (int i=0;i<CHAT_MAX_MESSAGES;i++) {
            swprintf(text,64,L"message %d",i);
            CHECK(chat_append(full,i%2 ? CHAT_ROLE_ASSISTANT : CHAT_ROLE_USER,text)>=0);
            if (i%2) c->messages[i].generation.state=CHAT_GENERATION_COMPLETE;
        }
        CHECK(c->message_count==CHAT_MAX_MESSAGES &&
            c->message_capacity==CHAT_MAX_MESSAGES);
        wchar_t *big=(wchar_t *)malloc(70001*sizeof *big);
        CHECK(big);
        for (int i=0;i<70000;i++) big[i]=L'a';
        big[70000]=0;
        CHECK(chat_message_set_text(&c->messages[511],big));
        CHECK(chat_message_set_reasoning(&c->messages[511],L"why"));
        free(big);
        c->messages[511].generation.reasoning_ms=42.0;
        uint64_t first_id=c->messages[0].id, last_id=c->messages[511].id;
        CHECK(storage_open(&f3store,f3dir));
        CHECK(storage_save(&f3store,full));
        char *saved3=NULL; size_t saved3_size=0;
        CHECK(read_file_bytes(f3store.path,&saved3,&saved3_size));
        CHECK(saved3 && strstr(saved3,"\"version\":3"));   /* format 3 on disk */
        free(saved3);
        CHECK(storage_load(&f3store,back)==1 && !f3store.recovered);
        CHECK(same_chat(back,full));   /* logical equality, field for field */
        CHECK(back->conversations[0].message_count==CHAT_MAX_MESSAGES);
        CHECK(back->conversations[0].messages[0].id==first_id);
        CHECK(back->conversations[0].messages[511].id==last_id);
        CHECK(!wcscmp(chat_message_text(&back->conversations[0].messages[0]),
            L"message 0"));
        CHECK(!wcscmp(chat_message_text(&back->conversations[0].messages[510]),
            L"message 510"));
        CHECK(wcslen(chat_message_text(&back->conversations[0].messages[511]))
            ==70000);
        CHECK(!wcscmp(chat_message_reasoning(&back->conversations[0].messages[511]),
            L"why"));
        chat_dispose(full); free(full);
        chat_dispose(back); free(back);
        storage_close(&f3store);
        remove_store(&f3store,f3dir);
    }
    /* A checksummed format 3 snapshot declaring 513 messages exceeds the
        persisted bound. With no recovery copies the load fails closed:
        transactionally (the destination keeps its previous content), writes
        stay disabled, and every snapshot file is preserved byte for byte. */
    {
        wchar_t odir[256]; swprintf(odir,256,L"build\\storage-over512-%lu",GetCurrentProcessId());
        ChatStorage ostore;
        Chat *before=calloc(1,sizeof *before);
        CHECK(before);
        chat_init(before); chat_clear(before);
        CHECK(chat_append(before,CHAT_ROLE_USER,L"baseline")>=0);
        Chat *dest=chat_snapshot(before);   /* owned deep copy as the baseline */
        CHECK(dest);
        CHECK(storage_open(&ostore,odir));
        char settings[384], conversation[256];
        snprintf(settings, sizeof settings,
            "{\"type\":\"settings\",\"version\":3,\"next_id\":515,\"active\":0,"
            "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
            "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
            "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
            "\"system_prompt\":\"\"}");
        snprintf(conversation, sizeof conversation,
            "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
            "\"modified_at\":1000,\"renamed\":0,\"message_count\":513,"
            "\"title\":\"c\",\"draft\":\"\"}");
        char (*msgs)[400]=malloc((CHAT_MAX_MESSAGES+1)*sizeof *msgs);
        const char **lines=malloc((CHAT_MAX_MESSAGES+3)*sizeof *lines);
        CHECK(msgs && lines);
        lines[0]=settings; lines[1]=conversation;
        for (int i=0;i<CHAT_MAX_MESSAGES+1;i++) {
            char id_field[24];
            snprintf(id_field,sizeof id_field,"\"id\":%d,",i+2);
            msg_raw(msgs[i],sizeof *msgs,id_field);
            lines[2+i]=msgs[i];
        }
        CHECK(write_snapshot(ostore.path,lines,(size_t)CHAT_MAX_MESSAGES+3));
        free(msgs); free(lines);
        DeleteFileW(ostore.backup); DeleteFileW(ostore.temporary);   /* no recovery copies */
        char *sent=NULL; size_t sent_size=0;
        CHECK(read_file_bytes(ostore.path,&sent,&sent_size));
        CHECK(storage_load(&ostore,dest)==-1);
        CHECK(!ostore.writable);
        CHECK(!storage_save(&ostore,dest));   /* writes disabled, files untouched */
        CHECK(same_chat(dest,before));        /* destination unchanged */
        char *again=NULL; size_t again_size=0;
        CHECK(read_file_bytes(ostore.path,&again,&again_size));
        CHECK(again_size==sent_size && !memcmp(again,sent,sent_size));
        CHECK(storage_load(&ostore,dest)==-1 && !ostore.writable);  /* stays failed closed */
        free(sent); free(again);
        chat_dispose(before); free(before);
        chat_dispose(dest); free(dest);
        storage_close(&ostore);
        remove_store(&ostore,odir);
    }
    /* Decode failures under the poisoned allocator. The destination passed to
       storage_load is the object under test: a successful recovery must give
       it the fallback's full content, and a failed load must leave its
       previous logical content untouched. */
    {
        /* Fixture-backed cases: two conversations exercise both reservation
           calls and the per-message decode path. */
        wchar_t fdir2[256]; swprintf(fdir2,256,L"build\\storage-fail-%lu",GetCurrentProcessId());
        ChatStorage fstore;
        CHECK(storage_open(&fstore,fdir2));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.path,FALSE));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.backup,FALSE));
        Chat *reference=calloc(1,sizeof *reference), *dest=calloc(1,sizeof *dest);
        CHECK(reference && dest);
        seam_reset();
        CHECK(storage_load(&fstore,reference)==1 && !fstore.recovered);
        /* 1. The first conversation's message-array reservation (the first
              realloc of a load) fails. The partially decoded store is
              quarantined and the untouched backup is delivered. */
        seam_reset(); fail_realloc_index=1;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        CHECK(fstore.writable);
        /* 2. The second conversation's reservation fails after the first
              conversation's messages were decoded, so the quarantine must
              dispose live messages and their overflow storage. */
        seam_reset(); fail_realloc_index=2;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        /* 3. A failure while decoding the settings line, before any
              conversation is reached, is recovered the same way. */
        seam_reset(); pass_mallocs=1; fail_mallocs=1;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        /* 4. A failure midway through decoding a message, after earlier
              messages succeeded. The oversized reasoning in the second
              message makes its UTF-16 expansion the first allocation at or
              above the threshold, so the partially constructed message must
              be disposed by the quarantine. */
        {
            Chat *big=calloc(1,sizeof *big);
            CHECK(big);
            chat_init(big); chat_clear(big);
            CHECK(chat_begin_response(big,CHAT_SEND,L"large reasoning")==1);
            wchar_t *wide=(wchar_t *)malloc(40001*sizeof *wide);
            CHECK(wide);
            for (int i=0;i<40000;i++) wide[i]=L'R';
            wide[40000]=0;
            CHECK(chat_message_set_reasoning(&big->conversations[0].messages[1],wide));
            free(wide);
            big->conversations[0].messages[1].generation.state=CHAT_GENERATION_COMPLETE;
            wchar_t bdir[256]; swprintf(bdir,256,L"build\\storage-middecode-%lu",GetCurrentProcessId());
            ChatStorage bstore;
            CHECK(storage_open(&bstore,bdir));
            CHECK(storage_save(&bstore,big));
            CHECK(storage_save(&bstore,big));   /* backup keeps recovery possible */
            seam_reset();
            fail_malloc_size=60000; fail_big_mallocs=1;
            CHECK(storage_load(&bstore,dest)==1 && bstore.recovered);
            seam_reset();
            CHECK(same_chat(dest,big));
            storage_close(&bstore);
            remove_store(&bstore,bdir);
            chat_dispose(big); free(big);
        }
        chat_dispose(reference); free(reference);
        chat_dispose(dest); free(dest);
        storage_close(&fstore);
        remove_store(&fstore,fdir2);
    }

    /* With no valid fallback the load fails: the destination keeps its
       previous logical content, the quarantine is fully disposed (nothing is
       partially adopted), and the store refuses further writes. */
    {
        wchar_t edir[256]; swprintf(edir,256,L"build\\storage-nodecode-%lu",GetCurrentProcessId());
        ChatStorage estore;
        CHECK(storage_open(&estore,edir));
        CHECK(storage_save(&estore,chat));   /* primary only, no backup */
        Chat *before=calloc(1,sizeof *before), *dest=calloc(1,sizeof *dest);
        CHECK(before && dest);
        CHECK(storage_load(&estore,before)==1);   /* destination baseline */
        CHECK(storage_load(&estore,dest)==1);     /* destination under test */
        CHECK(same_chat(dest,before));
        /* The first conversation's reservation fails and there is nothing to
           fall back to. */
        seam_reset(); fail_realloc_index=1;
        CHECK(storage_load(&estore,dest)==-1);
        seam_reset();
        CHECK(same_chat(dest,before));            /* destination preserved */
        CHECK(!estore.writable);
        CHECK(!storage_save(&estore,dest));       /* unreadable files are preserved */
        CHECK(storage_load(&estore,dest)==-1 && !estore.writable);
        CHECK(same_chat(dest,before));
        storage_close(&estore);
        remove_store(&estore,edir);
        chat_dispose(before); free(before);
        chat_dispose(dest); free(dest);
    }

    /* Stage 3: stable persisted message identities. Every present id must be
       an exact integer in [1, stored_next_id] and globally unused; absent ids
       (older snapshots) are synthesized deterministically in file order; any
       other form rejects the snapshot transactionally. */
    {
        char a[512], b[512];
        /* New-format ids roundtrip, including the stored-counter boundary. */
        msg_raw(a,sizeof a,"\"id\":2,"); msg_raw(b,sizeof b,"\"id\":100,");
        CHECK(load_case(L"valid",100,a,b,loaded)==1);
        CHECK(loaded->conversations[0].messages[0].id==2);
        CHECK(loaded->conversations[0].messages[1].id==100);
        CHECK(loaded->next_id==100);
        /* Unknown optional fields remain tolerated: decode queries only the
           names it knows, so extra fields neither break nor shift anything. */
        msg_raw(a,sizeof a,"\"id\":2,"); msg_raw(b,sizeof b,"\"id\":3,");
        a[strlen(a)-1]=0; strcat(a, ",\"future_field\":true}");
        CHECK(load_case(L"unknown",100,a,b,loaded)==1);
        CHECK(loaded->conversations[0].messages[0].id==2);
        CHECK(loaded->conversations[0].messages[1].id==3);
        /* Id-less records migrate: ids are assigned in file order from the
           stored counter and the counter ends at the last synthesized id. */
        msg_raw(a,sizeof a,""); msg_raw(b,sizeof b,"");
        CHECK(load_case(L"migrate",100,a,b,loaded)==1);
        CHECK(loaded->conversations[0].messages[0].id==101);
        CHECK(loaded->conversations[0].messages[1].id==102);
        CHECK(loaded->next_id==102);
        /* Mixed old/new records are valid in both orders. */
        msg_raw(a,sizeof a,"\"id\":5,"); msg_raw(b,sizeof b,"");
        CHECK(load_case(L"mixed-a",100,a,b,loaded)==1);
        CHECK(loaded->conversations[0].messages[0].id==5);
        CHECK(loaded->conversations[0].messages[1].id==101);
        CHECK(loaded->next_id==101);
        msg_raw(a,sizeof a,""); msg_raw(b,sizeof b,"\"id\":5,");
        CHECK(load_case(L"mixed-b",100,a,b,loaded)==1);
        CHECK(loaded->conversations[0].messages[0].id==101);
        CHECK(loaded->conversations[0].messages[1].id==5);
        CHECK(loaded->next_id==101);
        /* Invalid forms: zero, negative, fractional, string, above the
           stored counter, and duplicate within the conversation. */
        msg_raw(a,sizeof a,"\"id\":0,"); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"zero",100,a,b,loaded)==-1);
        msg_raw(a,sizeof a,"\"id\":-1,"); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"negative",100,a,b,loaded)==-1);
        msg_raw(a,sizeof a,"\"id\":2.5,"); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"fractional",100,a,b,loaded)==-1);
        msg_raw(a,sizeof a,"\"id\":\"2\","); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"string",100,a,b,loaded)==-1);
        msg_raw(a,sizeof a,"\"id\":101,"); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"above",100,a,b,loaded)==-1);
        msg_raw(a,sizeof a,"\"id\":2,"); msg_raw(b,sizeof b,"\"id\":2,");
        CHECK(load_case(L"duplicate",100,a,b,loaded)==-1);
        /* A message id colliding with its own conversation id. */
        msg_raw(a,sizeof a,"\"id\":1,"); msg_raw(b,sizeof b,"\"id\":3,");
        CHECK(load_case(L"convhit",100,a,b,loaded)==-1);
        /* A later persisted id equal to the id this pass just synthesized is
           rejected: validation always uses the stored counter, never the
           mutated one. */
        msg_raw(a,sizeof a,""); msg_raw(b,sizeof b,"\"id\":101,");
        CHECK(load_case(L"synthhit",100,a,b,loaded)==-1);
        /* Counter exhaustion while synthesizing is corruption and fails the
           whole snapshot. */
        msg_raw(a,sizeof a,""); msg_raw(b,sizeof b,"\"id\":1,");
        CHECK(load_case(L"exhausted",(long long)CHAT_MAX_ID,a,b,loaded)==-1);
        /* Global uniqueness across conversations, in both directions. */
        {
            wchar_t gdir[256]; swprintf(gdir,256,L"build\\storage-id-global-%lu",GetCurrentProcessId());
            ChatStorage gstore;
            char settings[384], c0[256], c1[256];
            CHECK(storage_open(&gstore,gdir));
            msg_raw(a,sizeof a,"\"id\":7,"); msg_raw(b,sizeof b,"\"id\":7,");
            snprintf(settings,sizeof settings,
                "{\"type\":\"settings\",\"version\":1,\"next_id\":100,\"active\":0,"
                "\"conversation_count\":2,\"model_history_count\":0,\"window_x\":0,"
                "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
                "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
                "\"system_prompt\":\"\"}");
            snprintf(c0,sizeof c0,
                "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
                "\"modified_at\":1000,\"renamed\":0,\"message_count\":1,"
                "\"title\":\"c\",\"draft\":\"\"}");
            snprintf(c1,sizeof c1,
                "{\"type\":\"conversation\",\"id\":2,\"created_at\":1000,"
                "\"modified_at\":1000,\"renamed\":0,\"message_count\":1,"
                "\"title\":\"c\",\"draft\":\"\"}");
            const char *dup_lines[5]={settings,c0,a,c1,b};
            CHECK(write_snapshot(gstore.path,dup_lines,5));
            CHECK(storage_load(&gstore,loaded)==-1);
            storage_close(&gstore); remove_store(&gstore,gdir);
            /* A conversation id equal to an earlier message id. */
            CHECK(storage_open(&gstore,gdir));
            msg_raw(a,sizeof a,"\"id\":7,");
            snprintf(c1,sizeof c1,
                "{\"type\":\"conversation\",\"id\":7,\"created_at\":1000,"
                "\"modified_at\":1000,\"renamed\":0,\"message_count\":1,"
                "\"title\":\"c\",\"draft\":\"\"}");
            const char *conv_lines[5]={settings,c0,a,c1,b};
            CHECK(write_snapshot(gstore.path,conv_lines,5));
            CHECK(storage_load(&gstore,loaded)==-1);
            storage_close(&gstore); remove_store(&gstore,gdir);
        }
        /* A rejected primary falls back to a valid backup. */
        {
            wchar_t rdir[256]; swprintf(rdir,256,L"build\\storage-id-recover-%lu",GetCurrentProcessId());
            ChatStorage rstore;
            char p0[512],p1[512],q0[512],q1[512],settings[384],conversation[256];
            CHECK(storage_open(&rstore,rdir));
            snprintf(settings,sizeof settings,
                "{\"type\":\"settings\",\"version\":1,\"next_id\":100,\"active\":0,"
                "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
                "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
                "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
                "\"system_prompt\":\"\"}");
            snprintf(conversation,sizeof conversation,
                "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
                "\"modified_at\":1000,\"renamed\":0,\"message_count\":2,"
                "\"title\":\"c\",\"draft\":\"\"}");
            msg_raw(p0,sizeof p0,"\"id\":0,"); msg_raw(p1,sizeof p1,"\"id\":3,");
            msg_raw(q0,sizeof q0,"\"id\":2,"); msg_raw(q1,sizeof q1,"\"id\":3,");
            const char *bad[4]={settings,conversation,p0,p1};
            const char *good[4]={settings,conversation,q0,q1};
            CHECK(write_snapshot(rstore.path,bad,4));
            CHECK(write_snapshot(rstore.backup,good,4));
            CHECK(storage_load(&rstore,loaded)==1 && rstore.recovered);
            CHECK(loaded->conversations[0].messages[0].id==2);
            CHECK(loaded->conversations[0].messages[1].id==3);
            storage_close(&rstore); remove_store(&rstore,rdir);
            /* Both snapshots invalid: no recovery, writes disabled. */
            CHECK(storage_open(&rstore,rdir));
            CHECK(write_snapshot(rstore.path,bad,4));
            CHECK(write_snapshot(rstore.backup,bad,4));
            CHECK(storage_load(&rstore,loaded)==-1);
            CHECK(!rstore.writable);
            CHECK(!storage_save(&rstore,loaded));
            storage_close(&rstore); remove_store(&rstore,rdir);
        }
        /* Downgrade simulation: strip message ids from a new-format snapshot,
           recompute the checksum, then reload and remigrate. The identity
           values are gone, so new ids are assigned above the stored counter
           and the counter is bumped again; the result is a valid, stable
           snapshot once more. */
        {
            wchar_t ddir[256]; swprintf(ddir,256,L"build\\storage-id-downgrade-%lu",GetCurrentProcessId());
            ChatStorage dstore;
            CHECK(storage_open(&dstore,ddir));
            CHECK(storage_save(&dstore,chat));
            uint64_t saved_next=chat->next_id;
            size_t message_total=0;
            for (size_t i=0;i<(size_t)chat->conversation_count;i++)
                message_total+=chat->conversations[i].message_count;
            CHECK(downgrade_strip_ids(dstore.path));
            CHECK(storage_load(&dstore,loaded)==1 && !dstore.recovered);
            CHECK(loaded->next_id==saved_next+message_total);
            for (int i=0,k=0;i<loaded->conversation_count;i++)
                for (size_t j=0;j<loaded->conversations[i].message_count;j++,k++)
                    CHECK(loaded->conversations[i].messages[j].id==saved_next+1+(uint64_t)k);
            /* The remigrated state saves and reloads idempotently. */
            CHECK(storage_save(&dstore,loaded));
            Chat *again=calloc(1,sizeof *again);
            CHECK(again);
            CHECK(storage_load(&dstore,again)==1 && !dstore.recovered);
            CHECK(same_chat(again,loaded));
            char *first=NULL,*second=NULL; size_t first_size=0,second_size=0;
            CHECK(storage_save(&dstore,again));
            CHECK(read_file_bytes(dstore.path,&first,&first_size));
            CHECK(storage_save(&dstore,again));
            CHECK(read_file_bytes(dstore.path,&second,&second_size));
            CHECK(first_size==second_size && !memcmp(first,second,first_size));
            free(first); free(second);
            chat_dispose(again); free(again);
            storage_close(&dstore); remove_store(&dstore,ddir);
        }
    }

    /* Stage 6: format 2 raised the conversation bound to 128 and format 3
        raised the message bound to 512. A hand-built version 1 snapshot
        declaring 128 conversations loads and round-trips as format 3; a
        declared 129th is ordinary corruption (rejected, no fallback, writes
        disabled, files preserved). */
    {
        wchar_t cdir[256]; swprintf(cdir,256,L"build\\storage-cap-%lu",GetCurrentProcessId());
        ChatStorage cstore;
        CHECK(storage_open(&cstore,cdir));
        for (int pass = 0; pass < 2; pass++) {
            int declared = pass == 0 ? CHAT_MAX_CONVERSATIONS
                                     : CHAT_MAX_CONVERSATIONS + 1;
            char settings[320];
            static char conv[CHAT_MAX_CONVERSATIONS + 1][160];
            const char *lines[CHAT_MAX_CONVERSATIONS + 2];
            int total = 0;
            snprintf(settings, sizeof settings,
                "{\"type\":\"settings\",\"version\":1,\"next_id\":%lld,\"active\":0,"
                "\"conversation_count\":%d,\"model_history_count\":0,\"window_x\":0,"
                "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
                "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
                "\"system_prompt\":\"\"}", (long long)(declared + 71), declared);
            lines[total++] = settings;
            for (int i = 0; i < declared; i++) {
                snprintf(conv[i], sizeof conv[i],
                    "{\"type\":\"conversation\",\"id\":%d,\"created_at\":1000,"
                    "\"modified_at\":1000,\"renamed\":0,\"message_count\":0,"
                    "\"title\":\"c\",\"draft\":\"\"}", i + 1);
                lines[total++] = conv[i];
            }
            CHECK(write_snapshot(cstore.path, lines, (size_t)total));
            if (pass == 1) {
                /* Remove the fallback copies so the declared 129th is tested
                   with nothing valid to recover. */
                DeleteFileW(cstore.backup);
                DeleteFileW(cstore.temporary);
            }
            if (pass == 0) {
                CHECK(storage_load(&cstore, loaded) == 1 && !cstore.recovered);
                CHECK(loaded->conversation_count == CHAT_MAX_CONVERSATIONS);
                for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++)
                    CHECK(loaded->conversations[i].id == (uint64_t)(i + 1));
                CHECK(storage_save(&cstore, loaded));
                CHECK(storage_load(&cstore, loaded) == 1 && !cstore.recovered);
                CHECK(loaded->conversation_count == CHAT_MAX_CONVERSATIONS);
            } else {
                CHECK(storage_load(&cstore, loaded) == -1);
                CHECK(!cstore.writable);
            }
        }
        storage_close(&cstore);
        remove_store(&cstore, cdir);
    }

    chat_dispose(chat); chat_dispose(loaded);
    free(chat); free(loaded);
    puts("Storage roundtrip, locking, corruption, backup and interrupted-write recovery passed");
    return 0;
}
