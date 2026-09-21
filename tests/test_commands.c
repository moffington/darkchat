#include "chat/core/commands.h"
#include "chat/core/chat.h"
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
        ACTION_REGENERATE, ACTION_EDIT, ACTION_CANCEL_EDIT,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "empty idle", empty_idle, ARRAY_LEN(empty_idle));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    const int complete_idle[] = { ACTION_SELECTION, ACTION_RETRY, ACTION_CANCEL_EDIT,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "complete idle", complete_idle, ARRAY_LEN(complete_idle));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_FAILED);
    chat_action_context_init(&context, chat);
    const int failed_idle[] = { ACTION_SELECTION, ACTION_CANCEL_EDIT,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "failed idle", failed_idle, ARRAY_LEN(failed_idle));

    reset(chat);
    chat_append(chat, CHAT_ROLE_USER, L"only");
    chat_action_context_init(&context, chat);
    const int user_only[] = { ACTION_COPY, ACTION_SELECTION, ACTION_CANCEL_EDIT,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "user only idle", user_only, ARRAY_LEN(user_only));

    const int generating[] = { ACTION_RENAME, ACTION_DELETE, ACTION_DELETE_ALL,
        ACTION_CLEAR, ACTION_RETRY, ACTION_REGENERATE, ACTION_EDIT,
        ACTION_CANCEL_EDIT, ACTION_SELECTION, ACTION_SYSTEM, ACTION_SIDEBAR,
        ACTION_MODELS, ACTION_BACKEND_OPENROUTER, ACTION_BACKEND_OLLAMA,
        ACTION_MODEL_USE_HERE, ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_HERE,
        ACTION_SYSTEM_CLEAR_HERE,         ACTION_PROFILE_APPLY, ACTION_PROFILE_SAVE,
        ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE,
        ACTION_EXPORT_MARKDOWN, ACTION_EXPORT_JSON, ACTION_EXPORT_ALL,
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
    const int editing[] = { ACTION_SELECTION, ACTION_RETRY,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "editing idle", editing, ARRAY_LEN(editing));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat_action_context_init(&context, chat);
    context.has_transcript_selection = true;
    const int selection[] = { ACTION_RETRY, ACTION_CANCEL_EDIT,
        ACTION_MODEL_CLEAR_HERE, ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE };
    expect_disabled(&context, "selection idle", selection, ARRAY_LEN(selection));

    reset(chat);
    add_turn(chat, CHAT_GENERATION_COMPLETE);
    chat->backend = CHAT_BACKEND_OLLAMA;
    chat_action_context_init(&context, chat);
    const int ollama_idle[] = { ACTION_SELECTION, ACTION_RETRY, ACTION_CANCEL_EDIT,
        ACTION_MODEL_USE_HERE, ACTION_MODEL_CLEAR_HERE,
        ACTION_SYSTEM_CLEAR_HERE,
        ACTION_PROFILE_APPLY, ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE,
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

/* The dynamic profile submenu ids are deliberately outside the static
    registry: id math, bounds, dispatch safety and their availability are
    tested here, separate from the ACTION_FIRST..ACTION_LAST walks. */
static void test_dynamic_ranges(void) {
    const int bases[] = { CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE,
        CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE, CHAT_ACTION_DYNAMIC_EDIT_BASE,
        CHAT_ACTION_DYNAMIC_DELETE_BASE };
    check((int)CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE > (int)ACTION_LAST,
        "dynamic ids start above the static registry");
    check(CHAT_ACTION_DYNAMIC_END <= 0xFFFF,
        "dynamic ids stay below the WM_COMMAND LOWORD ceiling");
    for (size_t r = 0; r < ARRAY_LEN(bases); r++) {
        check(bases[r] + CHAT_MAX_PROMPT_PROFILES <=
            (r + 1 < ARRAY_LEN(bases) ? bases[r + 1]
                                      : CHAT_ACTION_DYNAMIC_END),
            "each dynamic range holds exactly the profile cap");
        for (int i = 0; i < CHAT_MAX_PROMPT_PROFILES; i++)
            check(chat_action_dynamic_profile_index(bases[r] + i) == i,
                "every in-range dynamic id resolves to its index");
    }
    check(chat_action_dynamic_profile_index(
        CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE - 1) == -1 &&
        chat_action_dynamic_profile_index(0) == -1 &&
        chat_action_dynamic_profile_index(CHAT_ACTION_DYNAMIC_END) == -1 &&
        chat_action_dynamic_profile_index(ACTION_LAST) == -1,
        "static, zero and out-of-range ids resolve to no profile");
}

/* The Customization group's context fields and their availability. */
static void test_customization_context(Chat *chat) {
    ChatActionContext context;
    reset(chat);
    chat_action_context_init(&context, chat);
    check(context.profile_count == 0 && !context.has_model_override &&
        !context.has_prompt_override && context.has_global_model,
        "a fresh chat has no overrides and a default global model");
    check(chat_action_available(ACTION_MODEL_USE_HERE, &context) &&
        !chat_action_available(ACTION_MODEL_CLEAR_HERE, &context) &&
        !chat_action_available(ACTION_SYSTEM_CLEAR_HERE, &context) &&
        chat_action_available(ACTION_SYSTEM_HERE, &context) &&
        chat_action_available(ACTION_PROFILE_SAVE, &context),
        "an uncustomized chat offers the copy-in, prompt edit and Save");
    check(chat_conversation_set_model(chat, 0, CHAT_BACKEND_OPENROUTER,
            L"here/model"),
        "fixture: a model override is set");
    chat_action_context_init(&context, chat);
    check(context.has_model_override && context.has_global_model,
        "the override context tracks presence for the active backend");
    check(!chat_action_available(ACTION_MODEL_USE_HERE, &context) &&
        chat_action_available(ACTION_MODEL_CLEAR_HERE, &context),
        "an active override flips Use and Clear");
    check(chat_conversation_set_model(chat, 0, CHAT_BACKEND_OPENROUTER, L""),
        "fixture: the model override is cleared");
    check(chat_conversation_apply_system_prompt(chat, 0, L""),
        "fixture: the empty prompt override is applied");
    chat_action_context_init(&context, chat);
    check(context.has_prompt_override &&
        chat_action_available(ACTION_SYSTEM_CLEAR_HERE, &context),
        "the deliberately-empty override still counts as an override");
    check(chat_conversation_set_system_prompt(chat, 0, L""),
        "fixture: the prompt override is cleared");
    check(chat_profile_add(chat, L"Terse", L"You are terse.") == 0 &&
        chat_profile_add(chat, L"Plain", L"") == 1,
        "fixture: two profiles exist");
    chat_action_context_init(&context, chat);
    check(context.profile_count == 2 &&
        chat_action_available(ACTION_PROFILE_APPLY, &context) &&
        chat_action_available(ACTION_PROFILE_EDIT, &context) &&
        chat_action_available(ACTION_PROFILE_DELETE, &context),
        "profiles enable Apply, Edit and Delete");
    context.profile_count = CHAT_MAX_PROMPT_PROFILES;
    check(!chat_action_available(ACTION_PROFILE_SAVE, &context) &&
        chat_action_available(ACTION_PROFILE_APPLY, &context),
        "the profile cap disables Save but not Apply");
    check(chat_profile_remove(chat, 0) && chat_profile_remove(chat, 0),
        "fixture: the profiles are removed");
    /* Dynamic availability follows the live census: below the count works,
        at and above it does not, and generating disables everything. */
    chat_profile_add(chat, L"Only", L"p");
    chat_action_context_init(&context, chat);
    for (int r = 0; r < 4; r++) {
        int base = CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE +
            r * CHAT_MAX_PROMPT_PROFILES;
        check(chat_action_available(base, &context) &&
            !chat_action_available(base + 1, &context),
            "dynamic ids resolve only below the live profile count");
    }
    context.generating = true;
    check(!chat_action_available(CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE,
            &context),
        "generating disables dynamic ids");
    chat_profile_remove(chat, 0);
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
    test_dynamic_ranges();
    test_availability_matrix(chat);
    test_customization_context(chat);
    test_null_safety(chat);
    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d command check(s) failed\n", failures); return 1; }
    printf("\nall command checks passed\n");
    return 0;
}
