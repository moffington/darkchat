#include "chat/persistence/storage.h"
#include "chat/persistence/attachments.h"
#include "chat/json.h"
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
   - fail_realloc_index: fail the nth realloc call (a load reallocs for the
     conversation message-array reservations and the attachment-table
     growth).
   The seam also tracks live wrapped allocations (malloc, calloc and fresh
   realloc minus free),
   so a decode-failure test can assert that the quarantine released exactly
   everything it built — a missed dispose shows up as a leaked allocation. */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__real_calloc(size_t count, size_t size);
void __real_free(void *pointer);
static long pass_mallocs, fail_mallocs;
static size_t fail_malloc_size;
static long fail_big_mallocs;
static long fail_realloc_index, realloc_seen;
static long live_allocs;
void *__wrap_malloc(size_t size) {
    if (pass_mallocs > 0) { --pass_mallocs; goto pass; }
    if (fail_mallocs > 0) { --fail_mallocs; return NULL; }
    if (fail_big_mallocs > 0 && fail_malloc_size && size >= fail_malloc_size) {
        --fail_big_mallocs; return NULL;
    }
pass: {
    void *result = __real_malloc(size);
    if (result) ++live_allocs;
    return result;
}
}
void *__wrap_realloc(void *pointer, size_t size) {
    ++realloc_seen;
    if (fail_realloc_index > 0 && realloc_seen == fail_realloc_index) return NULL;
    void *grown = __real_realloc(pointer, size);
    if (grown && !pointer) {
        memset(grown, 0x5C, size);
        /* A fresh realloc is a fresh allocation (exactly malloc): count it
           so the live balance also holds across message-array and
           attachment-table growth. A resize of a counted block keeps its
           count. */
        ++live_allocs;
    }
    return grown;
}
void *__wrap_calloc(size_t count, size_t size) {
    void *result = __real_calloc(count, size);
    if (result) ++live_allocs;
    return result;
}
void __wrap_free(void *pointer) {
    if (pointer) --live_allocs;
    __real_free(pointer);
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
/* Compares two messages' ordered content parts field-for-field through the
   logical view: kind, flags and the payload (TEXT compares its string,
   IMAGE every ChatImagePart field). */
static bool same_parts(const ChatMessage *a, const ChatMessage *b) {
    size_t count = chat_message_part_count(a);
    if (count != chat_message_part_count(b)) return false;
    for (size_t i = 0; i < count; i++) {
        ChatPartView x, y;
        if (!chat_message_part_at(a, i, &x) ||
            !chat_message_part_at(b, i, &y)) return false;
        if (x.kind != y.kind || x.flags != y.flags) return false;
        if (x.kind == CHAT_PART_TEXT) {
            if (x.u.text.length != y.u.text.length) return false;
            if (wcsncmp(x.u.text.data, y.u.text.data, x.u.text.length))
                return false;
        } else {
            const ChatImagePart *p = &x.u.image, *q = &y.u.image;
            if (p->attachment_id != q->attachment_id ||
                p->pixel_width != q->pixel_width ||
                p->pixel_height != q->pixel_height ||
                strcmp(p->mime, q->mime) ||
                wcscmp(p->display_name, q->display_name)) return false;
        }
    }
    return true;
}
static bool same_message(const ChatMessage *a, const ChatMessage *b) {
    return a->role == b->role && a->id == b->id &&
        a->created_at == b->created_at &&
        a->modified_at == b->modified_at &&
        !wcscmp(chat_message_text(a), chat_message_text(b)) &&
        !wcscmp(chat_message_reasoning(a), chat_message_reasoning(b)) &&
        same_parts(a, b) &&
        same_generation(&a->generation, &b->generation);
}
/* Compares every logically persisted field, including the stable message ids.
    View bookkeeping (revisions, expansion) and runtime-only state (status,
    reply counter) are deliberately excluded: they are not part of a snapshot.
    Format 4 adds the profile library and the per-conversation customization;
    both are part of the persisted state, so both are compared here. */
static bool same_text(const ChatText *a, const ChatText *b) {
    return (a->data == NULL) == (b->data == NULL) &&
        (a->data == NULL || !wcscmp(a->data, b->data));
}
static bool same_profile(const ChatPromptProfile *a,
    const ChatPromptProfile *b) {
    return !wcscmp(a->name, b->name) && same_text(&a->prompt, &b->prompt);
}
static bool same_chat(const Chat *a, const Chat *b) {
    if (a->next_id != b->next_id || a->active != b->active ||
        a->conversation_count != b->conversation_count ||
        a->model_history_count != b->model_history_count ||
        a->window_x != b->window_x || a->window_y != b->window_y ||
        a->window_width != b->window_width ||
        a->window_height != b->window_height ||
        a->maximized != b->maximized || a->sidebar_width != b->sidebar_width ||
        a->sidebar_collapsed != b->sidebar_collapsed ||
        a->notify_disabled != b->notify_disabled ||
        wcscmp(a->model, b->model) ||
        a->backend != b->backend ||
        wcscmp(a->ollama_model, b->ollama_model) ||
        wcscmp(a->system_prompt, b->system_prompt) ||
        a->provider_routing.sort != b->provider_routing.sort ||
        a->provider_routing.disallow_fallbacks != b->provider_routing.disallow_fallbacks ||
        a->provider_routing.data_collection != b->provider_routing.data_collection ||
        a->provider_routing.zdr != b->provider_routing.zdr ||
        a->profile_count != b->profile_count) return false;
    for (int i = 0; i < a->profile_count; i++)
        if (!same_profile(&a->profiles[i], &b->profiles[i])) return false;
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
            wcscmp(ca->title, cb->title) || wcscmp(ca->draft, cb->draft) ||
            wcscmp(ca->model, cb->model) ||
            wcscmp(ca->ollama_model, cb->ollama_model) ||
            ca->system_prompt_present != cb->system_prompt_present ||
            !same_text(&ca->system_prompt, &cb->system_prompt))
            return false;
        if (ca->message_count && (!ca->messages || !cb->messages)) return false;
        for (size_t j = 0; j < ca->message_count; j++)
            if (!same_message(&ca->messages[j], &cb->messages[j])) return false;
    }
    /* The attachment metadata table is persisted state from format 6 and
       compared record-by-record, in order. */
    if (a->attachment_count != b->attachment_count) return false;
    if (a->attachment_count && (!a->attachments || !b->attachments))
        return false;
    for (size_t i = 0; i < a->attachment_count; i++) {
        const ChatAttachmentMeta *x = &a->attachments[i];
        const ChatAttachmentMeta *y = &b->attachments[i];
        if (x->id != y->id || x->bytes != y->bytes ||
            x->pixel_width != y->pixel_width ||
            x->pixel_height != y->pixel_height ||
            x->created_at != y->created_at ||
            strcmp(x->digest, y->digest) || strcmp(x->mime, y->mime) ||
            wcscmp(x->display_name, y->display_name))
            return false;
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
/* Builds a format-4-shaped snapshot: `settings_extra` is spliced into the
    settings record before its closing brace, each `profiles` entry is a
    complete profile record line emitted between the (empty) model history
    and the conversation record, and `conversation_extra` is spliced into
    the conversation record before its closing brace. Two messages with ids
    2 and 3 follow the conversation. Loads and removes the store; returns
    storage_load's result, or -2 when building failed. */
static int load_v4_case(const wchar_t *tag, int version,
    const char *settings_extra, const char *const *profiles,
    size_t profile_count, const char *conversation_extra, Chat *dest) {
    wchar_t dir[256];
    swprintf(dir, 256, L"build\\storage-v4-%ls-%lu", tag,
        GetCurrentProcessId());
    ChatStorage store;
    if (!storage_open(&store, dir)) return -2;
    if (!settings_extra) settings_extra = "";
    if (!conversation_extra) conversation_extra = "";
    char settings[768], conversation[512], a[512], b[512];
    snprintf(settings, sizeof settings,
        "{\"type\":\"settings\",\"version\":%d,\"next_id\":100,\"active\":0,"
        "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
        "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
        "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
        "\"system_prompt\":\"\"%s%s}", version,
        settings_extra[0] ? "," : "", settings_extra);
    snprintf(conversation, sizeof conversation,
        "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
        "\"modified_at\":1000,\"renamed\":0,\"message_count\":2,"
        "\"title\":\"c\",\"draft\":\"\"%s%s}",
        conversation_extra[0] ? "," : "", conversation_extra);
    msg_raw(a, sizeof a, "\"id\":2,"); msg_raw(b, sizeof b, "\"id\":3,");
    const char **lines = malloc((4 + profile_count) * sizeof *lines);
    if (!lines) { storage_close(&store); remove_store(&store, dir); return -2; }
    lines[0] = settings;
    for (size_t i = 0; i < profile_count; i++) lines[1 + i] = profiles[i];
    lines[1 + profile_count] = conversation;
    lines[2 + profile_count] = a;
    lines[3 + profile_count] = b;
    int result = write_snapshot(store.path, lines, 4 + profile_count) ?
        storage_load(&store, dest) : -2;
    free(lines);
    storage_close(&store);
    remove_store(&store, dir);
    return result;
}
/* One version-6 message record. `id_field` is `""` or something like
   `"\"id\":2,"`; `text` is the JSON-encoded `text` field; `parts_field` is
   `""` or a trailing `,"parts":...` spliced before the closing brace. */
static void msg6(char *out, size_t cap, const char *id_field,
    const char *text, const char *parts_field) {
    snprintf(out, cap,
        "{\"type\":\"message\",%s\"role\":0,\"created_at\":1000,"
        "\"modified_at\":1000,\"text\":%s,\"state\":0,\"started_at\":0,"
        "\"finished_at\":0,\"first_token_at\":0,\"ttft_ms\":-1,\"latency_ms\":-1,"
        "\"prompt_tokens\":-1,\"completion_tokens\":-1,\"total_tokens\":-1,"
        "\"cost\":-1,\"requested_model\":\"\",\"actual_model\":\"\","
        "\"finish_reason\":\"\",\"error\":\"\"%s}", id_field, text, parts_field);
}
/* Builds a one-conversation snapshot whose version is `version`, whose
   `attachment_lines` (may be none) sit between the settings record and the
   conversation record, and whose `message_lines` (may be none) follow it.
   Every line is a complete record. Returns storage_load's result (or -2
   when building failed). */
static int load_v6_case(const wchar_t *tag, int version, long long next_id,
    const char *const *attachment_lines, size_t attachment_count,
    const char *const *message_lines, size_t message_count, Chat *dest) {
    wchar_t dir[256];
    swprintf(dir, 256, L"build\\storage-v6-%ls-%lu", tag,
        GetCurrentProcessId());
    ChatStorage store;
    if (!storage_open(&store, dir)) return -2;
    char settings[384], conversation[256];
    snprintf(settings, sizeof settings,
        "{\"type\":\"settings\",\"version\":%d,\"next_id\":%lld,\"active\":0,"
        "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
        "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
        "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
        "\"system_prompt\":\"\",\"profile_count\":0}", version, next_id);
    snprintf(conversation, sizeof conversation,
        "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
        "\"modified_at\":1000,\"renamed\":0,\"message_count\":%zu,"
        "\"title\":\"c\",\"draft\":\"\"}", message_count);
    size_t total = 2 + attachment_count + message_count;
    const char **lines = malloc(total * sizeof *lines);
    if (!lines) {
        storage_close(&store); remove_store(&store, dir); return -2;
    }
    size_t at = 0;
    lines[at++] = settings;
    for (size_t i = 0; i < attachment_count; i++)
        lines[at++] = attachment_lines[i];
    lines[at++] = conversation;
    for (size_t i = 0; i < message_count; i++)
        lines[at++] = message_lines[i];
    int result = write_snapshot(store.path, lines, total) ?
        storage_load(&store, dest) : -2;
    free(lines);
    storage_close(&store);
    remove_store(&store, dir);
    return result;
}

/* Two fixed 64-hex digests for the format-6 cases: load never hashes blob
   content, so any stable hex names work. */
static const char k_digest_a[65] =
    "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
static const char k_digest_b[65] =
    "1111111111111111111111111111111111111111111111111111111111111111";

/* Collects scanned digests for the live-set scan tests. */
struct DigestSet { char dig[8][65]; size_t count; };
static bool collect_digest(void *user, const char digest[65]) {
    struct DigestSet *set = (struct DigestSet *)user;
    if (set->count >= 8) return false;
    memcpy(set->dig[set->count++], digest, 65);
    return true;
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
/* Downgrade surgery for format 4: drops every profile record line, cuts the
    customization fields off each conversation record (they are the record
    tail), rewrites the settings version to 3, recomputes the checksum and
    rewrites the file — exactly the snapshot a v3-only build would have
    produced had the customization never existed. */
static bool strip_customization(const wchar_t *path) {
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
    char *body_end = NULL;
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
        bool drop = false;
        if (!commit && strstr(line, "\"type\":\"profile\"")) {
            drop = true;
        } else if (!commit && strstr(line, "\"type\":\"conversation\"")) {
            static const char *const overrides[] = {
                "\"system_prompt\":", "\"model\":", "\"ollama_model\":"};
            /* The customization fields are the record tail: cutting at the
               earliest one removes every field after it. */
            char *cut = NULL;
            for (int i = 0; i < 3; i++) {
                char *field = strstr(line, overrides[i]);
                if (field && (!cut || field < cut)) cut = field;
            }
            if (cut) { cut[-1] = '}'; cut[0] = 0; }
        } else if (!commit) {
            char *version = strstr(line, "\"version\":4");
            if (version) version[strlen("\"version\":")] = '3';
            /* The settings record must lose its profile_count too: present
               at v3 it is corruption. It is the record tail. */
            char *count = strstr(line, "\"profile_count\":");
            if (count) { count[-1] = '}'; count[0] = 0; }
        }
        cursor = end + 1;
        if (drop) { free(line); continue; }
        size_t written = strlen(line);
        memcpy(out + used, line, written);
        used += written;
        out[used++] = '\n';
        free(line);
        if (commit) { body_end = out + used - (written + 1); break; }
    }
    if (!body_end) { free(data); free(out); return false; }
    char commit_record[96];
    snprintf(commit_record, sizeof commit_record,
        "{\"type\":\"commit\",\"checksum\":%u}",
        fnv1a(out, (size_t)(body_end - out)));
    f = _wfopen(path, L"wb");
    ok = f && fwrite(out, 1, (size_t)(body_end - out), f) ==
        (size_t)(body_end - out) &&
        fwrite(commit_record, 1, strlen(commit_record), f) ==
            strlen(commit_record) &&
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
    /* The completion-notification preference is additive at format 3: emitted
       only when disabled, absent means enabled, and a present malformed value
       rejects the snapshot. */
    {
        char *notify=NULL; size_t notify_size=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&notify,&notify_size));
        CHECK(strstr(notify,"\"notify_disabled\"")==NULL);
        free(notify);
        chat->notify_disabled=1;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&notify,&notify_size));
        CHECK(strstr(notify,"\"notify_disabled\":1")!=NULL);
        free(notify);
        CHECK(storage_load(&store,loaded)==1);
        CHECK(loaded->notify_disabled==1);
        chat->notify_disabled=0;
        /* An absent field defaults to enabled; malformed values reject. */
        Chat *dest=calloc(1,sizeof *dest); CHECK(dest);
        CHECK(load_backend_case("\"notify_disabled\":1",dest)==1);
        CHECK(dest->notify_disabled==1);
        CHECK(load_backend_case(NULL,dest)==1);
        CHECK(dest->notify_disabled==0);
        static const char *const malformed_notify[]={
            "\"notify_disabled\":2","\"notify_disabled\":-1",
            "\"notify_disabled\":0.5","\"notify_disabled\":true",
            "\"notify_disabled\":\"1\"","\"notify_disabled\":null"
        };
        for (size_t i=0;i<sizeof malformed_notify/sizeof malformed_notify[0];i++) {
            CHECK(load_backend_case(malformed_notify[i],dest)==-1);
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
    CHECK(strstr(first_bytes,"\"version\":3")); /* uncustomized state stays format 3 */
    free(first_bytes);
    FILE *future=_wfopen(store.path,L"r+b"); CHECK(future);
    char header[64]={0}; CHECK(fread(header,1,63,future)==63);
    char *version=strstr(header,"\"version\":3"); CHECK(version);
    CHECK(fseek(future,(long)(version-header)+(long)strlen("\"version\":"),SEEK_SET)==0);
    /* This build writes at most format 6, so the unsupported-boundary
        fixture must claim a version beyond what this build decodes. */
    fputc('7',future); fclose(future);
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
    /* Format 4 boundary, encoder side: an entirely uncustomized snapshot
        remains byte-for-byte format 3 — no profile_count field, no profile
        records and no per-conversation override fields. */
    {
        CHECK(storage_open(&store,dir));   /* the earlier store was closed */
        CHECK(storage_save(&store,chat));
        char *plain=NULL; size_t plain_size=0;
        CHECK(read_file_bytes(store.path,&plain,&plain_size));
        CHECK(strstr(plain,"\"version\":3")!=NULL);
        CHECK(strstr(plain,"\"profile_count\"")==NULL);
        CHECK(strstr(plain,"\"type\":\"profile\"")==NULL);
        free(plain);
        CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    }
    /* Per-conversation customization forces format 4: the fields are not
        v3-additive, because an older binary would ignore them and silently
        erase them on its next save. Clearing every override returns the
        file to the v3 byte shape. */
    {
        CHECK(chat_conversation_set_system_prompt(chat,0,L"Concise replies only."));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OPENROUTER,L"override/model"));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OLLAMA,L"override:local"));
        CHECK(storage_save(&store,chat));
        char *saved=NULL; size_t saved_size=0;
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":4")!=NULL);
        CHECK(strstr(saved,"\"profile_count\":0")!=NULL);
        CHECK(strstr(saved,"\"type\":\"profile\"")==NULL);
        CHECK(strstr(saved,"\"system_prompt\":\"Concise replies only.\"")!=NULL);
        CHECK(strstr(saved,"\"model\":\"override/model\"")!=NULL);
        CHECK(strstr(saved,"\"ollama_model\":\"override:local\"")!=NULL);
        free(saved);
        CHECK(storage_load(&store,loaded)==1 && !store.recovered);
        CHECK(same_chat(loaded,chat));
        CHECK(chat_conversation_set_system_prompt(chat,0,L""));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OPENROUTER,L""));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OLLAMA,L""));
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":3")!=NULL);
        CHECK(strstr(saved,"\"Concise replies only.\"")==NULL);
        free(saved);
        CHECK(storage_load(&store,loaded)==1);
    }
    /* The deliberately-empty prompt override forces format 5 (a v4 reader
        would drop the flag on its next save, so it is version-gated in both
        directions), round-trips exactly (empty override, not inherit),
        keeps repeated saves byte-stable, and clears back to the v3 shape. */
    {
        CHECK(chat_conversation_apply_system_prompt(chat,0,L""));
        CHECK(chat->conversations[0].system_prompt_present &&
            !chat->conversations[0].system_prompt.data);
        CHECK(storage_save(&store,chat));
        char *saved=NULL; size_t saved_size=0;
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":5")!=NULL);
        CHECK(strstr(saved,"\"system_prompt_present\":1")!=NULL);
        free(saved);
        CHECK(storage_load(&store,loaded)==1 && !store.recovered);
        CHECK(same_chat(loaded,chat));
        CHECK(loaded->conversations[0].system_prompt_present &&
            !loaded->conversations[0].system_prompt.data);
        char *first=NULL,*second=NULL; size_t fs=0,ss=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&first,&fs));
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&second,&ss));
        CHECK(fs==ss && !memcmp(first,second,fs));
        free(first); free(second);
        CHECK(chat_conversation_set_system_prompt(chat,0,L""));
        CHECK(!chat->conversations[0].system_prompt_present);
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":3")!=NULL);
        CHECK(strstr(saved,"\"system_prompt_present\"")==NULL);
        free(saved);
        CHECK(storage_load(&store,loaded)==1);
    }
    /* Prompt profiles force format 4: profile records sit between the model
        history and the first conversation, the round trip is logically
        exact, repeated saves are byte-stable, and removal of the last
        profile returns the file to format 3. */
    {
        CHECK(chat_profile_add(chat,L"Terse",L"You are terse.")==0);
        CHECK(chat_profile_add(chat,L"Plain",L"")==1);
        CHECK(chat_profile_add(chat,L"Unicode \x03bb",L"prompt \xd83d\xde80")==2);
        CHECK(storage_save(&store,chat));
        char *saved=NULL; size_t saved_size=0;
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":4")!=NULL);
        CHECK(strstr(saved,"\"profile_count\":3")!=NULL);
        CHECK(strstr(saved,"\"type\":\"profile\"")!=NULL);
        free(saved);
        CHECK(storage_load(&store,loaded)==1 && !store.recovered);
        CHECK(same_chat(loaded,chat));
        CHECK(loaded->profile_count==3);
        CHECK(!wcscmp(loaded->profiles[1].name,L"Plain") &&
            loaded->profiles[1].prompt.data==NULL);
        char *first=NULL,*second=NULL; size_t fs=0,ss=0;
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&first,&fs));
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&second,&ss));
        CHECK(fs==ss && !memcmp(first,second,fs));
        free(first); free(second);
        /* Removing a profile renumbers the survivors and stays at v4. */
        CHECK(chat_profile_remove(chat,0));
        CHECK(storage_save(&store,chat));
        CHECK(storage_load(&store,loaded)==1 && !store.recovered);
        CHECK(same_chat(loaded,chat) && loaded->profile_count==2 &&
            !wcscmp(loaded->profiles[0].name,L"Plain"));
        CHECK(chat_profile_remove(chat,0));
        CHECK(chat_profile_remove(chat,0));
        CHECK(chat->profile_count==0);
        CHECK(storage_save(&store,chat));
        CHECK(read_file_bytes(store.path,&saved,&saved_size));
        CHECK(strstr(saved,"\"version\":3")!=NULL);
        free(saved);
        storage_close(&store);
        remove_store(&store,dir);
    }
    /* Format 4 grammar is version-gated and strictly validated: every
        malformed shape below rejects the snapshot. Overrides are legal only
        at v4 in both directions, and the profile count must match the
        profile records exactly, in order. */
    {
        Chat *dest=calloc(1,sizeof *dest); CHECK(dest);
        static const char *one_profile[1] = {
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"p\"}"};
        static const char *two_profiles[2] = {
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"\"}",
            "{\"type\":\"profile\",\"name\":\"B\",\"prompt\":\"p\"}"};
        /* profile_count present at v3 is corruption. */
        CHECK(load_v4_case(L"v3count",3,"\"profile_count\":1",NULL,0,NULL,dest)==-1);
        /* A profile record at v3 is corruption. */
        CHECK(load_v4_case(L"v3profile",3,NULL,one_profile,1,NULL,dest)==-1);
        /* Overrides present at v3 are corruption, each field. */
        CHECK(load_v4_case(L"v3sp",3,NULL,NULL,0,"\"system_prompt\":\"x\"",dest)==-1);
        CHECK(load_v4_case(L"v3spflag",3,NULL,NULL,0,
            "\"system_prompt_present\":1",dest)==-1);
        CHECK(load_v4_case(L"v3model",3,NULL,NULL,0,"\"model\":\"x\"",dest)==-1);
        CHECK(load_v4_case(L"v3oll",3,NULL,NULL,0,"\"ollama_model\":\"x\"",dest)==-1);
        /* The deliberately-empty override is v5-only: at v4 the field is
            corruption (a v4 reader would drop it), at v5 it decodes as a
            present-empty override, and an out-of-range value is corruption. */
        CHECK(load_v4_case(L"spflagv4",4,"\"profile_count\":0",NULL,0,
            "\"system_prompt_present\":1",dest)==-1);
        CHECK(load_v4_case(L"spflag",5,"\"profile_count\":0",NULL,0,
            "\"system_prompt_present\":1",dest)==1);
        CHECK(dest->conversations[0].system_prompt_present &&
            !dest->conversations[0].system_prompt.data);
        CHECK(load_v4_case(L"spflagbad",5,"\"profile_count\":0",NULL,0,
            "\"system_prompt_present\":2",dest)==-1);
        /* The flag and a system_prompt value are mutually exclusive: a
            record carrying both is corruption (the encoder could never
            have written it). */
        CHECK(load_v4_case(L"spboth",5,"\"profile_count\":0",NULL,0,
            "\"system_prompt\":\"x\",\"system_prompt_present\":1",dest)==-1);
        /* The count and the records must match exactly. */
        CHECK(load_v4_case(L"count2one",4,"\"profile_count\":2",one_profile,1,NULL,dest)==-1);
        CHECK(load_v4_case(L"count0one",4,"\"profile_count\":0",one_profile,1,NULL,dest)==-1);
        CHECK(load_v4_case(L"count1none",4,"\"profile_count\":1",NULL,0,NULL,dest)==-1);
        CHECK(load_v4_case(L"count1two",4,"\"profile_count\":1",two_profiles,2,NULL,dest)==-1);
        /* Out-of-range and malformed counts. */
        CHECK(load_v4_case(L"count25",4,"\"profile_count\":25",two_profiles,2,NULL,dest)==-1);
        CHECK(load_v4_case(L"countneg",4,"\"profile_count\":-1",NULL,0,NULL,dest)==-1);
        CHECK(load_v4_case(L"countfrac",4,"\"profile_count\":0.5",NULL,0,NULL,dest)==-1);
        CHECK(load_v4_case(L"countstr",4,"\"profile_count\":\"1\"",NULL,0,NULL,dest)==-1);
        /* Profile names are unique up to ordinal case-insensitive
           comparison, the invariant the chat_profile_* API enforces: a
           stored duplicate, exact or case-variant, is corruption because no
           snapshot the API accepts could have written one. */
        static const char *dup_exact[2] = {
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"p\"}",
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"q\"}"};
        static const char *dup_case[2] = {
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"p\"}",
            "{\"type\":\"profile\",\"name\":\"a\",\"prompt\":\"q\"}"};
        CHECK(load_v4_case(L"dupexact",4,"\"profile_count\":2",dup_exact,2,NULL,dest)==-1);
        CHECK(load_v4_case(L"dupcase",4,"\"profile_count\":2",dup_case,2,NULL,dest)==-1);
        /* A malformed later profile must not leak the earlier profiles'
           already-decoded prompts: every profile becomes live (profile_count
           tracks it) before any fallible decoding, so the quarantine
           disposes exactly what it built. The allocation-balance seam
           proves it: the failing load returns the live wrapped-allocation
           count to its starting value and performs no realloc at all
           (profile records decode before any message-array reservation). */
        {
            static const char *leak[3] = {
                "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"kept one\"}",
                "{\"type\":\"profile\",\"name\":\"B\",\"prompt\":\"kept two\"}",
                "{\"type\":\"profile\",\"name\":\"\",\"prompt\":\"\"}"};
            long before = live_allocs, seen = realloc_seen;
            CHECK(load_v4_case(L"leak",4,"\"profile_count\":3",leak,3,NULL,dest)==-1);
            CHECK(live_allocs == before);
            CHECK(realloc_seen == seen);
        }
        /* A profile record after the first conversation record is the wrong
           order: the trailing line is neither a conversation nor the commit. */
        {
            wchar_t odir[256]; swprintf(odir,256,L"build\\storage-v4-order-%lu",GetCurrentProcessId());
            ChatStorage ostore;
            CHECK(storage_open(&ostore,odir));
            char a[512], b[512];
            msg_raw(a,sizeof a,"\"id\":2,"); msg_raw(b,sizeof b,"\"id\":3,");
            const char *lines[5] = {
                "{\"type\":\"settings\",\"version\":4,\"next_id\":100,\"active\":0,"
                "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
                "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
                "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
                "\"system_prompt\":\"\",\"profile_count\":1}",
                "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
                "\"modified_at\":1000,\"renamed\":0,\"message_count\":2,"
                "\"title\":\"c\",\"draft\":\"\"}",
                a, b,
                "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"\"}"};
            CHECK(write_snapshot(ostore.path,lines,5));
            CHECK(storage_load(&ostore,dest)==-1);
            storage_close(&ostore); remove_store(&ostore,odir);
        }
        /* Malformed names and prompts. */
        static const char *empty_name[1] = {
            "{\"type\":\"profile\",\"name\":\"\",\"prompt\":\"p\"}"};
        CHECK(load_v4_case(L"emptyname",4,"\"profile_count\":1",empty_name,1,NULL,dest)==-1);
        {
            static char name100[128], longname_line[256];
            for (int i=0;i<100;i++) name100[i]='n';
            name100[100]=0;
            snprintf(longname_line,sizeof longname_line,
                "{\"type\":\"profile\",\"name\":\"%s\",\"prompt\":\"p\"}",name100);
            const char *over[1]={longname_line};
            CHECK(load_v4_case(L"longname",4,"\"profile_count\":1",over,1,NULL,dest)==-1);
        }
        static const char *numname[1] = {
            "{\"type\":\"profile\",\"name\":7,\"prompt\":\"\"}"};
        CHECK(load_v4_case(L"numname",4,"\"profile_count\":1",numname,1,NULL,dest)==-1);
        static const char *numprompt[1] = {
            "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":7}"};
        CHECK(load_v4_case(L"numprompt",4,"\"profile_count\":1",numprompt,1,NULL,dest)==-1);
        {
            static char big[70000];
            size_t used = (size_t)snprintf(big,64,
                "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"");
            for (int i=0;i<CHAT_COMPOSER_TEXT;i++) big[used++]='p';
            snprintf(big+used,sizeof big-used,"\"}");
            const char *overp[1]={big};
            CHECK(load_v4_case(L"bigprompt",4,"\"profile_count\":1",overp,1,NULL,dest)==-1);
            used = (size_t)snprintf(big,64,
                "{\"type\":\"profile\",\"name\":\"A\",\"prompt\":\"");
            for (int i=0;i<CHAT_COMPOSER_TEXT-1;i++) big[used++]='p';
            snprintf(big+used,sizeof big-used,"\"}");
            CHECK(load_v4_case(L"maxprompt",4,"\"profile_count\":1",overp,1,NULL,dest)==1);
            CHECK(wcslen(chat_text_value(&dest->profiles[0].prompt))==CHAT_COMPOSER_TEXT-1);
        }
        /* Valid v4 customization decodes, including empty → unset. */
        CHECK(load_v4_case(L"oksp",4,"\"profile_count\":1",one_profile,1,
            "\"system_prompt\":\"Concise\",\"model\":\"ov/m\",\"ollama_model\":\"ov:local\"",dest)==1);
        CHECK(!wcscmp(chat_text_value(&dest->conversations[0].system_prompt),L"Concise"));
        CHECK(!wcscmp(dest->conversations[0].model,L"ov/m"));
        CHECK(!wcscmp(dest->conversations[0].ollama_model,L"ov:local"));
        CHECK(!wcscmp(dest->profiles[0].name,L"A") &&
            !wcscmp(chat_text_value(&dest->profiles[0].prompt),L"p"));
        CHECK(load_v4_case(L"empties",4,"\"profile_count\":0",NULL,0,
            "\"system_prompt\":\"\",\"model\":\"\",\"ollama_model\":\"\"",dest)==1);
        CHECK(dest->conversations[0].system_prompt.data==NULL &&
            !dest->conversations[0].model[0] &&
            !dest->conversations[0].ollama_model[0]);
        /* An absent profile prompt decodes as unset. */
        static const char *no_prompt[1] = {
            "{\"type\":\"profile\",\"name\":\"A\"}"};
        CHECK(load_v4_case(L"noprompt",4,"\"profile_count\":1",no_prompt,1,NULL,dest)==1);
        CHECK(dest->profiles[0].prompt.data==NULL);
        chat_dispose(dest); free(dest);
    }
    /* Downgrade surgery: a format 4 file whose customization is removed
        (profile records dropped, override fields cut, version rewritten to
        3, checksum recomputed) is exactly what a v3-only build would have
        produced, so it loads and re-saves as format 3. */
    {
        wchar_t ddir[256]; swprintf(ddir,256,L"build\\storage-v4-down-%lu",GetCurrentProcessId());
        ChatStorage dstore;
        CHECK(storage_open(&dstore,ddir));
        CHECK(chat_profile_add(chat,L"Down",L"downgrade prompt")==0);
        CHECK(chat_conversation_set_system_prompt(chat,0,L"override before downgrade"));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OPENROUTER,L"down/model"));
        CHECK(storage_save(&dstore,chat));
        CHECK(strip_customization(dstore.path));
        CHECK(storage_load(&dstore,loaded)==1 && !dstore.recovered);
        CHECK(loaded->profile_count==0);
        CHECK(loaded->conversations[0].system_prompt.data==NULL);
        CHECK(!loaded->conversations[0].model[0] &&
            !loaded->conversations[0].ollama_model[0]);
        char *again=NULL; size_t asize=0;
        CHECK(storage_save(&dstore,loaded));
        CHECK(read_file_bytes(dstore.path,&again,&asize));
        CHECK(strstr(again,"\"version\":3")!=NULL);
        free(again);
        CHECK(chat_profile_remove(chat,0));
        CHECK(chat_conversation_set_system_prompt(chat,0,L""));
        CHECK(chat_conversation_set_model(chat,0,CHAT_BACKEND_OPENROUTER,L""));
        storage_close(&dstore); remove_store(&dstore,ddir);
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

    /* ---- Format 6: content parts and attachment records ------------- */

    /* Round trip through the app API: parts and attachment records persist
        field-for-field, repeated saves are byte-stable, and text-only
        messages keep their byte shape inside a v6 file. */
    {
        Chat *six = calloc(1, sizeof *six);
        CHECK(six);
        chat_init(six);
        chat_clear(six);
        ChatAttachmentMeta meta;
        memset(&meta, 0, sizeof meta);
        meta.id = 77;
        memcpy(meta.digest, k_digest_a, sizeof meta.digest);
        strcpy(meta.mime, "image/png");
        meta.bytes = 184320;
        meta.pixel_width = 1280;
        meta.pixel_height = 720;
        meta.created_at = 1758000000000;
        wcscpy(meta.display_name, L"photo.jpg");
        CHECK(chat_attachment_add(six, &meta));
        int u0 = chat_append(six, CHAT_ROLE_USER, L"what's this?");
        CHECK(u0 == 0);
        ChatImagePart image;
        memset(&image, 0, sizeof image);
        image.attachment_id = meta.id;
        image.pixel_width = meta.pixel_width;
        image.pixel_height = meta.pixel_height;
        memcpy(image.mime, meta.mime, sizeof image.mime);
        wcscpy(image.display_name, meta.display_name);
        CHECK(chat_message_add_image(&six->conversations[0].messages[u0],
            &image, CHAT_PART_FLAG_FIRST_FRAME));
        int a0 = chat_append(six, CHAT_ROLE_ASSISTANT, L"A cat.");
        CHECK(a0 == 1);
        ChatAttachmentMeta meta2 = meta;
        meta2.id = 78;
        memcpy(meta2.digest, k_digest_b, sizeof meta2.digest);
        strcpy(meta2.mime, "image/jpeg");
        meta2.bytes = 2048;
        meta2.pixel_width = 0;
        meta2.pixel_height = 0;
        meta2.created_at = 1758000000001;
        meta2.display_name[0] = 0;
        CHECK(chat_attachment_add(six, &meta2));
        int u1 = chat_append(six, CHAT_ROLE_USER, L"");
        CHECK(u1 == 2);
        ChatImagePart image2;
        memset(&image2, 0, sizeof image2);
        image2.attachment_id = meta2.id;
        memcpy(image2.mime, meta2.mime, sizeof image2.mime);
        CHECK(chat_message_add_image(&six->conversations[0].messages[u1],
            &image2, 0));
        /* A part may reference a record another message also references. */
        CHECK(chat_message_add_image(&six->conversations[0].messages[u1],
            &image, 0));
        CHECK(chat_message_part_count(&six->conversations[0].messages[u0]) == 2);
        CHECK(chat_message_part_count(&six->conversations[0].messages[u1]) == 2);
        CHECK(six->attachment_count == 2);
        wchar_t sdir[256];
        swprintf(sdir, 256, L"build\\storage-six-%lu", GetCurrentProcessId());
        ChatStorage sstore;
        CHECK(storage_open(&sstore, sdir));
        CHECK(storage_save(&sstore, six));
        {
            char *bytes = NULL;
            size_t size = 0;
            CHECK(read_file_bytes(sstore.path, &bytes, &size));
            CHECK(strstr(bytes, "\"version\":6") != NULL);
            CHECK(strstr(bytes, "\"type\":\"attachment\"") != NULL);
            CHECK(strstr(bytes, "\"parts\":[") != NULL);
            CHECK(strstr(bytes, "\"flags\":1") != NULL);
            CHECK(strstr(bytes, "\"k\":\"text\"") != NULL);
            CHECK(strstr(bytes, "\"k\":\"image\"") != NULL);
            free(bytes);
        }
        CHECK(storage_load(&sstore, loaded) == 1 && !sstore.recovered);
        CHECK(same_chat(six, loaded));
        {
            char *first = NULL, *second = NULL;
            size_t first_size = 0, second_size = 0;
            CHECK(storage_save(&sstore, loaded));
            CHECK(read_file_bytes(sstore.path, &first, &first_size));
            CHECK(storage_save(&sstore, loaded));
            CHECK(read_file_bytes(sstore.path, &second, &second_size));
            CHECK(first_size == second_size &&
                !memcmp(first, second, first_size));
            CHECK(strstr(first, "\"version\":6") != NULL);
            free(first); free(second);
        }
        chat_dispose(six); free(six);
        storage_close(&sstore);
        remove_store(&sstore, sdir);
    }

    /* The checked-in v6 fixture loads field-for-field; a missing blob is a
        degraded load, never corruption; the round trip is exact. */
    {
        wchar_t fdir[256];
        swprintf(fdir, 256, L"build\\storage-v6fix-%lu", GetCurrentProcessId());
        ChatStorage fstore;
        CHECK(storage_open(&fstore, fdir));
        CHECK(CopyFileW(L"tests\\state-v6-fixture.jsonl", fstore.path, FALSE));
        Chat *fixture = calloc(1, sizeof *fixture);
        CHECK(fixture);
        CHECK(storage_load(&fstore, fixture) == 1 && !fstore.recovered);
        CHECK(fixture->attachment_count == 2);
        CHECK(fixture->attachments[0].id == 5);
        CHECK(!strcmp(fixture->attachments[0].digest, k_digest_a));
        CHECK(!strcmp(fixture->attachments[0].mime, "image/png"));
        CHECK(fixture->attachments[0].bytes == 184320);
        CHECK(fixture->attachments[0].pixel_width == 1280);
        CHECK(fixture->attachments[0].pixel_height == 720);
        CHECK(fixture->attachments[0].created_at == 1758000000000);
        CHECK(!wcscmp(fixture->attachments[0].display_name, L"photo.jpg"));
        CHECK(fixture->attachments[1].id == 6);
        CHECK(!strcmp(fixture->attachments[1].digest, k_digest_b));
        CHECK(!strcmp(fixture->attachments[1].mime, "image/jpeg"));
        CHECK(fixture->attachments[1].bytes == 2048);
        CHECK(fixture->attachments[1].pixel_width == 0);
        CHECK(fixture->attachments[1].pixel_height == 0);
        CHECK(fixture->attachments[1].created_at == 1758000000001);
        CHECK(fixture->attachments[1].display_name[0] == 0);
        CHECK(fixture->conversation_count == 1);
        const ChatConversation *fc = &fixture->conversations[0];
        CHECK(fc->message_count == 3);
        ChatPartView view;
        CHECK(!wcscmp(chat_message_text(&fc->messages[0]), L"what's this?"));
        CHECK(chat_message_part_count(&fc->messages[0]) == 2);
        CHECK(chat_message_part_at(&fc->messages[0], 0, &view) &&
            view.kind == CHAT_PART_TEXT && view.flags == 0 &&
            !wcscmp(view.u.text.data, L"what's this?"));
        CHECK(chat_message_part_at(&fc->messages[0], 1, &view) &&
            view.kind == CHAT_PART_IMAGE &&
            view.flags == CHAT_PART_FLAG_FIRST_FRAME &&
            view.u.image.attachment_id == 5 &&
            view.u.image.pixel_width == 1280 &&
            view.u.image.pixel_height == 720 &&
            !strcmp(view.u.image.mime, "image/png") &&
            !wcscmp(view.u.image.display_name, L"photo.jpg"));
        /* A text-only message inside a v6 file stays on the fast path. */
        CHECK(chat_message_part_count(&fc->messages[1]) == 1);
        CHECK(!chat_message_has_images(&fc->messages[1]));
        CHECK(!wcscmp(chat_message_text(&fc->messages[1]), L"A cat."));
        /* Image-only: empty projection, two image parts, shared record. */
        CHECK(!wcscmp(chat_message_text(&fc->messages[2]), L""));
        CHECK(chat_message_part_count(&fc->messages[2]) == 2);
        CHECK(chat_message_part_at(&fc->messages[2], 0, &view) &&
            view.kind == CHAT_PART_IMAGE && view.flags == 0 &&
            view.u.image.attachment_id == 6);
        CHECK(chat_message_part_at(&fc->messages[2], 1, &view) &&
            view.kind == CHAT_PART_IMAGE && view.flags == 0 &&
            view.u.image.attachment_id == 5);
        CHECK(storage_save(&fstore, fixture));
        CHECK(storage_load(&fstore, loaded) == 1 && !fstore.recovered);
        CHECK(same_chat(fixture, loaded));
        {
            char *first = NULL, *second = NULL;
            size_t first_size = 0, second_size = 0;
            CHECK(storage_save(&fstore, fixture));
            CHECK(read_file_bytes(fstore.path, &first, &first_size));
            CHECK(storage_save(&fstore, fixture));
            CHECK(read_file_bytes(fstore.path, &second, &second_size));
            CHECK(first_size == second_size &&
                !memcmp(first, second, first_size));
            CHECK(strstr(first, "\"version\":6") != NULL);
            free(first); free(second);
        }
        chat_dispose(fixture); free(fixture);
        storage_close(&fstore);
        remove_store(&fstore, fdir);
    }

    /* Reverse gate: the parts field and attachment records are grammar
        changes gated to v6 in both directions. */
    {
        char att[256], msg[512];
        snprintf(att, sizeof att,
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"%s\","
            "\"mime\":\"image/png\",\"bytes\":184320,\"pixel_width\":1280,"
            "\"pixel_height\":720,\"created_at\":1758000000000,"
            "\"display_name\":\"photo.jpg\"}", k_digest_a);
        msg6(msg, sizeof msg, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"hi\"},"
            "{\"k\":\"image\",\"id\":5}]");
        const char *one_msg[1] = { msg };
        CHECK(load_v6_case(L"rev-parts", 5, 10, NULL, 0, one_msg, 1,
            loaded) == -1);
        const char *one_att[1] = { att };
        msg6(msg, sizeof msg, "\"id\":2,", "\"hi\"", "");
        CHECK(load_v6_case(L"rev-att", 5, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
    }

    /* Corruption matrix: every non-canonical or malformed format-6 shape is
        rejected transactionally (the destination stays untouched). */
    {
        char att[256], att_bad[256], att_min[256], a[512], big[2048];
        snprintf(att, sizeof att,
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"%s\","
            "\"mime\":\"image/png\",\"bytes\":184320,\"pixel_width\":1280,"
            "\"pixel_height\":720,\"created_at\":1758000000000,"
            "\"display_name\":\"photo.jpg\"}", k_digest_a);
        /* 63 hex characters: one short of a digest. */
        snprintf(att_bad, sizeof att_bad,
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"%.63s\","
            "\"mime\":\"image/png\",\"bytes\":10}", k_digest_a);
        /* The required core only: every optional field absent. */
        snprintf(att_min, sizeof att_min,
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"%s\","
            "\"bytes\":10}", k_digest_a);
        const char *one_att[1] = { att };
        const char *bad_att[1] = { att_bad };
        const char *min_att[1] = { att_min };
        const char *dup_att[2] = { att, att };
        const char *no_id[1] = {
            "{\"type\":\"attachment\",\"digest\":\"000000000000000000000000"
            "0000000000000000000000000000000000000000\",\"bytes\":10}" };
        const char *no_bytes[1] = {
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"0000000000000000"
            "000000000000000000000000000000000000000000000000\"}" };
        const char *one_msg[1];
        /* Unknown part kind. */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"audio\",\"id\":5}]");
        one_msg[0] = a;
        CHECK(load_v6_case(L"bad-kind", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* Flags outside CHAT_PART_FLAG_MASK. */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":5,\"flags\":2}]");
        CHECK(load_v6_case(L"bad-flags", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* Nonzero flags on a TEXT part. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"hi\",\"flags\":1},"
            "{\"k\":\"image\",\"id\":5}]");
        CHECK(load_v6_case(L"text-flags", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* Flags of the wrong type. */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":5,\"flags\":\"1\"}]");
        CHECK(load_v6_case(L"flags-type", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* A part referencing an id with no attachment record. */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":9}]");
        CHECK(load_v6_case(L"dangling", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* An attachment record without an id (no synthesis for these). */
        CHECK(load_v6_case(L"no-id", 6, 10, no_id, 1, NULL, 0, loaded) == -1);
        /* An attachment record without the required byte length. */
        CHECK(load_v6_case(L"no-bytes", 6, 10, no_bytes, 1, NULL, 0,
            loaded) == -1);
        /* Duplicate attachment ids. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", "");
        one_msg[0] = a;
        CHECK(load_v6_case(L"dup-att", 6, 10, dup_att, 2, one_msg, 1,
            loaded) == -1);
        /* A malformed digest (63 hex characters). */
        CHECK(load_v6_case(L"bad-digest", 6, 10, bad_att, 1, one_msg, 1,
            loaded) == -1);
        /* Attachment id == message id (one shared counter). */
        msg6(a, sizeof a, "\"id\":5,", "\"hi\"", "");
        CHECK(load_v6_case(L"collide-msg", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* Attachment id == conversation id (the fixture conversation is 1). */
        char att1[256];
        snprintf(att1, sizeof att1,
            "{\"type\":\"attachment\",\"id\":1,\"digest\":\"%s\","
            "\"bytes\":10}", k_digest_a);
        const char *conv_att[1] = { att1 };
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", "");
        CHECK(load_v6_case(L"collide-conv", 6, 10, conv_att, 1, one_msg, 1,
            loaded) == -1);
        /* text != the projection of the TEXT parts. */
        msg6(a, sizeof a, "\"id\":2,", "\"foo\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"bar\"},"
            "{\"k\":\"image\",\"id\":5}]");
        CHECK(load_v6_case(L"proj-mismatch", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* TEXT after IMAGE (outside the v1 shape). */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":5},"
            "{\"k\":\"text\",\"text\":\"hi\"}]");
        CHECK(load_v6_case(L"order", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* More than one TEXT part. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"hi\"},"
            "{\"k\":\"text\",\"text\":\"hi\"}]");
        CHECK(load_v6_case(L"two-text", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* A TEXT-only array (no IMAGE) is not emittable. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"hi\"}]");
        CHECK(load_v6_case(L"text-only", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* An empty TEXT part with an empty projection. */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"text\",\"text\":\"\"},"
            "{\"k\":\"image\",\"id\":5}]");
        CHECK(load_v6_case(L"empty-text", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* A nonempty projection with no TEXT part. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":5}]");
        CHECK(load_v6_case(L"no-text-part", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* parts of the wrong type. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", ",\"parts\":5");
        CHECK(load_v6_case(L"parts-num", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", ",\"parts\":\"x\"");
        CHECK(load_v6_case(L"parts-str", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", ",\"parts\":null");
        CHECK(load_v6_case(L"parts-null", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* More parts than CHAT_MAX_PARTS. */
        {
            size_t at = (size_t)snprintf(big, sizeof big, ",\"parts\":[");
            for (int i = 0; i < CHAT_MAX_PARTS + 1; i++)
                at += (size_t)snprintf(big + at, sizeof big - at,
                    "%s{\"k\":\"image\",\"id\":5}", i ? "," : "");
            snprintf(big + at, sizeof big - at, "]");
        }
        msg6(a, sizeof a, "\"id\":2,", "\"\"", big);
        one_msg[0] = a;
        CHECK(load_v6_case(L"too-many", 6, 10, one_att, 1, one_msg, 1,
            loaded) == -1);
        /* Accepted: an empty parts array is the fast path. */
        msg6(a, sizeof a, "\"id\":2,", "\"hi\"", ",\"parts\":[]");
        one_msg[0] = a;
        CHECK(load_v6_case(L"empty-array", 6, 10, one_att, 1, one_msg, 1,
            loaded) == 1);
        CHECK(loaded->conversation_count == 1);
        CHECK(loaded->conversations[0].message_count == 1);
        CHECK(chat_message_part_count(&loaded->conversations[0].messages[0]) == 1);
        CHECK(!chat_message_has_images(&loaded->conversations[0].messages[0]));
        CHECK(!wcscmp(chat_message_text(&loaded->conversations[0].messages[0]),
            L"hi"));
        /* Accepted: optional record fields default (absent MIME is empty). */
        msg6(a, sizeof a, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"image\",\"id\":5}]");
        CHECK(load_v6_case(L"defaults", 6, 10, min_att, 1, one_msg, 1,
            loaded) == 1);
        CHECK(loaded->attachment_count == 1);
        CHECK(!strcmp(loaded->attachments[0].mime, ""));
        CHECK(loaded->attachments[0].bytes == 10);
        CHECK(loaded->attachments[0].pixel_width == 0);
        CHECK(loaded->attachments[0].created_at == 0);
        CHECK(loaded->attachments[0].display_name[0] == 0);
        CHECK(chat_message_part_count(&loaded->conversations[0].messages[0]) == 1);
        {
            ChatPartView v;
            CHECK(chat_message_part_at(&loaded->conversations[0].messages[0],
                0, &v) && v.kind == CHAT_PART_IMAGE &&
                !strcmp(v.u.image.mime, "") &&
                v.u.image.attachment_id == 5);
        }
    }

    /* OOM during decode: the quarantine releases exactly what it built and
        the load fails cleanly (or succeeds when the injection misses). */
    {
        long baseline = live_allocs;
        for (int pass = 0; pass < 2; pass++) {
            for (long n = 0; n < 32; n++) {
                wchar_t odir[256];
                swprintf(odir, 256, L"build\\storage-oom-%d-%ld-%lu",
                    pass, n, GetCurrentProcessId());
                ChatStorage ostore;
                CHECK(storage_open(&ostore, odir));
                CHECK(CopyFileW(L"tests\\state-v6-fixture.jsonl",
                    ostore.path, FALSE));
                Chat *scratch = calloc(1, sizeof *scratch);
                CHECK(scratch);
                seam_reset();
                if (pass == 0) {
                    pass_mallocs = n;
                    fail_mallocs = 1;
                } else {
                    fail_realloc_index = n + 1;
                }
                int result = storage_load(&ostore, scratch);
                seam_reset();
                CHECK(result == 1 || result == -1);
                chat_dispose(scratch);
                free(scratch);
                CHECK(live_allocs == baseline);
                storage_close(&ostore);
                remove_store(&ostore, odir);
            }
        }
    }

    /* Live-set scan (A4.4.4): digests are collected from all three state
        files, so a blob referenced only by state.bak.jsonl survives
        attachment_store_collect after a delete + single save; unreferenced
        orphans are swept and pending keep-ids survive. */
    {
        wchar_t ldir[256];
        swprintf(ldir, 256, L"build\\storage-live-%lu", GetCurrentProcessId());
        ChatStorage lstore;
        CHECK(storage_open(&lstore, ldir));
        Chat *lc = calloc(1, sizeof *lc);
        CHECK(lc);
        chat_init(lc);
        chat_clear(lc);
        ChatAttachmentStore astore;
        CHECK(attachment_store_open(&astore, ldir, &lc->next_id));
        /* Blob A: referenced by a message and saved. Blob C: pending (put
           this session, referenced by no message). Blob B: an orphan. */
        ChatAttachmentMeta meta_a, meta_b, meta_c;
        CHECK(attachment_store_put(&astore, "png-a", 5, "image/png", L"a.png",
            &meta_a, NULL));
        CHECK(attachment_store_put(&astore, "png-c", 5, "image/png", L"c.png",
            &meta_c, NULL));
        CHECK(attachment_store_put(&astore, "png-b", 5, "image/png", L"b.png",
            &meta_b, NULL));
        CHECK(chat_attachment_add(lc, &meta_a));
        int mi = chat_append(lc, CHAT_ROLE_USER, L"hi");
        CHECK(mi == 0);
        ChatImagePart image;
        memset(&image, 0, sizeof image);
        image.attachment_id = meta_a.id;
        memcpy(image.mime, meta_a.mime, sizeof image.mime);
        CHECK(chat_message_add_image(&lc->conversations[0].messages[mi],
            &image, 0));
        CHECK(storage_save(&lstore, lc));
        /* Delete the last image and save once: the record rotates into the
           backup and the primary no longer names blob A. */
        chat_message_clear_parts(&lc->conversations[0].messages[mi]);
        CHECK(chat_attachment_prune(lc, NULL, 0));
        CHECK(lc->attachment_count == 0);
        CHECK(storage_save(&lstore, lc));
        /* The A4.4.4 live set: all three state files plus the live parts
           (none) plus the pending keep-ids. */
        struct DigestSet set;
        memset(&set, 0, sizeof set);
        CHECK(storage_scan_attachment_digests(lstore.path, collect_digest,
            &set));
        CHECK(storage_scan_attachment_digests(lstore.backup, collect_digest,
            &set));
        CHECK(storage_scan_attachment_digests(lstore.temporary,
            collect_digest, &set));
        CHECK(set.count == 1);
        CHECK(!memcmp(set.dig[0], meta_a.digest, 64));
        /* The pending attachment's digest joins the live set as a keep. */
        memcpy(set.dig[set.count++], meta_c.digest, 65);
        const char *live[8];
        for (size_t i = 0; i < set.count; i++) live[i] = set.dig[i];
        CHECK(attachment_store_collect(&astore, live, set.count));
        /* A survives (backup-referenced), C survives (pending), B is swept. */
        unsigned char *data = NULL;
        size_t n = 0;
        CHECK(attachment_store_get(&astore, meta_a.digest, &data, &n));
        free(data); data = NULL;
        CHECK(attachment_store_get(&astore, meta_c.digest, &data, &n));
        free(data); data = NULL;
        CHECK(!attachment_store_get(&astore, meta_b.digest, &data, &n));
        /* A corrupt file contributes nothing and reports the failure. */
        {
            FILE *bad = _wfopen(lstore.backup, L"wb");
            CHECK(bad);
            fputs("{\"truncated\":", bad);
            fclose(bad);
            struct DigestSet empty;
            memset(&empty, 0, sizeof empty);
            CHECK(!storage_scan_attachment_digests(lstore.backup,
                collect_digest, &empty));
            CHECK(empty.count == 0);
            CHECK(storage_scan_attachment_digests(lstore.temporary,
                collect_digest, &empty));
        }
        /* Teardown: sweep the remaining blobs and remove the store tree. */
        CHECK(attachment_store_collect(&astore, NULL, 0));
        attachment_store_close(&astore);
        wchar_t sub[1200];
        swprintf(sub, 1200, L"%ls\\attachments\\.tmp", ldir);
        RemoveDirectoryW(sub);
        swprintf(sub, 1200, L"%ls\\attachments\\corrupt", ldir);
        RemoveDirectoryW(sub);
        swprintf(sub, 1200, L"%ls\\attachments", ldir);
        RemoveDirectoryW(sub);
        chat_dispose(lc); free(lc);
        storage_close(&lstore);
        remove_store(&lstore, ldir);
    }

    /* Digest collector contract: the whole snapshot is validated through
        the loader's grammar and its digests staged before any callback runs,
        file-not-found is the only empty contribution, and an uninspectable
        file reports failure so a sweep can never run against a live set it
        could not verify. */
    {
        wchar_t sdir[256];
        swprintf(sdir, 256, L"build\\storage-scan-%lu", GetCurrentProcessId());
        ChatStorage sstore;
        CHECK(storage_open(&sstore, sdir));
        char settings[384], conversation[256], att[256], msg[512];
        snprintf(settings, sizeof settings,
            "{\"type\":\"settings\",\"version\":6,\"next_id\":10,\"active\":0,"
            "\"conversation_count\":1,\"model_history_count\":0,\"window_x\":0,"
            "\"window_y\":0,\"window_width\":1100,\"window_height\":720,"
            "\"maximized\":0,\"sidebar_width\":232,\"model\":\"m\","
            "\"system_prompt\":\"\",\"profile_count\":0}");
        snprintf(conversation, sizeof conversation,
            "{\"type\":\"conversation\",\"id\":1,\"created_at\":1000,"
            "\"modified_at\":1000,\"renamed\":0,\"message_count\":1,"
            "\"title\":\"c\",\"draft\":\"\"}");
        snprintf(att, sizeof att,
            "{\"type\":\"attachment\",\"id\":5,\"digest\":\"%s\","
            "\"mime\":\"image/png\",\"bytes\":184320,\"pixel_width\":1280,"
            "\"pixel_height\":720,\"created_at\":1758000000000,"
            "\"display_name\":\"photo.jpg\"}", k_digest_a);
        const char *lines[4] = { settings, att, conversation, NULL };
        struct DigestSet set;
        /* Checksum-valid but loader-rejected (unknown part kind): the
           record's digest must never reach the live set. */
        msg6(msg, sizeof msg, "\"id\":2,", "\"\"",
            ",\"parts\":[{\"k\":\"audio\",\"id\":5}]");
        lines[3] = msg;
        CHECK(write_snapshot(sstore.path, lines, 4));
        memset(&set, 0, sizeof set);
        CHECK(!storage_scan_attachment_digests(sstore.path, collect_digest,
            &set));
        CHECK(set.count == 0);
        /* The same file really is rejected by the loader. */
        Chat *scratch = calloc(1, sizeof *scratch);
        CHECK(scratch);
        CHECK(storage_load(&sstore, scratch) == -1 && !sstore.writable);
        chat_dispose(scratch); free(scratch);
        /* A valid snapshot reports exactly its own records. */
        msg6(msg, sizeof msg, "\"id\":2,", "\"hi\"", "");
        lines[3] = msg;
        CHECK(write_snapshot(sstore.path, lines, 4));
        memset(&set, 0, sizeof set);
        CHECK(storage_scan_attachment_digests(sstore.path, collect_digest,
            &set));
        CHECK(set.count == 1);
        CHECK(!memcmp(set.dig[0], k_digest_a, 64));
        /* File-not-found inside an existing directory is the only empty
           contribution. */
        wchar_t absent[1200];
        swprintf(absent, 1200, L"%ls\\absent.jsonl", sdir);
        memset(&set, 0, sizeof set);
        CHECK(storage_scan_attachment_digests(absent, collect_digest, &set));
        CHECK(set.count == 0);
        /* A missing parent directory is uninspectable, not empty. */
        wchar_t notdir[1200];
        swprintf(notdir, 1200, L"%ls\\no-such-dir\\state.jsonl", sdir);
        CHECK(!storage_scan_attachment_digests(notdir, collect_digest, &set));
        CHECK(set.count == 0);
        /* A file that exists but cannot be opened reports failure too. */
        HANDLE lock = CreateFileW(sstore.path, GENERIC_READ, 0, NULL,
            OPEN_EXISTING, 0, NULL);
        CHECK(lock != INVALID_HANDLE_VALUE);
        memset(&set, 0, sizeof set);
        CHECK(!storage_scan_attachment_digests(sstore.path, collect_digest,
            &set));
        CHECK(set.count == 0);
        CloseHandle(lock);
        storage_close(&sstore);
        remove_store(&sstore, sdir);
    }

    /* Pruning: unreferenced records are dropped before the snapshot
        hand-off, the emitted version falls back off 6, keep-ids protect
        pending records, and survivor order is preserved. */
    {
        wchar_t pdir[256];
        swprintf(pdir, 256, L"build\\storage-prune-%lu", GetCurrentProcessId());
        ChatStorage pstore;
        CHECK(storage_open(&pstore, pdir));
        Chat *pc = calloc(1, sizeof *pc);
        CHECK(pc);
        chat_init(pc);
        chat_clear(pc);
        ChatAttachmentMeta m1, m2;
        memset(&m1, 0, sizeof m1);
        m1.id = 301;
        memcpy(m1.digest, k_digest_a, sizeof m1.digest);
        strcpy(m1.mime, "image/png");
        m1.bytes = 10;
        m2 = m1;
        m2.id = 302;
        memcpy(m2.digest, k_digest_b, sizeof m2.digest);
        CHECK(chat_attachment_add(pc, &m1));
        CHECK(chat_attachment_add(pc, &m2));
        int mi = chat_append(pc, CHAT_ROLE_USER, L"both");
        CHECK(mi == 0);
        ChatImagePart image;
        memset(&image, 0, sizeof image);
        image.attachment_id = 301;
        CHECK(chat_message_add_image(&pc->conversations[0].messages[mi],
            &image, 0));
        image.attachment_id = 302;
        CHECK(chat_message_add_image(&pc->conversations[0].messages[mi],
            &image, 0));
        CHECK(storage_save(&pstore, pc));
        {
            char *bytes = NULL;
            size_t size = 0;
            CHECK(read_file_bytes(pstore.path, &bytes, &size));
            CHECK(strstr(bytes, "\"version\":6") != NULL);
            CHECK(strstr(bytes, "\"type\":\"attachment\"") != NULL);
            free(bytes);
        }
        /* Delete the last image parts and prune: the records go with them
           and the emitted version falls back off 6. */
        chat_message_clear_parts(&pc->conversations[0].messages[mi]);
        CHECK(chat_attachment_prune(pc, NULL, 0));
        CHECK(pc->attachment_count == 0);
        CHECK(storage_save(&pstore, pc));
        {
            char *bytes = NULL;
            size_t size = 0;
            CHECK(read_file_bytes(pstore.path, &bytes, &size));
            CHECK(strstr(bytes, "\"version\":3") != NULL);
            CHECK(strstr(bytes, "\"type\":\"attachment\"") == NULL);
            CHECK(strstr(bytes, "\"parts\"") == NULL);
            free(bytes);
        }
        /* A pending/staged keep-id protects its record from pruning, and
           compaction preserves the survivors' order. */
        CHECK(chat_attachment_add(pc, &m1));
        CHECK(chat_attachment_add(pc, &m2));
        uint64_t keep = 302;
        CHECK(chat_attachment_prune(pc, &keep, 1));
        CHECK(pc->attachment_count == 1);
        CHECK(pc->attachments[0].id == 302);
        CHECK(chat_attachment(pc, 301) == NULL);
        CHECK(chat_attachment(pc, 302) != NULL);
        CHECK(chat_attachment_prune(pc, NULL, 0));
        CHECK(pc->attachment_count == 0);
        chat_dispose(pc); free(pc);
        storage_close(&pstore);
        remove_store(&pstore, pdir);
    }

    chat_dispose(chat); chat_dispose(loaded);
    free(chat); free(loaded);
    puts("Storage roundtrip, locking, corruption, backup and interrupted-write recovery passed");
    return 0;
}
