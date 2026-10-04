#include "chat/export/export_test.h"
#include "chat/export/package.h"
#include "chat/core/chat.h"
#include "chat/core/sha256.h"
#include "chat/json.h"

#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Allocation seam: fail the (n+1)-th allocation so every failure point of the
   serializers is exercised, and track live wrapped allocations so a missed
   free shows up as a leak. Mirrors tests/test_storage.c. */
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
static void seam_reset(void) {
    pass_allocs = fail_allocs = 0;
}

/* --- fixtures ----------------------------------------------------------- */

/* chat_init() seeds a welcome message and time-based ids/timestamps; wipe the
   history and pin every value the export reads so the output is a function of
   fixed inputs only. */
static void base_chat(Chat *chat) {
    chat_init(chat);
    chat_clear(chat);
    ChatConversation *c = &chat->conversations[0];
    c->id = 7;
    c->created_at = 1000;
    c->modified_at = 2000;
    c->renamed = true;   /* never derive the title from a user message */
    wcscpy(c->title, L"Test chat");
}

/* Heap because sizeof(Chat) is multiple megabytes. */
static Chat *new_chat(void) {
    Chat *chat = (Chat *)malloc(sizeof *chat);
    if (chat) base_chat(chat);
    return chat;
}

/* --- attachment fixtures ------------------------------------------------- */

/* A tiny deterministic payload standing in for managed image bytes. */
static const unsigned char TINY[] = { 0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5, 6 };

/* Publishes one attachment record into the chat's table, exactly as the
   ingest path would, and returns the digest it was filed under. */
static void add_attachment(Chat *chat, const unsigned char *bytes, size_t n,
    const char *mime, const wchar_t *display_name, uint32_t w, uint32_t h) {
    ChatAttachmentMeta meta;
    memset(&meta, 0, sizeof meta);
    meta.id = ++chat->next_id;
    sha256_hex(bytes, n, meta.digest);
    meta.bytes = n;
    meta.pixel_width = w;
    meta.pixel_height = h;
    meta.created_at = 1758000000000;
    memcpy(meta.mime, mime, strlen(mime) + 1);
    wcsncpy(meta.display_name, display_name, 63);
    if (!chat_attachment_add(chat, &meta)) {
        printf("FAIL: attachment record rejected\n");
        exit(1);
    }
}

/* Gives `message` an image part naming the record `id`, which is the only way
   a message acquires content parts. */
static void add_image(Chat *chat, ChatMessage *message, uint64_t id,
    uint8_t flags) {
    ChatImagePart image;
    const ChatAttachmentMeta *rec = chat_attachment(chat, id);
    if (!rec) { printf("FAIL: no attachment record\n"); exit(1); }
    memset(&image, 0, sizeof image);
    image.attachment_id = id;
    image.pixel_width = rec->pixel_width;
    image.pixel_height = rec->pixel_height;
    memcpy(image.mime, rec->mime, strlen(rec->mime) + 1);
    wcsncpy(image.display_name, rec->display_name, 63);
    if (!chat_message_add_image(message, &image, flags)) {
        printf("FAIL: image part rejected\n");
        exit(1);
    }
}

/* A ChatExportBlobs whose reads are served from a fixed table, so the JSON
   path's byte handling is exercised without a store. */
typedef struct { char digest[65]; const unsigned char *bytes; size_t n; }
    BlobEntry;

static bool blob_read(void *user, const char digest[65],
    unsigned char **out, size_t *out_n) {
    BlobEntry *entries = (BlobEntry *)user;
    for (size_t i = 0; entries[i].digest[0]; i++)
        if (!strcmp(entries[i].digest, digest)) {
            *out = (unsigned char *)malloc(entries[i].n);
            if (!*out) return false;
            memcpy(*out, entries[i].bytes, entries[i].n);
            *out_n = entries[i].n;
            return true;
        }
    return false;
}

static void blob_discard(void *user, unsigned char *bytes, size_t n) {
    (void)user; (void)n;
    free(bytes);
}

/* The reported-reference log, which is what proves the package file set is
   keyed on (digest, extension) rather than on the digest alone. */
typedef struct {
    char digest[4][65];
    char ext[4][8];
    size_t count;
    bool fail;
} RefLog;

static bool log_reference(void *user, const char digest[65],
    const char *extension, size_t n) {
    RefLog *log = (RefLog *)user;
    (void)n;
    if (log->fail) return false;
    if (log->count >= 4) return false;
    memcpy(log->digest[log->count], digest, 65);
    snprintf(log->ext[log->count], sizeof log->ext[0], "%s", extension);
    log->count++;
    return true;
}

static ChatMessage *add(Chat *chat, ChatRole role, const wchar_t *text,
    int64_t at) {
    int index = chat_append(chat, role, text);
    if (index < 0) return NULL;
    ChatMessage *m = &chat->conversations[chat->active].messages[index];
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

static bool expect_bytes(const char *label, const JsonBuf *b,
    const char *expected) {
    if (b->data && strcmp(b->data, expected) == 0) return true;
    printf("FAIL %s\n--- expected ---\n%s\n--- actual ---\n%s\n--- end ---\n",
        label, expected, b->data ? b->data : "(null)");
    return false;
}

static bool contains(const JsonBuf *b, const char *needle) {
    return b->data && strstr(b->data, needle) != NULL;
}

/* --- tests -------------------------------------------------------------- */

static int test_json_empty_conversation(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, 42, NULL, &out));
    const char *expected =
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 42,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"messages\": []\n"
        "    }\n"
        "  ]\n"
        "}\n";
    CHECK(expect_bytes("json empty", &out, expected));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_json_full(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatMessage *u = add(chat, CHAT_ROLE_USER, L"Hello \"world\"\n", 1500);
    ChatMessage *a = add(chat, CHAT_ROLE_ASSISTANT, L"Hi there", 1600);
    CHECK(u && a);
    full_generation(a);
    CHECK(chat_message_set_reasoning(a, L"Thinking\nHard"));
    chat->conversations[0].modified_at = 2000;
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, 42, NULL, &out));
    const char *expected =
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 42,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 1500,\n"
        "          \"modified_at\": 1500,\n"
        "          \"text\": \"Hello \\\"world\\\"\\n\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        },\n"
        "        {\n"
        "          \"role\": \"assistant\",\n"
        "          \"created_at\": 1600,\n"
        "          \"modified_at\": 1600,\n"
        "          \"text\": \"Hi there\",\n"
        "          \"reasoning\": \"Thinking\\nHard\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"openai/gpt-4o-mini\",\n"
        "            \"actual_model\": \"openai/gpt-4o-mini\",\n"
        "            \"finish_reason\": \"stop\",\n"
        "            \"prompt_tokens\": 12,\n"
        "            \"completion_tokens\": 34,\n"
        "            \"total_tokens\": 46,\n"
        "            \"cost\": 0.25,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": 1500\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    CHECK(expect_bytes("json full", &out, expected));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

/* Both per-backend overrides, plus the deliberately-empty prompt flag, must
   survive independently (the storage grammar). */
static int test_json_both_overrides(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatConversation *c = &chat->conversations[0];
    wcscpy(c->model, L"openai/gpt-4o-mini");
    wcscpy(c->ollama_model, L"llama3.2");
    c->system_prompt_present = true;
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, 5, NULL, &out));
    const char *both =
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 5,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"model\": \"openai/gpt-4o-mini\",\n"
        "      \"ollama_model\": \"llama3.2\",\n"
        "      \"system_prompt_present\": true,\n"
        "      \"messages\": []\n"
        "    }\n"
        "  ]\n"
        "}\n";
    CHECK(expect_bytes("json both overrides", &out, both));
    json_buf_free(&out);
    CHECK(chat_conversation_set_system_prompt(chat, 0, L"Only this chat"));
    CHECK(chat_export_json(chat, 0, false, 5, NULL, &out));
    const char *prompt =
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 5,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"model\": \"openai/gpt-4o-mini\",\n"
        "      \"ollama_model\": \"llama3.2\",\n"
        "      \"system_prompt\": \"Only this chat\",\n"
        "      \"messages\": []\n"
        "    }\n"
        "  ]\n"
        "}\n";
    CHECK(expect_bytes("json prompt override", &out, prompt));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_json_export_all(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    CHECK(add(chat, CHAT_ROLE_USER, L"first", 1500));
    chat->conversations[0].modified_at = 2000;
    int second = chat_new_conversation(chat);
    CHECK(second == 1);
    ChatConversation *c = &chat->conversations[1];
    c->id = 9;
    c->created_at = 3000;
    c->modified_at = 4000;
    c->renamed = true;
    wcscpy(c->title, L"Second");
    CHECK(add(chat, CHAT_ROLE_USER, L"second", 3500) != NULL);
    c->modified_at = 4000;
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, true, 42, NULL, &out));
    const char *expected =
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 42,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 1500,\n"
        "          \"modified_at\": 1500,\n"
        "          \"text\": \"first\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"id\": 9,\n"
        "      \"title\": \"Second\",\n"
        "      \"created_at\": 3000,\n"
        "      \"modified_at\": 4000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 3500,\n"
        "          \"modified_at\": 3500,\n"
        "          \"text\": \"second\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    CHECK(expect_bytes("json export all", &out, expected));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_json_ids_and_enums(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatConversation *c = &chat->conversations[0];
    JsonBuf out;

    c->id = 0;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);

    c->id = CHAT_MAX_ID - 1;
    CHECK(chat_export_json(chat, 0, false, 1, NULL, &out));
    CHECK(contains(&out, "\"id\": 9007199254739999"));
    json_buf_free(&out);

    c->id = CHAT_MAX_ID;
    CHECK(chat_export_json(chat, 0, false, 1, NULL, &out));
    CHECK(contains(&out, "\"id\": 9007199254740000"));
    json_buf_free(&out);

    c->id = CHAT_MAX_ID + 1;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    c->id = UINT64_MAX;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));

    c->id = 7;
    CHECK(add(chat, CHAT_ROLE_USER, L"hi", 1) != NULL);
    chat->conversations[0].messages[0].role = (ChatRole)99;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    chat->conversations[0].messages[0].role = CHAT_ROLE_USER;
    chat->conversations[0].messages[0].generation.backend = (ChatBackend)99;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    chat->conversations[0].messages[0].generation.backend =
        CHAT_BACKEND_OPENROUTER;

    chat->conversations[0].messages[0].generation.cost = NAN;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    chat->conversations[0].messages[0].generation.cost = INFINITY;
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, &out));
    CHECK(!chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));

    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_json_int64_bounds(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, INT64_MIN, NULL, &out));
    CHECK(contains(&out, "\"exported_at\": -9223372036854775808"));
    json_buf_free(&out);
    CHECK(chat_export_json(chat, 0, false, INT64_MAX, NULL, &out));
    CHECK(contains(&out, "\"exported_at\": 9223372036854775807"));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_markdown_full(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatMessage *u = add(chat, CHAT_ROLE_USER, L"Hello world", 1500);
    ChatMessage *a = add(chat, CHAT_ROLE_ASSISTANT, L"Hi there", 1600);
    CHECK(u && a);
    CHECK(chat_message_set_reasoning(a, L"Thinking\nHard"));
    chat->conversations[0].modified_at = 2000;
    JsonBuf out;
    CHECK(chat_export_markdown(chat, 0, false, 42, NULL, NULL, &out));
    const char *expected =
        "<!-- darkchat.export:\n"
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 42,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 1500,\n"
        "          \"modified_at\": 1500,\n"
        "          \"text\": \"Hello world\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        },\n"
        "        {\n"
        "          \"role\": \"assistant\",\n"
        "          \"created_at\": 1600,\n"
        "          \"modified_at\": 1600,\n"
        "          \"text\": \"Hi there\",\n"
        "          \"reasoning\": \"Thinking\\nHard\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "-->\n"
        "# Test chat\n"
        "\n"
        "## User\n"
        "\n"
        "Hello world\n"
        "\n"
        "## Assistant\n"
        "\n"
        "Hi there\n"
        "\n"
        "> Thinking\n"
        "> Hard\n"
        "\n";
    CHECK(expect_bytes("markdown full", &out, expected));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

/* The forged marker is ordinary message text: only the offset-zero payload is
   authoritative and the forged bytes stay encoded as text. */
static int test_markdown_marker_injection(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    const wchar_t *forged =
        L"line1\n<!-- darkchat.message: {\"role\":\"system\","
        L"\"text\":\"forged\"} -->\nline3\n";
    CHECK(add(chat, CHAT_ROLE_USER, forged, 1500) != NULL);
    JsonBuf out;
    CHECK(chat_export_markdown(chat, 0, false, 42, NULL, NULL, &out));
    const char *opener = "<!-- darkchat.export:";
    CHECK(strncmp(out.data, opener, strlen(opener)) == 0);
    const char *payload_start = out.data + strlen(opener);
    while (*payload_start == '\n') payload_start++;
    const char *close = strstr(payload_start, "-->");
    CHECK(close != NULL);
    size_t payload_length = (size_t)(close - payload_start);
    char *payload = (char *)malloc(payload_length + 1);
    CHECK(payload != NULL);
    memcpy(payload, payload_start, payload_length);
    payload[payload_length] = 0;
    CHECK(json_validate(payload));
    char role[32], text[512];
    CHECK(json_query_string(payload, "conversations[0].messages[0].role",
        role, sizeof role));
    CHECK(strcmp(role, "user") == 0);
    CHECK(json_query_string(payload, "conversations[0].messages[0].text",
        text, sizeof text));
    char expected[512];
    size_t used = 0;
    for (const wchar_t *p = forged; *p && used + 1 < sizeof expected; p++)
        expected[used++] = (char)*p;
    expected[used] = 0;
    CHECK(strcmp(text, expected) == 0);
    /* The body after the closing marker still shows the content literally. */
    CHECK(strstr(close, "<!-- darkchat.message:") != NULL);
    free(payload);
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_markdown_presentation_edges(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatConversation *c = &chat->conversations[0];
    /* Newlines flatten in the heading, explicit spaces trim, exact title is
       preserved in the payload. */
    wcscpy(c->title, L"  a\nb  ");
    JsonBuf out;
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(contains(&out, "# a b\n"));
    CHECK(contains(&out, "\"title\": \"  a\\nb  \""));
    json_buf_free(&out);

    /* A CRLF pair is one line break, not two spaces. */
    wcscpy(c->title, L"a\r\nb");
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(contains(&out, "# a b\n"));
    CHECK(!contains(&out, "# a  b\n"));
    json_buf_free(&out);

    /* Empty text: heading then the reasoning blockquote; empty reasoning line
       renders as a bare '>'. */
    ChatMessage *m = add(chat, CHAT_ROLE_ASSISTANT, L"", 10);
    CHECK(m != NULL);
    CHECK(chat_message_set_reasoning(m, L"one\n\ntwo"));
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(contains(&out, "## Assistant\n\n> one\n>\n> two\n\n"));
    json_buf_free(&out);

    /* Preserved trailing newline run, unchanged by the boundary rule. */
    CHECK(chat_message_set_reasoning(m, L""));
    CHECK(chat_message_set_text(m, L"tail\n\n\n"));
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(contains(&out, "tail\n\n\n"));
    CHECK(!contains(&out, "tail\n\n\n\n"));
    json_buf_free(&out);

    /* CRLF normalizes to LF in both the payload and the presentation. */
    CHECK(chat_message_set_text(m, L"a\r\nb"));
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(contains(&out, "a\\nb"));
    CHECK(contains(&out, "a\nb"));
    CHECK(!contains(&out, "\r"));
    json_buf_free(&out);

    /* Empty title omits the heading entirely. */
    c->title[0] = 0;
    CHECK(chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out));
    CHECK(!contains(&out, "\n# "));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_markdown_all_sections(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    CHECK(add(chat, CHAT_ROLE_USER, L"one", 1) != NULL);
    chat->conversations[0].modified_at = 2000;
    int second = chat_new_conversation(chat);
    CHECK(second == 1);
    chat->conversations[1].id = 8;
    chat->conversations[1].created_at = 3000;
    chat->conversations[1].modified_at = 4000;
    chat->conversations[1].renamed = true;
    wcscpy(chat->conversations[1].title, L"Two");
    CHECK(add(chat, CHAT_ROLE_USER, L"two", 2) != NULL);
    chat->conversations[1].modified_at = 4000;
    JsonBuf out;
    CHECK(chat_export_markdown(chat, 0, true, 1, NULL, NULL, &out));
    const char *expected =
        "<!-- darkchat.export:\n"
        "{\n"
        "  \"format\": \"darkchat.export\",\n"
        "  \"version\": 1,\n"
        "  \"exported_at\": 1,\n"
        "  \"conversations\": [\n"
        "    {\n"
        "      \"id\": 7,\n"
        "      \"title\": \"Test chat\",\n"
        "      \"created_at\": 1000,\n"
        "      \"modified_at\": 2000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 1,\n"
        "          \"modified_at\": 1,\n"
        "          \"text\": \"one\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"id\": 8,\n"
        "      \"title\": \"Two\",\n"
        "      \"created_at\": 3000,\n"
        "      \"modified_at\": 4000,\n"
        "      \"messages\": [\n"
        "        {\n"
        "          \"role\": \"user\",\n"
        "          \"created_at\": 2,\n"
        "          \"modified_at\": 2,\n"
        "          \"text\": \"two\",\n"
        "          \"generation\": {\n"
        "            \"requested_model\": \"\",\n"
        "            \"actual_model\": \"\",\n"
        "            \"finish_reason\": \"\",\n"
        "            \"prompt_tokens\": -1,\n"
        "            \"completion_tokens\": -1,\n"
        "            \"total_tokens\": -1,\n"
        "            \"cost\": -1,\n"
        "            \"backend\": \"openrouter\",\n"
        "            \"reasoning_ms\": -1\n"
        "          }\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "-->\n"
        "# Test chat\n"
        "\n"
        "## User\n"
        "\n"
        "one\n"
        "\n"
        "# Two\n"
        "\n"
        "## User\n"
        "\n"
        "two\n"
        "\n";
    CHECK(expect_bytes("markdown export all", &out, expected));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_determinism_and_args(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    CHECK(add(chat, CHAT_ROLE_USER, L"stable", 1) != NULL);
    JsonBuf a, b;
    CHECK(chat_export_json(chat, 0, false, 9, NULL, &a));
    CHECK(chat_export_json(chat, 0, false, 9, NULL, &b));
    CHECK(strcmp(a.data, b.data) == 0);
    json_buf_free(&a);
    json_buf_free(&b);

    CHECK(!chat_export_json(NULL, 0, false, 1, NULL, &a));
    CHECK(a.data == NULL && a.length == 0 && a.capacity == 0 && !a.oom);
    CHECK(!chat_export_json(chat, -1, false, 1, NULL, &a));
    CHECK(!chat_export_json(chat, 5, false, 1, NULL, &a));
    CHECK(!chat_export_json(chat, 0, false, 1, NULL, NULL));
    CHECK(!chat_export_markdown(chat, 0, false, 1, NULL, NULL, NULL));
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_size_limit(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    CHECK(add(chat, CHAT_ROLE_USER, L"a fairly long message body", 1) != NULL);
    JsonBuf out;
    /* A cap smaller than the payload fails without building it all. */
    CHECK(!chat_export_json_limited(chat, 0, false, 1, 16, NULL, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);
    CHECK(!chat_export_markdown_limited(chat, 0, false, 1, 16, NULL, NULL, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);
    /* A generous cap succeeds. */
    CHECK(chat_export_json_limited(chat, 0, false, 1, 4096, NULL, &out));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_allocation_failure(void) {
    long baseline = live_allocs;
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    CHECK(add(chat, CHAT_ROLE_USER, L"leak check", 1) != NULL);
    for (long n = 0; n < 256; n++) {
        long live_before = live_allocs;
        pass_allocs = n;
        fail_allocs = 1;
        JsonBuf out;
        bool ok = chat_export_json(chat, 0, false, 1, NULL, &out);
        if (ok) {
            CHECK(n > 0);
            CHECK(out.data != NULL);
            json_buf_free(&out);
            break;
        }
        CHECK(out.data == NULL && out.length == 0 && out.capacity == 0);
        CHECK(live_allocs == live_before);
        if (n == 255) { printf("FAIL: export never succeeded\n"); return 1; }
    }
    for (long n = 0; n < 256; n++) {
        long live_before = live_allocs;
        pass_allocs = n;
        fail_allocs = 1;
        JsonBuf out;
        bool ok = chat_export_markdown(chat, 0, false, 1, NULL, NULL, &out);
        if (ok) {
            CHECK(out.data != NULL);
            json_buf_free(&out);
            break;
        }
        CHECK(out.data == NULL && out.length == 0 && out.capacity == 0);
        CHECK(live_allocs == live_before);
        if (n == 255) { printf("FAIL: markdown export never succeeded\n"); return 1; }
    }
    seam_reset();
    chat_dispose(chat);
    free(chat);
    CHECK(live_allocs == baseline);
    return 0;
}

/* %g observes LC_NUMERIC; the serializer must still emit a dot so the payload
   is valid and deterministic JSON. The comma locale may be unavailable on a
   stripped system, in which case the run still checks the default-locale
   output. */
static int test_locale_decimal(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    ChatMessage *a = add(chat, CHAT_ROLE_ASSISTANT, L"x", 1);
    CHECK(a != NULL);
    a->generation.cost = 0.25;
    char saved[64] = "C";
    const char *current = setlocale(LC_NUMERIC, NULL);
    if (current) {
        strncpy(saved, current, sizeof saved - 1);
        saved[sizeof saved - 1] = 0;
    }
    setlocale(LC_NUMERIC, "German_Germany.1252");
    JsonBuf out;
    bool ok = chat_export_json(chat, 0, false, 1, NULL, &out);
    setlocale(LC_NUMERIC, saved);
    CHECK(ok);
    CHECK(json_validate(out.data));
    CHECK(contains(&out, "\"cost\": 0.25"));
    CHECK(!contains(&out, "\"cost\": 0,25"));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

typedef struct {
    const char *name;
    int (*run)(void);
} ExportTest;

/* --- package naming rules ------------------------------------------------ */

static int test_package_names(void) {
    char ext[8];
    /* Only the four types ingest can produce are nameable. An unmapped MIME
       must fail rather than invent an extension nobody recognises. */
    CHECK(chat_export_sidecar_extension("image/png", ext) && !strcmp(ext, "png"));
    CHECK(chat_export_sidecar_extension("image/jpeg", ext) && !strcmp(ext, "jpg"));
    CHECK(chat_export_sidecar_extension("image/webp", ext) && !strcmp(ext, "webp"));
    CHECK(chat_export_sidecar_extension("image/gif", ext) && !strcmp(ext, "gif"));
    CHECK(chat_export_sidecar_extension("IMAGE/PNG", ext) && !strcmp(ext, "png"));
    CHECK(!chat_export_sidecar_extension("image/tiff", ext));
    CHECK(!chat_export_sidecar_extension("image/bmp", ext));
    CHECK(!chat_export_sidecar_extension("", ext));
    CHECK(!chat_export_sidecar_extension(NULL, ext));

    /* Reserved device names, shared with the save dialog's default name. */
    CHECK(chat_filename_is_reserved_device(L"CON", 3));
    CHECK(chat_filename_is_reserved_device(L"con.txt", 7));
    CHECK(chat_filename_is_reserved_device(L"LPT9", 4));
    CHECK(!chat_filename_is_reserved_device(L"CONSOLE", 7));
    CHECK(!chat_filename_is_reserved_device(L"COM10", 5));
    CHECK(!chat_filename_is_reserved_device(L"trip", 4));
    CHECK(!chat_filename_is_reserved_device(L"", 0));

    /* The stem comes from a whole path, so the rules apply to the basename
       only: a drive colon and separators in the directory are legal. */
    wchar_t stem[256];
    CHECK(chat_export_sidecar_stem_from_path(L"C:\\Users\\a\\b\\x.md", stem, 256)
        && !wcscmp(stem, L"x"));
    CHECK(chat_export_sidecar_stem_from_path(L"C:\\trip 1.md", stem, 256)
        && !wcscmp(stem, L"trip 1"));
    CHECK(chat_export_sidecar_stem_from_path(L"/tmp/draft..final.md", stem, 256)
        && !wcscmp(stem, L"draft..final"));
    CHECK(chat_export_sidecar_stem_from_path(L"trip#1.md", stem, 256)
        && !wcscmp(stem, L"trip#1"));
    CHECK(chat_export_sidecar_stem_from_path(L"photo].md", stem, 256)
        && !wcscmp(stem, L"photo]"));
    CHECK(chat_export_sidecar_stem_from_path(L"a.b.markdown", stem, 256)
        && !wcscmp(stem, L"a.b"));
    CHECK(chat_export_sidecar_stem_from_path(L"UPPER.MD", stem, 256)
        && !wcscmp(stem, L"UPPER"));
    /* A name the filesystem could not create, or could not resolve. */
    CHECK(!chat_export_sidecar_stem_from_path(L"", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L".md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"dir\\", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"CON.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"lpt3.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"bad?name.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"star*.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"pipe|.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(L"a\tb.md", stem, 256));
    CHECK(!chat_export_sidecar_stem_from_path(NULL, stem, 256));
    {
        wchar_t tiny[2];
        CHECK(!chat_export_sidecar_stem_from_path(L"abc.md", tiny, 2));
    }
    return 0;
}

static int test_package_reference(void) {
    char folder[80], basename[80];
    const char digest[] =
        "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";
    /* A well-formed reference splits into two separator-free tokens. */
    char good[160];
    snprintf(good, sizeof good, "trip 1.attachments/%s.png", digest);
    CHECK(chat_import_sidecar_reference(good, strlen(good), folder, basename));
    CHECK(!strcmp(folder, "trip 1.attachments"));
    CHECK(!strcmp(basename, "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881.png"));
    /* A folder that is not a package folder cannot be resolved by a reader. */
    snprintf(good, sizeof good, "photos/%s.png", digest);
    CHECK(!chat_import_sidecar_reference(good, strlen(good), folder, basename));
    /* "draft..final" is a legal filename and must stay importable: the
       substring ".." is not what makes a path dangerous, a separator is. */
    snprintf(good, sizeof good, "draft..final.attachments/%s.jpg", digest);
    CHECK(chat_import_sidecar_reference(good, strlen(good), folder, basename));
    CHECK(!strcmp(folder, "draft..final.attachments"));

    /* Everything a crafted document might use to escape the .md's directory. */
    struct { const char *text; const char *why; } bad[] = {
        { "", "an empty reference" },
        { "a.png", "a reference with no folder" },
        { ".attachments/x.png", "a reference with no folder name" },
        { "/x.png", "an absolute reference" },
        { "a.attachments/", "a reference with no basename" },
        { "a.attachments/b/c.png", "two separators" },
        { "a.attachments/../../b.png", "a traversal segment" },
        { "a.attachments/..\\b.png", "a backslash" },
        { "a.attachments/C:b.png", "a drive colon" },
        { "a\\b.attachments/x.png", "a backslash in the folder" },
        { "a:b.attachments/x.png", "a colon in the folder" },
        { "..attachments/x.png", "a folder that is a traversal segment" },
        { ".attachments/../x.png", "a basename that is a traversal segment" },
        { "a.attachments/b<c.png", "a forbidden character in the basename" },
        { "a<b.attachments/x.png", "a forbidden character in the folder" },
        { "a.attachments/b*c.png", "an asterisk" },
        { "a.attachments/b?c.png", "a question mark" },
        { "a.attachments/b\"c.png", "a quote" },
        { "a.attachments/b|c.png", "a pipe" },
        { "a.attachments/b c.png", "a space in a basename" },
        { "a.attachments/short.png", "a basename that is not a digest" },
        { "a.attachments/3f786850e387550fdab836ed7e6dc881de23001b3f786850e387550fdab836ed7.PNG",
          "an uppercase extension" },
        { "a.attachments/3F786850E387550FDAB836ED7E6DC881DE23001B3F786850E387550FDAB836ED7.png",
          "an uppercase digest" },
        { "a.attachments/3f786850e387550fdab836ed7e6dc881de23001b3f786850e387550fdab836ed7.tiff",
          "an unmapped extension" },
        { "a.attachments/3f786850e387550fdab836ed7e6dc881de23001b3f786850e387550fdab836ed7.pngx",
          "a trailing suffix on the extension" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!chat_import_sidecar_reference(bad[i].text,
            strlen(bad[i].text), folder, basename));
        CHECK(!folder[0] && !basename[0]);
    }
    CHECK(!chat_import_sidecar_reference(NULL, 4, folder, basename));
    return 0;
}

static int test_package_markdown_escape(void) {
    wchar_t out[256];
    /* The characters Windows allows in a filename but that still carry URL
       meaning must not be able to retarget or truncate the link. */
    CHECK(chat_export_markdown_destination(L"a/b.png", out, 256) &&
        !wcscmp(out, L"a/b.png"));
    CHECK(chat_export_markdown_destination(L"trip 1.attachments/a.png", out, 256)
        && !wcscmp(out, L"trip%201.attachments/a.png"));
    CHECK(chat_export_markdown_destination(L"trip#1.attachments/a.png", out, 256)
        && !wcscmp(out, L"trip%231.attachments/a.png"));
    CHECK(chat_export_markdown_destination(L"100%.attachments/a.png", out, 256)
        && !wcscmp(out, L"100%25.attachments/a.png"));
    CHECK(chat_export_markdown_destination(L"a?b.attachments/c.png", out, 256)
        && !wcscmp(out, L"a%3Fb.attachments/c.png"));
    CHECK(chat_export_markdown_destination(L"photo].png", out, 256)
        && !wcscmp(out, L"photo%5D.png"));
    /* Non-ASCII is emitted as its UTF-8 bytes, so the link resolves to the same
       file the filesystem holds. */
    CHECK(chat_export_markdown_destination(L"café.png", out, 256) &&
        !wcscmp(out, L"caf%C3%A9.png"));
    /* An unpaired low surrogate names no file. */
    {
        wchar_t lone[8];
        lone[0] = L'a'; lone[1] = (wchar_t)0xdc00; lone[2] = L'.'; lone[3] = 0;
        CHECK(!chat_export_markdown_destination(lone, out, 256));
    }
    CHECK(chat_export_markdown_destination(L"", out, 256) && !out[0]);
    {
        wchar_t tiny[4];
        CHECK(!chat_export_markdown_destination(L"abcd.png", tiny, 4));
    }

    /* Alt text is user text: it must not be able to open a second link. */
    CHECK(chat_export_markdown_alt(L"photo.png", out, 256) &&
        !wcscmp(out, L"photo.png"));
    CHECK(chat_export_markdown_alt(L"a]b[c", out, 256) &&
        !wcscmp(out, L"a\\]b\\[c"));
    CHECK(chat_export_markdown_alt(L"back\\slash", out, 256) &&
        !wcscmp(out, L"back\\\\slash"));
    CHECK(chat_export_markdown_alt(L"two\nlines\r\nhere", out, 256) &&
        !wcscmp(out, L"two lines here"));
    CHECK(chat_export_markdown_alt(L"", out, 256) && !out[0]);
    CHECK(chat_export_markdown_alt(NULL, out, 256) && !out[0]);
    return 0;
}

/* --- schema version 2: ordered content parts ------------------------------ */

/* A conversation whose user turn carries one image, with the attachment
   recorded so the part has something to resolve. */
static Chat *chat_with_image(const wchar_t *text, const char *mime,
    const wchar_t *display_name, uint64_t *out_id) {
    Chat *chat = new_chat();
    if (!chat) return NULL;
    add_attachment(chat, TINY, sizeof TINY, mime, display_name, 1280, 720);
    *out_id = chat->attachment_count ? chat->attachments[0].id : 0;
    ChatMessage *m = add(chat, CHAT_ROLE_USER, text, 1500);
    if (m) add_image(chat, m, *out_id, 0);
    return chat;
}

/* The version is content-derived, so a text-only document is byte-identical to
   what it always was and stays importable by every earlier build. */
static int test_version_is_content_derived(void) {
    Chat *plain = new_chat();
    CHECK(plain);
    add(plain, CHAT_ROLE_USER, L"Hello", 1500);
    JsonBuf out;
    CHECK(chat_export_json(plain, 0, false, 42, NULL, &out));
    CHECK(contains(&out, "\"version\": 1"));
    json_buf_free(&out);
    chat_dispose(plain);
    free(plain);

    uint64_t id = 0;
    Chat *rich = chat_with_image(L"what is this?", "image/png", L"photo.png",
        &id);
    CHECK(rich);
    CHECK(chat_export_json(rich, 0, false, 42, NULL, &out) == false);
    /* JSON needs its bytes, and no callback was supplied: refuse rather than
       emit a reference that cannot be filled. */
    chat_dispose(rich);
    free(rich);
    return 0;
}

static int test_json_v2_image_message(void) {
    uint64_t id = 0;
    Chat *chat = chat_with_image(L"what is this?", "image/png", L"photo.png",
        &id);
    CHECK(chat);
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    BlobEntry entries[2];
    memset(entries, 0, sizeof entries);
    memcpy(entries[0].digest, digest, 65);
    entries[0].bytes = TINY;
    entries[0].n = sizeof TINY;
    ChatExportBlobs blobs;
    memset(&blobs, 0, sizeof blobs);
    blobs.user = entries;
    blobs.read = blob_read;
    blobs.discard = blob_discard;

    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, 42, &blobs, &out));
    CHECK(contains(&out, "\"version\": 2"));
    /* The parts come after the audit block, so a message with none costs zero
       bytes and the v1 shape is untouched. */
    CHECK(contains(&out, "\"generation\": {"));
    CHECK(contains(&out, "\"content\": [\n"));
    CHECK(contains(&out, "\"type\": \"text\",\n"));
    CHECK(contains(&out, "\"text\": \"what is this?\"\n"));
    CHECK(contains(&out, "\"type\": \"image\""));
    CHECK(contains(&out, "\"mime\": \"image/png\""));
    CHECK(contains(&out, "\"pixel_width\": 1280"));
    CHECK(contains(&out, "\"pixel_height\": 720"));
    CHECK(contains(&out, "\"display_name\": \"photo.png\""));
    CHECK(contains(&out, "\"sha256\": \""));
    CHECK(contains(&out, digest));
    /* Base64 needs no JSON escape, so it lands verbatim. */
    CHECK(contains(&out, "\"data\": \"iVBORwECAwQFBg==\""));
    /* A zero-valued optional is omitted rather than written as 0. */
    CHECK(!contains(&out, "\"flags\""));
    /* Determinism: the same chat exported twice yields the same bytes. */
    JsonBuf first;
    CHECK(chat_export_json(chat, 0, false, 42, &blobs, &first));
    JsonBuf second;
    CHECK(chat_export_json(chat, 0, false, 42, &blobs, &second));
    CHECK(second.length == first.length &&
        !memcmp(second.data, first.data, first.length));
    json_buf_free(&first);
    json_buf_free(&second);
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

/* An image-only turn is legal: the projection is empty and the parts carry the
   message. */
static int test_json_v2_image_only(void) {
    uint64_t id = 0;
    Chat *chat = chat_with_image(L"", "image/jpeg", L"shot.jpg", &id);
    CHECK(chat);
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    BlobEntry entries[2];
    memset(entries, 0, sizeof entries);
    memcpy(entries[0].digest, digest, 65);
    entries[0].bytes = TINY;
    entries[0].n = sizeof TINY;
    ChatExportBlobs blobs;
    memset(&blobs, 0, sizeof blobs);
    blobs.user = entries;
    blobs.read = blob_read;
    blobs.discard = blob_discard;
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, 42, &blobs, &out));
    CHECK(contains(&out, "\"text\": \"\""));
    /* No leading text part, because the projection is empty. */
    CHECK(!contains(&out, "\"type\": \"text\""));
    CHECK(contains(&out, "\"type\": \"image\""));
    CHECK(contains(&out, "\"pixel_width\""));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

/* Markdown names sidecars instead of embedding bytes, and reports each
   reference so the caller can materialize the package. */
static int test_markdown_v2_package(void) {
    uint64_t id = 0;
    Chat *chat = chat_with_image(L"what is this?", "image/png", L"photo.png",
        &id);
    CHECK(chat);
    RefLog log;
    memset(&log, 0, sizeof log);
    ChatExportBlobs blobs;
    memset(&blobs, 0, sizeof blobs);
    blobs.user = &log;
    blobs.reference = log_reference;
    JsonBuf out;
    CHECK(chat_export_markdown(chat, 0, false, 42, &blobs, L"trip 1", &out));
    CHECK(log.count == 1);
    CHECK(!strcmp(log.ext[0], "png"));
    /* The payload's reference is the literal relative path, so an importer's
       shape check is a plain token test. */
    CHECK(contains(&out, "\"file\": \"trip 1.attachments/"));
    CHECK(contains(&out, ".png\""));
    /* The body link is the escaped form, and the '#' in a stem cannot retarget
       it. */
    CHECK(contains(&out, "![photo.png](trip%201.attachments/"));
    CHECK(!contains(&out, "\"data\""));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);

    /* One blob under two MIMEs needs two files: the reference is reported per
       part, and the extension differs, so a file set keyed on (digest, ext)
       is complete where one keyed on digest alone would be short. */
    chat = new_chat();
    CHECK(chat);
    add_attachment(chat, TINY, sizeof TINY, "image/png", L"a.png", 1, 1);
    add_attachment(chat, TINY, sizeof TINY, "image/jpeg", L"b.jpg", 1, 1);
    ChatMessage *m = add(chat, CHAT_ROLE_USER, L"two views", 1500);
    add_image(chat, m, chat->attachments[0].id, 0);
    add_image(chat, m, chat->attachments[1].id, 0);
    memset(&log, 0, sizeof log);
    CHECK(chat_export_markdown(chat, 0, false, 42, &blobs, L"pair", &out));
    CHECK(log.count == 2);
    CHECK(!strcmp(log.digest[0], log.digest[1]));
    CHECK(!strcmp(log.ext[0], "png") && !strcmp(log.ext[1], "jpg"));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);
    return 0;
}

/* A parts-bearing Markdown export needs a reference callback and a usable
   stem; a parts-free one needs neither and is byte-identical either way. */
static int test_markdown_v2_requirements(void) {
    uint64_t id = 0;
    Chat *chat = chat_with_image(L"hi", "image/png", L"p.png", &id);
    CHECK(chat);
    JsonBuf out;
    /* No callback: the sidecar set could not be built, so refuse. */
    CHECK(!chat_export_markdown(chat, 0, false, 42, NULL, L"trip", &out));
    /* No stem: no reference a reader could resolve. */
    RefLog log;
    memset(&log, 0, sizeof log);
    ChatExportBlobs blobs;
    memset(&blobs, 0, sizeof blobs);
    blobs.user = &log;
    blobs.reference = log_reference;
    CHECK(!chat_export_markdown(chat, 0, false, 42, &blobs, NULL, &out));
    /* A caller that cannot hold the file set must fail the whole export rather
       than lose a sidecar silently. */
    log.fail = true;
    CHECK(!chat_export_markdown(chat, 0, false, 42, &blobs, L"trip", &out));
    log.fail = false;
    /* Markdown never reads a byte: no read callback is consulted. */
    CHECK(chat_export_markdown(chat, 0, false, 42, &blobs, L"trip", &out));
    json_buf_free(&out);
    chat_dispose(chat);
    free(chat);

    /* A parts-free conversation ignores both, and the two forms agree. */
    Chat *plain = new_chat();
    CHECK(plain);
    add(plain, CHAT_ROLE_USER, L"Hello", 1500);
    JsonBuf without, with;
    CHECK(chat_export_markdown(plain, 0, false, 42, NULL, NULL, &without));
    CHECK(chat_export_markdown(plain, 0, false, 42, &blobs, L"trip", &with));
    CHECK(without.length == with.length &&
        !memcmp(without.data, with.data, without.length));
    CHECK(!contains(&without, "\"version\": 2"));
    json_buf_free(&without);
    json_buf_free(&with);
    chat_dispose(plain);
    free(plain);
    return 0;
}

/* An export is never partial. A part it cannot fill fails the whole document,
 * because a file that quietly lacks an image is the failure format 5 exists to
 * prevent. */
static int test_export_fails_closed(void) {
    char digest[65];
    sha256_hex(TINY, sizeof TINY, digest);
    BlobEntry entries[2];
    memset(entries, 0, sizeof entries);
    ChatExportBlobs blobs;
    memset(&blobs, 0, sizeof blobs);
    blobs.user = entries;
    blobs.read = blob_read;
    blobs.discard = blob_discard;
    JsonBuf out;

    /* A part whose attachment record is missing is a dangling reference. */
    {
        uint64_t id = 0;
        Chat *chat = chat_with_image(L"x", "image/png", L"p.png", &id);
        CHECK(chat);
        /* Pruning cannot produce this: a record a part references is exactly
           what it keeps. Emptying the table directly is the only way to reach
           the dangling-reference state the snapshot loader rejects at load. */
        chat->attachment_count = 0;
        memcpy(entries[0].digest, digest, 65);
        entries[0].bytes = TINY;
        entries[0].n = sizeof TINY;
        CHECK(!chat_export_json(chat, 0, false, 42, &blobs, &out));
        CHECK(!out.data);
        chat_dispose(chat);
        free(chat);
    }
    /* A blob the store cannot produce. */
    {
        uint64_t id = 0;
        Chat *chat = chat_with_image(L"x", "image/png", L"p.png", &id);
        CHECK(chat);
        memset(entries, 0, sizeof entries);   /* nothing resolves */
        CHECK(!chat_export_json(chat, 0, false, 42, &blobs, &out));
        CHECK(!out.data);
        chat_dispose(chat);
        free(chat);
    }
    /* A blob whose length disagrees with the record it is named by. */
    {
        uint64_t id = 0;
        Chat *chat = chat_with_image(L"x", "image/png", L"p.png", &id);
        CHECK(chat);
        memcpy(entries[0].digest, digest, 65);
        entries[0].bytes = TINY;
        entries[0].n = sizeof TINY - 1;
        CHECK(!chat_export_json(chat, 0, false, 42, &blobs, &out));
        CHECK(!out.data);
        chat_dispose(chat);
        free(chat);
    }
    /* An empty payload would encode an image with no bytes. */
    {
        uint64_t id = 0;
        Chat *chat = chat_with_image(L"x", "image/png", L"p.png", &id);
        CHECK(chat);
        memcpy(entries[0].digest, digest, 65);
        entries[0].bytes = TINY;
        entries[0].n = 0;
        CHECK(!chat_export_json(chat, 0, false, 42, &blobs, &out));
        chat_dispose(chat);
        free(chat);
    }
    /* A MIME no sidecar could be named for fails both formats. */
    {
        uint64_t id = 0;
        Chat *chat = chat_with_image(L"x", "image/tiff", L"scan.tif", &id);
        CHECK(chat);
        RefLog log;
        memset(&log, 0, sizeof log);
        ChatExportBlobs md;
        memset(&md, 0, sizeof md);
        md.user = &log;
        md.reference = log_reference;
        CHECK(!chat_export_markdown(chat, 0, false, 42, &md, L"trip", &out));
        CHECK(log.count == 0);   /* reported nothing before failing */
        memcpy(entries[0].digest, digest, 65);
        entries[0].bytes = TINY;
        entries[0].n = sizeof TINY;
        CHECK(!chat_export_json(chat, 0, false, 42, &blobs, &out));
        chat_dispose(chat);
        free(chat);
    }
    return 0;
}

static const ExportTest export_tests[] = {
    { "json_empty_conversation", test_json_empty_conversation },
    { "json_full", test_json_full },
    { "json_both_overrides", test_json_both_overrides },
    { "json_export_all", test_json_export_all },
    { "json_ids_and_enums", test_json_ids_and_enums },
    { "json_int64_bounds", test_json_int64_bounds },
    { "markdown_full", test_markdown_full },
    { "markdown_marker_injection", test_markdown_marker_injection },
    { "markdown_presentation_edges", test_markdown_presentation_edges },
    { "markdown_all_sections", test_markdown_all_sections },
    { "determinism_and_args", test_determinism_and_args },
    { "size_limit", test_size_limit },
    { "allocation_failure", test_allocation_failure },
    { "locale_decimal", test_locale_decimal },
    { "package_names", test_package_names },
    { "package_reference", test_package_reference },
    { "package_markdown_escape", test_package_markdown_escape },
    { "version_is_content_derived", test_version_is_content_derived },
    { "json_v2_image_message", test_json_v2_image_message },
    { "json_v2_image_only", test_json_v2_image_only },
    { "markdown_v2_package", test_markdown_v2_package },
    { "markdown_v2_requirements", test_markdown_v2_requirements },
    { "export_fails_closed", test_export_fails_closed },
};

int main(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof export_tests / sizeof export_tests[0]; i++) {
        long before = live_allocs;
        failures += export_tests[i].run();
        if (live_allocs != before)
            printf("LEAK in %s: %ld allocation(s)\n", export_tests[i].name,
                live_allocs - before);
    }
    if (failures) { printf("%d export test(s) failed\n", failures); return 1; }
    printf("test_export passed\n");
    return 0;
}
