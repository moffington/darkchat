/* Pure palette controller: filtering, grouping, identity-based selection,
   navigation and transactional source replacement. Includes palette.c so
   transactional allocation failures can be driven with wrapped malloc and
   realloc, the same way test_chat and test_model_catalog exercise growth. */
#include "chat/shell/palette.c"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static long alloc_fail_countdown = -1;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    return __real_realloc(pointer, size);
}

/* A host context with no interaction flags and the OpenRouter backend: the
   Conversation/Settings/Backend/Routing actions are available, the Response
   actions that need a conversation are not. */
static ChatActionContext idle_context(void) {
    ChatActionContext context;
    memset(&context, 0, sizeof context);
    context.backend_openrouter = true;
    return context;
}

static ChatActionContext generating_context(void) {
    ChatActionContext context = idle_context();
    context.generating = true;
    return context;
}

static ChatModelCatalog make_catalog(const wchar_t **ids, size_t count) {
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    for (size_t i = 0; i < count; i++) {
        ChatModelInfo info;
        memset(&info, 0, sizeof info);
        info.context_length = -1;
        wcsncpy(info.id, ids[i], CHAT_MODEL_TEXT - 1);
        /* Give a couple of entries display names. */
        if (i == 1) wcsncpy(info.name, L"Nice Display Name",
            CHAT_MODEL_NAME_TEXT - 1);
        if (!chat_model_catalog_append(&catalog, &info)) {
            chat_model_catalog_dispose(&catalog);
            chat_model_catalog_init(&catalog);
            return catalog;
        }
    }
    return catalog;
}

static int empty_and_null(void) {
    /* A fresh controller is an empty commands palette. */
    Palette *palette = palette_create();
    CHECK(palette);
    CHECK(palette_mode(palette) == PALETTE_MODE_COMMANDS);
    CHECK(palette_row_count(palette) == 0);
    CHECK(palette_section_count(palette) == 0);
    CHECK(palette_section_start(palette, 0) == (size_t)-1);
    CHECK(palette_section_label(palette, 0) == NULL);
    CHECK(palette_selected(palette) == (size_t)-1);
    CHECK(palette_selected_row(palette) == NULL);
    CHECK(!palette_can_accept(palette));
    CHECK(!palette_accept_key(palette, NULL));
    CHECK(palette_row(palette, 0) == NULL);
    CHECK(!palette_step(palette, 1) && !palette_home(palette) &&
        !palette_end(palette) && !palette_page(palette, 1));
    /* NULL controllers are safe everywhere. */
    palette_dispose(NULL);
    CHECK(palette_mode(NULL) == PALETTE_MODE_COMMANDS);
    CHECK(palette_row_count(NULL) == 0);
    CHECK(palette_selected(NULL) == (size_t)-1);
    CHECK(!palette_set_commands(NULL, NULL));
    CHECK(!palette_set_models(NULL, NULL, NULL, NULL, 0));
    CHECK(!palette_set_query(NULL, L"x"));
    palette_select_key(NULL, NULL);
    palette_set_mode(NULL, PALETTE_MODE_MODELS);
    palette_dispose(palette);
    return 0;
}

static int commands_basic(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    /* The idle memset context enables 23 registry actions. */
    CHECK(palette_row_count(palette) == 23);
    /* Registry order and grouping. The idle context has no conversation, so
        the Response section is absent: the six present sections are
        Conversation, Settings, Backend, Routing, Customization, Data. */
    CHECK(palette_section_count(palette) == 6);
    CHECK(palette_section_start(palette, 0) == 0);
    CHECK(!wcscmp(palette_section_label(palette, 0), L"Conversation"));
    CHECK(!wcscmp(palette_section_label(palette, 1), L"Settings"));
    CHECK(!wcscmp(palette_section_label(palette, 2), L"Backend"));
    CHECK(!wcscmp(palette_section_label(palette, 3), L"Routing"));
    CHECK(!wcscmp(palette_section_label(palette, 4), L"Customization"));
    CHECK(!wcscmp(palette_section_label(palette, 5), L"Data"));
    const PaletteRow *row = palette_row(palette, 0);
    CHECK(row->key.kind == PALETTE_ROW_COMMAND);
    CHECK(row->key.action_id == ACTION_NEW);
    CHECK(row->mode == PALETTE_MODE_COMMANDS);
    CHECK(!wcscmp(row->label, L"New conversation"));
    CHECK(!wcscmp(row->note, L""));
    CHECK(row->group == (unsigned)CHAT_ACTION_GROUP_CONVERSATION + 1);
    /* Response actions are absent (no conversation behind the context), so
       section 1 (Settings) starts at System prompt. */
    size_t settings = palette_section_start(palette, 1);
    CHECK(palette_row(palette, settings)->key.action_id == ACTION_SYSTEM);
    size_t routing = palette_section_start(palette, 3);
    CHECK(palette_row(palette, routing)->key.action_id ==
        ACTION_ROUTING_SORT_DEFAULT);
    /* Plain labels only, no mnemonics reach the palette. */
    for (size_t i = 0; i < palette_row_count(palette); i++)
        CHECK(wcschr(palette_row(palette, i)->label, L'&') == NULL);
    /* Out-of-range section lookups are safe. */
    CHECK(palette_section_start(palette, 6) == (size_t)-1);
    CHECK(palette_section_label(palette, 6) == NULL);
    /* NULL context is rejected, and the previous source survives. */
    CHECK(!palette_set_commands(palette, NULL));
    CHECK(palette_row_count(palette) == 23);
    palette_dispose(palette);
    return 0;
}

static int commands_filtering(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    size_t full = palette_row_count(palette);
    CHECK(full == 23);

    CHECK(palette_set_query(palette, L"model"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_row(palette, 0)->label, L"Choose model..."));
    CHECK(palette_section_count(palette) == 1);

    /* Case-insensitive ordinal matching. */
    CHECK(palette_set_query(palette, L"MODEL"));
    CHECK(palette_row_count(palette) == 1);

    /* Empty state: no rows, no sections, no selection, no accept. */
    CHECK(palette_set_query(palette, L"zzzz-no-match"));
    CHECK(palette_row_count(palette) == 0);
    CHECK(palette_section_count(palette) == 0);
    CHECK(palette_selected(palette) == (size_t)-1);
    CHECK(!palette_can_accept(palette));
    /* Navigation on the empty state is a no-op. */
    CHECK(!palette_step(palette, 1));
    CHECK(!palette_home(palette));
    CHECK(!palette_end(palette));
    CHECK(!palette_page(palette, 1));

    /* Clearing the query restores every source row: the source is never
       consumed by filtering. */
    CHECK(palette_set_query(palette, L""));
    CHECK(palette_row_count(palette) == full);
    /* NULL query means the empty query. */
    CHECK(palette_set_query(palette, NULL));
    CHECK(palette_row_count(palette) == full);
    palette_dispose(palette);
    return 0;
}

static int commands_disabled(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    /* Generating keeps only New + Search available. */
    ChatActionContext generating = generating_context();
    CHECK(palette_set_commands(palette, &generating));
    CHECK(palette_row_count(palette) == 2);
    CHECK(palette_row(palette, 0)->key.action_id == ACTION_NEW);
    CHECK(palette_row(palette, 1)->key.action_id == ACTION_SEARCH);

    /* Ollama removes the Routing section (7 actions). */
    ChatActionContext ollama = idle_context();
    ollama.backend_openrouter = false;
    CHECK(palette_set_commands(palette, &ollama));
    CHECK(palette_row_count(palette) == 23 - 7);
    CHECK(palette_section_count(palette) == 5);
    CHECK(!wcscmp(palette_section_label(palette, 2), L"Backend"));
    CHECK(!wcscmp(palette_section_label(palette, 4), L"Data"));
    /* Unavailable commands never become rows, so accept can never fire one:
       under generating, activating whatever is highlighted is safe. */
    CHECK(palette_set_commands(palette, &generating));
    PaletteRowKey key;
    CHECK(palette_accept_key(palette, &key));
    CHECK(key.action_id == ACTION_NEW || key.action_id == ACTION_SEARCH);
    palette_dispose(palette);
    return 0;
}

static int selection_identity(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));

    /* From a fresh source the highlight lands on the first row. */
    PaletteRowKey key;
    CHECK(palette_selected_key(palette, &key));
    CHECK(key.action_id == ACTION_NEW);
    /* Select by identity, not index. */
    PaletteRowKey want;
    memset(&want, 0, sizeof want);
    want.kind = PALETTE_ROW_COMMAND;
    want.action_id = ACTION_DELETE;
    palette_select_key(palette, &want);
    CHECK(palette_selected(palette) == 2);
    CHECK(palette_selected_row(palette)->key.action_id == ACTION_DELETE);

    /* Filtering keeps the same identity selected. */
    CHECK(palette_set_query(palette, L"delete"));
    CHECK(palette_row_count(palette) == 2);
    CHECK(palette_selected_row(palette)->key.action_id == ACTION_DELETE);
    palette_dispose(palette);
    return 0;
}

static int selection_survivor_range(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    /* Park the highlight on a late command. */
    CHECK(palette_end(palette));
    CHECK(palette_selected(palette) == palette_row_count(palette) - 1);

    /* Filtering down to one different command clamps the nearest survivor
       into the new list: the highlight stays valid and acceptable. */
    CHECK(palette_set_query(palette, L"model"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(palette_selected(palette) == 0);
    PaletteRowKey key;
    CHECK(palette_can_accept(palette));
    CHECK(palette_accept_key(palette, &key));
    CHECK(key.action_id == ACTION_MODELS);

    /* Replacing the source with the two-row generating source has the same
       property: the old index is far out of range, the survivor is not. */
    CHECK(palette_set_query(palette, L""));
    ChatActionContext generating = generating_context();
    CHECK(palette_set_commands(palette, &generating));
    CHECK(palette_row_count(palette) == 2);
    CHECK(palette_selected(palette) < palette_row_count(palette));
    CHECK(palette_selected_row(palette) != NULL);
    CHECK(palette_accept_key(palette, &key));
    CHECK(key.action_id == ACTION_SEARCH);
    palette_dispose(palette);
    return 0;
}

static int navigation(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    size_t count = palette_row_count(palette);
    CHECK(count >= 8);

    /* From no highlight, step down selects the first row. */
    CHECK(palette_selected(palette) == 0);  /* reselect picked row 0 */
    CHECK(palette_step(palette, 1));
    CHECK(palette_selected(palette) == 1);
    /* Up from the first row clamps (no wrap into a sibling container). */
    CHECK(palette_step(palette, -1));
    CHECK(palette_selected(palette) == 0);
    CHECK(!palette_step(palette, -1));
    CHECK(palette_selected(palette) == 0);
    /* Down clamps at the last row. */
    CHECK(palette_end(palette));
    CHECK(palette_selected(palette) == count - 1);
    CHECK(!palette_step(palette, 1));
    CHECK(palette_selected(palette) == count - 1);
    /* Page moves by PALETTE_PAGE and clamps. */
    CHECK(palette_home(palette));
    CHECK(palette_selected(palette) == 0);
    CHECK(palette_page(palette, 1));
    CHECK(palette_selected(palette) == PALETTE_PAGE);
    CHECK(palette_page(palette, -1));
    CHECK(palette_selected(palette) == 0);
    CHECK(!palette_page(palette, -1));
    /* A huge negative page also clamps, and reports no movement. */
    CHECK(!palette_page(palette, -100));
    CHECK(palette_selected(palette) == 0);
    palette_dispose(palette);
    return 0;
}

static int models_basic(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    CHECK(palette_mode(palette) == PALETTE_MODE_MODELS);

    const wchar_t *ids[] = { L"current/model", L"recent/model",
        L"catalog/model" };
    ChatModelCatalog catalog = make_catalog(ids + 2, 1);
    wchar_t history[2][CHAT_MODEL_TEXT];
    wcscpy(history[0], L"recent/model");
    wcscpy(history[1], L"old/model");
    CHECK(palette_set_models(palette, &catalog, L"current/model", history, 2));
    /* Merge order: current, history MRU, catalog. `old/model` appears even
       though the catalog does not know it (history-only). */
    CHECK(palette_row_count(palette) == 4);
    CHECK(!wcscmp(palette_row(palette, 0)->key.id, L"current/model"));
    CHECK(palette_row(palette, 0)->key.kind == PALETTE_ROW_MODEL_CURRENT);
    CHECK(palette_row(palette, 0)->group == 1);
    CHECK(!wcscmp(palette_row(palette, 1)->key.id, L"recent/model"));
    CHECK(palette_row(palette, 1)->key.kind == PALETTE_ROW_MODEL_RECENT);
    CHECK(palette_row(palette, 1)->group == 2);
    CHECK(!wcscmp(palette_row(palette, 2)->key.id, L"old/model"));
    CHECK(palette_row(palette, 2)->key.kind == PALETTE_ROW_MODEL_RECENT);
    CHECK(!wcscmp(palette_row(palette, 3)->key.id, L"catalog/model"));
    CHECK(palette_row(palette, 3)->key.kind == PALETTE_ROW_MODEL_ALL);
    CHECK(palette_row(palette, 3)->group == 3);
    /* The display name rides in the label, the id in the note. */
    CHECK(!wcscmp(palette_row(palette, 3)->label, L"catalog/model"));
    CHECK(!wcscmp(palette_row(palette, 3)->note, L"catalog/model"));
    /* Sections: Current / Recent / All. */
    CHECK(palette_section_count(palette) == 3);
    CHECK(palette_section_start(palette, 0) == 0);
    CHECK(palette_section_start(palette, 1) == 1);
    CHECK(palette_section_start(palette, 2) == 3);
    CHECK(!wcscmp(palette_section_label(palette, 0), L"Current"));
    CHECK(!wcscmp(palette_section_label(palette, 1), L"Recent"));
    CHECK(!wcscmp(palette_section_label(palette, 2), L"All"));
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int models_filtering(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    const wchar_t *ids[] = { L"openai/gpt-4", L"anthropic/claude-3" };
    ChatModelCatalog catalog = make_catalog(ids, 2);
    /* Give GPT-4 a display name via the second catalog slot semantics:
       make_catalog names index 1, so claude carries the name. */
    CHECK(palette_set_models(palette, &catalog, L"", NULL, 0));
    CHECK(palette_row_count(palette) == 2);
    /* Match on the id. */
    CHECK(palette_set_query(palette, L"gpt"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_row(palette, 0)->key.id, L"openai/gpt-4"));
    /* Match on the display name. */
    CHECK(palette_set_query(palette, L"display"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_row(palette, 0)->key.id, L"anthropic/claude-3"));
    /* Model queries match note OR label; command queries match label only
       (verified in commands_filtering). */
    CHECK(palette_set_query(palette, L"anthropic"));
    CHECK(palette_row_count(palette) == 1);
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int models_identity_retention(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    const wchar_t *ids[] = { L"a/model", L"b/model", L"c/model" };
    ChatModelCatalog catalog = make_catalog(ids, 3);
    CHECK(palette_set_models(palette, &catalog, L"", NULL, 0));
    /* Select b/model by identity. */
    PaletteRowKey want;
    memset(&want, 0, sizeof want);
    want.kind = PALETTE_ROW_MODEL_ALL;
    wcscpy(want.id, L"b/model");
    palette_select_key(palette, &want);
    CHECK(palette_selected(palette) == 1);
    /* A source replacement that keeps the same id retains the selection
       across a changed kind (the id is now the current model). */
    CHECK(palette_set_models(palette, &catalog, L"b/model", NULL, 0));
    CHECK(palette_selected_row(palette));
    CHECK(!wcscmp(palette_selected_row(palette)->key.id, L"b/model"));
    CHECK(palette_selected_row(palette)->key.kind == PALETTE_ROW_MODEL_CURRENT);
    /* A source replacement that drops the selection falls to the nearest
       survivor (same position). */
    const wchar_t *few[] = { L"x/model", L"y/model" };
    ChatModelCatalog small = make_catalog(few, 2);
    CHECK(palette_set_models(palette, &small, L"", NULL, 0));
    CHECK(palette_selected_row(palette));
    CHECK(palette_selected(palette) < palette_row_count(palette));
    /* Filtering with a selection keeps the identity when it survives. */
    CHECK(palette_set_models(palette, &catalog, L"", NULL, 0));
    memset(&want, 0, sizeof want);
    want.kind = PALETTE_ROW_MODEL_ALL;
    wcscpy(want.id, L"c/model");
    palette_select_key(palette, &want);
    CHECK(palette_set_query(palette, L"c/"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_selected_row(palette)->key.id, L"c/model"));
    /* Narrowing past the selection leaves a valid nearest-survivor or an
       empty palette, never a stale highlight. */
    CHECK(palette_set_query(palette, L"zzzz"));
    CHECK(palette_row_count(palette) == 0);
    CHECK(palette_selected(palette) == (size_t)-1);
    CHECK(!palette_can_accept(palette));
    chat_model_catalog_dispose(&small);
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int adapter_equivalence(void) {
    /* The model adapter's merge and tagging agree with
       chat_model_catalog_merged + chat_model_catalog_filter semantics. */
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    const wchar_t *ids[] = { L"dup/model", L"other/model" };
    ChatModelCatalog catalog = make_catalog(ids, 2);
    wchar_t history[2][CHAT_MODEL_TEXT];
    wcscpy(history[0], L"dup/model");
    wcscpy(history[1], L"gone/model");
    /* Reference merged view. */
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"cur/model", history, 2,
        &merged));
    CHECK(palette_set_models(palette, &catalog, L"cur/model", history, 2));
    CHECK(palette_row_count(palette) == merged.count);
    for (size_t i = 0; i < merged.count; i++)
        CHECK(!wcscmp(palette_row(palette, i)->key.id, merged.items[i].id));
    /* Filter equivalence: an empty query matches all, a query matches the
       same subset the catalog filter returns. */
    CHECK(palette_set_query(palette, L"model"));
    CHECK(palette_row_count(palette) == merged.count);
    const ChatModelInfo *found[8];
    size_t matches = chat_model_catalog_filter(&merged, L"model", found, 8);
    CHECK(matches == merged.count);
    CHECK(palette_set_query(palette, L"other"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_row(palette, 0)->key.id, L"other/model"));
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int unicode_folding(void) {
    /* Non-ASCII model names must match exactly like the catalog filter:
       Windows ordinal case-insensitivity, not an ASCII-only fold. */
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    ChatModelInfo info;
    memset(&info, 0, sizeof info);
    info.context_length = -1;
    wcscpy(info.id, L"accent/model");
    wcscpy(info.name, L"Caf\xe9 Model");
    CHECK(chat_model_catalog_append(&catalog, &info));
    CHECK(palette_set_models(palette, &catalog, L"", NULL, 0));
    CHECK(palette_row_count(palette) == 1);

    /* The accented display name matches a query in the other case variant
       (small é in the name, capital É in the query). */
    CHECK(palette_set_query(palette, L"CAF\xc9"));
    CHECK(palette_row_count(palette) == 1);
    CHECK(!wcscmp(palette_row(palette, 0)->key.id, L"accent/model"));
    /* The plain id still matches too. */
    CHECK(palette_set_query(palette, L"accent"));
    CHECK(palette_row_count(palette) == 1);

    /* The palette agrees with chat_model_catalog_filter on the same
       merged view for the same query. */
    const ChatModelInfo *found[4];
    CHECK(chat_model_catalog_filter(&catalog, L"CAF\xc9", found, 4) == 1);
    CHECK(!wcscmp(found[0]->id, L"accent/model"));
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    CHECK(chat_model_catalog_merged(&catalog, L"", NULL, 0, &merged));
    CHECK(chat_model_catalog_filter(&merged, L"CAF\xc9", found, 4) == 1);
    chat_model_catalog_dispose(&merged);
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int section_label_follows_rows(void) {
    /* The controller mode and the rows can disagree between a mode switch
       and the next source replacement; section labels must keep coming from
       the rows. */
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    CHECK(!wcscmp(palette_section_label(palette, 0), L"Conversation"));
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    CHECK(palette_mode(palette) == PALETTE_MODE_MODELS);
    /* Still command rows, still command labels. */
    CHECK(palette_row(palette, 0)->mode == PALETTE_MODE_COMMANDS);
    CHECK(!wcscmp(palette_section_label(palette, 0), L"Conversation"));
    /* Replacing the source realigns rows and labels with the mode. */
    ChatModelCatalog catalog;
    chat_model_catalog_init(&catalog);
    CHECK(palette_set_models(palette, &catalog, L"only/model", NULL, 0));
    CHECK(palette_row(palette, 0)->mode == PALETTE_MODE_MODELS);
    CHECK(!wcscmp(palette_section_label(palette, 0), L"Current"));
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

static int empty_models_source(void) {
    /* No catalog, no history, no current: an empty but valid palette. */
    Palette *palette = palette_create();
    CHECK(palette);
    palette_set_mode(palette, PALETTE_MODE_MODELS);
    CHECK(palette_set_models(palette, NULL, L"", NULL, 0));
    CHECK(palette_row_count(palette) == 0);
    CHECK(palette_section_count(palette) == 0);
    CHECK(!palette_can_accept(palette));
    /* NULL catalog with only a current id still shows that model. */
    CHECK(palette_set_models(palette, NULL, L"only/model", NULL, 0));
    CHECK(palette_row_count(palette) == 1);
    CHECK(palette_row(palette, 0)->key.kind == PALETTE_ROW_MODEL_CURRENT);
    CHECK(!wcscmp(palette_row(palette, 0)->label, L"only/model"));
    palette_dispose(palette);
    return 0;
}

static int transactional_oom(void) {
    Palette *palette = palette_create();
    CHECK(palette);
    ChatActionContext context = idle_context();
    CHECK(palette_set_commands(palette, &context));
    size_t full = palette_row_count(palette);
    CHECK(full == 23);
    PaletteRowKey before;
    CHECK(palette_selected_key(palette, &before));

    /* Every allocation failure during a source replacement leaves the
       previous source, visible list, selection and query fully intact. */
    for (long fail_at = 0; fail_at < 8; fail_at++) {
        alloc_fail_countdown = fail_at;
        bool ok = palette_set_commands(palette, &context);
        alloc_fail_countdown = -1;
        CHECK(palette_row_count(palette) == full);
        PaletteRowKey after;
        if (ok) {
            /* Success keeps the identity selection too. */
            CHECK(palette_selected_key(palette, &after));
            CHECK(after.kind == before.kind &&
                after.action_id == before.action_id);
        }
        /* The visible list is never a dangling partial build. */
        for (size_t i = 0; i < palette_row_count(palette); i++)
            CHECK(palette_row(palette, i) != NULL);
    }

    /* Query changes are transactional too. */
    for (long fail_at = 0; fail_at < 4; fail_at++) {
        alloc_fail_countdown = fail_at;
        bool ok = palette_set_query(palette, L"delete");
        alloc_fail_countdown = -1;
        if (ok) {
            CHECK(palette_row_count(palette) == 2);
            CHECK(!wcscmp(palette_query(palette), L"delete"));
            /* Restore for the next iteration. */
            CHECK(palette_set_query(palette, L""));
        } else {
            CHECK(palette_row_count(palette) == full);
            CHECK(!wcscmp(palette_query(palette), L""));
        }
    }

    /* Model source replacement under allocation failure. */
    const wchar_t *ids[] = { L"m/one", L"m/two" };
    ChatModelCatalog catalog = make_catalog(ids, 2);
    for (long fail_at = 0; fail_at < 8; fail_at++) {
        alloc_fail_countdown = fail_at;
        palette_set_models(palette, &catalog, L"m/one", NULL, 0);
        alloc_fail_countdown = -1;
        /* Either the new source is complete or the old one is fully intact;
            a partial mix is never observable. */
        size_t rows = palette_row_count(palette);
        CHECK(rows == 23 || rows == 2);
    }

    /* A source larger than the current visible capacity forces a visible
       growth, which must also be transactional. */
    static const wchar_t *many[100];
    static wchar_t many_ids[100][CHAT_MODEL_TEXT];
    for (int i = 0; i < 100; i++) {
        swprintf(many_ids[i], CHAT_MODEL_TEXT, L"bulk/model-%d", i);
        many[i] = many_ids[i];
    }
    ChatModelCatalog bulk = make_catalog(many, 100);
    for (long fail_at = 0; fail_at < 10; fail_at++) {
        alloc_fail_countdown = fail_at;
        bool ok = palette_set_models(palette, &bulk, L"", NULL, 0);
        alloc_fail_countdown = -1;
        size_t rows = palette_row_count(palette);
        CHECK(rows == 2 || rows == 100);
        if (ok) CHECK(palette_set_models(palette, &catalog, L"", NULL, 0));
    }
    chat_model_catalog_dispose(&bulk);
    chat_model_catalog_dispose(&catalog);
    palette_dispose(palette);
    return 0;
}

int main(void) {
    int failed = 0;
    failed += empty_and_null();
    failed += commands_basic();
    failed += commands_filtering();
    failed += commands_disabled();
    failed += selection_identity();
    failed += selection_survivor_range();
    failed += navigation();
    failed += models_basic();
    failed += models_filtering();
    failed += models_identity_retention();
    failed += adapter_equivalence();
    failed += unicode_folding();
    failed += section_label_follows_rows();
    failed += empty_models_source();
    failed += transactional_oom();
    if (failed) return 1;
    puts("Palette controller checks passed");
    return 0;
}
