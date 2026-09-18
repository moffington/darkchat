#include "../chat/commands.h"
#include "../chat/chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

#define ARRAY_LEN(a) (sizeof (a) / sizeof (a)[0])

static void reset(Chat *chat) {
    chat_dispose(chat);
    memset(chat, 0, sizeof *chat);
    chat_init(chat);
    chat_clear(chat);
}

static void add_turn(Chat *chat, ChatGenerationState state) {
    chat_append(chat, CHAT_ROLE_USER, L"prompt");
    int index = chat_append(chat, CHAT_ROLE_ASSISTANT, L"answer");
    chat->conversations[chat->active].messages[index].generation.state = state;
}

static void test_table(void) {
    size_t count = 0;
    const ChatActionInfo *table = chat_action_table(&count);
    check(table != NULL, "registry table is present");
    check(count == (size_t)(ACTION_LAST - ACTION_FIRST + 1),
        "registry has one entry per action id");
    bool unique = true, valid = true, grouped = true;
    for (size_t i = 0; i < count; i++) {
        if (table[i].id < ACTION_FIRST || table[i].id > ACTION_LAST ||
            !table[i].menu_label || !table[i].menu_label[0]) valid = false;
        if (i && table[i].group < table[i - 1].group) grouped = false;
        for (size_t k = i + 1; k < count; k++)
            if (table[i].id == table[k].id) unique = false;
    }
    check(unique, "every action id appears at most once");
    check(valid, "every entry has an in-range id and a label");
    check(grouped, "table is ordered by group");
    for (int id = ACTION_FIRST; id <= ACTION_LAST; id++)
        check(chat_action_info(id) != NULL, "every action id resolves in the registry");
    check(chat_action_info(ACTION_FIRST - 1) == NULL &&
        chat_action_info(ACTION_LAST + 1) == NULL,
        "out-of-range ids do not resolve");
}

static void test_labels(void) {
    size_t count = 0;
    const ChatActionInfo *table = chat_action_table(&count);
    bool one_mnemonic = true, plain_unique = true;
    for (size_t i = 0; i < count; i++) {
        int marks = 0;
        for (const wchar_t *p = table[i].menu_label; *p; p++)
            if (*p == L'&') ++marks;
        if (marks > 1) one_mnemonic = false;

        wchar_t plain[CHAT_ACTION_LABEL_TEXT];
        check(chat_action_plain_label(table[i].id, plain, CHAT_ACTION_LABEL_TEXT),
            "plain label renders");
        check(plain[0] != 0, "plain label is non-empty");
        check(wcschr(plain, L'&') == NULL, "plain label carries no mnemonic");
        for (size_t k = 0; k < count; k++) {
            if (k == i) continue;
            wchar_t other[CHAT_ACTION_LABEL_TEXT];
            if (chat_action_plain_label(table[k].id, other,
                    CHAT_ACTION_LABEL_TEXT) && !wcscmp(plain, other))
                plain_unique = false;
        }
    }
    check(one_mnemonic, "menu labels carry at most one mnemonic");
    check(plain_unique, "plain display labels are unique");
    check(chat_action_menu_label(ACTION_NEW) != NULL, "menu label lookup resolves");
    check(chat_action_menu_label(0) == NULL && chat_action_shortcut(0) == NULL,
        "unknown label and shortcut lookups are safe");
    check(chat_action_shortcut(ACTION_RENAME) != NULL &&
        !wcscmp(chat_action_shortcut(ACTION_RENAME), L"F2"),
        "Rename exposes the F2 binding");
    check(chat_action_shortcut(ACTION_DELETE) != NULL &&
        !wcscmp(chat_action_shortcut(ACTION_DELETE), L"Del"),
        "Delete exposes the Del binding");
    wchar_t tiny[6];
    check(!chat_action_plain_label(ACTION_REGENERATE, tiny, 6),
        "truncated plain label reports failure");
    check(tiny[5] == 0, "truncated plain label is terminated");
    wchar_t none[8];
    check(!chat_action_plain_label(0, none, 8), "unknown id has no plain label");
    check(!chat_action_plain_label(ACTION_NEW, none, 0), "zero capacity fails safely");
}

static void expect_disabled(const ChatActionContext *context, const char *state,
    const int *disabled, size_t count) {
    bool ok = true;
    for (int id = ACTION_FIRST; id <= ACTION_LAST; id++) {
        bool want = true;
        for (size_t i = 0; i < count; i++)
            if (disabled[i] == id) { want = false; break; }
        if (chat_action_available(id, context) != want) {
            if (ok) printf("FAIL: %s availability\n", state);
            printf("  action %d expected %s\n", id, want ? "enabled" : "disabled");
            ++failures;
            ok = false;
        }
    }
    if (ok) printf("ok: %s availability matrix\n", state);
}

static void test_availability_matrix(Chat *chat) {
    ChatActionContext context;

    reset(chat);
    chat_action_context_init(&context, chat);
    const int empty_idle[] = { ACTION_COPY, ACTION_SELECTION, ACTION_RETRY,
        ACTION_REGENERATE, ACTION_EDIT, ACTION_CANCEL_EDIT };
    expect_disabled(&context, "empty idle", empty_idle, ARRAY_LEN(empty_idle));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    const int complete_idle[] = { ACTION_SELECTION, ACTION_RETRY, ACTION_CANCEL_EDIT };
    expect_disabled(&context, "complete idle", complete_idle, ARRAY_LEN(complete_idle));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_FAILED);
    chat_action_context_init(&context, chat);
    const int failed_idle[] = { ACTION_SELECTION, ACTION_CANCEL_EDIT };
    expect_disabled(&context, "failed idle", failed_idle, ARRAY_LEN(failed_idle));

    reset(chat);
    chat_append(chat, CHAT_ROLE_USER, L"only");
    chat_action_context_init(&context, chat);
    const int user_only[] = { ACTION_COPY, ACTION_SELECTION, ACTION_CANCEL_EDIT };
    expect_disabled(&context, "user only idle", user_only, ARRAY_LEN(user_only));

    const int generating[] = { ACTION_RENAME, ACTION_DELETE, ACTION_DELETE_ALL,
        ACTION_CLEAR, ACTION_RETRY, ACTION_REGENERATE, ACTION_EDIT,
        ACTION_CANCEL_EDIT, ACTION_SELECTION, ACTION_SYSTEM, ACTION_SIDEBAR,
        ACTION_MODELS, ACTION_BACKEND_OPENROUTER, ACTION_BACKEND_OLLAMA,
        ACTION_ROUTING_SORT_DEFAULT, ACTION_ROUTING_SORT_PRICE,
        ACTION_ROUTING_SORT_THROUGHPUT, ACTION_ROUTING_SORT_LATENCY,
        ACTION_ROUTING_ALLOW_FALLBACKS, ACTION_ROUTING_DATA_COLLECTION,
        ACTION_ROUTING_ZDR };
    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    context.generating = true;
    expect_disabled(&context, "complete generating", generating, ARRAY_LEN(generating));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_RUNNING);
    chat_action_context_init(&context, chat);
    context.generating = true;
    expect_disabled(&context, "running generating", generating, ARRAY_LEN(generating));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    context.editing = true;
    const int editing[] = { ACTION_SELECTION, ACTION_RETRY };
    expect_disabled(&context, "editing idle", editing, ARRAY_LEN(editing));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    context.has_transcript_selection = true;
    const int selection[] = { ACTION_RETRY, ACTION_CANCEL_EDIT };
    expect_disabled(&context, "selection idle", selection, ARRAY_LEN(selection));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat->backend = CHAT_BACKEND_OLLAMA;
    chat_action_context_init(&context, chat);
    const int ollama_idle[] = { ACTION_SELECTION, ACTION_RETRY, ACTION_CANCEL_EDIT,
        ACTION_ROUTING_SORT_DEFAULT, ACTION_ROUTING_SORT_PRICE,
        ACTION_ROUTING_SORT_THROUGHPUT, ACTION_ROUTING_SORT_LATENCY,
        ACTION_ROUTING_ALLOW_FALLBACKS, ACTION_ROUTING_DATA_COLLECTION,
        ACTION_ROUTING_ZDR };
    expect_disabled(&context, "Ollama idle", ollama_idle, ARRAY_LEN(ollama_idle));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat->backend = CHAT_BACKEND_OLLAMA;
    chat_action_context_init(&context, chat);
    context.generating = true;
    expect_disabled(&context, "Ollama generating", generating, ARRAY_LEN(generating));
}

static void test_null_safety(Chat *chat) {
    check(!chat_action_available(ACTION_NEW, NULL), "null context is unavailable");
    ChatActionContext context;
    memset(&context, 0xff, sizeof context);
    chat_action_context_init(&context, NULL);
    check(context.chat == NULL && !context.has_latest_user &&
        !context.backend_openrouter, "null chat yields a zeroed context");
    chat_action_context_init(NULL, chat);
}

int main(void) {
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!chat) return 2;
    chat_init(chat);
    test_table();
    test_labels();
    test_availability_matrix(chat);
    test_null_safety(chat);
    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d command check(s) failed\n", failures); return 1; }
    printf("\nall command checks passed\n");
    return 0;
}
