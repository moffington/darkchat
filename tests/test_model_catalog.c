/* Pure model-catalog parsing, merging and filtering. Includes the module so
   the transactional allocation failures can be driven with wrapped malloc,
   realloc and free (the same way test_chat exercises growth), and the
   outstanding-allocation count can prove nothing leaks through failure
   paths. */
#include "chat/models/model_catalog.c"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static long alloc_fail_countdown = -1;
static long outstanding;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    void *block = __real_malloc(size);
    if (block) ++outstanding;
    return block;
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    void *block = __real_realloc(pointer, size);
    if (block && !pointer) ++outstanding;
    return block;
}
void __wrap_free(void *pointer) {
    if (pointer) --outstanding;
    __real_free(pointer);
}

static const char *basic_json =
    "{\"data\":["
    "{\"id\":\"openai/gpt-4\",\"name\":\"GPT-4\",\"context_length\":8192},"
    "{\"id\":\"anthropic/claude-3\",\"name\":\"Claude 3\",\"context_length\":200000},"
    "{\"id\":\"meta-llama/llama-3\",\"name\":\"Llama \\u2014 3\"}"
    "]}";

static int parse_basic(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    CHECK(stats.parsed == 3 && stats.invalid == 0 && stats.duplicate == 0);
    CHECK(stats.too_long == 0 && stats.truncated == 0);
    CHECK(catalog.count == 3);
    CHECK(!wcscmp(catalog.items[0].id, L"openai/gpt-4"));
    CHECK(!wcscmp(catalog.items[0].name, L"GPT-4"));
    CHECK(catalog.items[0].context_length == 8192);
    CHECK(catalog.items[2].context_length == -1);
    CHECK(wcschr(catalog.items[2].name, 0x2014) != NULL);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int parse_required_and_types(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    /* id missing, id empty, id wrong type -> invalid; the valid entry keeps an
       empty name and an unknown context length. */
    const char *json =
        "{\"data\":["
        "{\"name\":\"no id\"},"
        "{\"id\":\"\",\"name\":\"empty\"},"
        "{\"id\":123,\"name\":\"wrong\"},"
        "{\"id\":\"ok/model\",\"name\":42,\"context_length\":\"big\"}"
        "]}";
    CHECK(chat_model_catalog_parse(&catalog, json, &stats));
    CHECK(stats.parsed == 1 && stats.invalid == 3);
    CHECK(catalog.count == 1 && !wcscmp(catalog.items[0].id, L"ok/model"));
    CHECK(!catalog.items[0].name[0] && catalog.items[0].context_length == -1);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int parse_duplicates_and_lengths(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    wchar_t long_id[200];
    for (int i = 0; i < 100; i++) long_id[i] = L'a';
    long_id[100] = 0;
    char json[512];
    char utf8[256];
    WideCharToMultiByte(CP_UTF8, 0, long_id, -1, utf8, sizeof utf8, NULL, NULL);
    snprintf(json, sizeof json,
        "{\"data\":[{\"id\":\"dup/model\"},{\"id\":\"dup/model\"},"
        "{\"id\":\"%s\"},{\"id\":\"ok/model\"}]}", utf8);
    CHECK(chat_model_catalog_parse(&catalog, json, &stats));
    CHECK(stats.parsed == 2 && stats.duplicate == 1 && stats.too_long == 1);
    CHECK(catalog.count == 2);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int parse_malformed_root(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    size_t before = catalog.count;
    CHECK(!chat_model_catalog_parse(&catalog, "{bad json", &stats));
    CHECK(catalog.count == before);
    CHECK(!chat_model_catalog_parse(&catalog, "{\"data\":{}}", &stats));
    CHECK(catalog.count == before);
    CHECK(chat_model_catalog_parse(&catalog, "{\"data\":[]}", &stats));
    CHECK(catalog.count == 0 && stats.parsed == 0);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int parse_cap(void) {
    static char big[400000];
    size_t offset = 0;
    offset += (size_t)snprintf(big + offset, sizeof big - offset, "{\"data\":[");
    for (int i = 0; i < CHAT_MODEL_CATALOG_MAX + 4; i++)
        offset += (size_t)snprintf(big + offset, sizeof big - offset,
            "%s{\"id\":\"p/m%d\"}", i ? "," : "", i);
    snprintf(big + offset, sizeof big - offset, "]}");
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, big, &stats));
    CHECK(catalog.count == CHAT_MODEL_CATALOG_MAX);
    CHECK(stats.parsed == CHAT_MODEL_CATALOG_MAX && stats.truncated == 4);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int merge_order(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    wchar_t history[2][CHAT_MODEL_TEXT];
    wcscpy(history[0], L"openai/gpt-4");     /* duplicate of catalog */
    wcscpy(history[1], L"old/model");        /* history-only */
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"typed/model", history, 2,
        &merged));
    /* current first, then history MRU (duplicate skipped), then catalog. */
    CHECK(merged.count == 5);
    CHECK(!wcscmp(merged.items[0].id, L"typed/model"));
    CHECK(!wcscmp(merged.items[1].id, L"openai/gpt-4"));
    CHECK(!wcscmp(merged.items[2].id, L"old/model"));
    CHECK(!wcscmp(merged.items[3].id, L"anthropic/claude-3"));
    CHECK(!wcscmp(merged.items[4].id, L"meta-llama/llama-3"));
    /* An over-capacity current/current history entry is skipped, never stored. */
    ChatModelCatalog merged2;
    chat_model_catalog_init(&merged2);
    CHECK(chat_model_catalog_merged(&catalog, NULL, NULL, 0, &merged2));
    CHECK(merged2.count == 3);
    chat_model_catalog_dispose(&merged2);
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int filter_matches(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    const ChatModelInfo *found[8];
    CHECK(chat_model_catalog_filter(&catalog, L"", found, 8) == 3);
    CHECK(chat_model_catalog_filter(&catalog, L"GPT", found, 8) == 1);
    CHECK(!wcscmp(found[0]->id, L"openai/gpt-4"));
    /* Substring of the id that is absent from every display name. */
    CHECK(chat_model_catalog_filter(&catalog, L"llama", found, 8) == 1);
    CHECK(!wcscmp(found[0]->id, L"meta-llama/llama-3"));
    /* Substring of a display name that is absent from the id. */
    CHECK(chat_model_catalog_filter(&catalog, L"Claude", found, 8) == 1);
    CHECK(!wcscmp(found[0]->id, L"anthropic/claude-3"));
    CHECK(chat_model_catalog_filter(&catalog, L"missing", found, 8) == 0);
    /* The count is complete even when max is smaller than the match set. */
    CHECK(chat_model_catalog_filter(&catalog, L"", found, 1) == 3);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int parse_transactional_oom(void) {
    const char *other =
        "{\"data\":[{\"id\":\"other/model\",\"name\":\"Other\"}]}";
    for (long fail_at = 0; fail_at < 12; fail_at++) {
        ChatModelCatalog catalog;
        chat_model_catalog_init(&catalog);
        ChatModelParseStats stats;
        CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
        CHECK(catalog.count == 3);
        alloc_fail_countdown = fail_at;
        bool ok = chat_model_catalog_parse(&catalog, other, &stats);
        alloc_fail_countdown = -1;
        if (ok) {
            CHECK(catalog.count == 1);
            CHECK(!wcscmp(catalog.items[0].id, L"other/model"));
        } else {
            /* The failed parse must leave the previous catalog untouched. */
            CHECK(catalog.count == 3);
            CHECK(!wcscmp(catalog.items[0].id, L"openai/gpt-4"));
        }
        chat_model_catalog_dispose(&catalog);
    }
    /* The merged view reports allocation failure and leaves out empty. */
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    alloc_fail_countdown = 0;
    bool ok = chat_model_catalog_merged(&catalog, L"a/b", NULL, 0, &merged);
    alloc_fail_countdown = -1;
    CHECK(!ok && merged.count == 0);
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_dispose(&catalog);

    /* The same contract at every allocation point of a merge big enough to
       grow twice, with leak accounting: after the loop, every allocation
       made inside it has been released (wrapped free decrements the
       outstanding count, so a leaked partial view would show here). */
    {
        ChatModelCatalog big;
        chat_model_catalog_init(&big);
        for (int i = 0; i < 100; i++) {
            ChatModelInfo info;
            memset(&info, 0, sizeof info);
            info.context_length = -1;
            swprintf(info.id, CHAT_MODEL_TEXT, L"big/model-%d", i);
            CHECK(chat_model_catalog_append(&big, &info));
        }
        long outstanding_before = outstanding;
        for (long fail_at = 0; fail_at < 8; fail_at++) {
            ChatModelCatalog view;
            chat_model_catalog_init(&view);
            alloc_fail_countdown = fail_at;
            ok = chat_model_catalog_merged(&big, L"cur/model", NULL, 0,
                &view);
            alloc_fail_countdown = -1;
            if (ok) CHECK(view.count == 101);
            else CHECK(view.count == 0);
            chat_model_catalog_dispose(&view);
        }
        CHECK(outstanding == outstanding_before);
        chat_model_catalog_dispose(&big);
    }
    return 0;
}

int main(void) {
    int failed = 0;
    failed += parse_basic();
    failed += parse_required_and_types();
    failed += parse_duplicates_and_lengths();
    failed += parse_malformed_root();
    failed += parse_cap();
    failed += merge_order();
    failed += filter_matches();
    failed += parse_transactional_oom();
    if (failed) return 1;
    puts("Model catalog parse/merge/filter and transactional OOM passed");
    return 0;
}
