#ifndef DARKCHAT_PALETTE_WIN32_H
#define DARKCHAT_PALETTE_WIN32_H

/* Retained DarkUI command palette. The popup is a thin native shell over the
   pure palette controller: it owns its own Ui tree, UiRenderer (the shared
   renderer caches one HWND target, so a second window needs its own) and
   UiAccessibility provider, renders the controller's rows as retained
   buttons under section headers, and forwards input. Selection stays in the
   controller by stable identity; the popup mirrors it into the retained tree
   so reveal scrolling and UIA focus come from the same primitive.
   The modal pump is a separate entry point so the host suite can substitute a
   deterministic pump and drive open/filter/accept/cancel without blocking. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include "commands.h"

typedef struct PalettePopup PalettePopup;

/* Creates the popup (hidden) in commands mode over `owner` and fills the
   controller from `context`, so only commands available under the live state
   become rows. Returns NULL when creation or the source build fails. */
PalettePopup *palette_popup_create(HWND owner,
    const ChatActionContext *context);
/* Destroys the popup. NULL is safe. */
void palette_popup_destroy(PalettePopup *popup);

/* Runs the modal loop until accept, cancel, or close: shows the popup,
   disables the owner, and re-enables it afterwards. */
void palette_popup_pump(PalettePopup *popup);

/* True when the popup was closed by accepting a highlighted row. */
bool palette_popup_accepted(const PalettePopup *popup);
/* The accepted command's registry action id, or 0 when nothing was accepted
   (or the popup is already destroyed). */
int palette_popup_action_id(const PalettePopup *popup);

/* Terminal actions; safe outside the pump as well. Accept is a no-op when no
   row is highlighted or the popup already finished. */
void palette_popup_accept(PalettePopup *popup);
void palette_popup_cancel(PalettePopup *popup);

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
size_t palette_popup_row_count(const PalettePopup *popup);
const wchar_t *palette_popup_row_label(const PalettePopup *popup,
    size_t index);
/* DPI the popup last rescaled itself to (0 when popup is NULL). */
UINT palette_popup_dpi(const PalettePopup *popup);
/* Test/diagnostic entry point over the popup's own UIA provider. The caller
   owns the returned reference; NULL when the popup has no provider. */
struct IRawElementProviderSimple;
struct IRawElementProviderSimple *palette_popup_root_provider(
    PalettePopup *popup);

#endif
