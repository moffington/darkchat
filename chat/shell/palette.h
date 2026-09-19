#ifndef DARKCHAT_PALETTE_H
#define DARKCHAT_PALETTE_H

/* Pure command/model palette controller. No UI: all selection, filtering,
   sectioning and navigation semantics live here so the native popup (a later
   commit) only renders rows and forwards input.

   The controller snapshots its source into owned rows; a query filters a
   stable visible list over that snapshot, so clearing the query always
   restores every source row. Rows are identified by a PaletteRowKey (the
   action id for commands, the model id for models), never by index:
   replacing the source or narrowing the query keeps the highlight on the
   same logical entry, or on the nearest surviving row when it vanished.
   Commands that are unavailable under the current ChatActionContext never
   become rows, so navigation can never land on a disabled entry. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "chat/core/chat.h"
#include "chat/core/commands.h"
#include "chat/models/model_catalog.h"

/* Filter query bound (the retired model picker used the same). */
#define PALETTE_QUERY_TEXT 128
/* Display bound for the per-row note (a model's routable id). */
#define PALETTE_NOTE_TEXT 128
/* Rows per page movement. */
#define PALETTE_PAGE 8

typedef enum {
    /* Commands from the action registry, sectioned per registry group. */
    PALETTE_MODE_COMMANDS = 0,
    /* Models: Current / Recent / All sections over the merged view. */
    PALETTE_MODE_MODELS = 1
} PaletteMode;

typedef enum {
    /* Commands carry their action id in `action_id`. */
    PALETTE_ROW_COMMAND = 0,
    /* The active backend's current model (at most one row). */
    PALETTE_ROW_MODEL_CURRENT,
    /* A model from the remembered history (MRU order). */
    PALETTE_ROW_MODEL_RECENT,
    /* A model that comes from the catalog only. */
    PALETTE_ROW_MODEL_ALL
} PaletteRowKind;

/* Stable identity of one row. For commands the identity is `action_id`; for
   models it is the model `id`: the model kind (Current/Recent/All) is a
   section tag that may legitimately change when the source changes and never
   participates in identity. Identity never depends on array position. */
typedef struct {
    /* Display/section tag: the row kind. Not part of identity for models. */
    PaletteRowKind kind;
    /* Action id for commands; 0 for models. */
    int action_id;
    /* Model id for models; empty for commands. */
    wchar_t id[CHAT_MODEL_TEXT];
} PaletteRowKey;

/* One renderable row. All rows are enabled: unavailable commands are
   excluded when the command source is set. */
typedef struct {
    PaletteRowKey key;
    PaletteMode mode;
    /* Mnemonic-free command label, or model display name (the id when the
       provider supplied no name). */
    wchar_t label[CHAT_ACTION_LABEL_TEXT];
    /* Secondary text: the routable id for models, empty for commands. */
    wchar_t note[PALETTE_NOTE_TEXT];
    /* Section tag; a section is one contiguous run of equal values. Commands
       carry registry group + 1, models 1..3 (Current/Recent/All). 0 is the
       "no section" value and never appears in a built source. */
    unsigned group;
} PaletteRow;

typedef struct PalettePalette Palette;

/* Creates an empty controller in commands mode. Returns NULL only on
   allocation failure. */
Palette *palette_create(void);
/* Releases the controller. NULL is safe. */
void palette_dispose(Palette *palette);

void palette_set_mode(Palette *palette, PaletteMode mode);
PaletteMode palette_mode(const Palette *palette);

/* Sets the filter query and rebuilds the visible list. An empty (or NULL)
   query restores every source row. Transactional: on allocation failure the
   previous query, visible list and selection are kept and false is returned. */
bool palette_set_query(Palette *palette, const wchar_t *query);
const wchar_t *palette_query(const Palette *palette);

/* Replaces the command source: every registry action available under
   `context` becomes a row, in registry order, grouped by CHAT_ACTION_GROUP_*.
   Transactional: on allocation failure the previous source, visible list and
   selection are kept and false is returned. */
bool palette_set_commands(Palette *palette, const ChatActionContext *context);

/* Replaces the model source. The merged view is built here from the catalog,
   the current model id and the history MRU (chat_model_catalog_merged
   ordering: current, history, catalog, deduplicated by exact id). Rows are
   tagged Current / Recent / All and sectioned in that order. Transactional
   like palette_set_commands. */
bool palette_set_models(Palette *palette, const ChatModelCatalog *catalog,
    const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count);

/* Visible-row census (after the query filter). */
size_t palette_row_count(const Palette *palette);
const PaletteRow *palette_row(const Palette *palette, size_t index);

/* Sections are derived from the row group tags: a section starts at the
   first visible row whose group differs from the previous row's. */
size_t palette_section_count(const Palette *palette);
/* First visible row index of section `section`, or (size_t)-1. */
size_t palette_section_start(const Palette *palette, size_t section);
/* Header text for a section ("Conversation", "Current", ...), or NULL when
   the section or a label does not exist. */
const wchar_t *palette_section_label(const Palette *palette, size_t section);

/* The highlighted visible-row index, or (size_t)-1 when nothing is
   highlighted (empty list). */
size_t palette_selected(const Palette *palette);
/* Resolves the highlighted row; NULL when nothing is highlighted. */
const PaletteRow *palette_selected_row(const Palette *palette);
/* Moves the highlight to the row with this identity; ignored when the
   identity is not a current visible row. */
void palette_select_key(Palette *palette, const PaletteRowKey *key);
/* Copies the highlighted row's identity; false when nothing is highlighted. */
bool palette_selected_key(const Palette *palette, PaletteRowKey *key);

/* Navigation over the visible list. Step moves by `delta` rows, page by
   PALETTE_PAGE; movement clamps at the ends and never wraps. From no
   highlight, a step or Home selects the first row and End the last. Each
   returns true when the highlight moved. */
bool palette_step(Palette *palette, int delta);
bool palette_page(Palette *palette, int delta);
bool palette_home(Palette *palette);
bool palette_end(Palette *palette);

/* True when a row is highlighted (an empty palette cannot be accepted). */
bool palette_can_accept(const Palette *palette);
/* Copies the highlighted row's identity; true when an activation should
   fire. */
bool palette_accept_key(const Palette *palette, PaletteRowKey *key);

#endif
