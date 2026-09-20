#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "chat/shell/palette.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Layout: `source` holds every built row (the full command/model source);
   `visible` holds indices into `source` after the query filter. The query is
   destructive to neither: clearing it always restores every source row. The
   highlight is an index into `visible`, re-resolved by stable PaletteRowKey
   identity (nearest survivor) whenever the source or the query changes. */
struct PalettePalette {
    PaletteMode mode;
    wchar_t query[PALETTE_QUERY_TEXT];
    PaletteRow *source;
    size_t source_count, source_capacity;
    size_t *visible;
    size_t visible_count, visible_capacity;
    size_t selected;    /* index into `visible`, or (size_t)-1 */
};

Palette *palette_create(void) {
    Palette *palette = (Palette *)calloc(1, sizeof *palette);
    if (!palette) return NULL;
    palette->mode = PALETTE_MODE_COMMANDS;
    palette->selected = (size_t)-1;
    return palette;
}

void palette_dispose(Palette *palette) {
    if (!palette) return;
    free(palette->source);
    free(palette->visible);
    free(palette);
}

PaletteMode palette_mode(const Palette *palette) {
    return palette ? palette->mode : PALETTE_MODE_COMMANDS;
}

void palette_set_mode(Palette *palette, PaletteMode mode) {
    if (!palette) return;
    if (mode != PALETTE_MODE_COMMANDS && mode != PALETTE_MODE_MODELS) return;
    palette->mode = mode;
}

const wchar_t *palette_query(const Palette *palette) {
    return palette ? palette->query : L"";
}

/* Ordinal case-insensitive substring with the exact semantics of
   chat_model_catalog_filter: CompareStringOrdinal folding (locale-independent
   Windows ordinal case-insensitivity, which covers non-ASCII names), an empty
   query matches everything. Sharing the operation keeps the model adapter and
   the catalog matcher in agreement. */
static bool fold_matches(const wchar_t *text, const wchar_t *query) {
    size_t length = wcslen(text);
    size_t query_length = wcslen(query);
    if (!query_length) return true;
    if (query_length > length || query_length > INT_MAX) return false;
    for (size_t i = 0; i + query_length <= length; i++)
        if (CompareStringOrdinal(text + i, (int)query_length, query,
                (int)query_length, TRUE) == CSTR_EQUAL) return true;
    return false;
}

/* Model rows match on label or note (the routable id), command rows on the
   label only, mirroring the picker's filter over id and display name. */
static bool row_visible(const Palette *palette, const PaletteRow *row) {
    if (!palette->query[0]) return true;
    if (row->mode == PALETTE_MODE_MODELS)
        return fold_matches(row->label, palette->query) ||
            fold_matches(row->note, palette->query);
    return fold_matches(row->label, palette->query);
}

static bool ensure_source(Palette *palette, size_t needed) {
    if (needed <= palette->source_capacity) return true;
    size_t capacity = palette->source_capacity ? palette->source_capacity : 64;
    while (capacity < needed) capacity *= 2;
    PaletteRow *source = (PaletteRow *)realloc(palette->source,
        capacity * sizeof *source);
    if (!source) return false;
    palette->source = source;
    palette->source_capacity = capacity;
    return true;
}

static bool ensure_visible(Palette *palette, size_t needed) {
    if (needed <= palette->visible_capacity) return true;
    size_t capacity = palette->visible_capacity ? palette->visible_capacity : 64;
    while (capacity < needed) capacity *= 2;
    size_t *visible = (size_t *)realloc(palette->visible,
        capacity * sizeof *visible);
    if (!visible) return false;
    palette->visible = visible;
    palette->visible_capacity = capacity;
    return true;
}

/* Stable identity: a row is the same logical entry when both are commands
   with the same action id, or both are models with the same id. The model
   kind (Current/Recent/All) is a section tag that legitimately changes when
   the source changes, so it never participates in identity. */
static bool key_equal(const PaletteRowKey *a, const PaletteRowKey *b) {
    if (a->action_id != b->action_id) return false;
    if ((a->kind == PALETTE_ROW_COMMAND) !=
        (b->kind == PALETTE_ROW_COMMAND))
        return false;
    return a->kind == PALETTE_ROW_COMMAND || !wcscmp(a->id, b->id);
}

/* Re-resolves the highlight against the visible list: the previous identity
   exactly, else the nearest enabled survivor at or after its old position,
   else the nearest before it, else nothing. With no previous highlight the
   first row becomes selected so keyboard activation has a target. */
static void reselect(Palette *palette, const PaletteRowKey *previous,
    size_t previous_index) {
    if (!palette->visible_count) {
        palette->selected = (size_t)-1;
        return;
    }
    if (previous) {
        for (size_t i = 0; i < palette->visible_count; i++) {
            const PaletteRow *row =
                &palette->source[palette->visible[i]];
            if (key_equal(&row->key, previous)) {
                palette->selected = i;
                return;
            }
        }
        /* Nearest survivor. The old position can sit past the shrunken
           visible list (a late selection filtered down to few rows), so
           clamp it first: both scans below must stay in range. */
        size_t start = previous_index < palette->visible_count
            ? previous_index
            : palette->visible_count - 1;
        for (size_t i = start; i < palette->visible_count; i++) {
            palette->selected = i;
            return;
        }
        for (size_t back = start; back-- > 0;) {
            palette->selected = back;
            return;
        }
    }
    palette->selected = 0;
}

/* Rebuilds the visible list under the current query and re-resolves the
   highlight. The caller has already ensured the visible capacity covers
   source_count, so this cannot fail and only runs after the source was
   replaced successfully. */
static void refilter(Palette *palette, const PaletteRowKey *previous,
    size_t previous_index) {
    size_t count = 0;
    for (size_t i = 0; i < palette->source_count; i++)
        if (row_visible(palette, &palette->source[i]))
            palette->visible[count++] = i;
    palette->visible_count = count;
    reselect(palette, previous, previous_index);
}

bool palette_set_query(Palette *palette, const wchar_t *query) {
    if (!palette) return false;
    wchar_t text[PALETTE_QUERY_TEXT];
    wcsncpy(text, query ? query : L"", PALETTE_QUERY_TEXT - 1);
    text[PALETTE_QUERY_TEXT - 1] = 0;
    PaletteRowKey previous;
    bool had_previous = palette_selected_key(palette, &previous);
    size_t previous_index =
        palette->selected < palette->visible_count ? palette->selected : 0;
    if (!ensure_visible(palette, palette->source_count)) return false;
    wcsncpy(palette->query, text, PALETTE_QUERY_TEXT - 1);
    palette->query[PALETTE_QUERY_TEXT - 1] = 0;
    refilter(palette, had_previous ? &previous : NULL, previous_index);
    return true;
}

bool palette_set_commands(Palette *palette, const ChatActionContext *context) {
    if (!palette || !context) return false;
    size_t count = 0;
    const ChatActionInfo *table = chat_action_table(&count);
    PaletteRowKey previous;
    bool had_previous = palette_selected_key(palette, &previous);
    size_t previous_index =
        palette->selected < palette->visible_count ? palette->selected : 0;

    /* Stage into scratch so a mid-build failure leaves the previous source
       and its visible list fully intact. The capacity headroom lets the
       staging run without touching `palette->source`. */
    PaletteRow *scratch = (PaletteRow *)malloc(
        (count ? count : 1) * sizeof *scratch);
    if (!scratch) return false;
    size_t built = 0;
    for (size_t i = 0; i < count; i++) {
        const ChatActionInfo *info = &table[i];
        /* Submenu headers have no single effect to invoke; their dynamic
            per-profile contents cannot become palette rows. */
        if (info->flags & CHAT_ACTION_FLAG_SUBMENU_ONLY) continue;
        if (!chat_action_available(info->id, context)) continue;
        PaletteRow *row = &scratch[built++];
        memset(row, 0, sizeof *row);
        row->key.kind = PALETTE_ROW_COMMAND;
        row->key.action_id = info->id;
        row->mode = PALETTE_MODE_COMMANDS;
        chat_action_plain_label(info->id, row->label,
            sizeof row->label / sizeof *row->label);
        row->group = (unsigned)info->group + 1;
    }

    /* Every fallible step precedes the first stored-state change: source
       capacity, then visible capacity (both cover `built`), so the copy and
       refilter below cannot fail halfway. */
    if (!ensure_source(palette, built) ||
        !ensure_visible(palette, built)) {
        free(scratch);
        return false;
    }
    memcpy(palette->source, scratch, built * sizeof *scratch);
    palette->source_count = built;
    free(scratch);
    refilter(palette, had_previous ? &previous : NULL, previous_index);
    return true;
}

bool palette_set_models(Palette *palette, const ChatModelCatalog *catalog,
    const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count) {
    if (!palette) return false;
    PaletteRowKey previous;
    bool had_previous = palette_selected_key(palette, &previous);
    size_t previous_index =
        palette->selected < palette->visible_count ? palette->selected : 0;

    /* Build the merged view first: current, history MRU, then catalog,
       deduplicated by exact id (first occurrence wins). On failure the
       partial view is released here; merged() itself also leaves its output
       empty, so this dispose is a cheap no-op in that case. */
    ChatModelCatalog merged;
    chat_model_catalog_init(&merged);
    if (!chat_model_catalog_merged(catalog, current_id, history,
            history_count, &merged)) {
        chat_model_catalog_dispose(&merged);
        return false;
    }

    /* Classify each merged id: Current (matches `current_id`), Recent (in
       the history), All (catalog only). The merged view is ordered, so a
       single scan tags rows and the section runs stay contiguous:
       Current, then Recent, then All. */
    PaletteRow *scratch = (PaletteRow *)malloc(
        (merged.count ? merged.count : 1) * sizeof *scratch);
    if (!scratch) {
        chat_model_catalog_dispose(&merged);
        return false;
    }
    size_t built = 0;
    for (size_t i = 0; i < merged.count; i++) {
        const ChatModelInfo *info = &merged.items[i];
        PaletteRow *row = &scratch[built++];
        memset(row, 0, sizeof *row);
        row->mode = PALETTE_MODE_MODELS;
        wcsncpy(row->key.id, info->id, CHAT_MODEL_TEXT - 1);
        wcsncpy(row->label, info->name[0] ? info->name : info->id,
            CHAT_ACTION_LABEL_TEXT - 1);
        wcsncpy(row->note, info->id, PALETTE_NOTE_TEXT - 1);
        if (current_id && current_id[0] && !wcscmp(info->id, current_id)) {
            row->key.kind = PALETTE_ROW_MODEL_CURRENT;
            row->group = 1;
        } else {
            bool recent = false;
            for (int h = 0; h < history_count; h++)
                if (!wcscmp(history[h], info->id)) {
                    recent = true;
                    break;
                }
            row->key.kind = recent ? PALETTE_ROW_MODEL_RECENT
                                   : PALETTE_ROW_MODEL_ALL;
            row->group = recent ? 2u : 3u;
        }
    }
    chat_model_catalog_dispose(&merged);

    if (!ensure_source(palette, built) ||
        !ensure_visible(palette, built)) {
        free(scratch);
        return false;
    }
    memcpy(palette->source, scratch, built * sizeof *scratch);
    palette->source_count = built;
    free(scratch);
    refilter(palette, had_previous ? &previous : NULL, previous_index);
    return true;
}

size_t palette_row_count(const Palette *palette) {
    return palette ? palette->visible_count : 0;
}

const PaletteRow *palette_row(const Palette *palette, size_t index) {
    if (!palette || index >= palette->visible_count) return NULL;
    return &palette->source[palette->visible[index]];
}

size_t palette_section_count(const Palette *palette) {
    if (!palette || !palette->visible_count) return 0;
    size_t sections = 1;
    for (size_t i = 1; i < palette->visible_count; i++)
        if (palette_row(palette, i)->group !=
            palette_row(palette, i - 1)->group)
            ++sections;
    return sections;
}

size_t palette_section_start(const Palette *palette, size_t section) {
    if (!palette || !palette->visible_count) return (size_t)-1;
    size_t seen = 0;
    for (size_t i = 0; i < palette->visible_count; i++) {
        if (i && palette_row(palette, i)->group !=
            palette_row(palette, i - 1)->group)
            ++seen;
        if (seen == section) return i;
    }
    return (size_t)-1;
}

const wchar_t *palette_section_label(const Palette *palette, size_t section) {
    static const wchar_t *command_labels[CHAT_ACTION_GROUP_COUNT] = {
        L"Conversation", L"Response", L"Settings", L"Backend", L"Routing",
        L"Customization"
    };
    static const wchar_t *model_labels[] = {
        L"Current", L"Recent", L"All"
    };
    size_t start = palette_section_start(palette, section);
    if (start == (size_t)-1) return NULL;
    /* Interpret the group tag by the row's own source mode, never the
       controller mode: the two can disagree between a mode switch and the
       next source replacement, and the rows must keep their meaning. */
    const PaletteRow *row = palette_row(palette, start);
    unsigned group = row->group;
    if (group == 0) return NULL;
    if (row->mode == PALETTE_MODE_COMMANDS)
        return group <= CHAT_ACTION_GROUP_COUNT
            ? command_labels[group - 1] : NULL;
    return group <= 3 ? model_labels[group - 1] : NULL;
}

size_t palette_selected(const Palette *palette) {
    return palette ? palette->selected : (size_t)-1;
}

const PaletteRow *palette_selected_row(const Palette *palette) {
    if (!palette || palette->selected >= palette->visible_count) return NULL;
    return &palette->source[palette->visible[palette->selected]];
}

void palette_select_key(Palette *palette, const PaletteRowKey *key) {
    if (!palette || !key) return;
    for (size_t i = 0; i < palette->visible_count; i++) {
        const PaletteRow *row = &palette->source[palette->visible[i]];
        if (key_equal(&row->key, key)) {
            palette->selected = i;
            return;
        }
    }
}

bool palette_selected_key(const Palette *palette, PaletteRowKey *key) {
    const PaletteRow *row = palette_selected_row(palette);
    if (!row || !key) return false;
    *key = row->key;
    return true;
}

bool palette_step(Palette *palette, int delta) {
    if (!palette || !delta || !palette->visible_count) return false;
    size_t current = palette->selected;
    if (current >= palette->visible_count) {
        palette->selected = delta > 0 ? 0 : palette->visible_count - 1;
        return true;
    }
    long next = (long)current + delta;
    if (next < 0) next = 0;
    if (next >= (long)palette->visible_count)
        next = (long)palette->visible_count - 1;
    if ((size_t)next == current) return false;
    palette->selected = (size_t)next;
    return true;
}

bool palette_page(Palette *palette, int delta) {
    if (!delta) return false;
    return palette_step(palette, delta * (int)PALETTE_PAGE);
}

bool palette_home(Palette *palette) {
    if (!palette || !palette->visible_count) return false;
    if (palette->selected == 0) return false;
    palette->selected = 0;
    return true;
}

bool palette_end(Palette *palette) {
    if (!palette || !palette->visible_count) return false;
    size_t last = palette->visible_count - 1;
    if (palette->selected == last) return false;
    palette->selected = last;
    return true;
}

bool palette_can_accept(const Palette *palette) {
    return palette_selected_row(palette) != NULL;
}

bool palette_accept_key(const Palette *palette, PaletteRowKey *key) {
    if (!palette_can_accept(palette)) return false;
    return palette_selected_key(palette, key);
}
