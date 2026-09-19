#include "chat/core/search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* One allocation sequence shared by malloc and realloc. chat.bat links this
   test with both wrappers so the Nth allocation made by chat_search_build can
   be failed deterministically without affecting fixture setup or assertions. */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
static bool allocation_armed;
static long allocation_count, fail_allocation;

static bool allocation_fails(void) {
    if (!allocation_armed) return false;
    ++allocation_count;
    return allocation_count == fail_allocation;
}

void *__wrap_malloc(size_t size) {
    if (allocation_fails()) return NULL;
    return __real_malloc(size);
}

void *__wrap_realloc(void *pointer, size_t size) {
    if (allocation_fails()) return NULL;
    return __real_realloc(pointer, size);
}

static void check(bool condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

static void reset(Chat *chat) {
    chat_dispose(chat);
    memset(chat, 0, sizeof *chat);
    chat_init(chat);
    chat_clear(chat);
}

static void test_scope_and_folding(Chat *chat) {
    ChatSearchResults results = {0};
    ChatSearchOptions body_only = { false }, with_reasoning = { true };
    reset(chat);
    int user = chat_append(chat, CHAT_ROLE_USER,
        L"Stra\u00dfe and \u00c4PFEL with \U0001f600");
    int assistant = chat_append(chat, CHAT_ROLE_ASSISTANT, L"Visible analysis");
    check(user == 0 && assistant == 1, "scope fixture appends");
    check(chat_message_set_reasoning(
        &chat->conversations[0].messages[assistant], L"Hidden \u00c4NALYSIS"),
        "reasoning fixture sets");
    wcscpy(chat->conversations[0].title, L"title-only-token");
    wcscpy(chat->conversations[0].draft, L"draft-only-token");

    check(chat_search_build(chat, L"\u00e4pfel", body_only, &results) &&
        results.count == 1 && results.items[0].field == CHAT_SEARCH_BODY,
        "ordinal Unicode case folding matches Latin case variants");
    check(results.items[0].conversation_id == chat->conversations[0].id &&
        results.items[0].message_id == chat->conversations[0].messages[user].id,
        "results retain stable identities");
    check(chat_search_build(chat, L"STRASSE", body_only, &results) &&
        results.count == 0,
        "length-changing case folds remain distinct");
    check(chat_search_build(chat, L"\U0001f600", body_only, &results) &&
        results.count == 1,
        "supplementary Unicode matches without splitting its surrogate pair");
    check(chat_search_build(chat, L"e\u0301", body_only, &results) &&
        results.count == 0,
        "canonically different Unicode spellings are not normalized");
    check(chat_search_build(chat, L"hidden \u00e4nalysis", body_only, &results) &&
        results.count == 0,
        "reasoning is excluded by option");
    check(chat_search_build(chat, L"hidden \u00e4nalysis", with_reasoning,
        &results) && results.count == 1 &&
        results.items[0].field == CHAT_SEARCH_REASONING,
        "reasoning is included by option");
    check(chat_search_build(chat, L"nalysis", with_reasoning, &results) &&
        results.count == 2 && results.items[0].field == CHAT_SEARCH_BODY &&
        results.items[1].field == CHAT_SEARCH_REASONING,
        "body precedes reasoning and each matching field contributes once");
    check(chat_search_build(chat, L"title-only-token", with_reasoning,
        &results) && results.count == 0,
        "conversation titles are outside message-content search");
    check(chat_search_build(chat, L"draft-only-token", with_reasoning,
        &results) && results.count == 0,
        "drafts are outside message-content search");
    chat_search_results_dispose(&results);
}

static void test_snippets_and_storage(Chat *chat) {
    ChatSearchResults results = {0};
    ChatSearchOptions options = { false };
    reset(chat);
    wchar_t text[241];
    for (int i = 0; i < 240; i++) text[i] = L'a';
    text[240] = 0;
    memcpy(text + 116, L"\r\nNeedle\t", 9 * sizeof(wchar_t));
    check(chat_append(chat, CHAT_ROLE_USER, text) == 0, "long snippet fixture appends");
    check(chat_search_build(chat, L"needle", options, &results) &&
        results.count == 1, "long snippet search succeeds");
    check(wcslen(results.items[0].snippet) < CHAT_SEARCH_SNIPPET &&
        !wcsncmp(results.items[0].snippet, L"...", 3) &&
        wcslen(results.items[0].snippet) >= 3 &&
        !wcscmp(results.items[0].snippet +
            wcslen(results.items[0].snippet) - 3, L"..."),
        "long snippet is bounded and marks both omitted sides");
    check(wcsstr(results.items[0].snippet, L"Needle") != NULL &&
        !wcschr(results.items[0].snippet, L'\r') &&
        !wcschr(results.items[0].snippet, L'\n') &&
        !wcschr(results.items[0].snippet, L'\t'),
        "snippet retains the match and flattens layout whitespace");
    check(results.items[0].match_offset == 118 &&
        results.items[0].match_length == 6,
        "result offsets name source text rather than the display snippet");

    reset(chat);
    for (int i = 0; i < 12; i++) {
        wchar_t item[32];
        swprintf(item, 32, L"dynamic result %d", i);
        chat_append(chat, CHAT_ROLE_USER, item);
    }
    wchar_t query[] = L"dynamic";
    check(chat_search_build(chat, query, options, &results) &&
        results.count == 12 && results.capacity >= results.count,
        "result storage grows dynamically beyond its initial allocation");
    query[0] = L'x';
    check(results.query && !wcscmp(results.query, L"dynamic"),
        "results own the query used to validate matches");
    check(chat_search_build(chat, L"", options, &results) &&
        results.count == 0 && results.items == NULL && results.query == NULL,
        "empty query clears retained results");
    chat_search_results_dispose(&results);
}

static void test_resolution_and_invalidation(Chat *chat) {
    ChatSearchResults results = {0};
    ChatSearchOptions options = { true };
    ChatSearchTarget target;
    reset(chat);
    chat_append(chat, CHAT_ROLE_USER, L"first conversation");
    int first_assistant = chat_append(chat, CHAT_ROLE_ASSISTANT, L"compact target");
    (void)first_assistant;
    int second = chat_new_conversation(chat);
    chat_append(chat, CHAT_ROLE_USER, L"stable user target");
    int assistant = chat_append(chat, CHAT_ROLE_ASSISTANT, L"stable assistant target");
    chat->conversations[second].messages[assistant].generation.state =
        CHAT_GENERATION_COMPLETE;
    chat_message_touch(&chat->conversations[second].messages[assistant]);
    uint64_t conversation_id = chat->conversations[second].id;
    uint64_t assistant_id = chat->conversations[second].messages[assistant].id;

    check(chat_search_build(chat, L"stable assistant", options, &results) &&
        results.count == 1, "stable target search succeeds");
    chat_append(chat, CHAT_ROLE_ERROR, L"unrelated growth");
    check(chat_search_resolve(chat, &results, 0, &target) &&
        target.conversation == second && target.message == assistant,
        "unrelated message growth does not invalidate a result");

    chat_select_conversation(chat, 0);
    check(chat_delete(chat), "earlier conversation deletes");
    check(chat_search_resolve(chat, &results, 0, &target) &&
        target.conversation == 0 &&
        chat->conversations[target.conversation].id == conversation_id &&
        chat->conversations[target.conversation].messages[target.message].id ==
            assistant_id,
        "stable ids resolve after conversation compaction");

    ChatConversation *conversation = &chat->conversations[0];
    uint64_t user_id = conversation->messages[0].id;
    check(chat_search_build(chat, L"stable user", options, &results) &&
        chat_search_resolve(chat, &results, 0, &target),
        "editable stable user result resolves initially");
    check(chat_message_set_text(&conversation->messages[0],
        L"stable user target edited") && conversation->messages[0].id == user_id,
        "edit preserves stable user identity");
    check(!chat_search_resolve(chat, &results, 0, &target),
        "observable edit invalidates the old result revision");

    check(chat_search_build(chat, L"stable assistant", options, &results) &&
        results.count == 1, "replacement target refreshes");
    uint64_t replaced_id = conversation->messages[assistant].id;
    int replacement = chat_begin_response(chat, CHAT_REGENERATE, NULL);
    check(replacement == assistant &&
        conversation->messages[replacement].id != replaced_id,
        "regenerate replaces the assistant with a fresh identity");
    check(!chat_search_resolve(chat, &results, 0, &target),
        "replaced message result cannot resolve to the reused slot");

    chat_message_set_text(&conversation->messages[replacement], L"delete target");
    check(chat_search_build(chat, L"delete target", options, &results) &&
        results.count == 1, "deletion target refreshes");
    chat_clear(chat);
    check(!chat_search_resolve(chat, &results, 0, &target),
        "deleted message result is invalid");
    chat_search_results_dispose(&results);
}

static void test_transactional_allocation_failure(Chat *chat) {
    ChatSearchResults results = {0};
    ChatSearchOptions options = { false };
    reset(chat);
    for (int i = 0; i < 12; i++) {
        wchar_t text[64];
        swprintf(text, 64, L"replacement-token %d%ls", i,
            i == 0 ? L" preserved-token" : L"");
        chat_append(chat, CHAT_ROLE_USER, text);
    }
    check(chat_search_build(chat, L"preserved-token", options, &results) &&
        results.count == 1, "recognizable prior results are established");
    wchar_t *saved_query = results.query;
    ChatSearchResult *saved_items = results.items;
    size_t saved_count = results.count, saved_capacity = results.capacity;
    ChatSearchResult saved_result = results.items[0];

    int failed_points = 0;
    for (long point = 1;; point++) {
        allocation_count = 0;
        fail_allocation = point;
        allocation_armed = true;
        bool built = chat_search_build(chat, L"replacement-token", options,
            &results);
        allocation_armed = false;
        if (built) {
            check(point == 4 && allocation_count == 3,
                "every search-build allocation point was failed once");
            break;
        }
        ++failed_points;
        check(results.query == saved_query && results.items == saved_items &&
            results.count == saved_count && results.capacity == saved_capacity &&
            !wcscmp(results.query, L"preserved-token") &&
            !memcmp(&results.items[0], &saved_result, sizeof saved_result),
            "allocation failure leaves existing search results byte-for-byte intact");
    }
    check(failed_points == 3 && results.count == 12 &&
        results.query && !wcscmp(results.query, L"replacement-token"),
        "search replacement commits after all failure points are exhausted");
    chat_search_results_dispose(&results);
}

int main(void) {
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!chat) return 2;
    test_scope_and_folding(chat);
    test_snippets_and_storage(chat);
    test_resolution_and_invalidation(chat);
    test_transactional_allocation_failure(chat);
    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d search check(s) failed\n", failures); return 1; }
    printf("\nall search checks passed\n");
    return 0;
}
