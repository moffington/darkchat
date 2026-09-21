#include "chat/import/import.h"
#include "chat/core/chat.h"
#include "chat/export/export.h"
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
    return chat_import_json(chat, text, strlen(text), stats);
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
    CHECK(chat_export_json(source, 0, true, 42, &out));
    ChatImportStats stats;
    CHECK(chat_import_json(dest, out.data, out.length, &stats) ==
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
    CHECK(chat_export_json(source, 0, false, 1, &out));
    CHECK(chat_import_json(dest, out.data, out.length, NULL) == CHAT_IMPORT_OK);
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
        CHECK(chat_import_json(dest, invalid, (size_t)length, NULL) ==
            CHAT_IMPORT_MALFORMED);
    }
    static const char embedded_nul[] = { '{', 0, '}' };
    CHECK(chat_import_json(dest, embedded_nul, sizeof embedded_nul, NULL) ==
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
    CHECK(chat_export_json(source, 0, false, 1, &out));

    Snapshot before;
    snapshot(dest, &before);
    bool succeeded = false;
    for (long n = 0; n < 512; n++) {
        long live_before = live_allocs;
        pass_allocs = n;
        fail_allocs = 1;
        ChatImportStats stats;
        ChatImportStatus status = chat_import_json(dest, out.data, out.length,
            &stats);
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
    CHECK(chat_export_json(source, 0, false, 1, &out));
    Snapshot before;
    snapshot(dest, &before);
    CHECK(chat_import_json(dest, out.data, out.length, NULL) ==
        CHAT_IMPORT_CAPACITY);
    CHECK(unchanged(dest, &before));
    json_buf_free(&out);
    chat_dispose(source); free(source);
    chat_dispose(dest); free(dest);
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
