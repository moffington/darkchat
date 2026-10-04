#include "chat/import/import.h"
#include "chat/core/chat.h"
#include "chat/core/sha256.h"
#include "chat/export/export.h"
#include "chat/export/package.h"
#include "chat/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Allocation seam: fail the (n+1)-th allocation while tracking live wrapped
   allocations so a missed free shows up as a leak. Mirrors tests/test_export.c
   and tests/test_storage.c. */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
static long pass_allocs, fail_allocs;
static long live_allocs;
void *__wrap_malloc(size_t size) {
    if (pass_allocs > 0) { --pass_allocs; goto pass; }
    if (fail_allocs > 0) { --fail_allocs; return NULL; }
pass: {
    void *result = __real_malloc(size);
    if (result) ++live_allocs;
    return result;
}
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (pass_allocs > 0) { --pass_allocs; goto pass; }
    if (fail_allocs > 0) { --fail_allocs; return NULL; }
pass: {
    void *result = __real_realloc(pointer, size);
    if (result && !pointer) ++live_allocs;
    return result;
}
}
void __wrap_free(void *pointer) {
    if (pointer) --live_allocs;
    __real_free(pointer);
}
static void seam_reset(void) { pass_allocs = fail_allocs = 0; }

/* --- fixtures ----------------------------------------------------------- */

static void base_chat(Chat *chat) {
    chat_init(chat);
    chat_clear(chat);
    ChatConversation *c = &chat->conversations[0];
    c->id = 7;
    c->created_at = 1000;
    c->modified_at = 2000;
    c->renamed = true;
    wcscpy(c->title, L"Test chat");
}

static Chat *new_chat(void) {
    Chat *chat = (Chat *)malloc(sizeof *chat);
    if (chat) base_chat(chat);
    return chat;
}

static ChatMessage *add(Chat *chat, int conversation, ChatRole role,
    const wchar_t *text, int64_t at) {
    int index = chat_append_at(chat, conversation, role, text);
    if (index < 0) return NULL;
    ChatMessage *m = &chat->conversations[conversation].messages[index];
    m->created_at = m->modified_at = at;
    return m;
}

static void full_generation(ChatMessage *m) {
    ChatGeneration *g = &m->generation;
    g->state = CHAT_GENERATION_COMPLETE;
    wcscpy(g->requested_model, L"openai/gpt-4o-mini");
    wcscpy(g->actual_model, L"openai/gpt-4o-mini");
    wcscpy(g->finish_reason, L"stop");
    g->prompt_tokens = 12;
    g->completion_tokens = 34;
    g->total_tokens = 46;
    g->cost = 0.25;
    g->reasoning_ms = 1500;
}

/* Builds a minimal, fully valid import document with `conversations` empty
   conversations (or with `messages` messages when conversations == 1). */
static char *minimal_doc(int conversations, int messages) {
    size_t capacity = 256 + (size_t)conversations * (size_t)(messages > 0
        ? messages * 80 + 128 : 128);
    char *buffer = (char *)malloc(capacity);
    if (!buffer) return NULL;
    size_t used = 0;
    used += (size_t)snprintf(buffer + used, capacity - used,
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":[");
    for (int c = 0; c < conversations; c++) {
        used += (size_t)snprintf(buffer + used, capacity - used,
            "%s{\"id\":%d,\"title\":\"Conv %d\",\"created_at\":1,"
            "\"modified_at\":2,\"messages\":[", c ? "," : "", c + 1, c);
        for (int m = 0; m < messages; m++)
            used += (size_t)snprintf(buffer + used, capacity - used,
                "%s{\"role\":\"user\",\"created_at\":1,\"modified_at\":1,"
                "\"text\":\"m\",\"generation\":{}}", m ? "," : "");
        used += (size_t)snprintf(buffer + used, capacity - used, "]}");
    }
    used += (size_t)snprintf(buffer + used, capacity - used, "]}");
    return buffer;
}

static ChatImportStatus import_text(Chat *chat, const char *text,
    ChatImportStats *stats) {
    return chat_import_json(chat, text, strlen(text), NULL, stats);
}

/* --- content parts (schema version 2) ------------------------------------- */

static const unsigned char TINY[] = { 0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5, 6 };

/* A blob source that records what the importer asked to be made durable, and
   can be told to fail partway. Nothing here is a filesystem: the point is to
   prove the importer writes only after a document has fully validated. */
typedef struct {
    unsigned char staged[4][64];
    size_t lengths[4];
    char digests[4][65];
    size_t count;
    int fail_at;      /* -1 never fails, otherwise fail at this index */
    bool read_missing;
    char read_folder[80];
    char read_basename[80];
    size_t read_count;
} BlobSink;

static void sink_init(BlobSink *sink) {
    memset(sink, 0, sizeof *sink);
    sink->fail_at = -1;
}

static bool sink_stage(void *user, const unsigned char *bytes, size_t length,
    char out_digest[65], bool *out_created) {
    BlobSink *sink = (BlobSink *)user;
    if (sink->fail_at >= 0 && (int)sink->count == sink->fail_at) return false;
    if (sink->count >= 4) return false;
    if (length > sizeof sink->staged[0]) return false;
    memcpy(sink->staged[sink->count], bytes, length);
    sink->lengths[sink->count] = length;
    sha256_hex(bytes, length, sink->digests[sink->count]);
    memcpy(out_digest, sink->digests[sink->count], 65);
    if (out_created) *out_created = true;
    sink->count++;
    return true;
}

static bool sink_read_file(void *user, const char *folder,
    const char *basename, unsigned char **out, size_t *out_n) {
    BlobSink *sink = (BlobSink *)user;
    snprintf(sink->read_folder, sizeof sink->read_folder, "%s", folder);
    snprintf(sink->read_basename, sizeof sink->read_basename, "%s", basename);
    sink->read_count++;
    if (sink->read_missing) return false;
    *out = (unsigned char *)malloc(sizeof TINY);
    if (!*out) return false;
    memcpy(*out, TINY, sizeof TINY);
    *out_n = sizeof TINY;
    return true;
}

static void sink_discard(void *user, unsigned char *bytes, size_t length) {
    (void)user; (void)length;
    free(bytes);
}

static void sink_blobs(BlobSink *sink, ChatImportBlobs *blobs) {
    memset(blobs, 0, sizeof *blobs);
    blobs->user = sink;
    blobs->read_file = sink_read_file;
    blobs->discard = sink_discard;
    blobs->stage = sink_stage;
}

/* A one-conversation, one-message document carrying a single image part. The
   payload is built as a string so a test can corrupt any field of it. */
static char *image_doc(const char *version, const char *text,
    const char *extra_part_fields, const char *extra_message_fields) {
    char digest[65];
    size_t capacity = 2048;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) return NULL;
    sha256_hex(TINY, sizeof TINY, digest);
    JsonBuf data;
    json_buf_init(&data, 16);
    json_buf_append_raw(&data, "\"", 1);
    json_buf_append_base64(&data, TINY, sizeof TINY);
    json_buf_append_raw(&data, "\"", 1);
    data.data[data.length] = 0;
    snprintf(buffer, capacity,
        "{\"format\":\"darkchat.export\",\"version\":%s,\"conversations\":["
        "{\"id\":5,\"title\":\"T\",\"created_at\":1,\"modified_at\":2,"
        "\"messages\":[{\"role\":\"user\",\"created_at\":9,\"modified_at\":9,"
        "\"text\":\"%s\",\"generation\":{}%s,\"content\":["
        "{\"type\":\"text\",\"text\":\"%s\"},"
        "{\"type\":\"image\",\"mime\":\"image/png\",\"pixel_width\":7,"
        "\"pixel_height\":9,\"display_name\":\"p.png\",\"sha256\":\"%s\","
        "\"data\":%s%s}]}]}]}",
        version, text, extra_message_fields ? extra_message_fields : "",
        text, digest, data.data, extra_part_fields ? extra_part_fields : "");
    json_buf_free(&data);
    return buffer;
}

/* Compares the imported message against what the document described. The part
   list must be [TEXT, IMAGE] with the metadata intact and the projection equal
   to the text. */
static int check_image_message(const Chat *chat, int conversation,
    uint32_t want_width, uint32_t want_height, const wchar_t *want_name) {
    const ChatMessage *m = &chat->conversations[conversation].messages[0];
    CHECK(chat_message_part_count(m) == 2);
    ChatPartView text, image;
    CHECK(chat_message_part_at(m, 0, &text));
    CHECK(text.kind == CHAT_PART_TEXT);
    CHECK(text.u.text.length == 4 &&
        !wmemcmp(text.u.text.data, L"look", 4));
    CHECK(chat_message_part_at(m, 1, &image));
    CHECK(image.kind == CHAT_PART_IMAGE);
    CHECK(image.u.image.pixel_width == want_width &&
        image.u.image.pixel_height == want_height);
    CHECK(!strcmp(image.u.image.mime, "image/png"));
    CHECK(!wcscmp(image.u.image.display_name, want_name));
    CHECK(image.u.image.attachment_id != 0);
    CHECK(chat_message_has_images(m));
    /* The projection is what the document's `text` said. */
    CHECK(!wcscmp(chat_message_text(m), L"look"));
    /* The record the part names must exist and agree with it, because the
       record is what the exporter and the request path read. */
    const ChatAttachmentMeta *rec =
        chat_attachment(chat, image.u.image.attachment_id);
    CHECK(rec);
    CHECK(rec->bytes == sizeof TINY);
    CHECK(!strcmp(rec->mime, image.u.image.mime));
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    CHECK(!strcmp(rec->digest, digest));
    return 0;
}

typedef struct {
    int active, count;
    uint64_t next_id;
    wchar_t status[CHAT_STATUS_TEXT];
    wchar_t title[CHAT_TITLE_TEXT];
    uint64_t id;
    size_t messages;
} Snapshot;

static void snapshot(const Chat *chat, Snapshot *s) {
    s->active = chat->active;
    s->count = chat->conversation_count;
    s->next_id = chat->next_id;
    wcscpy(s->status, chat->status);
    s->id = chat->conversations[0].id;
    wcscpy(s->title, chat->conversations[0].title);
    s->messages = chat->conversations[0].message_count;
}

static bool unchanged(const Chat *chat, const Snapshot *s) {
    return chat->active == s->active && chat->conversation_count == s->count &&
        chat->next_id == s->next_id && !wcscmp(chat->status, s->status) &&
        chat->conversations[0].id == s->id &&
        !wcscmp(chat->conversations[0].title, s->title) &&
        chat->conversations[0].message_count == s->messages;
}

/* --- tests -------------------------------------------------------------- */

static int test_round_trip(void) {
    Chat *source = new_chat();
    Chat *dest = new_chat();
    CHECK(source && dest);
    Snapshot before;
    snapshot(dest, &before);

    ChatConversation *c0 = &source->conversations[0];
    CHECK(add(source, 0, CHAT_ROLE_USER, L"Hello", 1500) != NULL);
    ChatMessage *a = add(source, 0, CHAT_ROLE_ASSISTANT, L"Hi there", 1600);
    CHECK(a != NULL);
    full_generation(a);
    CHECK(chat_message_set_reasoning(a, L"Thinking\nHard"));
    wcscpy(c0->model, L"openai/gpt-4o-mini");
    wcscpy(c0->ollama_model, L"llama3.2");
    c0->modified_at = 2000;

    int second = chat_new_conversation(source);
    CHECK(second == 1);
    ChatConversation *c1 = &source->conversations[1];
    c1->renamed = true;
    wcscpy(c1->title, L"Emoji chat");
    CHECK(add(source, 1, CHAT_ROLE_USER, L"caf\xc3\xa9 \xf0\x9f\x98\x80", 3500) != NULL);
    c1->created_at = 3000;
    c1->modified_at = 4000;
    CHECK(chat_conversation_apply_system_prompt(source, 1, L""));

    JsonBuf out;
    CHECK(chat_export_json(source, 0, true, 42, NULL, &out));
    ChatImportStats stats;
    CHECK(chat_import_json(dest, out.data, out.length, NULL, &stats) ==
        CHAT_IMPORT_OK);
    CHECK(stats.conversations_added == 2 && stats.messages_added == 3);

    /* Destination context untouched: the pre-existing conversation is intact,
       the active selection is unchanged, and status did not move. */
    CHECK(dest->conversation_count == 3);
    CHECK(dest->active == before.active);
    CHECK(!wcscmp(dest->status, before.status));
    CHECK(dest->conversations[0].id == before.id);
    CHECK(!wcscmp(dest->conversations[0].title, before.title));
    CHECK(dest->conversations[0].message_count == before.messages);
    CHECK(dest->next_id > before.next_id);

    ChatConversation *d0 = &dest->conversations[1];
    CHECK(!wcscmp(d0->title, L"Test chat"));
    CHECK(d0->created_at == 1000 && d0->modified_at == 2000);
    CHECK(!wcscmp(d0->model, L"openai/gpt-4o-mini"));
    CHECK(!wcscmp(d0->ollama_model, L"llama3.2"));
    CHECK(d0->id != 7 && d0->id <= dest->next_id);
    CHECK(d0->message_count == 2);
    CHECK(d0->messages[0].role == CHAT_ROLE_USER);
    CHECK(!wcscmp(chat_message_text(&d0->messages[0]), L"Hello"));
    CHECK(d0->messages[0].created_at == 1500);
    CHECK(d0->messages[0].generation.state == CHAT_GENERATION_NONE);
    CHECK(d0->messages[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(!wcscmp(chat_message_text(&d0->messages[1]), L"Hi there"));
    CHECK(!wcscmp(chat_message_reasoning(&d0->messages[1]), L"Thinking\nHard"));
    /* Sanitization: Complete, but no audit metric survives. */
    const ChatGeneration *g = &d0->messages[1].generation;
    CHECK(g->state == CHAT_GENERATION_COMPLETE);
    CHECK(!g->requested_model[0] && !g->actual_model[0] &&
        !g->finish_reason[0] && !g->error[0]);
    CHECK(g->backend == CHAT_BACKEND_OPENROUTER);
    CHECK(g->prompt_tokens == -1 && g->completion_tokens == -1 &&
        g->total_tokens == -1 && g->cost == -1 && g->reasoning_ms == -1);

    ChatConversation *d1 = &dest->conversations[2];
    CHECK(!wcscmp(d1->title, L"Emoji chat"));
    CHECK(d1->system_prompt_present && d1->system_prompt.data == NULL);
    CHECK(!wcscmp(chat_message_text(&d1->messages[0]),
        L"caf\xc3\xa9 \xf0\x9f\x98\x80"));

    json_buf_free(&out);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    return 0;
}

/* Imported titles are authoritative: a later user message must not derive a
   new title over a custom one. The source intentionally has no messages, so a
   non-authoritative title would be replaced the moment the first user message
   is appended. */
static int test_title_authority(void) {
    Chat *source = new_chat();
    Chat *dest = new_chat();
    CHECK(source && dest);
    wcscpy(source->conversations[0].title, L"Custom title");
    JsonBuf out;
    CHECK(chat_export_json(source, 0, false, 1, NULL, &out));
    CHECK(chat_import_json(dest, out.data, out.length, NULL, NULL) == CHAT_IMPORT_OK);
    int imported = dest->conversation_count - 1;
    CHECK(!wcscmp(dest->conversations[imported].title, L"Custom title"));
    CHECK(add(dest, imported, CHAT_ROLE_USER, L"first", 2) != NULL);
    CHECK(!wcscmp(dest->conversations[imported].title, L"Custom title"));
    json_buf_free(&out);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    return 0;
}

/* Exhausting the shared id counter mid-import is a capacity failure, not an
   allocation failure, and leaves the destination untouched. */
static int test_id_exhaustion(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    char *doc = minimal_doc(1, 2);
    CHECK(doc);
    dest->next_id = CHAT_MAX_ID - 1;
    Snapshot before;
    snapshot(dest, &before);
    CHECK(import_text(dest, doc, NULL) == CHAT_IMPORT_CAPACITY);
    CHECK(unchanged(dest, &before));
    free(doc);
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_malformed(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    const char *bad[] = {
        "not json",
        "{}",
        "{\"format\":\"darkchat.export\",\"version\":2,"
            "\"conversations\":[{\"title\":\"T\",\"created_at\":1,"
            "\"modified_at\":1,\"messages\":[]}]}",
        "{\"format\":\"other\",\"version\":1,"
            "\"conversations\":[{\"title\":\"T\",\"created_at\":1,"
            "\"modified_at\":1,\"messages\":[]}]}",
        /* empty conversations array */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":[]}",
        /* empty title */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
        /* missing messages */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1}]}",
        /* unknown role */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"robot\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"x\"}]}]}",
        /* missing text */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1}]}]}",
        /* duplicate recognized field */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"title\":\"U\",\"created_at\":1,"
            "\"modified_at\":1,\"messages\":[]}]}",
        /* both prompt override and presence flag */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"system_prompt\":\"x\",\"system_prompt_present\":true,"
            "\"messages\":[]}]}",
        /* out-of-range timestamp */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":0,\"modified_at\":1,"
            "\"messages\":[]}]}",
        /* unpaired surrogate escape in text */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"\\uD800\"}]}]}",
        /* escaped NUL in text */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"a\\u0000b\"}]}]}",
        /* duplicate message text after a decoded allocation (leak regression) */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"x\",\"text\":\"y\","
            "\"generation\":{}}]}]}",
        /* a conversation id is required */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
        /* a generation object is required */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"x\"}]}]}",
        /* a negative conversation id is invalid */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"id\":-3,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
        /* a fractional conversation id is invalid */
        "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"id\":1.5,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ChatImportStatus status = import_text(dest, bad[i], NULL);
        CHECK(status == CHAT_IMPORT_MALFORMED);
        CHECK(unchanged(dest, &before));
    }
    CHECK(import_text(dest, "", NULL) == CHAT_IMPORT_MALFORMED);
    CHECK(unchanged(dest, &before));

    /* A decoded value past its bound is reported as oversized, not malformed. */
    {
        char over[256];
        char title[80];
        memset(title, 'a', sizeof title);
        title[sizeof title - 1] = 0;
        snprintf(over, sizeof over,
            "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"%s\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}", title);
        CHECK(import_text(dest, over, NULL) == CHAT_IMPORT_TOO_LARGE);
        CHECK(unchanged(dest, &before));
    }

    /* Invalid raw UTF-8 inside a string is rejected, not replaced with U+FFFD. */
    {
        char invalid[256];
        int length = snprintf(invalid, sizeof invalid,
            "{\"format\":\"darkchat.export\",\"version\":1,\"conversations\":["
            "{\"title\":\"X%cY\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}", (char)0xff);
        CHECK(length > 0 && (size_t)length < sizeof invalid);
        CHECK(chat_import_json(dest, invalid, (size_t)length, NULL, NULL) ==
            CHAT_IMPORT_MALFORMED);
    }
    static const char embedded_nul[] = { '{', 0, '}' };
    CHECK(chat_import_json(dest, embedded_nul, sizeof embedded_nul, NULL, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(unchanged(dest, &before));

    /* Unknown fields, including duplicated unknown keys, are ignored. */
    const char *unknown =
        "{\"format\":\"darkchat.export\",\"version\":1,\"exported_at\":9,"
        "\"extra\":1,\"conversations\":[{\"title\":\"T\",\"created_at\":1,"
        "\"modified_at\":1,\"id\":99,\"brand_new\":{\"a\":1},"
        "\"brand_new\":2,\"messages\":[]}]}";
    CHECK(import_text(dest, unknown, NULL) == CHAT_IMPORT_OK);
    int imported = dest->conversation_count - 1;
    CHECK(!wcscmp(dest->conversations[imported].title, L"T"));
    CHECK(dest->conversations[imported].id != 99);

    chat_dispose(dest); free(dest);
    return 0;
}

static int test_capacity(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    /* One pre-existing conversation leaves room for 127 more. */
    char *too_many = minimal_doc(CHAT_MAX_CONVERSATIONS, 0);
    CHECK(too_many);
    Snapshot before;
    snapshot(dest, &before);
    CHECK(import_text(dest, too_many, NULL) == CHAT_IMPORT_CAPACITY);
    CHECK(unchanged(dest, &before));
    free(too_many);

    /* A single conversation may hold at most CHAT_MAX_MESSAGES. */
    char *too_many_messages = minimal_doc(1, CHAT_MAX_MESSAGES + 1);
    CHECK(too_many_messages);
    CHECK(import_text(dest, too_many_messages, NULL) == CHAT_IMPORT_CAPACITY);
    CHECK(unchanged(dest, &before));
    free(too_many_messages);

    chat_dispose(dest); free(dest);
    return 0;
}

static int test_allocation_failure(void) {
    long baseline = live_allocs;
    Chat *source = new_chat();
    Chat *dest = new_chat();
    CHECK(source && dest);
    CHECK(add(source, 0, CHAT_ROLE_USER, L"leak check", 1) != NULL);
    ChatMessage *a = add(source, 0, CHAT_ROLE_ASSISTANT, L"answer", 2);
    CHECK(a != NULL);
    full_generation(a);
    JsonBuf out;
    CHECK(chat_export_json(source, 0, false, 1, NULL, &out));

    Snapshot before;
    snapshot(dest, &before);
    bool succeeded = false;
    for (long n = 0; n < 512; n++) {
        long live_before = live_allocs;
        pass_allocs = n;
        fail_allocs = 1;
        ChatImportStats stats;
        ChatImportStatus status = chat_import_json(dest, out.data, out.length,
            NULL, &stats);
        if (status == CHAT_IMPORT_OK) {
            CHECK(n > 0);
            CHECK(!unchanged(dest, &before));
            succeeded = true;
            /* Reset the destination for the next checks. */
            break;
        }
        CHECK(status == CHAT_IMPORT_OOM);
        CHECK(unchanged(dest, &before));
        CHECK(live_allocs == live_before);
    }
    CHECK(succeeded);
    seam_reset();
    json_buf_free(&out);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    CHECK(live_allocs == baseline);
    return 0;
}

static int test_reject_when_full(void) {
    Chat *source = new_chat();
    Chat *dest = new_chat();
    CHECK(source && dest);
    while (dest->conversation_count < CHAT_MAX_CONVERSATIONS)
        CHECK(chat_new_conversation(dest) >= 0);
    CHECK(add(source, 0, CHAT_ROLE_USER, L"x", 1) != NULL);
    JsonBuf out;
    CHECK(chat_export_json(source, 0, false, 1, NULL, &out));
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_json(dest, out.data, out.length, NULL, NULL) ==
        CHAT_IMPORT_CAPACITY);
    CHECK(unchanged(dest, &before));
    json_buf_free(&out);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    return 0;
}

/* --- Markdown ------------------------------------------------------------ */

static ChatImportStatus import_markdown(Chat *chat, const char *text,
    const wchar_t *fallback, ChatImportStats *stats) {
    return chat_import_markdown(chat, text, strlen(text), fallback, NULL, stats);
}

static int test_markdown_round_trip(void) {
    Chat *source = new_chat();
    Chat *dest = new_chat();
    CHECK(source && dest);
    Snapshot before;
    snapshot(dest, &before);

    ChatConversation *c0 = &source->conversations[0];
    CHECK(add(source, 0, CHAT_ROLE_USER, L"Hello <world> & stuff", 1500) != NULL);
    ChatMessage *a = add(source, 0, CHAT_ROLE_ASSISTANT, L"Hi there", 1600);
    CHECK(a != NULL);
    full_generation(a);
    CHECK(chat_message_set_reasoning(a, L"Thinking\nHard"));
    wcscpy(c0->model, L"openai/gpt-4o-mini");
    wcscpy(c0->ollama_model, L"llama3.2");
    c0->modified_at = 2000;

    int second = chat_new_conversation(source);
    CHECK(second == 1);
    ChatConversation *c1 = &source->conversations[1];
    c1->renamed = true;
    wcscpy(c1->title, L"Emoji chat");
    CHECK(add(source, 1, CHAT_ROLE_USER,
        L"caf\xc3\xa9 \xf0\x9f\x98\x80", 3500) != NULL);
    c1->created_at = 3000;
    c1->modified_at = 4000;
    CHECK(chat_conversation_apply_system_prompt(source, 1, L""));

    JsonBuf md;
    CHECK(chat_export_markdown(source, 0, true, 42, NULL, NULL, &md));
    ChatImportStats stats;
    CHECK(chat_import_markdown(dest, md.data, md.length, NULL, NULL, &stats) ==
        CHAT_IMPORT_OK);
    CHECK(stats.conversations_added == 2 && stats.messages_added == 3);

    CHECK(dest->conversation_count == 3);
    CHECK(dest->active == before.active);
    CHECK(!wcscmp(dest->status, before.status));
    CHECK(dest->conversations[0].id == before.id);
    CHECK(dest->next_id > before.next_id);

    ChatConversation *d0 = &dest->conversations[1];
    CHECK(!wcscmp(d0->title, L"Test chat"));
    CHECK(d0->created_at == 1000 && d0->modified_at == 2000);
    CHECK(!wcscmp(d0->model, L"openai/gpt-4o-mini"));
    CHECK(!wcscmp(d0->ollama_model, L"llama3.2"));
    CHECK(d0->message_count == 2);
    CHECK(!wcscmp(chat_message_text(&d0->messages[0]),
        L"Hello <world> & stuff"));
    CHECK(!wcscmp(chat_message_text(&d0->messages[1]), L"Hi there"));
    CHECK(!wcscmp(chat_message_reasoning(&d0->messages[1]),
        L"Thinking\nHard"));
    const ChatGeneration *g = &d0->messages[1].generation;
    CHECK(g->state == CHAT_GENERATION_COMPLETE);
    CHECK(!g->requested_model[0] && g->cost == -1);

    ChatConversation *d1 = &dest->conversations[2];
    CHECK(!wcscmp(d1->title, L"Emoji chat"));
    CHECK(d1->system_prompt_present && d1->system_prompt.data == NULL);
    CHECK(!wcscmp(chat_message_text(&d1->messages[0]),
        L"caf\xc3\xa9 \xf0\x9f\x98\x80"));

    json_buf_free(&md);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    return 0;
}

/* Only the offset-zero payload is authoritative: a marker later in a
   payload-less file is ordinary body text. */
static int test_markdown_offset_authority(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    const char *md = "# Notes\n\n<!-- darkchat.export:\n{}\n-->\n\nbody\n";
    Snapshot before;
    snapshot(dest, &before);
    ChatImportStats stats;
    CHECK(import_markdown(dest, md, NULL, &stats) == CHAT_IMPORT_OK);
    CHECK(stats.conversations_added == 1 && stats.messages_added == 1);
    ChatConversation *c = &dest->conversations[before.count];
    CHECK(!wcscmp(c->title, L"Notes"));
    CHECK(c->message_count == 1);
    CHECK(c->messages[0].role == CHAT_ROLE_USER);
    CHECK(!wcscmp(chat_message_text(&c->messages[0]),
        L"# Notes\n\n<!-- darkchat.export:\n{}\n-->\n\nbody\n"));
    chat_dispose(dest); free(dest);
    return 0;
}

/* A payload-less file is one verbatim user message; role headings inside it
   are never re-parsed into history. */
static int test_markdown_fallback(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    const char *md =
        "# My Notes\n\n## User\n\nhello\n\n## Assistant\n\nworld\n";
    ChatImportStats stats;
    CHECK(import_markdown(dest, md, NULL, &stats) == CHAT_IMPORT_OK);
    CHECK(stats.conversations_added == 1 && stats.messages_added == 1);
    ChatConversation *c = &dest->conversations[1];
    CHECK(!wcscmp(c->title, L"My Notes"));
    CHECK(c->message_count == 1);
    CHECK(c->messages[0].role == CHAT_ROLE_USER);
    CHECK(!wcscmp(chat_message_text(&c->messages[0]),
        L"# My Notes\n\n## User\n\nhello\n\n## Assistant\n\nworld\n"));
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_title_rules(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    /* No heading: the fallback title is used verbatim. */
    CHECK(import_markdown(dest, "just body\n", L"notes.txt", NULL) ==
        CHAT_IMPORT_OK);
    CHECK(!wcscmp(dest->conversations[1].title, L"notes.txt"));
    /* An empty or NULL fallback title falls through to the default. */
    CHECK(import_markdown(dest, "just body\n", L"", NULL) == CHAT_IMPORT_OK);
    CHECK(!wcscmp(dest->conversations[2].title, L"Imported conversation"));
    CHECK(import_markdown(dest, "just body\n", NULL, NULL) == CHAT_IMPORT_OK);
    CHECK(!wcscmp(dest->conversations[3].title, L"Imported conversation"));
    /* A heading containing only spaces is absent. */
    CHECK(import_markdown(dest, "#   \nbody\n", NULL, NULL) == CHAT_IMPORT_OK);
    CHECK(!wcscmp(dest->conversations[4].title, L"Imported conversation"));
    /* A heading wins over the fallback; trailing CR/space/tab is trimmed. */
    CHECK(import_markdown(dest, "# Real title \t\r\nbody\n", L"ignored", NULL)
        == CHAT_IMPORT_OK);
    CHECK(!wcscmp(dest->conversations[5].title, L"Real title"));
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_copy_title(void) {
    wchar_t out[CHAT_TITLE_TEXT];
    /* The high surrogate lands on the last copied index: it is dropped. */
    wchar_t src1[70];
    size_t n = 0;
    for (int i = 0; i < 62; i++) src1[n++] = L'a';
    src1[n++] = (wchar_t)0xd83d;
    src1[n++] = (wchar_t)0xde00;
    src1[n] = 0;
    chat_import_copy_title(out, CHAT_TITLE_TEXT, src1, n);
    CHECK(wcslen(out) == 62 && out[61] == L'a');

    /* The boundary lands on the low surrogate: the pair survives intact. */
    wchar_t src2[70];
    n = 0;
    for (int i = 0; i < 61; i++) src2[n++] = L'a';
    src2[n++] = (wchar_t)0xd83d;
    src2[n++] = (wchar_t)0xde00;
    src2[n++] = L'b';
    src2[n] = 0;
    chat_import_copy_title(out, CHAT_TITLE_TEXT, src2, n);
    CHECK(wcslen(out) == 63);
    CHECK(out[61] == (wchar_t)0xd83d && out[62] == (wchar_t)0xde00);

    /* Degenerate capacities and a NULL source are safe. */
    chat_import_copy_title(out, 1, src1, n);
    CHECK(out[0] == 0);
    chat_import_copy_title(out, 0, src1, n);
    chat_import_copy_title(out, CHAT_TITLE_TEXT, NULL, 5);
    CHECK(out[0] == 0);

    /* Heading-derived title through the importer, emoji at the boundary. */
    Chat *dest = new_chat();
    CHECK(dest);
    char narrow[512];
    size_t used = 0;
    narrow[used++] = '#';
    narrow[used++] = ' ';
    for (int i = 0; i < 62; i++) narrow[used++] = 'a';
    narrow[used++] = (char)0xf0;
    narrow[used++] = (char)0x9f;
    narrow[used++] = (char)0x98;
    narrow[used++] = (char)0x80;
    narrow[used++] = '\n';
    narrow[used++] = 'b';
    narrow[used] = 0;
    CHECK(chat_import_markdown(dest, narrow, used, NULL, NULL, NULL) ==
        CHAT_IMPORT_OK);
    CHECK(wcslen(dest->conversations[1].title) == 62);
    CHECK(dest->conversations[1].title[61] == L'a');
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_bom(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    /* The BOM is stripped, the heading is found, and the message never starts
       with U+FEFF. */
    const char *body = "\xEF\xBB\xBF# BOM Title\nbody\n";
    ChatImportStats stats;
    CHECK(chat_import_markdown(dest, body, strlen(body), NULL, NULL, &stats) ==
        CHAT_IMPORT_OK);
    ChatConversation *c = &dest->conversations[1];
    CHECK(!wcscmp(c->title, L"BOM Title"));
    CHECK(chat_message_text(&c->messages[0])[0] == L'#');

    /* A BOM-only file is empty. */
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_markdown(dest, "\xEF\xBB\xBF", 3, NULL, NULL, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(unchanged(dest, &before));

    /* A BOM before the authoritative payload still round-trips. */
    Chat *source = new_chat();
    CHECK(source);
    CHECK(add(source, 0, CHAT_ROLE_USER, L"bom round", 5) != NULL);
    JsonBuf md;
    CHECK(chat_export_markdown(source, 0, false, 9, NULL, NULL, &md));
    char *prefixed = (char *)malloc(md.length + 3);
    CHECK(prefixed);
    prefixed[0] = (char)0xEF;
    prefixed[1] = (char)0xBB;
    prefixed[2] = (char)0xBF;
    memcpy(prefixed + 3, md.data, md.length);
    int count = dest->conversation_count;
    CHECK(chat_import_markdown(dest, prefixed, md.length + 3, NULL, NULL, &stats) ==
        CHAT_IMPORT_OK);
    CHECK(dest->conversation_count == count + 1);
    free(prefixed);
    json_buf_free(&md);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_invalid(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    const char *bad[] = {
        "<!-- darkchat.export:\n{ not json }\n-->\n",
        "<!-- darkchat.export:\n{\"format\":\"other\",\"version\":1,"
            "\"conversations\":[]}\n-->\n",
        "<!-- darkchat.export:\n-->\n",
        "<!-- darkchat.export:\n{\"format\":\"darkchat.export\"",
        "<!-- darkchat.export:\n{}\n-->\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(import_markdown(dest, bad[i], NULL, NULL) ==
            CHAT_IMPORT_MALFORMED);
        CHECK(unchanged(dest, &before));
    }
    CHECK(import_markdown(dest, "", NULL, NULL) == CHAT_IMPORT_MALFORMED);
    static const char embedded_nul[] = { '#', 0, 'x' };
    CHECK(chat_import_markdown(dest, embedded_nul, sizeof embedded_nul, NULL, NULL,
        NULL) == CHAT_IMPORT_MALFORMED);
    const char invalid[] = { '#', (char)0xff, 'x' };
    CHECK(chat_import_markdown(dest, invalid, sizeof invalid, NULL, NULL, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(unchanged(dest, &before));
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_capacity(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    while (dest->conversation_count < CHAT_MAX_CONVERSATIONS)
        CHECK(chat_new_conversation(dest) >= 0);
    Snapshot before;
    snapshot(dest, &before);
    ChatImportStats stats;
    CHECK(import_markdown(dest, "# T\nbody", NULL, &stats) ==
        CHAT_IMPORT_CAPACITY);
    CHECK(stats.conversations_added == 0 && stats.messages_added == 0);
    CHECK(unchanged(dest, &before));
    chat_dispose(dest); free(dest);
    return 0;
}

/* The fallback consumes one conversation id and one message id; exhaustion is
   capacity, not allocation failure. */
static int test_markdown_id_exhaustion(void) {
    Chat *dest = new_chat();
    CHECK(dest);
    dest->next_id = CHAT_MAX_ID - 1;
    Snapshot before;
    snapshot(dest, &before);
    ChatImportStats stats;
    CHECK(import_markdown(dest, "# T\nbody", NULL, &stats) ==
        CHAT_IMPORT_CAPACITY);
    CHECK(stats.conversations_added == 0 && stats.messages_added == 0);
    CHECK(unchanged(dest, &before));
    chat_dispose(dest); free(dest);
    return 0;
}

static int test_markdown_allocation_failure(void) {
    long baseline = live_allocs;
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    const char *md = "# Title\n\nsome body text\n";
    bool succeeded = false;
    for (long n = 0; n < 512; n++) {
        long live_before = live_allocs;
        pass_allocs = n;
        fail_allocs = 1;
        ChatImportStats stats;
        ChatImportStatus status = chat_import_markdown(dest, md, strlen(md),
            NULL, NULL, &stats);
        if (status == CHAT_IMPORT_OK) {
            CHECK(n > 0);
            CHECK(!unchanged(dest, &before));
            succeeded = true;
            break;
        }
        CHECK(status == CHAT_IMPORT_OOM);
        CHECK(unchanged(dest, &before));
        CHECK(live_allocs == live_before);
    }
    CHECK(succeeded);
    seam_reset();
    chat_dispose(dest); free(dest);
    CHECK(live_allocs == baseline);
    return 0;
}

/* --- schema version 2: ordered content parts ------------------------------ */

static int test_version_2_round_trip(void) {
    char *doc = image_doc("2", "look", NULL, NULL);
    CHECK(doc);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    ChatImportStats stats;
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, &stats) ==
        CHAT_IMPORT_OK);
    CHECK(stats.conversations_added == 1);
    CHECK(stats.messages_added == 1);
    CHECK(stats.attachments_added == 1);
    CHECK(dest->conversation_count == 2);   /* the fixture plus the import */
    if (check_image_message(dest, 1, 7, 9, L"p.png")) return 1;
    /* Identity is rewritten: the fresh id is not the file's, and it came from
       the destination's own counter. */
    CHECK(dest->conversations[1].id != 5);
    CHECK(dest->conversations[1].id <= dest->next_id);
    CHECK(dest->next_id > before.next_id);
    /* Exactly one blob was made durable, and it is the right one. */
    CHECK(sink.count == 1);
    CHECK(sink.lengths[0] == sizeof TINY);
    CHECK(!memcmp(sink.staged[0], TINY, sizeof TINY));
    /* The title is still authoritative and the turn keeps its timestamps. */
    CHECK(!wcscmp(dest->conversations[1].title, L"T"));
    CHECK(dest->conversations[1].messages[0].created_at == 9);
    free(doc);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* A version 1 document has no parts by construction, so carrying one is a
   document that contradicts its own declared grammar. */
static int test_version_gate(void) {
    char *doc = image_doc("1", "look", NULL, NULL);
    CHECK(doc);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(dest->conversation_count == 1);
    CHECK(sink.count == 0);
    free(doc);
    chat_dispose(dest);
    free(dest);

    /* A later version is refused rather than partially understood. */
    static const char *later[] = {
        "{\"format\":\"darkchat.export\",\"version\":3,\"conversations\":["
            "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
        "{\"format\":\"darkchat.export\",\"version\":0,\"conversations\":["
            "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[]}]}",
    };
    for (size_t i = 0; i < sizeof later / sizeof later[0]; i++) {
        Chat *d = new_chat();
        CHECK(d);
        CHECK(chat_import_json(d, later[i], strlen(later[i]), NULL, NULL) ==
            CHAT_IMPORT_MALFORMED);
        CHECK(d->conversation_count == 1);
        chat_dispose(d);
        free(d);
    }
    /* Version 1 without content parts is untouched by all of this. */
    {
        Chat *d = new_chat();
        CHECK(d);
        char *plain = minimal_doc(1, 1);
        CHECK(plain);
        CHECK(chat_import_json(d, plain, strlen(plain), NULL, NULL) ==
            CHAT_IMPORT_OK);
        free(plain);
        chat_dispose(d);
        free(d);
    }
    return 0;
}

/* `content` present means authoritative. Every way a document can contradict
   itself is rejected, and none of them reaches the store. */
static int test_content_strictness(void) {
    /* Every way a document can contradict itself. None of them may reach the
       store, and none may disturb the destination. */
    struct { const char *fields; ChatImportStatus expected; const char *why; }
    bad[] = {
        /* FIRST_FRAME is a real flag on an image; a bit outside the mask is
           not, and is corruption rather than something to ignore. */
        { ",\"flags\":2", CHAT_IMPORT_MALFORMED,
          "an image part with an undefined flag bit" },
        { ",\"mime\":\"\"", CHAT_IMPORT_MALFORMED, "an empty mime" },
        { ",\"mime\":\"image/\\\"png\"", CHAT_IMPORT_MALFORMED,
          "a mime containing a quote" },
        { ",\"pixel_width\":-1", CHAT_IMPORT_MALFORMED,
          "a negative pixel width" },
        /* A bound that is exceeded is reported as a bound, not as corruption:
           the document is well formed, its field is simply too long. */
        { ",\"display_name\":\"" "0123456789012345678901234567890123456789"
          "0123456789012345678901234567890123456789\"",
          CHAT_IMPORT_TOO_LARGE, "an over-long display name" },
    };
    BlobSink sink;
    ChatImportBlobs blobs;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *doc = image_doc("2", "look", bad[i].fields, NULL);
        CHECK(doc);
        sink_init(&sink);
        sink_blobs(&sink, &blobs);
        Chat *dest = new_chat();
        CHECK(dest);
        Snapshot before;
        snapshot(dest, &before);
        ChatImportStatus got = chat_import_json(dest, doc, strlen(doc),
            &blobs, NULL);
        if (got != bad[i].expected) {
            printf("case %zu got %d want %d (%s)\n", i, got, bad[i].expected,
                bad[i].why);
            return 1;
        }
        CHECK(unchanged(dest, &before));
        CHECK(sink.count == 0);
        free(doc);
        chat_dispose(dest);
        free(dest);
    }
    /* A `content` that is not an array, and a duplicated `content`. */
    {
        static const char *shapes[] = {
            "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
                "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
                "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
                "\"modified_at\":1,\"text\":\"x\",\"generation\":{},"
                "\"content\":\"nope\"}]}]}",
            "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
                "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
                "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
                "\"modified_at\":1,\"text\":\"x\",\"generation\":{},"
                "\"content\":[],\"content\":[]}]}]}",
            /* An unknown part field is tolerated, so a later version can add
               one without breaking this reader. */
            "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
                "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
                "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
                "\"modified_at\":1,\"text\":\"x\",\"generation\":{},"
                "\"content\":[{\"type\":\"image\",\"future\":1}]}]}]}",
        };
        for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
            sink_init(&sink);
            sink_blobs(&sink, &blobs);
            Chat *dest = new_chat();
            CHECK(dest);
            CHECK(chat_import_json(dest, shapes[i], strlen(shapes[i]), &blobs,
                NULL) == CHAT_IMPORT_MALFORMED);
            CHECK(sink.count == 0);
            chat_dispose(dest);
            free(dest);
        }
    }
    /* The digest is verified before anything is written, so a document whose
       bytes do not match what it declares never reaches the store. */
    {
        char *doc = image_doc("2", "look", NULL, NULL);
        CHECK(doc);
        char *lie = strstr(doc, "\"sha256\":\"");
        CHECK(lie);
        memcpy(lie + 10,
            "0000000000000000000000000000000000000000000000000000000000000000",
            64);
        Chat *dest = new_chat();
        CHECK(dest);
        sink_init(&sink);
        sink_blobs(&sink, &blobs);
        Snapshot before;
        snapshot(dest, &before);
        CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
            CHAT_IMPORT_MALFORMED);
        CHECK(sink.count == 0);
        CHECK(unchanged(dest, &before));
        free(doc);
        chat_dispose(dest);
        free(dest);
    }
    return 0;
}

/* `text` is the projection of the parts, so a document where the two disagree
   describes two different messages. */
static int test_content_projection_mismatch(void) {
    char *doc = image_doc("2", "look", NULL, NULL);
    CHECK(doc);
    char *at = strstr(doc, "\"text\":\"look\",\"generation\"");
    CHECK(at);
    memcpy(at, "\"text\":\"other\",\"generation\"", 28);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(unchanged(dest, &before));
    CHECK(sink.count == 0);
    free(doc);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* An image-only turn is legal: the projection is empty and the parts carry the
   message. */
static int test_content_image_only(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    JsonBuf data;
    json_buf_init(&data, 16);
    json_buf_append_raw(&data, "\"", 1);
    json_buf_append_base64(&data, TINY, sizeof TINY);
    json_buf_append_raw(&data, "\"", 1);
    data.data[data.length] = 0;
    char doc[2048];
    snprintf(doc, sizeof doc,
        "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
        "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
        "\"messages\":[{\"role\":\"user\",\"created_at\":1,\"modified_at\":1,"
        "\"text\":\"\",\"generation\":{},\"content\":["
        "{\"type\":\"image\",\"mime\":\"image/png\",\"sha256\":\"%s\","
        "\"data\":%s}]}]}]}", digest, data.data);
    json_buf_free(&data);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_OK);
    const ChatMessage *m = &dest->conversations[1].messages[0];
    /* No leading text part, because the projection is empty. */
    CHECK(chat_message_part_count(m) == 1);
    ChatPartView view;
    CHECK(chat_message_part_at(m, 0, &view));
    CHECK(view.kind == CHAT_PART_IMAGE);
    CHECK(!chat_message_text(m)[0]);
    CHECK(chat_message_has_images(m));
    CHECK(sink.count == 1);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* A Markdown package names its sidecars; the reference is validated before any
   filesystem call, and the bytes are re-hashed against the digest the document
   itself declares. */
static int test_markdown_sidecar(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    char doc[2048];
    snprintf(doc, sizeof doc,
        "<!-- darkchat.export:\n"
        "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
        "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
        "\"messages\":[{\"role\":\"user\",\"created_at\":1,\"modified_at\":1,"
        "\"text\":\"look\",\"generation\":{},\"content\":["
        "{\"type\":\"text\",\"text\":\"look\"},"
        "{\"type\":\"image\",\"mime\":\"image/png\",\"sha256\":\"%s\","
        "\"file\":\"trip 1.attachments/%s.png\"}]}]}]}\n"
        "-->\n# T\n\n## User\n\nlook\n\n![p.png](trip%%201.attachments/%s.png)\n",
        digest, digest, digest);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    CHECK(chat_import_markdown(dest, doc, strlen(doc), NULL, &blobs, NULL) ==
        CHAT_IMPORT_OK);
    /* The reference was split, not handed over whole: both components are
       bounded tokens, which is what makes the host's join safe. */
    CHECK(sink.read_count == 1);
    CHECK(!strcmp(sink.read_folder, "trip 1.attachments"));
    CHECK(sink.read_basename[0] != 0);
    CHECK(!strncmp(sink.read_basename, digest, 64));
    CHECK(!strcmp(sink.read_basename + 64, ".png"));
    if (check_image_message(dest, 1, 0, 0, L"")) return 1;   /* the doc omits both */
    CHECK(sink.count == 1);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* A sidecar that is not there is a distinct outcome from a malformed
   document: the file was valid, the bytes were not available. */
static int test_sidecar_missing_is_attachment_io(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    char doc[2048];
    snprintf(doc, sizeof doc,
        "<!-- darkchat.export:\n"
        "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
        "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
        "\"messages\":[{\"role\":\"user\",\"created_at\":1,\"modified_at\":1,"
        "\"text\":\"look\",\"generation\":{},\"content\":["
        "{\"type\":\"text\",\"text\":\"look\"},"
        "{\"type\":\"image\",\"mime\":\"image/png\",\"sha256\":\"%s\","
        "\"file\":\"trip.attachments/%s.png\"}]}]}]}\n"
        "-->\n# T\n", digest, digest);
    BlobSink sink;
    sink_init(&sink);
    sink.read_missing = true;
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_markdown(dest, doc, strlen(doc), NULL, &blobs, NULL) ==
        CHAT_IMPORT_ATTACHMENT_IO);
    CHECK(unchanged(dest, &before));
    /* Nothing was staged: the bytes never arrived, so nothing was written. */
    CHECK(sink.count == 0);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* A reference whose folder is not shaped like a package folder is malformed
   before any read is attempted, so a crafted document cannot steer a read. */
static int test_sidecar_reference_rejected(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    const char *folders[] = {
        "photos", "..", ".", "a.attachments/../b.attachments",
        "a\\b.attachments", "C:evil.attachments",
    };
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) {
        char doc[2048];
        snprintf(doc, sizeof doc,
            "<!-- darkchat.export:\n"
            "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
            "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
            "\"messages\":[{\"role\":\"user\",\"created_at\":1,"
            "\"modified_at\":1,\"text\":\"look\",\"generation\":{},"
            "\"content\":[{\"type\":\"text\",\"text\":\"look\"},"
            "{\"type\":\"image\",\"mime\":\"image/png\",\"sha256\":\"%s\","
            "\"file\":\"%s/%s.png\"}]}]}]}\n"
            "-->\n# T\n", digest, folders[i], digest);
        BlobSink sink;
        sink_init(&sink);
        ChatImportBlobs blobs;
        sink_blobs(&sink, &blobs);
        Chat *dest = new_chat();
        CHECK(dest);
        Snapshot before;
        snapshot(dest, &before);
        CHECK(chat_import_markdown(dest, doc, strlen(doc), NULL, &blobs, NULL)
            == CHAT_IMPORT_MALFORMED);
        CHECK(unchanged(dest, &before));
        CHECK(sink.read_count == 0);   /* never attempted */
        CHECK(sink.count == 0);
        chat_dispose(dest);
        free(dest);
    }
    return 0;
}

/* JSON never resolves a sidecar: our JSON exports embed the bytes, so a `file`
   reference in a JSON document is a format error, not a lookup. */
static int test_file_rejected_in_json(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    char doc[2048];
    snprintf(doc, sizeof doc,
        "{\"format\":\"darkchat.export\",\"version\":2,\"conversations\":["
        "{\"id\":1,\"title\":\"T\",\"created_at\":1,\"modified_at\":1,"
        "\"messages\":[{\"role\":\"user\",\"created_at\":1,\"modified_at\":1,"
        "\"text\":\"look\",\"generation\":{},\"content\":["
        "{\"type\":\"text\",\"text\":\"look\"},"
        "{\"type\":\"image\",\"mime\":\"image/png\",\"sha256\":\"%s\","
        "\"file\":\"trip.attachments/%s.png\"}]}]}]}", digest, digest);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_MALFORMED);
    CHECK(sink.read_count == 0);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* When the durable write fails partway, the destination is untouched and the
   caller is left holding the digests it must reclaim. */
static int test_stage_failure_is_transactional(void) {
    char *doc = image_doc("2", "look", NULL, NULL);
    CHECK(doc);
    BlobSink sink;
    sink_init(&sink);
    sink.fail_at = 0;
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_ATTACHMENT_IO);
    CHECK(unchanged(dest, &before));
    CHECK(dest->attachment_count == 0);
    chat_dispose(dest);
    free(dest);
    free(doc);
    return 0;
}

/* An existing record survives an import that adds another, and the imported
   one gets a fresh id rather than colliding. */
static int test_attachment_table_merge(void) {
    ChatAttachmentMeta seed;
    Chat *dest = new_chat();
    CHECK(dest);
    memset(&seed, 0, sizeof seed);
    seed.id = ++dest->next_id;
    sha256_hex("seed", 4, seed.digest);
    strcpy(seed.mime, "image/png");
    seed.bytes = 4;
    wcscpy(seed.display_name, L"seed.png");
    CHECK(chat_attachment_add(dest, &seed));

    char *doc = image_doc("2", "look", NULL, NULL);
    CHECK(doc);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_OK);
    CHECK(dest->attachment_count == 2);
    /* The pre-existing record is intact and still resolvable. */
    const ChatAttachmentMeta *found =
        chat_attachment(dest, seed.id);
    CHECK(found && !strcmp(found->digest, seed.digest));
    if (check_image_message(dest, 1, 7, 9, L"p.png")) return 1;
    free(doc);
    chat_dispose(dest);
    free(dest);
    return 0;
}

/* The FIRST_FRAME flag round-trips, because it is what tells the transcript a
   still image stands for an animation, and it is persisted in the snapshot. */
static int test_content_flags_round_trip(void) {
    char *doc = image_doc("2", "look", ",\"flags\":1", NULL);
    CHECK(doc);
    BlobSink sink;
    sink_init(&sink);
    ChatImportBlobs blobs;
    sink_blobs(&sink, &blobs);
    Chat *dest = new_chat();
    CHECK(dest);
    CHECK(chat_import_json(dest, doc, strlen(doc), &blobs, NULL) ==
        CHAT_IMPORT_OK);
    const ChatMessage *m = &dest->conversations[1].messages[0];
    ChatPartView view;
    CHECK(chat_message_part_at(m, 1, &view));
    CHECK(view.kind == CHAT_PART_IMAGE);
    CHECK(view.flags == CHAT_PART_FLAG_FIRST_FRAME);
    free(doc);
    chat_dispose(dest);
    free(dest);
    return 0;
}

typedef struct { const char *name; int (*run)(void); } ImportTest;

static const ImportTest import_tests[] = {
    { "round_trip", test_round_trip },
    { "title_authority", test_title_authority },
    { "malformed", test_malformed },
    { "capacity", test_capacity },
    { "id_exhaustion", test_id_exhaustion },
    { "reject_when_full", test_reject_when_full },
    { "allocation_failure", test_allocation_failure },
    { "markdown_round_trip", test_markdown_round_trip },
    { "markdown_offset_authority", test_markdown_offset_authority },
    { "markdown_fallback", test_markdown_fallback },
    { "markdown_title_rules", test_markdown_title_rules },
    { "markdown_copy_title", test_markdown_copy_title },
    { "markdown_bom", test_markdown_bom },
    { "markdown_invalid", test_markdown_invalid },
    { "markdown_capacity", test_markdown_capacity },
    { "markdown_id_exhaustion", test_markdown_id_exhaustion },
    { "markdown_allocation_failure", test_markdown_allocation_failure },
    { "version_2_round_trip", test_version_2_round_trip },
    { "version_gate", test_version_gate },
    { "content_strictness", test_content_strictness },
    { "content_flags_round_trip", test_content_flags_round_trip },
    { "content_projection_mismatch", test_content_projection_mismatch },
    { "content_image_only", test_content_image_only },
    { "markdown_sidecar", test_markdown_sidecar },
    { "sidecar_missing_is_attachment_io", test_sidecar_missing_is_attachment_io },
    { "sidecar_reference_rejected", test_sidecar_reference_rejected },
    { "file_rejected_in_json", test_file_rejected_in_json },
    { "stage_failure_is_transactional", test_stage_failure_is_transactional },
    { "attachment_table_merge", test_attachment_table_merge },
};

int main(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof import_tests / sizeof import_tests[0]; i++) {
        long before = live_allocs;
        failures += import_tests[i].run();
        if (live_allocs != before) {
            printf("LEAK in %s: %ld allocation(s)\n", import_tests[i].name,
                live_allocs - before);
            ++failures;
        }
    }
    if (failures) { printf("%d import test(s) failed\n", failures); return 1; }
    printf("test_import passed\n");
    return 0;
}
