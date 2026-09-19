#ifndef DARKCHAT_PALETTE_WIN32_H
#define DARKCHAT_PALETTE_WIN32_H

/* Retained DarkUI command/model palette. The popup is a thin native shell over
   the pure palette controller: it owns its own Ui tree, UiRenderer (the shared
   renderer caches one HWND target, so a second window needs its own) and
   UiAccessibility provider, renders the controller's rows as retained buttons
   under section headers, and forwards input. Selection stays in the controller
   by stable identity; the popup only realizes a bounded window of rows around
   the viewport and rebinds it as the list scrolls, so a catalog of thousands of
   models costs the same fixed node budget as a handful of commands.
   The modal pump is a separate entry point so the host suite can substitute a
   deterministic pump and drive open/filter/accept/cancel without blocking. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "commands.h"
#include "model_catalog.h"
#include "palette.h"
#include "../ui/ui.h"

typedef struct PalettePopup PalettePopup;

/* Creates the popup (hidden) in commands mode over `owner` and fills the
   controller from `context`, so only commands available under the live state
   become rows. Returns NULL when creation or the source build fails. */
PalettePopup *palette_popup_create(HWND owner,
    const ChatActionContext *context);

/* Creates the popup (hidden) in models mode. The merged view (current, history
   MRU, catalog) is built inside the controller from the caller's catalog,
   current model and backend-filtered history; the caller never passes the raw
   combined history. `initial_id` pre-selects the current model when present.
   Returns NULL when creation or the source build fails. */
PalettePopup *palette_popup_create_models(HWND owner,
    const ChatModelCatalog *catalog, const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    const wchar_t *status, const wchar_t *initial_id);

/* Destroys the popup. NULL is safe. */
void palette_popup_destroy(PalettePopup *popup);

/* Runs the modal loop until accept, cancel, or close: shows the popup,
   disables the owner, and re-enables it afterwards. */
void palette_popup_pump(PalettePopup *popup);

/* True when the popup was closed by accepting a highlighted row. */
bool palette_popup_accepted(const PalettePopup *popup);
/* The accepted command's registry action id, or 0 when nothing was accepted,
   the popup is destroyed, or a model row was accepted. */
int palette_popup_action_id(const PalettePopup *popup);
/* The accepted model id, copied into `out` (size CHAT_MODEL_TEXT). False when
   nothing was accepted, a command was accepted, or the popup is NULL. The id
   is captured at acceptance time, so it survives any later source change. */
bool palette_popup_accepted_model(const PalettePopup *popup,
    wchar_t out[CHAT_MODEL_TEXT]);

PaletteMode palette_popup_mode(const PalettePopup *popup);

/* Terminal actions; safe outside the pump as well. Accept is a no-op when no
   row is highlighted or the popup already finished. */
void palette_popup_accept(PalettePopup *popup);
void palette_popup_cancel(PalettePopup *popup);

/* Transactional in-place model source replacement: preserves the query, the
   selected id and the current scroll position. Returns false (leaving the
   previous source, display and selection fully intact) on allocation failure. */
bool palette_popup_set_models(PalettePopup *popup,
    const ChatModelCatalog *catalog, const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    const wchar_t *status);

/* Deterministic seams for the host suite. */
HWND palette_popup_window(const PalettePopup *popup);
/* The dimmer overlay parked between the owner and the palette (owned by the
   owner, layered, click-through and non-activating); NULL when the popup has
   none or the platform refused it. */
HWND palette_popup_overlay(const PalettePopup *popup);
/* The popup's current window rectangle in screen coordinates. */
RECT palette_popup_bounds(const PalettePopup *popup);
void palette_popup_set_query(PalettePopup *popup, const wchar_t *query);
const wchar_t *palette_popup_query(const PalettePopup *popup);
/* The current status line text (models mode), or "" for commands/NULL. */
const wchar_t *palette_popup_status(const PalettePopup *popup);
size_t palette_popup_row_count(const PalettePopup *popup);
/* The controller's display label for a visible row (name-or-id for models,
   the mnemonic-free command label for commands). Presentation composition is
   not stored per row; use palette_popup_format_model_row for that. */
const wchar_t *palette_popup_row_label(const PalettePopup *popup,
    size_t index);
/* Copies a visible row's stable identity (action id or model id). False when
   the index is out of range. */
bool palette_popup_row_key(const PalettePopup *popup, size_t index,
    PaletteRowKey *out);
/* Copies the currently highlighted model id. False when nothing is
   highlighted or a command row is highlighted. */
bool palette_popup_highlighted_model(const PalettePopup *popup,
    wchar_t out[CHAT_MODEL_TEXT]);
/* Selects the model with this id and scrolls it into view. */
void palette_popup_set_selected_model(PalettePopup *popup, const wchar_t *id);
/* One-line presentation of a model row: "<name, truncated if needed>… —
   <complete id>". The complete id always survives; the name is shortened with
   a UTF-16 surrogate-safe cut. `out` must hold UI_TEXT_CAPACITY code units. */
void palette_popup_format_model_row(wchar_t out[UI_TEXT_CAPACITY],
    const wchar_t *name, const wchar_t *id);
/* DPI the popup last rescaled itself to (0 when popup is NULL). */
UINT palette_popup_dpi(const PalettePopup *popup);
/* Current scroll offset of the row list in DIPs (0 when popup is NULL or the
   list has never been laid out). Diagnostic/test seam. */
float palette_popup_scroll_offset(const PalettePopup *popup);
/* Test/diagnostic entry point over the popup's own UIA provider. The caller
   owns the returned reference; NULL when the popup has no provider. */
struct IRawElementProviderSimple;
struct IRawElementProviderSimple *palette_popup_root_provider(
    PalettePopup *popup);

#endif
