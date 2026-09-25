#include "chat/generation/context.h"
#include "chat/core/chat.h"
#include "chat/json.h"
#include "chat/generation/provider_routing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Request-context tests. Pure C, no Windows APIs, so they run anywhere.
   Built and run by `chat.bat test`. */

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

/* The test keeps its own copy of the request framing, so a divergence in
   chat/generation/context.c is caught here instead of being hidden behind the same
   helper the implementation uses. */
#define TEST_ENVELOPE 67u
#define TEST_MESSAGE 22u

static size_t role_name_bytes(ChatRole role) {
    return role == CHAT_ROLE_ASSISTANT ? 9u : role == CHAT_ROLE_SYSTEM ? 6u : 4u;
}

static size_t message_bytes(ChatRole role, const wchar_t *text) {
    return TEST_MESSAGE + role_name_bytes(role) + json_encoded_string_size(text);
}

/* Body size the entries of a built context encode to. */
static size_t expected_body(const Chat *chat, const ChatRequestContext *context) {
    size_t total = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    for (int i = 0; i < context->count; i++)
        total += message_bytes(context->messages[i].role, context->messages[i].text);
    if (context->count > 0) total += (size_t)context->count - 1;
    return total;
}

/* A rejected build must leave a fully zeroed, readable output: the tests poison
   the struct first, so a field the implementation forgets shows up here. */
static int zeroed_output(const ChatRequestContext *context) {
    if (context->count != 0 || context->bytes != 0 ||
        context->first_kept_index != -1 || context->dropped_messages != 0 ||
        context->required_bytes != 0 || context->part_slots_used != 0) return 0;
    for (int i = 0; i < CHAT_CONTEXT_MAX_ENTRIES; i++)
        if (context->messages[i].text != NULL ||
            context->messages[i].parts != NULL ||
            context->messages[i].part_count != 0) return 0;
    return 1;
}

static Chat *fresh_chat(void) {
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!chat) return NULL;
    chat_init(chat);
    chat_clear(chat);   /* no local welcome note: history starts empty */
    return chat;
}

/* Appends a user/assistant pair with the given assistant state. */
static int add_turn(Chat *chat, const wchar_t *prompt, const wchar_t *answer,
    ChatGenerationState state) {
    chat_append(chat, CHAT_ROLE_USER, prompt);
    int index = chat_append(chat, CHAT_ROLE_ASSISTANT, answer);
    chat->conversations[chat->active].messages[index].generation.state = state;
    return index;
}

static const ChatConversation *active(Chat *chat) {
    return &chat->conversations[chat->active];
}

/* Fills a heap text of `units` copies of `fill` (NUL-terminated). Message text
   is not globally capped: values past the small inline residue
   (CHAT_MESSAGE_INLINE) land in overflow storage, which is what these tests
   exercise. */
static wchar_t *long_text(size_t units, wchar_t fill) {
    wchar_t *text = (wchar_t *)malloc((units + 1) * sizeof *text);
    if (!text) return NULL;
    for (size_t i = 0; i < units; i++) text[i] = fill;
    text[units] = 0;
    return text;
}
static void test_order_and_identity(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    add_turn(chat, L"two", L"answer two", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"three");
    const ChatConversation *c = active(chat);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "a huge budget builds a context");
    check(context.count == 5, "every eligible message is kept");
    check(context.dropped_messages == 0, "nothing is dropped under a huge budget");
    check(context.first_kept_index == 0, "the oldest eligible message is kept");
    check(context.bytes == expected_body(chat, &context),
        "the reported size matches the framing model");
    check(context.messages[0].role == CHAT_ROLE_USER &&
        !wcscmp(context.messages[0].text, L"one"), "first entry is the oldest user message");
    check(context.messages[1].role == CHAT_ROLE_ASSISTANT &&
        !wcscmp(context.messages[1].text, L"answer one"), "its answer follows");
    check(context.messages[2].role == CHAT_ROLE_USER &&
        !wcscmp(context.messages[2].text, L"two"), "history keeps conversation order");
    check(context.messages[3].role == CHAT_ROLE_ASSISTANT &&
        !wcscmp(context.messages[3].text, L"answer two"), "the newest answer follows");
    check(context.messages[4].role == CHAT_ROLE_USER &&
        !wcscmp(context.messages[4].text, L"three"), "the triggering message comes last");
    check(context.messages[4].text == chat_message_text(&c->messages[trigger]),
        "the trigger entry borrows the live message text");
    check(context.messages[0].text == chat_message_text(&c->messages[0]),
        "history entries borrow the live text too");
    int copies = 0;
    for (int i = 0; i < context.count; i++)
        if (!wcscmp(context.messages[i].text, L"three")) ++copies;
    check(copies == 1, "the triggering message appears exactly once");
    chat_dispose(chat); free(chat);
}

static void test_oldest_dropped_first(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    add_turn(chat, L"two", L"answer two", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"three");
    const ChatConversation *c = active(chat);
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    size_t u2 = message_bytes(CHAT_ROLE_USER, L"two");
    size_t a2 = message_bytes(CHAT_ROLE_ASSISTANT, L"answer two");
    size_t u3 = message_bytes(CHAT_ROLE_USER, L"three");

    static ChatRequestContext context;
    /* Exactly room for the newest history message and the trigger. */
    size_t budget = base + u2 + a2 + u3 + 2;
    check(chat_context_build(chat, c, trigger, budget, &context) == CHAT_CONTEXT_OK &&
        context.count == 3, "a tight budget keeps only what fits");
    check(context.bytes == budget, "a tight budget is filled exactly");
    check(context.dropped_messages == 2, "only the two oldest messages are dropped");
    check(context.first_kept_index == 2, "the kept history starts at the newest one that fit");
    check(!wcscmp(context.messages[0].text, L"two") &&
        !wcscmp(context.messages[2].text, L"three"), "the kept slice is the newest");

    /* One byte less drops exactly one more message. */
    check(chat_context_build(chat, c, trigger, budget - 1, &context) == CHAT_CONTEXT_OK &&
        context.count == 2, "one byte less drops one more message");
    check(context.dropped_messages == 3 && context.first_kept_index == 3,
        "the next oldest eligible message goes");
    check(context.bytes == base + a2 + u3 + 1, "the smaller context is exactly sized");

    /* The trigger alone, exactly and one byte under. */
    size_t trigger_only = base + u3;
    check(chat_context_build(chat, c, trigger, trigger_only - 1, &context) ==
        CHAT_CONTEXT_OVERSIZE_USER, "a trigger that cannot fit fails explicitly");
    check(context.count == 0 && context.bytes == 0,
        "a failed build returns no entries and no size");
    check(context.required_bytes == trigger_only,
        "required_bytes is the whole indispensable body");
    check(chat_context_build(chat, c, trigger, trigger_only, &context) == CHAT_CONTEXT_OK &&
        context.count == 1, "the trigger alone fits at exactly its own size");
    check(context.dropped_messages == 4 && context.first_kept_index == -1,
        "a trigger-only budget drops every history message");
    chat_dispose(chat); free(chat);
}

static void test_exclusions(void) {
    Chat *chat = fresh_chat();
    chat_append(chat, CHAT_ROLE_ASSISTANT, L"Welcome to DarkChat.");        /* 0  excluded */
    chat_append(chat, CHAT_ROLE_USER, L"q1");                               /* 1  kept */
    int a1 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"a1");                 /* 2  kept */
    chat->conversations[0].messages[a1].generation.state = CHAT_GENERATION_COMPLETE;
    chat_append(chat, CHAT_ROLE_ERROR, L"local error note");                /* 3  excluded */
    chat_append(chat, CHAT_ROLE_USER, L"q2");                               /* 4  kept */
    int a2 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"failed answer");      /* 5  excluded */
    chat->conversations[0].messages[a2].generation.state = CHAT_GENERATION_FAILED;
    chat_append(chat, CHAT_ROLE_USER, L"q3");                               /* 6  kept */
    int a3 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"still running");      /* 7  excluded */
    chat->conversations[0].messages[a3].generation.state = CHAT_GENERATION_RUNNING;
    int a4 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"cancelled");          /* 8  excluded */
    chat->conversations[0].messages[a4].generation.state = CHAT_GENERATION_CANCELLED;
    chat_append(chat, CHAT_ROLE_USER, L"q4");                               /* 9  kept */
    int a5 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"cut off");            /* 10 excluded */
    chat->conversations[0].messages[a5].generation.state = CHAT_GENERATION_INTERRUPTED;
    chat_append(chat, CHAT_ROLE_USER, L"q5");                               /* 11 kept */
    int a6 = chat_append(chat, CHAT_ROLE_ASSISTANT, L"");                   /* 12 kept (empty) */
    chat->conversations[0].messages[a6].generation.state = CHAT_GENERATION_COMPLETE;
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"trigger");            /* 13 kept */
    const ChatConversation *c = active(chat);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "the mixed conversation builds");
    static const ChatRole expected_roles[] = {
        CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_USER, CHAT_ROLE_USER,
        CHAT_ROLE_USER, CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_USER };
    check(context.count == 8, "only eligible history is kept");
    int roles_match = context.count == 8;
    for (int i = 0; roles_match && i < context.count; i++)
        roles_match = context.messages[i].role == expected_roles[i];
    check(roles_match, "welcome text, error notes and unfinished answers are excluded");
    check(context.messages[6].role == CHAT_ROLE_ASSISTANT &&
        !wcscmp(context.messages[6].text, L""),
        "an eligible empty message is preserved exactly as today");
    check(!wcscmp(context.messages[7].text, L"trigger"), "the trigger is last");

    /* A tiny budget drops every eligible history message, counted as messages. */
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    size_t trigger_only = base + message_bytes(CHAT_ROLE_USER, L"trigger");
    check(chat_context_build(chat, c, trigger, trigger_only, &context) == CHAT_CONTEXT_OK &&
        context.count == 1, "only the trigger survives a trigger-only budget");
    check(context.dropped_messages == 7,
        "exactly the seven eligible history messages are counted");
    check(context.first_kept_index == -1, "no history index survives");
    chat_dispose(chat); free(chat);
}

static void test_system_prompt(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"two");
    const ChatConversation *c = active(chat);
    wcscpy(chat->system_prompt, L"Be concise.");

    static ChatRequestContext context;
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    size_t system = message_bytes(CHAT_ROLE_SYSTEM, L"Be concise.");
    size_t trigger_bytes = message_bytes(CHAT_ROLE_USER, L"two");

    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "a system prompt builds");
    check(context.messages[0].role == CHAT_ROLE_SYSTEM &&
        context.messages[0].text == chat->system_prompt,
        "the system prompt is first and borrows the live text");
    check(context.count == 4, "the system prompt does not replace history");

    size_t system_only = base + system + 1 + trigger_bytes;
    check(chat_context_build(chat, c, trigger, system_only, &context) == CHAT_CONTEXT_OK &&
        context.count == 2, "the system prompt and trigger survive without history");
    check(context.first_kept_index == -1 && context.dropped_messages == 2,
        "history is dropped before the system prompt or trigger");
    check(context.bytes == system_only, "the system prompt costs one separator before the trigger");
    check(chat_context_build(chat, c, trigger, system_only - 1, &context) ==
        CHAT_CONTEXT_OVERSIZE_COMBINED,
        "a system prompt and trigger that only fail together are reported as combined");
    chat_dispose(chat); free(chat);
}

/* The conversation's effective prompt and model override the global slots:
    a text override replaces the global prompt, the deliberately-empty
    override suppresses it entirely, and an empty override inherits it. The
    oversize diagnostics are measured against the effective prompt. */
static void test_effective_prompt(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"two");
    const ChatConversation *c = active(chat);
    wcscpy(chat->system_prompt, L"Global persona.");
    check(chat_conversation_apply_system_prompt(chat, 0, L"Local persona."),
        "fixture: a text override is applied");
    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "an overridden prompt builds");
    check(context.messages[0].role == CHAT_ROLE_SYSTEM &&
        !wcscmp(context.messages[0].text, L"Local persona."),
        "the conversation override replaces the global prompt");
    check(chat_conversation_apply_system_prompt(chat, 0, L""),
        "fixture: the deliberately-empty override is applied");
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK &&
        context.count == 3,
        "the empty override sends no system message at all");
    for (int i = 0; i < context.count; i++)
        check(context.messages[i].role != CHAT_ROLE_SYSTEM,
            "the empty override contributes no system entry");
    check(chat_conversation_set_system_prompt(chat, 0, L""),
        "fixture: the override is cleared");
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK &&
        context.messages[0].role == CHAT_ROLE_SYSTEM &&
        !wcscmp(context.messages[0].text, L"Global persona."),
        "a cleared override inherits the global prompt again");

    /* The oversize diagnostic measures the effective prompt: a huge override
        fails like a huge global prompt would, and clearing it makes the same
        budget succeed. */
    wchar_t *big = long_text(4000, L'\u2014');
    check(chat_conversation_apply_system_prompt(chat, 0, big),
        "fixture: an oversized override is applied");
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    check(chat_context_build(chat, c, trigger, base + 1000, &context) ==
        CHAT_CONTEXT_OVERSIZE_SYSTEM,
        "an oversized override is reported as an oversize system prompt");
    check(chat_conversation_set_system_prompt(chat, 0, L""),
        "fixture: the oversized override is cleared");
    check(chat_context_build(chat, c, trigger, base + 1000, &context) ==
        CHAT_CONTEXT_OK,
        "the same budget succeeds once the override is gone");
    free(big);
    chat_dispose(chat); free(chat);
}

static void test_oversize(void) {
    Chat *chat = fresh_chat();
    const ChatConversation *c;
    static ChatRequestContext context;
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    /* 3 UTF-8 bytes per code unit, so the indispensable pair outgrows a small
       budget without any single message being enormous. Promoted overflow
       storage is covered separately. */
    wchar_t *big = long_text(4000, L'\u2014');
    wchar_t *medium = long_text(1000, L'\u2014');

    wcscpy(chat->system_prompt, big);
    chat_append(chat, CHAT_ROLE_USER, L"small");
    c = active(chat);
    size_t system_bytes = message_bytes(CHAT_ROLE_SYSTEM, big);
    size_t small_bytes = message_bytes(CHAT_ROLE_USER, L"small");
    size_t budget = base + 1000;
    check(chat_context_build(chat, c, 0, budget, &context) == CHAT_CONTEXT_OVERSIZE_SYSTEM,
        "an oversized system prompt fails explicitly");
    check(context.count == 0 && context.bytes == 0 && context.dropped_messages == 0,
        "an oversized system prompt returns no context");
    check(context.required_bytes == base + system_bytes + 1 + small_bytes,
        "the system-prompt diagnostic reports the complete indispensable body");
    check(context.required_bytes > budget, "the reported requirement exceeds the budget");

    chat->system_prompt[0] = 0;
    chat_clear(chat);
    int trigger = chat_append(chat, CHAT_ROLE_USER, big);
    c = active(chat);
    check(chat_context_build(chat, c, trigger, base + 1000, &context) ==
        CHAT_CONTEXT_OVERSIZE_USER, "an oversized message fails explicitly");
    check(context.required_bytes == base + message_bytes(CHAT_ROLE_USER, big),
        "a message-only failure reports the body with no system prompt");
    check(context.required_bytes > base + 1000, "the message requirement exceeds the budget");

    wcscpy(chat->system_prompt, big);
    chat_clear(chat);
    trigger = chat_append(chat, CHAT_ROLE_USER, medium);
    c = active(chat);
    size_t medium_bytes = message_bytes(CHAT_ROLE_USER, medium);
    size_t combined_budget = base + system_bytes + 500;
    check(combined_budget >= base + system_bytes && combined_budget >= base + medium_bytes,
        "the combined budget fits each indispensable message alone");
    check(chat_context_build(chat, c, trigger, combined_budget, &context) ==
        CHAT_CONTEXT_OVERSIZE_COMBINED,
        "system and message that only fail together are combined");
    check(context.required_bytes == base + system_bytes + 1 + medium_bytes,
        "the combined diagnostic reports the complete indispensable body");
    check(context.required_bytes > combined_budget, "the combined requirement exceeds the budget");
    check(chat_context_build(chat, c, trigger, 0, &context) == CHAT_CONTEXT_INVALID,
        "a zero budget is invalid arguments, not an oversize result");

    free(big); free(medium);
    chat_dispose(chat); free(chat);
}

static void test_invalid(void) {
    Chat *chat = fresh_chat();
    int user = chat_append(chat, CHAT_ROLE_USER, L"only");
    const ChatConversation *c = active(chat);
    static ChatRequestContext context;

    /* Every rejected build must return -- and zero -- the output it was given,
       whatever the reason: the struct is poisoned before each call and the
       result must be indistinguishable from a fresh zeroed one. */
    #define REJECT(call, what) do { \
        memset(&context, 0xa5, sizeof context); \
        check((call) == CHAT_CONTEXT_INVALID && zeroed_output(&context), what); \
    } while (0)
    if (1) {
        REJECT(chat_context_build(NULL, c, user, SIZE_MAX, &context),
            "a missing chat is invalid and zeroes the output");
        REJECT(chat_context_build(chat, NULL, user, SIZE_MAX, &context),
            "a missing conversation is invalid and zeroes the output");
        REJECT(chat_context_build(NULL, NULL, user, SIZE_MAX, &context),
            "two missing inputs are invalid and zero the output");
        REJECT(chat_context_build(chat, c, user, 0, &context),
            "a zero budget is invalid and zeroes the output");
        int bad_indices[] = { -1, 2, 99 };
        for (size_t i = 0; i < sizeof bad_indices / sizeof bad_indices[0]; i++)
            REJECT(chat_context_build(chat, c, bad_indices[i], SIZE_MAX, &context),
                "an out-of-range index is invalid and zeroes the output");
        check(chat_context_build(chat, c, user, SIZE_MAX, NULL) == CHAT_CONTEXT_INVALID,
            "a missing output is invalid");
    }
    #undef REJECT
    check(chat_context_build(chat, c, user, SIZE_MAX, &context) == CHAT_CONTEXT_OK &&
        context.count == 1 && context.first_kept_index == -1 && context.dropped_messages == 0,
        "a valid build overwrites a poisoned output");

    chat_append(chat, CHAT_ROLE_ASSISTANT, L"answer");
    chat_append(chat, CHAT_ROLE_ERROR, L"note");
    c = active(chat);
    memset(&context, 0xa5, sizeof context);
    check(chat_context_build(chat, c, 1, SIZE_MAX, &context) == CHAT_CONTEXT_INVALID &&
        zeroed_output(&context), "a non-user triggering message is invalid and zeroes the output");
    memset(&context, 0xa5, sizeof context);
    check(chat_context_build(chat, c, 2, SIZE_MAX, &context) == CHAT_CONTEXT_INVALID &&
        zeroed_output(&context), "an error-role triggering message is invalid and zeroes the output");

    chat_clear(chat);
    c = active(chat);
    memset(&context, 0xa5, sizeof context);
    check(chat_context_build(chat, c, 0, SIZE_MAX, &context) == CHAT_CONTEXT_INVALID &&
        zeroed_output(&context), "an empty conversation is invalid and zeroes the output");
    chat_dispose(chat); free(chat);
}

static void test_beyond_trigger(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"two");
    int running = chat_append(chat, CHAT_ROLE_ASSISTANT, L"");
    const ChatConversation *c = active(chat);
    chat->conversations[0].messages[running].generation.state = CHAT_GENERATION_RUNNING;

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK &&
        context.count == 3, "the running response after the trigger is not part of the context");
    check(context.messages[context.count - 1].role == CHAT_ROLE_USER &&
        !wcscmp(context.messages[context.count - 1].text, L"two"),
        "the trigger stays last, so the pending response cannot leak in");
    check(context.bytes == expected_body(chat, &context),
        "the pending response is not measured either");
    chat_dispose(chat); free(chat);
}

static void test_huge_dropped_history(void) {
    Chat *chat = fresh_chat();
    wchar_t *huge = long_text(200000, L'x');   /* 200 KB of ASCII, past any budget */
    add_turn(chat, huge, huge, CHAT_GENERATION_COMPLETE);
    add_turn(chat, L"small one", L"small answer", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"small two");
    const ChatConversation *c = active(chat);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, CHAT_CONTEXT_BUDGET_BYTES, &context) ==
        CHAT_CONTEXT_OK, "a history message far past the budget does not fail the request");
    check(context.count == 3 && context.first_kept_index == 2,
        "the oversized history is dropped and the newest history is kept");
    check(context.dropped_messages == 2, "both older messages are dropped");
    check(context.bytes <= CHAT_CONTEXT_BUDGET_BYTES, "the kept context fits the budget");
    check(context.bytes == expected_body(chat, &context), "the dropped text is not measured");
    chat_dispose(chat); free(chat); free(huge);
}

static void test_read_only(void) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"one", L"answer one", CHAT_GENERATION_COMPLETE);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"two");
    /* Push a message onto overflow storage so the build must not touch it. */
    wchar_t *big = long_text(20000, L'y');
    chat_message_append_text(&chat->conversations[0].messages[1], big);
    const ChatConversation *c = active(chat);
    size_t count = c->message_count, capacity = c->message_capacity;
    int64_t modified = c->modified_at;
    uint64_t next_id = chat->next_id;
    wchar_t status[CHAT_STATUS_TEXT];
    wcscpy(status, chat->status);
    ChatMessage *before = (ChatMessage *)malloc(count * sizeof *before);
    if (!before) { check(0, "allocate the read-only snapshot"); return; }
    memcpy(before, c->messages, count * sizeof *before);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, 200, &context) == CHAT_CONTEXT_OK,
        "a tiny budget builds");
    check(chat_context_build(chat, c, trigger, 0, &context) == CHAT_CONTEXT_INVALID,
        "a zero budget is invalid");
    check(zeroed_output(&context), "a zero budget leaves a zeroed output");
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "a huge budget builds");

    c = active(chat);
    check(c->message_count == count && c->message_capacity == capacity,
        "the conversation shape is unchanged");
    check(c->modified_at == modified && chat->next_id == next_id,
        "timestamps and the id counter are unchanged");
    check(memcmp(before, c->messages, count * sizeof *before) == 0,
        "no message field, overflow pointers included, changed");
    check(c->messages[1].text_overflow != NULL, "overflow storage still owns the long answer");
    check(!wcscmp(status, chat->status), "the status line is unchanged");
    free(before); free(big);
    chat_dispose(chat); free(chat);
}

static unsigned lcg(unsigned *state) {
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fffu;
}

static void test_budget_sweep(void) {
    Chat *chat = fresh_chat();
    const ChatConversation *c;
    unsigned seed = 12345u;
    for (int turn = 0; turn < 10; turn++) {
        wchar_t prompt[512], answer[512];
        int length = (int)(lcg(&seed) % 300u);
        for (int i = 0; i < length; i++) prompt[i] = (wchar_t)(L'a' + lcg(&seed) % 26u);
        prompt[length] = 0;
        length = (int)(lcg(&seed) % 400u);
        for (int i = 0; i < length; i++) answer[i] = (wchar_t)(L'a' + lcg(&seed) % 26u);
        answer[length] = 0;
        int index = add_turn(chat, prompt, answer, CHAT_GENERATION_COMPLETE);
        if (turn % 3 == 1)
            chat->conversations[0].messages[index].generation.state = CHAT_GENERATION_FAILED;
        if (turn % 5 == 2)
            chat->conversations[0].messages[index].generation.state = CHAT_GENERATION_INTERRUPTED;
    }
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"final question");
    c = active(chat);
    wcscpy(chat->system_prompt, L"Answer briefly.");

    int history_total = 0;
    for (int i = 0; i < trigger; i++)
        if (chat_history_message(&c->messages[i])) ++history_total;

    size_t budgets[] = { 1u, 90u, 200u, 1000u, 4000u, 16000u, 65536u, SIZE_MAX };
    int previous_dropped = -1;
    for (size_t b = 0; b < sizeof budgets / sizeof budgets[0]; b++) {
        static ChatRequestContext context;
        ChatContextResult result = chat_context_build(chat, c, trigger, budgets[b], &context);
        if (result != CHAT_CONTEXT_OK) {
            check(result == CHAT_CONTEXT_OVERSIZE_SYSTEM ||
                result == CHAT_CONTEXT_OVERSIZE_USER ||
                result == CHAT_CONTEXT_OVERSIZE_COMBINED,
                "sweep: an insufficient budget is an explicit oversize failure");
            check(context.count == 0 && context.bytes == 0 && context.dropped_messages == 0,
                "sweep: a failure returns no entries");
            continue;
        }
        check(context.bytes <= budgets[b], "sweep: the context fits the budget");
        check(context.bytes == expected_body(chat, &context),
            "sweep: the reported size matches the framing");
        check(context.count >= 1 && context.count <= CHAT_CONTEXT_MAX_ENTRIES,
            "sweep: the entry count stays bounded");
        check(context.messages[0].role == CHAT_ROLE_SYSTEM,
            "sweep: the system prompt is preserved first");
        check(context.messages[context.count - 1].role == CHAT_ROLE_USER &&
            context.messages[context.count - 1].text == chat_message_text(&c->messages[trigger]),
            "sweep: the triggering message is preserved last");
        int expected_kept = 0, expected_dropped = 0;
        for (int i = 0; i < trigger; i++) {
            if (!chat_history_message(&c->messages[i])) continue;
            if (context.first_kept_index >= 0 && i >= context.first_kept_index) ++expected_kept;
            else ++expected_dropped;
        }
        int kept = context.count - 1;   /* without the trigger */
        if (context.messages[0].role == CHAT_ROLE_SYSTEM) --kept;
        check(kept == expected_kept, "sweep: the newest contiguous history is kept");
        check(context.dropped_messages == expected_dropped,
            "sweep: only the oldest eligible messages are counted as dropped");
        check(context.dropped_messages + kept == history_total,
            "sweep: every eligible history message is either kept or counted");
        if (previous_dropped >= 0)
            check(context.dropped_messages <= previous_dropped,
                "sweep: a larger budget never drops more");
        previous_dropped = context.dropped_messages;
    }
    chat_dispose(chat); free(chat);
}

/* Drives one real send path and checks the trigger the host would pass is the
   user message directly before the pending response. */
static void check_mode(ChatSendMode mode, const wchar_t *prompt, const wchar_t *expected) {
    Chat *chat = fresh_chat();
    add_turn(chat, L"first", L"answer",
        mode == CHAT_RETRY ? CHAT_GENERATION_FAILED : CHAT_GENERATION_COMPLETE);
    int response = chat_begin_response(chat, mode, prompt);
    check(response > 0, "the send mode starts a response");
    static ChatRequestContext context;
    check(chat_context_build(chat, &chat->conversations[0], response - 1,
        CHAT_CONTEXT_BUDGET_BYTES, &context) == CHAT_CONTEXT_OK,
        "the send mode's user message is a valid trigger");
    check(context.messages[context.count - 1].role == CHAT_ROLE_USER &&
        !wcscmp(context.messages[context.count - 1].text, expected),
        "the send mode's context ends with the triggering user message");
    check(context.bytes == expected_body(chat, &context),
        "the send mode's size matches the framing");
    chat_dispose(chat); free(chat);
}

static void test_send_modes(void) {
    check_mode(CHAT_SEND, L"new question", L"new question");
    check_mode(CHAT_RETRY, NULL, L"first");
    check_mode(CHAT_REGENERATE, NULL, L"first");
    check_mode(CHAT_EDIT_RESEND, L"edited", L"edited");
}

/* The budget must charge exactly the provider object the encoder writes, and
   nothing at all when every routing control is at OpenRouter's default. */
static void test_provider_routing(void) {
    Chat *chat = fresh_chat();
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"hello");
    const ChatConversation *c = active(chat);
    static ChatRequestContext context;
    size_t base = TEST_ENVELOPE + json_encoded_string_size(chat->model);
    size_t trigger_bytes = message_bytes(CHAT_ROLE_USER, L"hello");

    check(chat_provider_envelope_bytes(&chat->provider_routing) == 0,
        "default routing serializes to no provider object");
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "default routing builds");
    check(context.bytes == expected_body(chat, &context),
        "default routing reports the plain body size");

    chat->provider_routing.sort = CHAT_PROVIDER_SORT_THROUGHPUT;
    chat->provider_routing.disallow_fallbacks = true;
    chat->provider_routing.data_collection = CHAT_DATA_COLLECTION_DENY;
    chat->provider_routing.zdr = true;
    check(chat_provider_envelope_bytes(&chat->provider_routing) == 93,
        "the provider envelope has a pinned, exact byte size");
    size_t routing_bytes = chat_provider_envelope_bytes(&chat->provider_routing);
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) == CHAT_CONTEXT_OK,
        "non-default routing builds");
    check(context.bytes == base + routing_bytes + trigger_bytes,
        "the provider object is charged exactly once in the envelope");

    /* The trigger-only boundary moves by exactly the provider bytes, and the
       OVERSIZE diagnostic reports the routed body. */
    size_t trigger_only = base + routing_bytes + trigger_bytes;
    check(chat_context_build(chat, c, trigger, trigger_only, &context) == CHAT_CONTEXT_OK &&
        context.count == 1, "the trigger still fits at the routed size");
    check(chat_context_build(chat, c, trigger, trigger_only - 1, &context) ==
        CHAT_CONTEXT_OVERSIZE_USER, "one byte under the routed size fails explicitly");
    check(context.required_bytes == trigger_only,
        "the oversize diagnostic includes the provider object");
    /* The reported size is exactly what the appender writes for every
       combination of controls: the size helper is a pure summation with no
       failure path, so it can never disagree with or understate the output. */
    for (int bits = 0; bits < 16; bits++) {
        ChatProviderRouting routing;
        chat_provider_routing_init(&routing);
        routing.sort = (bits & 1) ? CHAT_PROVIDER_SORT_PRICE
            : CHAT_PROVIDER_SORT_DEFAULT;
        routing.disallow_fallbacks = (bits & 2) != 0;
        routing.data_collection = (bits & 4) ? CHAT_DATA_COLLECTION_DENY
            : CHAT_DATA_COLLECTION_ALLOW;
        routing.zdr = (bits & 8) != 0;
        JsonBuf buf;
        json_buf_init(&buf, 16);
        check(chat_provider_append(&buf, &routing), "the provider appender succeeds");
        size_t measured = chat_provider_envelope_bytes(&routing);
        check(measured == buf.length,
            "the reported provider size is exactly the appended size");
        if (measured)
            check(!strncmp(buf.data, ",\"provider\":{", 13) &&
                buf.data[buf.length - 1] == '}',
                "the provider envelope is well formed");
        json_buf_free(&buf);
    }
    chat_dispose(chat); free(chat);
}

/* ---- Commit 5: the ordered-part request view ---------------------------- */

/* Fixture: one attachment record, so an IMAGE part can resolve its rec. */
static void add_attachment(Chat *chat, uint64_t id, size_t bytes) {
    ChatAttachmentMeta rec;
    memset(&rec, 0, sizeof rec);
    rec.id = id;
    memset(rec.digest, 'a', 64);
    rec.digest[64] = 0;
    strcpy(rec.mime, "image/png");
    rec.bytes = bytes;
    rec.created_at = 1;
    wcscpy(rec.display_name, L"photo.png");
    check(chat_attachment_add(chat, &rec), "fixture: the attachment record is added");
}

/* Fixture: one image part on a message (promotes on first call). */
static void add_image(ChatMessage *m, uint64_t id, uint8_t flags,
    uint32_t w, uint32_t h) {
    ChatImagePart image;
    memset(&image, 0, sizeof image);
    image.attachment_id = id;
    image.pixel_width = w;
    image.pixel_height = h;
    strcpy(image.mime, "image/png");
    wcscpy(image.display_name, L"photo.png");
    check(chat_message_add_image(m, &image, flags),
        "fixture: the image part is added");
}

/* True when every placed run lies inside the scratch pool, runs are disjoint
   and ordered like the entries that own them, and part_slots_used is exactly
   the sum of the placed part counts. */
static int runs_consistent(const ChatRequestContext *context) {
    const ChatRequestPart *next = context->part_scratch;
    for (int i = 0; i < context->count; i++) {
        const ChatRequestMessage *entry = &context->messages[i];
        if (entry->part_count == 0) {
            if (entry->parts != NULL) return 0;
            continue;
        }
        if (entry->parts != next) return 0;
        if (entry->parts < context->part_scratch ||
            entry->parts + entry->part_count >
                context->part_scratch + context->part_slots_used) return 0;
        next += entry->part_count;
    }
    return next == context->part_scratch + context->part_slots_used;
}

static void test_view_parts(void) {
    Chat *chat = fresh_chat();
    add_attachment(chat, 1, 1000);
    add_attachment(chat, 2, 2000);
    add_attachment(chat, 3, 3000);

    /* Fast path with nonempty text: no run, identical bytes. */
    int plain = chat_append(chat, CHAT_ROLE_USER, L"plain question");
    int plain_answer = chat_append(chat, CHAT_ROLE_ASSISTANT, L"plain answer");
    chat->conversations[0].messages[plain_answer].generation.state =
        CHAT_GENERATION_COMPLETE;

    /* Promoted: text + two images. */
    int shown = chat_append(chat, CHAT_ROLE_USER, L"what's this?");
    ChatMessage *m = &chat->conversations[0].messages[shown];
    add_image(m, 1, 0, 2, 3);
    add_image(m, 2, CHAT_PART_FLAG_FIRST_FRAME, 4, 5);

    /* Image-only: empty projection. */
    int only = chat_append(chat, CHAT_ROLE_USER, L"");
    add_image(&chat->conversations[0].messages[only], 3, 0, 6, 7);

    int trigger = chat_append(chat, CHAT_ROLE_USER, L"follow up");
    add_image(&chat->conversations[0].messages[trigger], 4, 0, 8, 9);
    add_attachment(chat, 4, 4000);
    const ChatConversation *c = active(chat);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) ==
        CHAT_CONTEXT_OK, "a multimodal conversation builds");
    check(context.count == 5, "every message is placed");

    /* Fast-path entries carry no run even with nonempty text. */
    check(context.messages[0].parts == NULL &&
        context.messages[0].part_count == 0,
        "a text-only entry has no part run");
    check(context.messages[0].text == chat_message_text(&c->messages[plain]),
        "the fast-path entry still borrows the live text");

    /* The promoted run mirrors chat_message_part_count/at exactly. */
    const ChatRequestMessage *shown_entry = &context.messages[2];
    check(shown_entry->part_count == (int)chat_message_part_count(m),
        "the run length equals chat_message_part_count");
    check(shown_entry->part_count == 3, "text + two images is three parts");
    check(shown_entry->text == chat_message_text(m),
        "the entry text is the plain-text projection");
    for (size_t i = 0; i < chat_message_part_count(m); i++) {
        ChatPartView view;
        check(chat_message_part_at(m, i, &view), "the logical view is readable");
        const ChatRequestPart *part = &shown_entry->parts[i];
        check(part->kind == view.kind, "run kind mirrors the logical view");
        check(part->flags == view.flags, "run flags mirror the logical view");
        if (view.kind == CHAT_PART_TEXT) {
            check(part->u.text == view.u.text.data,
                "the TEXT run entry borrows the part payload");
        } else {
            check(part->u.image.meta->attachment_id ==
                view.u.image.attachment_id, "meta carries the attachment id");
            check(part->u.image.meta->pixel_width == view.u.image.pixel_width &&
                part->u.image.meta->pixel_height == view.u.image.pixel_height,
                "meta carries the display pixel size");
            check(part->u.image.rec == chat_attachment(chat,
                view.u.image.attachment_id),
                "rec resolves from the attachment table");
            check(part->u.image.byte_length == part->u.image.rec->bytes,
                "byte_length is the stored length");
            check(part->u.image.bytes == NULL,
                "no blob bytes are read at view construction");
        }
    }

    /* Image-only: empty projection, all-image run. */
    const ChatRequestMessage *only_entry = &context.messages[3];
    check(!wcscmp(only_entry->text, L""), "an image-only entry has an empty projection");
    check(only_entry->part_count == 1 &&
        only_entry->parts[0].kind == CHAT_PART_IMAGE,
        "an image-only entry carries just the image parts");

    /* Slot accounting: only placed multimodal messages consume slots, runs
       are disjoint and in entry order. */
    check(runs_consistent(&context), "runs lie in the scratch pool and add up");
    check(context.part_slots_used == 3 + 1 + 2,
        "part_slots_used counts the placed parts and nothing else");

    /* Byte-stability pin: at this commit the images cost nothing and the
       body is byte-identical to the projection-only framing. */
    check(context.bytes == expected_body(chat, &context),
        "parts do not change a single request byte yet");

    /* Idempotence: a second build of the same state fills the same views. */
    size_t count = (size_t)context.count;
    int slots = context.part_slots_used;
    ChatRequestMessage *saved_messages = (ChatRequestMessage *)malloc(
        count * sizeof *saved_messages);
    ChatRequestPart *saved_scratch = (ChatRequestPart *)malloc(
        (size_t)slots * sizeof *saved_scratch);
    check(saved_messages && saved_scratch, "fixture: the view copies are allocated");
    if (saved_messages && saved_scratch) {
        memcpy(saved_messages, context.messages, count * sizeof *saved_messages);
        memcpy(saved_scratch, context.part_scratch, (size_t)slots * sizeof *saved_scratch);
        check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) ==
            CHAT_CONTEXT_OK, "the second build succeeds");
        check(context.count == (int)count && context.part_slots_used == slots &&
            memcmp(saved_messages, context.messages, count * sizeof *saved_messages) == 0 &&
            memcmp(saved_scratch, context.part_scratch,
                (size_t)slots * sizeof *saved_scratch) == 0,
            "two builds of the same state produce identical views");
    }
    free(saved_messages); free(saved_scratch);

    chat_dispose(chat); free(chat);
}

/* Dropped multimodal history consumes no scratch and never resolves its
   attachments -- even a dangling reference in dropped history must not block
   the send, while a dangling reference in a placed message is fatal. */
static void test_view_dropped_and_dangling(void) {
    Chat *chat = fresh_chat();
    add_attachment(chat, 1, 500);

    /* Oldest: huge text + a DANGLING image (no attachment record at all). */
    wchar_t *huge = long_text(200000, L'x');
    int old = chat_append(chat, CHAT_ROLE_USER, huge);
    add_image(&chat->conversations[0].messages[old], 999, 0, 1, 1);
    int mid = chat_append(chat, CHAT_ROLE_ASSISTANT, L"old answer");
    chat->conversations[0].messages[mid].generation.state =
        CHAT_GENERATION_COMPLETE;
    int kept = chat_append(chat, CHAT_ROLE_USER, L"kept question");
    add_image(&chat->conversations[0].messages[kept], 1, 0, 2, 2);
    int trigger = chat_append(chat, CHAT_ROLE_USER, L"trigger");
    add_image(&chat->conversations[0].messages[trigger], 1, 0, 3, 3);
    const ChatConversation *c = active(chat);

    static ChatRequestContext context;
    check(chat_context_build(chat, c, trigger, CHAT_CONTEXT_BUDGET_BYTES,
        &context) == CHAT_CONTEXT_OK,
        "a dangling reference in dropped history does not block the send");
    check(context.count == 3 && context.dropped_messages == 1,
        "the huge turn is dropped and the rest is kept");
    check(runs_consistent(&context), "the kept runs still add up");
    check(context.part_slots_used == 2 + 2,
        "the dropped multimodal message consumes no scratch");
    check(context.messages[0].part_count == 0 &&
        context.messages[1].part_count == 2 &&
        context.messages[2].part_count == 2,
        "each placed entry carries exactly its own parts");

    /* A dangling reference in a placed history message is fatal, and the
       partial fill is reset to the zeroed INVALID output. */
    chat_message_clear_parts(&chat->conversations[0].messages[old]);
    chat_message_clear_parts(&chat->conversations[0].messages[kept]);
    add_image(&chat->conversations[0].messages[kept], 888, 0, 2, 2);
    memset(&context, 0xa5, sizeof context);
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) ==
        CHAT_CONTEXT_INVALID && zeroed_output(&context),
        "a dangling reference in placed history fails and zeroes the output");

    /* The same for a dangling reference on the trigger itself. */
    chat_message_clear_parts(&chat->conversations[0].messages[kept]);
    add_image(&chat->conversations[0].messages[kept], 1, 0, 2, 2);
    chat_message_clear_parts(&chat->conversations[0].messages[trigger]);
    add_image(&chat->conversations[0].messages[trigger], 777, 0, 3, 3);
    memset(&context, 0xa5, sizeof context);
    check(chat_context_build(chat, c, trigger, SIZE_MAX, &context) ==
        CHAT_CONTEXT_INVALID && zeroed_output(&context),
        "a dangling reference on the trigger fails and zeroes the output");

    free(huge);
    chat_dispose(chat); free(chat);
}

int main(void) {
    test_order_and_identity();
    test_oldest_dropped_first();
    test_exclusions();
    test_system_prompt();
    test_effective_prompt();
    test_oversize();
    test_invalid();
    test_beyond_trigger();
    test_huge_dropped_history();
    test_read_only();
    test_budget_sweep();
    test_send_modes();
    test_provider_routing();
    test_view_parts();
    test_view_dropped_and_dangling();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    puts("Bounded request context: budget, oldest-first dropping, eligibility, "
        "diagnostics, read-only access, send-mode and part-view tests passed");
    return 0;
}

