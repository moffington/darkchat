#include "chat/export/export_test.h"
#include "chat/core/chat.h"
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
    CHECK(chat_export_json(chat, 0, false, 42, &out));
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
    CHECK(chat_export_json(chat, 0, false, 42, &out));
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
    CHECK(chat_export_json(chat, 0, false, 5, &out));
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
    CHECK(chat_export_json(chat, 0, false, 5, &out));
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
    CHECK(chat_export_json(chat, 0, true, 42, &out));
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
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);

    c->id = CHAT_MAX_ID - 1;
    CHECK(chat_export_json(chat, 0, false, 1, &out));
    CHECK(contains(&out, "\"id\": 9007199254739999"));
    json_buf_free(&out);

    c->id = CHAT_MAX_ID;
    CHECK(chat_export_json(chat, 0, false, 1, &out));
    CHECK(contains(&out, "\"id\": 9007199254740000"));
    json_buf_free(&out);

    c->id = CHAT_MAX_ID + 1;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    c->id = UINT64_MAX;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));

    c->id = 7;
    CHECK(add(chat, CHAT_ROLE_USER, L"hi", 1) != NULL);
    chat->conversations[0].messages[0].role = (ChatRole)99;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    chat->conversations[0].messages[0].role = CHAT_ROLE_USER;
    chat->conversations[0].messages[0].generation.backend = (ChatBackend)99;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    chat->conversations[0].messages[0].generation.backend =
        CHAT_BACKEND_OPENROUTER;

    chat->conversations[0].messages[0].generation.cost = NAN;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    chat->conversations[0].messages[0].generation.cost = INFINITY;
    CHECK(!chat_export_json(chat, 0, false, 1, &out));
    CHECK(!chat_export_markdown(chat, 0, false, 1, &out));

    chat_dispose(chat);
    free(chat);
    return 0;
}

static int test_json_int64_bounds(void) {
    Chat *chat = new_chat();
    CHECK(chat != NULL);
    JsonBuf out;
    CHECK(chat_export_json(chat, 0, false, INT64_MIN, &out));
    CHECK(contains(&out, "\"exported_at\": -9223372036854775808"));
    json_buf_free(&out);
    CHECK(chat_export_json(chat, 0, false, INT64_MAX, &out));
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
    CHECK(chat_export_markdown(chat, 0, false, 42, &out));
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
    CHECK(chat_export_markdown(chat, 0, false, 42, &out));
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
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
    CHECK(contains(&out, "# a b\n"));
    CHECK(contains(&out, "\"title\": \"  a\\nb  \""));
    json_buf_free(&out);

    /* A CRLF pair is one line break, not two spaces. */
    wcscpy(c->title, L"a\r\nb");
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
    CHECK(contains(&out, "# a b\n"));
    CHECK(!contains(&out, "# a  b\n"));
    json_buf_free(&out);

    /* Empty text: heading then the reasoning blockquote; empty reasoning line
       renders as a bare '>'. */
    ChatMessage *m = add(chat, CHAT_ROLE_ASSISTANT, L"", 10);
    CHECK(m != NULL);
    CHECK(chat_message_set_reasoning(m, L"one\n\ntwo"));
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
    CHECK(contains(&out, "## Assistant\n\n> one\n>\n> two\n\n"));
    json_buf_free(&out);

    /* Preserved trailing newline run, unchanged by the boundary rule. */
    CHECK(chat_message_set_reasoning(m, L""));
    CHECK(chat_message_set_text(m, L"tail\n\n\n"));
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
    CHECK(contains(&out, "tail\n\n\n"));
    CHECK(!contains(&out, "tail\n\n\n\n"));
    json_buf_free(&out);

    /* CRLF normalizes to LF in both the payload and the presentation. */
    CHECK(chat_message_set_text(m, L"a\r\nb"));
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
    CHECK(contains(&out, "a\\nb"));
    CHECK(contains(&out, "a\nb"));
    CHECK(!contains(&out, "\r"));
    json_buf_free(&out);

    /* Empty title omits the heading entirely. */
    c->title[0] = 0;
    CHECK(chat_export_markdown(chat, 0, false, 1, &out));
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
    CHECK(chat_export_markdown(chat, 0, true, 1, &out));
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
    CHECK(chat_export_json(chat, 0, false, 9, &a));
    CHECK(chat_export_json(chat, 0, false, 9, &b));
    CHECK(strcmp(a.data, b.data) == 0);
    json_buf_free(&a);
    json_buf_free(&b);

    CHECK(!chat_export_json(NULL, 0, false, 1, &a));
    CHECK(a.data == NULL && a.length == 0 && a.capacity == 0 && !a.oom);
    CHECK(!chat_export_json(chat, -1, false, 1, &a));
    CHECK(!chat_export_json(chat, 5, false, 1, &a));
    CHECK(!chat_export_json(chat, 0, false, 1, NULL));
    CHECK(!chat_export_markdown(chat, 0, false, 1, NULL));
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
    CHECK(!chat_export_json_limited(chat, 0, false, 1, 16, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);
    CHECK(!chat_export_markdown_limited(chat, 0, false, 1, 16, &out));
    CHECK(out.data == NULL && out.length == 0 && out.capacity == 0 && !out.oom);
    /* A generous cap succeeds. */
    CHECK(chat_export_json_limited(chat, 0, false, 1, 4096, &out));
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
        bool ok = chat_export_json(chat, 0, false, 1, &out);
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
        bool ok = chat_export_markdown(chat, 0, false, 1, &out);
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
    bool ok = chat_export_json(chat, 0, false, 1, &out);
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
