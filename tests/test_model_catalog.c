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

/* --- image-input capability ---------------------------------------------- */

/* The documented OpenRouter shape. One entry per classification the plan
   names, so a single fixture pins the whole table. */
static const char *modalities_json =
    "{\"data\":["
    /* image present -> YES */
    "{\"id\":\"yes/model\",\"architecture\":{\"modality\":\"text->text\","
    "\"input_modalities\":[\"text\",\"image\",\"file\"]}},"
    /* a readable list without image -> NO */
    "{\"id\":\"no/model\",\"architecture\":{\"modality\":\"text->text\","
    "\"input_modalities\":[\"text\",\"file\",\"audio\",\"video\"]}},"
    /* an empty list is still a readable list -> NO */
    "{\"id\":\"empty/model\",\"architecture\":{\"input_modalities\":[]}},"
    /* image present, other elements wrong-typed -> still a verified YES */
    "{\"id\":\"mixed-yes/model\",\"architecture\":"
    "{\"input_modalities\":[\"image\",5,null]}},"
    /* a wrong-typed element without image -> UNKNOWN, never NO */
    "{\"id\":\"mixed/model\",\"architecture\":"
    "{\"input_modalities\":[\"text\",7]}},"
    /* no architecture, no modalities, and wrong-typed modalities -> UNKNOWN */
    "{\"id\":\"bare/model\"},"
    "{\"id\":\"no-arch/model\",\"context_length\":4096},"
    "{\"id\":\"string-modalities/model\","
    "\"architecture\":{\"input_modalities\":\"image\"}},"
    "{\"id\":\"object-modalities/model\","
    "\"architecture\":{\"input_modalities\":{\"image\":true}}}]}";

static ChatCapability capability_of(const wchar_t *id) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    if (!chat_model_catalog_parse(&catalog, modalities_json, &stats)) return 999;
    ChatCapability result = 999;
    for (size_t i = 0; i < catalog.count; i++)
        if (!wcscmp(catalog.items[i].id, id)) result = catalog.items[i].accepts_image;
    chat_model_catalog_dispose(&catalog);
    return result;
}

static int parse_capability(void) {
    CHECK(capability_of(L"yes/model") == CHAT_CAP_YES);
    CHECK(capability_of(L"no/model") == CHAT_CAP_NO);
    CHECK(capability_of(L"empty/model") == CHAT_CAP_NO);
    CHECK(capability_of(L"mixed-yes/model") == CHAT_CAP_YES);
    /* The one rule: NO requires a fully readable list. */
    CHECK(capability_of(L"mixed/model") == CHAT_CAP_UNKNOWN);
    CHECK(capability_of(L"bare/model") == CHAT_CAP_UNKNOWN);
    CHECK(capability_of(L"no-arch/model") == CHAT_CAP_UNKNOWN);
    CHECK(capability_of(L"string-modalities/model") == CHAT_CAP_UNKNOWN);
    CHECK(capability_of(L"object-modalities/model") == CHAT_CAP_UNKNOWN);
    CHECK(capability_of(L"absent/model") == 999);
    /* A catalog entry that only has an id keeps the field's zero value. */
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, basic_json, &stats));
    for (size_t i = 0; i < catalog.count; i++)
        CHECK(catalog.items[i].accepts_image == CHAT_CAP_UNKNOWN);
    chat_model_catalog_dispose(&catalog);
    /* Ollama's /v1/models payload has no capability field at all. */
    CHECK(chat_model_catalog_parse(&catalog,
        "{\"object\":\"list\",\"data\":[{\"id\":\"qwen3-vl:8b\"}]}", &stats));
    CHECK(catalog.count == 1 &&
        catalog.items[0].accepts_image == CHAT_CAP_UNKNOWN);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

/* Wrong-typed capability data is additive noise: it may not change which
   entries are retained, their order, or any statistic. This claim is bounded
   to well-formed documents -- a malformed one is rejected by the existing
   root check before any field is read, which is asserted separately. */
static int parse_capability_is_inert(void) {
    const char *plain =
        "{\"data\":[{\"id\":\"a/model\"},{\"id\":\"b/model\"},"
        "{\"id\":\"c/model\"},{\"id\":\"d/model\"}]}";
    /* Longer than the membership comparison limit, so this element is not
       compared at all and its list cannot be read whole. */
    char long_token[96];
    memset(long_token, 'a', sizeof long_token - 1);
    long_token[sizeof long_token - 1] = 0;
    char hostile[512];
    snprintf(hostile, sizeof hostile,
        "{\"data\":[{\"id\":\"a/model\",\"architecture\":"
        "{\"input_modalities\":[\"text\",7]}},"
        "{\"id\":\"b/model\",\"architecture\":\"nope\"},"
        "{\"id\":\"c/model\",\"architecture\":{\"input_modalities\":[{}]}},"
        "{\"id\":\"d/model\",\"architecture\":{\"input_modalities\":"
        "[\"%s\"]}}]}", long_token);
    ChatModelCatalog plain_catalog, hostile_catalog;
    chat_model_catalog_init(&plain_catalog);
    chat_model_catalog_init(&hostile_catalog);
    ChatModelParseStats plain_stats, hostile_stats;
    CHECK(chat_model_catalog_parse(&plain_catalog, plain, &plain_stats));
    CHECK(chat_model_catalog_parse(&hostile_catalog, hostile, &hostile_stats));
    CHECK(plain_catalog.count == hostile_catalog.count);
    CHECK(plain_stats.parsed == hostile_stats.parsed);
    CHECK(plain_stats.invalid == hostile_stats.invalid);
    CHECK(plain_stats.duplicate == hostile_stats.duplicate);
    CHECK(plain_stats.too_long == hostile_stats.too_long);
    CHECK(plain_stats.truncated == hostile_stats.truncated);
    for (size_t i = 0; i < plain_catalog.count; i++) {
        CHECK(!wcscmp(plain_catalog.items[i].id, hostile_catalog.items[i].id));
        CHECK(hostile_catalog.items[i].accepts_image == CHAT_CAP_UNKNOWN);
    }
    /* Both merged views are identical too, capability aside. */
    wchar_t history[1][CHAT_MODEL_TEXT];
    wcscpy(history[0], L"z/typed");
    ChatModelCatalog plain_merged, hostile_merged;
    chat_model_catalog_init(&plain_merged);
    chat_model_catalog_init(&hostile_merged);
    CHECK(chat_model_catalog_merged(&plain_catalog, L"cur/model", history, 1,
        &plain_merged));
    CHECK(chat_model_catalog_merged(&hostile_catalog, L"cur/model", history, 1,
        &hostile_merged));
    CHECK(plain_merged.count == hostile_merged.count);
    for (size_t i = 0; i < plain_merged.count; i++)
        CHECK(!wcscmp(plain_merged.items[i].id, hostile_merged.items[i].id));
    chat_model_catalog_dispose(&hostile_merged);
    chat_model_catalog_dispose(&plain_merged);
    chat_model_catalog_dispose(&hostile_catalog);
    chat_model_catalog_dispose(&plain_catalog);
    /* Malformed structure inside `data` is a different matter: the pre-existing
       array-length check reads the whole document first, so the parse fails
       whole rather than degrading one entry's capability. */
    CHECK(!chat_model_catalog_parse(&hostile_catalog,
        "{\"data\":[{\"id\":\"a/model\",\"architecture\":"
        "{\"input_modalities\":[\"text\"}}]}", &hostile_stats));
    CHECK(!chat_model_catalog_parse(&hostile_catalog,
        "{\"data\":[{\"id\":\"a/model\",\"architecture\":"
        "{\"input_modalities\":[\"imag]}}]}", &hostile_stats));
    return 0;
}

/* The merged view is the palette's data source, so where a capability
   survives into it is part of this module's contract. */
static int merge_capability(void) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelParseStats stats;
    CHECK(chat_model_catalog_parse(&catalog, modalities_json, &stats));
    wchar_t history[2][CHAT_MODEL_TEXT];
    wcscpy(history[0], L"history-only/model");   /* not in the catalog */
    wcscpy(history[1], L"yes/model");            /* duplicated by the catalog */
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"current/model", history, 2,
        &merged));
    /* current first, then history, then catalog -- the duplicate is claimed
       by the history row, so eleven rows come from nine catalog entries. */
    CHECK(merged.count == 11);
    CHECK(!wcscmp(merged.items[0].id, L"current/model"));
    CHECK(!wcscmp(merged.items[1].id, L"history-only/model"));
    CHECK(!wcscmp(merged.items[2].id, L"yes/model"));
    /* Synthesized rows carry no capability: there is no source behind them. */
    CHECK(merged.items[0].accepts_image == CHAT_CAP_UNKNOWN);
    CHECK(merged.items[1].accepts_image == CHAT_CAP_UNKNOWN);
    /* First occurrence wins, so a history row shadows the catalog entry it
       duplicates and the catalog's own YES does not reach the merged view.
       Pinned, not endorsed: a consumer that needs the capability must resolve
       it from the catalog rather than from this row. */
    CHECK(merged.items[2].accepts_image == CHAT_CAP_UNKNOWN);
    /* A row the history does not duplicate keeps what the catalog parsed. */
    CHECK(merged.items[3].accepts_image == CHAT_CAP_NO);
    CHECK(!wcscmp(merged.items[3].id, L"no/model"));
    CHECK(merged.items[4].accepts_image == CHAT_CAP_NO);
    CHECK(!wcscmp(merged.items[4].id, L"empty/model"));
    CHECK(merged.items[5].accepts_image == CHAT_CAP_YES);
    CHECK(!wcscmp(merged.items[5].id, L"mixed-yes/model"));
    /* The same for a current-model id the catalog also carries. */
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"no/model", NULL, 0, &merged));
    CHECK(merged.count == 9);
    CHECK(!wcscmp(merged.items[0].id, L"no/model"));
    CHECK(merged.items[0].accepts_image == CHAT_CAP_UNKNOWN);
    CHECK(merged.items[1].accepts_image == CHAT_CAP_YES);
    chat_model_catalog_dispose(&merged);
    /* An empty catalog leaves every row synthesized. */
    chat_model_catalog_dispose(&catalog);
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"a/b", history, 2, &merged));
    CHECK(merged.count == 3);
    for (size_t i = 0; i < merged.count; i++)
        CHECK(merged.items[i].accepts_image == CHAT_CAP_UNKNOWN);
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_dispose(&catalog);
    return 0;
}

static int show_capability(void) {
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\",\"thinking\",\"vision\"]}") ==
        CHAT_CAP_YES);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\",\"tools\"]}") == CHAT_CAP_NO);
    CHECK(chat_model_capability_from_show("{\"capabilities\":[]}") ==
        CHAT_CAP_NO);
    /* A verified match wins over an unreadable list. */
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"vision\",3]}") == CHAT_CAP_YES);
    /* Anything that cannot be read whole is UNKNOWN, never NO. */
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\",3]}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":{\"vision\":true}}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":\"vision\"}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show("{\"model_info\":{}}") ==
        CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show("{}") == CHAT_CAP_UNKNOWN);
    /* A transport-level document that is not JSON at all. */
    CHECK(chat_model_capability_from_show("") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show("<html>404</html>") ==
        CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show("{oops") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(NULL) == CHAT_CAP_UNKNOWN);
    /* A locally complete array is not enough: the whole document is validated
       first, so a body with trailing junk can never become a proven refusal. */
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\"]}garbage") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"vision\"]}garbage") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\"]}{\"more\":1}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "garbage{\"capabilities\":[\"completion\"]}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"completion\"],}") == CHAT_CAP_UNKNOWN);
    CHECK(chat_model_capability_from_show(
        "{\"capabilities\":[\"vision\"],}") == CHAT_CAP_UNKNOWN);
    /* The gate must not reject a well-formed document it used to accept. */
    CHECK(chat_model_capability_from_show(
        "{\"model\":\"qwen3-vl:8b\",\"capabilities\":[\"completion\","
        "\"thinking\",\"vision\"],\"model_info\":{\"general.architecture\":"
        "\"qwen3vl\"}}") == CHAT_CAP_YES);
    CHECK(chat_model_capability_from_show(
        "  {\"capabilities\":[\"completion\"]}  ") == CHAT_CAP_NO);
    return 0;
}

/* Classification must not add an allocation: a modality-bearing payload has
   to fail, and succeed, at exactly the same points as one without it. */
static int capability_allocs_nothing(void) {
    const char *without =
        "{\"data\":[{\"id\":\"only/model\",\"name\":\"Only\"}]}";
    const char *with =
        "{\"data\":[{\"id\":\"only/model\",\"name\":\"Only\","
        "\"architecture\":{\"modality\":\"text->text\","
        "\"input_modalities\":[\"text\",\"image\"]}}]}";
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    long outstanding_before = outstanding;
    for (long fail_at = 0; fail_at < 6; fail_at++) {
        bool ok_without, ok_with;
        ChatModelParseStats stats;
        chat_model_catalog_init(&catalog);
        alloc_fail_countdown = fail_at;
        ok_without = chat_model_catalog_parse(&catalog, without, &stats);
        alloc_fail_countdown = -1;
        chat_model_catalog_dispose(&catalog);
        chat_model_catalog_init(&catalog);
        alloc_fail_countdown = fail_at;
        ok_with = chat_model_catalog_parse(&catalog, with, &stats);
        alloc_fail_countdown = -1;
        CHECK(ok_with == ok_without);
        if (ok_with) {
            CHECK(catalog.count == 1);
            CHECK(catalog.items[0].accepts_image == CHAT_CAP_YES);
        }
        chat_model_catalog_dispose(&catalog);
    }
    CHECK(outstanding == outstanding_before);
    return 0;
}

/* Trailing junk after the root value still parses -- entries, order and every
   statistic are exactly what the clean document produced -- but a document
   that does not validate may not be read as evidence for anything. */
static int parse_capability_untrusted_document(void) {
    const char *clean_yes =
        "{\"data\":[{\"id\":\"a/model\",\"name\":\"A\","
        "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]}},"
        "{\"id\":\"b/model\",\"name\":\"B\",\"context_length\":4096},"
        "{\"id\":\"c/model\"},{\"id\":\"a/model\"},"
        "{\"id\":\"c/model\",\"context_length\":1}]}";
    char with_junk[512];
    snprintf(with_junk, sizeof with_junk, "%sgarbage", clean_yes);
    ChatModelCatalog clean, junked;
    chat_model_catalog_init(&clean);
    chat_model_catalog_init(&junked);
    ChatModelParseStats clean_stats, junk_stats;
    CHECK(chat_model_catalog_parse(&clean, clean_yes, &clean_stats));
    CHECK(chat_model_catalog_parse(&junked, with_junk, &junk_stats));
    /* The clean document is classified, and one duplicate plus one repeat
       exercise the duplicate and over-long-free statistic paths. */
    CHECK(clean.count == 3);
    CHECK(clean.items[0].accepts_image == CHAT_CAP_YES);
    CHECK(clean.items[1].accepts_image == CHAT_CAP_UNKNOWN);
    CHECK(clean_stats.parsed == 3 && clean_stats.duplicate == 2);
    /* The untrusted document keeps every entry, in the same order, with the
       same statistics -- only the inference is withheld. */
    CHECK(junked.count == clean.count);
    CHECK(junk_stats.parsed == clean_stats.parsed);
    CHECK(junk_stats.invalid == clean_stats.invalid);
    CHECK(junk_stats.duplicate == clean_stats.duplicate);
    CHECK(junk_stats.too_long == clean_stats.too_long);
    CHECK(junk_stats.truncated == clean_stats.truncated);
    for (size_t i = 0; i < clean.count; i++) {
        CHECK(!wcscmp(clean.items[i].id, junked.items[i].id));
        CHECK(!wcscmp(clean.items[i].name, junked.items[i].name));
        CHECK(clean.items[i].context_length == junked.items[i].context_length);
        CHECK(junked.items[i].accepts_image == CHAT_CAP_UNKNOWN);
    }
    /* The would-be refusal is withheld too: a document with a readable list
       missing "image" is exactly the case that must not become a NO. */
    const char *clean_no =
        "{\"data\":[{\"id\":\"n/model\","
        "\"architecture\":{\"input_modalities\":[\"text\",\"file\"]}}]}";
    char no_junk[256];
    snprintf(no_junk, sizeof no_junk, "%s {\"trailing\":1}", clean_no);
    chat_model_catalog_dispose(&junked);
    chat_model_catalog_init(&junked);
    CHECK(chat_model_catalog_parse(&junked, clean_no, &junk_stats));
    CHECK(junked.count == 1 && junked.items[0].accepts_image == CHAT_CAP_NO);
    chat_model_catalog_dispose(&junked);
    chat_model_catalog_init(&junked);
    CHECK(chat_model_catalog_parse(&junked, no_junk, &junk_stats));
    CHECK(junked.count == 1);
    CHECK(junked.items[0].accepts_image == CHAT_CAP_UNKNOWN);
    /* A match is not honored either: nothing in an untrusted document is.
       This variant puts the junk inside the root object, after `data`, so the
       array itself is untouched and only the document is discredited. */
    const char *clean_single =
        "{\"data\":[{\"id\":\"a/model\","
        "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]}}]}";
    char inner_junk[256];
    snprintf(inner_junk, sizeof inner_junk,
        "{\"data\":[{\"id\":\"a/model\","
        "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]}}],"
        "\"garbage\":x}");
    chat_model_catalog_dispose(&junked);
    chat_model_catalog_init(&junked);
    CHECK(chat_model_catalog_parse(&junked, clean_single, &junk_stats));
    CHECK(junked.count == 1 && junked.items[0].accepts_image == CHAT_CAP_YES);
    chat_model_catalog_dispose(&junked);
    chat_model_catalog_init(&junked);
    CHECK(chat_model_catalog_parse(&junked, inner_junk, &junk_stats));
    CHECK(junked.count == 1);
    CHECK(junked.items[0].accepts_image == CHAT_CAP_UNKNOWN);
    chat_model_catalog_dispose(&junked);
    chat_model_catalog_dispose(&clean);
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
    failed += parse_capability();
    failed += parse_capability_is_inert();
    failed += parse_capability_untrusted_document();
    failed += merge_capability();
    failed += show_capability();
    failed += capability_allocs_nothing();
    if (failed) return 1;
    puts("Model catalog parse/merge/filter, image capability and "
        "transactional OOM passed");
    return 0;
}
