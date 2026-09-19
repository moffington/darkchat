#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include "palette_win32.h"
#include "palette.h"
#include "../platform/renderer.h"
#include "../platform/accessibility.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uiautomationcore.h>

/* Layout is authored in 96-DPI DIPs inside the retained tree; the popup
   window itself is sized in pixels via MulDiv so it stays crisp at any DPI.
   The window is a borderless dark WS_POPUP: the renderer clears the client
   with the theme background and the retained tree paints everything else.
   A separate owned layered dimmer window is parked between the owner and the
   palette while the modal loop is live.

   Virtualized rows: the controller owns every filtered row, but the tree keeps
   only a bounded window of presentation nodes around the viewport. Two spacer
   labels encode the off-screen extents so the scroll container's content
   height still represents the complete logical list (headers included). The
   spacers carry a non-breaking-space accessible name so they never surface as
   unnamed UIA elements. */
#define PALETTE_WINDOW_WIDTH 580.0f
#define PALETTE_WINDOW_HEIGHT 420.0f
#define PALETTE_ROW_HEIGHT 32.0f
#define PALETTE_HEADER_HEIGHT 26.0f
#define PALETTE_QUERY_HEIGHT 44.0f
#define PALETTE_STATUS_HEIGHT 22.0f
#define PALETTE_EDGE 16.0f        /* clamp margin around the owner */
#define PALETTE_CENTER_BIAS 0.40f /* vertical placement above center */
#define PALETTE_CLASS L"DarkChat.Palette"
#define PALETTE_DIMMER_CLASS L"DarkChat.PaletteDimmer"
#define PALETTE_DIMMER_ALPHA 30   /* ~12% black over the disabled owner */
#define PALETTE_QUERY_HINT_COMMANDS L"Type to filter commands"
#define PALETTE_QUERY_HINT_MODELS L"Type to filter models"
/* Presentation nodes realized at once. Sized far above any viewport (the
   window shows at most ~14 lines) so headers cannot exhaust it, and far below
   the fixed Ui arena capacity once chrome and spacers are counted. */
#define PALETTE_SLOT_CAPACITY 64

typedef struct {
    UiId node;              /* current generation handle, or UI_NONE */
    PaletteRowKey key;      /* valid for rows; cleared for headers */
    size_t logical_index;   /* index into the logical line list */
    bool header;
} PalettePopupSlot;

struct PalettePopup {
    HWND owner, window;
    Ui ui;                          /* retained tree: query + status + rows */
    UiRenderer renderer;            /* one HWND target per renderer */
    UiAccessibility *accessibility;
    Palette *controller;            /* pure selection/filter state */
    UiId query_text;                /* shows the live filter query */
    UiId status_text;               /* model catalog status (models mode) */
    UiId scroll;                    /* row list scroll container */
    UiId spacer_top, spacer_bottom; /* off-window logical extent */
    PalettePopupSlot *slots;        /* bounded presentation pool */
    size_t slot_capacity, slot_count;
    size_t bound_first;             /* first logical line currently realized */
    PaletteMode mode;
    wchar_t query[PALETTE_QUERY_TEXT];
    wchar_t status[CHAT_STATUS_TEXT];
    HWND overlay;                   /* owned, non-activating dimmer */
    UINT dpi;
    bool done, accepted, accepted_is_model, tracking;
    int action_id;
    wchar_t accepted_model[CHAT_MODEL_TEXT];
};

static LRESULT CALLBACK palette_proc(HWND window, UINT message, WPARAM w,
    LPARAM l);
static void bind_window(PalettePopup *popup, size_t first);
static void rebind_if_scroll_changed(PalettePopup *popup);
static void sync_selection(PalettePopup *popup, bool keyboard);

static UINT popup_dpi(HWND owner) {
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    return dpi ? dpi : 96;
}

static const wchar_t *query_hint(const PalettePopup *popup) {
    return popup->mode == PALETTE_MODE_MODELS
        ? PALETTE_QUERY_HINT_MODELS : PALETTE_QUERY_HINT_COMMANDS;
}

/* One-line model presentation: "<name, truncated if needed>… — <complete id>".
   The complete routable id always survives; the name is cut (never splitting a
   UTF-16 surrogate pair) only when the whole string would exceed the toolkit's
   text capacity, so two long identical names stay distinguishable by their
   ids in both the visible text and the accessible name. */
void palette_popup_format_model_row(wchar_t out[UI_TEXT_CAPACITY],
    const wchar_t *name, const wchar_t *id) {
    if (!out) return;
    if (!id) id = L"";
    size_t id_length = wcslen(id);
    if (id_length > UI_TEXT_CAPACITY - 1) id_length = UI_TEXT_CAPACITY - 1;
    if (!name || !name[0] || !wcscmp(name, id)) {
        wcsncpy(out, id, id_length);
        out[id_length] = 0;
        return;
    }
    /* Reserve the id, the " — " separator (3) and the ellipsis (1). */
    size_t budget = UI_TEXT_CAPACITY - 1 - id_length - 4;
    size_t name_length = wcslen(name);
    size_t take = name_length;
    if (take > budget) {
        take = budget;
        if (take && name[take - 1] >= 0xd800 && name[take - 1] <= 0xdbff)
            --take;
    }
    size_t at = 0;
    wmemcpy(out, name, take);
    at = take;
    if (name_length > take) out[at++] = 0x2026; /* … */
    out[at++] = L' ';
    out[at++] = 0x2014; /* — */
    out[at++] = L' ';
    wmemcpy(out + at, id, id_length);
    at += id_length;
    out[at] = 0;
}

/* Stable identity comparison, mirroring the controller's rule: kind
   (command vs model) and the id (or action id) decide; the section tag never
   participates. */
static bool key_same(const PaletteRowKey *a, const PaletteRowKey *b) {
    if (a->action_id != b->action_id) return false;
    if ((a->kind == PALETTE_ROW_COMMAND) !=
        (b->kind == PALETTE_ROW_COMMAND))
        return false;
    return a->kind == PALETTE_ROW_COMMAND || !wcscmp(a->id, b->id);
}

/* ---- Logical line model ------------------------------------------------- */

/* A section header precedes row 0 and any row whose group differs from the
   previous row's. */
static size_t line_count(const Palette *controller) {
    return palette_row_count(controller) +
        palette_section_count(controller);
}

/* One contiguous section run: rows [start, end) preceded by one header line.
   All line arithmetic below iterates sections (at most one per registry group
   or model bucket), never rows, so it is independent of catalog size. */
static void section_run(const Palette *controller, size_t section,
    size_t *start, size_t *end) {
    size_t sections = palette_section_count(controller);
    *start = palette_section_start(controller, section);
    *end = section + 1 < sections
        ? palette_section_start(controller, section + 1)
        : palette_row_count(controller);
}

static bool line_is_header(const Palette *controller, size_t line) {
    size_t sections = palette_section_count(controller);
    size_t cursor = 0;
    for (size_t s = 0; s < sections; s++) {
        size_t start, end;
        section_run(controller, s, &start, &end);
        if (line == cursor) return true;
        cursor += 1 + (end - start);
        if (line < cursor) return false;
    }
    return false;
}

static float line_height(const Palette *controller, size_t line) {
    return line_is_header(controller, line)
        ? PALETTE_HEADER_HEIGHT : PALETTE_ROW_HEIGHT;
}

static float line_offset(const Palette *controller, size_t line) {
    float y = 0;
    size_t cursor = 0;
    size_t sections = palette_section_count(controller);
    for (size_t s = 0; s < sections && cursor < line; s++) {
        size_t start, end;
        section_run(controller, s, &start, &end);
        y += PALETTE_HEADER_HEIGHT;
        ++cursor;
        size_t count = end - start;
        size_t remaining = cursor < line ? line - cursor : 0;
        size_t take = remaining < count ? remaining : count;
        y += (float)take * PALETTE_ROW_HEIGHT;
        cursor += take;
    }
    return y;
}

static size_t line_of_offset(const Palette *controller, float offset) {
    if (offset <= 0) return 0;
    float y = 0;
    size_t cursor = 0;
    size_t sections = palette_section_count(controller);
    for (size_t s = 0; s < sections; s++) {
        size_t start, end;
        section_run(controller, s, &start, &end);
        if (y + PALETTE_HEADER_HEIGHT > offset) return cursor;
        y += PALETTE_HEADER_HEIGHT;
        ++cursor;
        size_t count = end - start;
        if (count) {
            float rows_height = (float)count * PALETTE_ROW_HEIGHT;
            if (y + rows_height > offset) {
                size_t skip = (size_t)((offset - y) / PALETTE_ROW_HEIGHT);
                if (skip >= count) skip = count - 1;
                return cursor + skip;
            }
            y += rows_height;
            cursor += count;
        }
    }
    size_t total = line_count(controller);
    return total ? total - 1 : 0;
}

static size_t line_of_row(const Palette *controller, size_t row) {
    size_t sections = palette_section_count(controller);
    size_t cursor = 0;
    for (size_t s = 0; s < sections; s++) {
        size_t start, end;
        section_run(controller, s, &start, &end);
        if (row < end) return cursor + 1 + (row - start);
        cursor += 1 + (end - start);
    }
    return cursor;
}

/* Section that owns `line` (its header line or one of its row lines). */
static size_t section_of_line(const Palette *controller, size_t line) {
    size_t sections = palette_section_count(controller);
    size_t cursor = 0;
    for (size_t s = 0; s < sections; s++) {
        size_t start, end;
        section_run(controller, s, &start, &end);
        if (line < cursor + 1 + (end - start)) return s;
        cursor += 1 + (end - start);
    }
    return sections ? sections - 1 : 0;
}

static void set_spacer(PalettePopup *popup, UiId id, float height) {
    UiNode *node = ui_node(&popup->ui, id);
    if (!node) return;
    node->style.height = ui_fixed(height);
    node->style.background = -1;
}

/* ---- Presentation mirror ------------------------------------------------ */

/* Rebuilds the bounded window of presentation nodes starting at logical line
   `first`. Presentation nodes are removed and re-added so their UiKind always
   matches the item (header label vs row button) and so a stale handle from the
   previous window cannot be invoked (ui_remove bumps the generation). All
   slots are re-added between the fixed top spacer and a freshly added bottom
   spacer, so sibling order is always top, items, bottom. */
static void bind_window(PalettePopup *popup, size_t first) {
    Ui *ui = &popup->ui;
    const Palette *controller = popup->controller;

    PaletteRowKey focused;
    bool had_focus = false, was_keyboard = ui->keyboard_focus;
    if (ui->focus) {
        for (size_t i = 0; i < popup->slot_count; i++)
            if (popup->slots[i].node == ui->focus &&
                !popup->slots[i].header) {
                focused = popup->slots[i].key;
                had_focus = true;
                break;
            }
    }

    for (size_t i = 0; i < popup->slot_count; i++) {
        if (popup->slots[i].node) ui_remove(ui, popup->slots[i].node);
        popup->slots[i].node = UI_NONE;
    }
    popup->slot_count = 0;
    if (popup->spacer_bottom) {
        ui_remove(ui, popup->spacer_bottom);
        popup->spacer_bottom = UI_NONE;
    }

    size_t total = line_count(controller);
    if (first > total) first = total;
    set_spacer(popup, popup->spacer_top, line_offset(controller, first));

    /* Walk the window with a row/section cursor instead of resolving each
       line independently: one section scan for the whole window, then O(1) per
       realized node. This keeps a rebuild cost independent of catalog size. */
    size_t sections = palette_section_count(controller);
    size_t section = section_of_line(controller, first);
    size_t section_start = section < sections
        ? palette_section_start(controller, section) : 0;
    size_t section_end = section + 1 < sections
        ? palette_section_start(controller, section + 1)
        : palette_row_count(controller);
    bool at_header = false;
    size_t row = section_start;
    if (first < total) {
        /* The section header sits one line before its first row. */
        size_t header_line = line_of_row(controller, section_start);
        header_line = header_line ? header_line - 1 : 0;
        if (first == header_line)
            at_header = true;
        else
            row = section_start + (first - (header_line + 1));
    }

    size_t realized = 0;
    for (size_t l = first; l < total && realized < popup->slot_capacity;
         l++) {
        bool header = at_header;
        size_t added_row = row;
        UiId id = UI_NONE;
        if (header) {
            const wchar_t *label = palette_section_label(controller, section);
            id = ui_add(ui, popup->scroll, UI_LABEL, label ? label : L"");
            if (id) {
                UiNode *node = ui_node(ui, id);
                if (node) {
                    node->style.height = ui_fixed(PALETTE_HEADER_HEIGHT);
                    node->style.font = UI_SECTION;
                    node->style.foreground = UI_MUTED;
                }
                ui_set_accessible_name(ui, id, label ? label : L"");
            }
            at_header = false;
        } else {
            const PaletteRow *source = palette_row(controller, row);
            if (!source) break;
            wchar_t text[UI_TEXT_CAPACITY];
            if (popup->mode == PALETTE_MODE_MODELS)
                palette_popup_format_model_row(text, source->label,
                    source->note);
            else {
                wcsncpy(text, source->label, UI_TEXT_CAPACITY - 1);
                text[UI_TEXT_CAPACITY - 1] = 0;
            }
            id = ui_add(ui, popup->scroll, UI_BUTTON, text);
            if (id) {
                UiNode *node = ui_node(ui, id);
                if (node) {
                    node->style.height = ui_fixed(PALETTE_ROW_HEIGHT);
                    node->style.font = UI_BODY;
                    node->style.flat = true;
                }
                ui_set_accessible_name(ui, id, text);
            }
            ++row;
            if (row >= section_end) {
                ++section;
                if (section < sections) {
                    section_start = section_end;
                    section_end = section + 1 < sections
                        ? palette_section_start(controller, section + 1)
                        : palette_row_count(controller);
                    row = section_start;
                    at_header = true;
                }
            }
        }
        if (!id) break;
        PalettePopupSlot *slot = &popup->slots[realized];
        slot->node = id;
        slot->header = header;
        slot->logical_index = l;
        if (header) memset(&slot->key, 0, sizeof slot->key);
        else slot->key = palette_row(controller, added_row)->key;
        ++realized;
    }
    popup->slot_count = realized;

    float total_height = line_offset(controller, total);
    float bottom = total_height - line_offset(controller, first + realized);
    if (bottom < 0) bottom = 0;
    popup->spacer_bottom = ui_add(ui, popup->scroll, UI_LABEL, L"");
    if (popup->spacer_bottom) {
        ui_set_accessibility_hidden(ui, popup->spacer_bottom, true);
        set_spacer(popup, popup->spacer_bottom, bottom);
    }

    if (had_focus) {
        for (size_t i = 0; i < popup->slot_count; i++)
            if (!popup->slots[i].header &&
                key_same(&popup->slots[i].key, &focused)) {
                ui_focus(ui, popup->slots[i].node, was_keyboard);
                break;
            }
    }

    popup->bound_first = first;
    ui_layout(ui, ui->width, ui->height);
    sync_selection(popup, false);
    ui_invalidate(ui, false);
}

/* Single funnel for every source of scrolling or layout change. The realized
   nodes depend only on the first logical line, so a fractional offset change
   inside the same line needs no rebuild (normal layout moves them); query and
   source replacement force a rebuild by invalidating bound_first. */
static void rebind_if_scroll_changed(PalettePopup *popup) {
    Ui *ui = &popup->ui;
    if (ui_layout_pending(ui)) ui_layout(ui, ui->width, ui->height);
    float offset = ui_scroll_offset(ui, popup->scroll);
    size_t first = line_of_offset(popup->controller, offset);
    if (first == popup->bound_first) return;
    bind_window(popup, first);
}

/* Mirrors the controller's highlight into the realized window, refreshing the
   selected flag on bound slots and (for keyboard movement) real retained focus
   so UIA sees the focus change and the reveal keeps the row in view. */
static void sync_selection(PalettePopup *popup, bool keyboard) {
    Ui *ui = &popup->ui;
    for (size_t i = 0; i < popup->slot_count; i++) {
        if (popup->slots[i].header) continue;
        UiNode *node = ui_node(ui, popup->slots[i].node);
        if (node) node->selected = false;
    }
    const PaletteRow *selected = palette_selected_row(popup->controller);
    if (!selected) { ui_invalidate(ui, false); return; }
    for (size_t i = 0; i < popup->slot_count; i++) {
        if (popup->slots[i].header) continue;
        if (!key_same(&popup->slots[i].key, &selected->key)) continue;
        UiNode *node = ui_node(ui, popup->slots[i].node);
        if (node) node->selected = true;
        if (keyboard) ui_focus(ui, popup->slots[i].node, true);
        break;
    }
    ui_invalidate(ui, false);
}

/* Scrolls the highlighted row into the window and rebinds, then refreshes the
   selection mirror. Used after keyboard navigation and after source changes. */
static void ensure_selected_bound(PalettePopup *popup, bool keyboard) {
    Ui *ui = &popup->ui;
    if (ui_layout_pending(ui)) ui_layout(ui, ui->width, ui->height);
    size_t selected = palette_selected(popup->controller);
    if (selected == (size_t)-1) {
        sync_selection(popup, keyboard);
        return;
    }
    size_t line = line_of_row(popup->controller, selected);
    float offset = ui_scroll_offset(ui, popup->scroll);
    float viewport = ui_scroll_viewport_h(ui, popup->scroll);
    float top = line_offset(popup->controller, line);
    float height = line_height(popup->controller, line);
    float next = offset;
    if (top < offset) next = top;
    else if (top + height > offset + viewport) next = top + height - viewport;
    if (next != offset) {
        ui_scroll_to(ui, popup->scroll, next);
        popup->bound_first = (size_t)-1;
    }
    rebind_if_scroll_changed(popup);
    sync_selection(popup, keyboard);
}

/* Applies the popup's query buffer to the controller and re-mirrors. */
static void apply_query(PalettePopup *popup) {
    if (palette_set_query(popup->controller, popup->query)) {
        popup->bound_first = (size_t)-1;
        rebind_if_scroll_changed(popup);
        ensure_selected_bound(popup, false);
    }
    wchar_t shown[PALETTE_QUERY_TEXT];
    swprintf(shown, PALETTE_QUERY_TEXT, L"%ls",
        popup->query[0] ? popup->query : query_hint(popup));
    ui_set_text(&popup->ui, popup->query_text, shown);
    ui_set_accessible_name(&popup->ui, popup->query_text, shown);
    InvalidateRect(popup->window, NULL, FALSE);
}

/* Retained activation (mouse click, UIA Invoke) resolves identity through the
   slot's stored key, never a logical index that may have moved, and ignores an
   event whose identity no longer resolves. */
static void palette_event(void *user, Ui *ui, UiEvent event) {
    (void)ui;
    PalettePopup *popup = (PalettePopup *)user;
    if (!popup || event.kind != UI_ACTIVATE) return;
    for (size_t i = 0; i < popup->slot_count; i++) {
        if (popup->slots[i].node != event.id || popup->slots[i].header)
            continue;
        palette_select_key(popup->controller, &popup->slots[i].key);
        const PaletteRow *selected = palette_selected_row(popup->controller);
        if (!selected || !key_same(&selected->key, &popup->slots[i].key))
            return;
        sync_selection(popup, false);
        palette_popup_accept(popup);
        return;
    }
}

void palette_popup_accept(PalettePopup *popup) {
    if (!popup || popup->done) return;
    PaletteRowKey key;
    if (!palette_accept_key(popup->controller, &key)) return;
    popup->accepted = true;
    popup->done = true;
    if (key.kind == PALETTE_ROW_COMMAND) {
        popup->action_id = key.action_id;
        popup->accepted_is_model = false;
        popup->accepted_model[0] = 0;
    } else {
        popup->action_id = 0;
        popup->accepted_is_model = true;
        wcsncpy(popup->accepted_model, key.id, CHAT_MODEL_TEXT - 1);
        popup->accepted_model[CHAT_MODEL_TEXT - 1] = 0;
    }
}

void palette_popup_cancel(PalettePopup *popup) {
    if (!popup || popup->done) return;
    popup->accepted = false;
    popup->done = true;
    popup->action_id = 0;
    popup->accepted_is_model = false;
    popup->accepted_model[0] = 0;
}

bool palette_popup_accepted(const PalettePopup *popup) {
    return popup && popup->accepted;
}

int palette_popup_action_id(const PalettePopup *popup) {
    return popup && popup->accepted && !popup->accepted_is_model
        ? popup->action_id : 0;
}

bool palette_popup_accepted_model(const PalettePopup *popup,
    wchar_t out[CHAT_MODEL_TEXT]) {
    if (out) out[0] = 0;
    if (!popup || !popup->accepted || !popup->accepted_is_model || !out)
        return false;
    wcsncpy(out, popup->accepted_model, CHAT_MODEL_TEXT - 1);
    out[CHAT_MODEL_TEXT - 1] = 0;
    return true;
}

PaletteMode palette_popup_mode(const PalettePopup *popup) {
    return popup ? popup->mode : PALETTE_MODE_COMMANDS;
}

/* Centers the palette over the owner's client area, slightly above vertical
   center, clamped so it never exceeds the owner or leaves the monitor's work
   area. Single authority for placement, so creation and DPI changes cannot
   drift apart. */
static void place_popup(PalettePopup *popup) {
    if (!popup->window || !popup->owner || !IsWindow(popup->owner)) return;
    UINT dpi = popup->dpi ? popup->dpi : 96;
    int margin = MulDiv((int)PALETTE_EDGE, (int)dpi, 96);
    int width = MulDiv((int)PALETTE_WINDOW_WIDTH, (int)dpi, 96);
    int height = MulDiv((int)PALETTE_WINDOW_HEIGHT, (int)dpi, 96);
    RECT client;
    GetClientRect(popup->owner, &client);
    POINT origin = { 0, 0 };
    ClientToScreen(popup->owner, &origin);
    RECT area = { origin.x, origin.y,
        origin.x + client.right, origin.y + client.bottom };
    int area_w = area.right - area.left;
    int area_h = area.bottom - area.top;
    MONITORINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    RECT work = area;
    HMONITOR monitor = MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) work = info.rcWork;
    if (width > (work.right - work.left) - 2 * margin)
        width = (work.right - work.left) - 2 * margin;
    if (height > (work.bottom - work.top) - 2 * margin)
        height = (work.bottom - work.top) - 2 * margin;
    /* A small owner keeps the palette inside its own client area. */
    if (width > area_w - 2 * margin)
        width = area_w > 2 * margin ? area_w - 2 * margin : area_w;
    if (height > area_h - 2 * margin)
        height = area_h > 2 * margin ? area_h - 2 * margin : area_h;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    int x = area.left + (area_w - width) / 2;
    int y = area.top + (int)((float)(area_h - height) * PALETTE_CENTER_BIAS);
    if (x < work.left + margin) x = work.left + margin;
    if (y < work.top + margin) y = work.top + margin;
    if (x + width > work.right - margin) x = work.right - margin - width;
    if (y + height > work.bottom - margin) y = work.bottom - margin - height;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(popup->window, NULL, x, y, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK palette_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    PalettePopup *popup = (PalettePopup *)GetWindowLongPtrW(window,
        GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        popup = (PalettePopup *)((CREATESTRUCTW *)l)->lpCreateParams;
        popup->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)popup);
    }
    if (!popup) return DefWindowProcW(window, message, w, l);
    Ui *ui = &popup->ui;
    switch (message) {
    case WM_CREATE: {
        popup->dpi = popup_dpi(popup->owner);
        const wchar_t *title = popup->mode == PALETTE_MODE_MODELS
            ? L"Choose model" : L"Command palette";
        UiId root = ui_add(ui, UI_NONE, UI_COLUMN, title);
        if (!root) return -1;
        UiNode *node = ui_node(ui, root);
        if (node) {
            node->style.padding = 10;
            node->style.gap = 8;
            node->style.background = UI_PANEL;
            node->style.border = true;
        }
        popup->query_text = ui_add(ui, root, UI_LABEL, query_hint(popup));
        if (!popup->query_text) return -1;
        node = ui_node(ui, popup->query_text);
        if (node) {
            node->style.height = ui_fixed(PALETTE_QUERY_HEIGHT);
            node->style.background = UI_TRACK;
            node->style.border = true;
            node->style.font = UI_BODY;
            /* Keep the hint off the field's border. */
            node->style.text_inset = 6;
            ui_set_help_text(ui, popup->query_text, query_hint(popup));
        }
        popup->status_text = ui_add(ui, root, UI_LABEL, popup->status);
        if (!popup->status_text) return -1;
        node = ui_node(ui, popup->status_text);
        if (node) {
            node->style.height = ui_fixed(PALETTE_STATUS_HEIGHT);
            node->style.font = UI_SMALL;
            node->style.foreground = UI_MUTED;
        }
        /* The status line is model-only chrome; command mode is pixel-identical
           to the command-only popup. */
        if (popup->mode != PALETTE_MODE_MODELS)
            ui_set_hidden(ui, popup->status_text, true);
        UiId separator = ui_add(ui, root, UI_SEPARATOR, L"");
        if (!separator) return -1;
        node = ui_node(ui, separator);
        if (node) node->style.height = ui_fixed(1);
        popup->scroll = ui_add(ui, root, UI_SCROLL, L"");
        if (!popup->scroll) return -1;
        node = ui_node(ui, popup->scroll);
        if (node) {
            node->style.height = ui_flex(1);
            /* Exact virtual extents: spacers and rows stack with no implicit
               gap, so spacer heights equal the true off-window offset. */
            node->style.gap = 0;
        }
        popup->accessibility = ui_accessibility_create(window, ui);
        if (!popup->accessibility) return -1;
        /* Establish the layout size before binding rows: the selection mirror
           runs a reveal, and a reveal against a zero-size viewport would
           scroll the first section header out of view. */
        ui_layout(ui, PALETTE_WINDOW_WIDTH, PALETTE_WINDOW_HEIGHT);
        popup->spacer_top = ui_add(ui, popup->scroll, UI_LABEL, L"");
        if (!popup->spacer_top) return -1;
        ui_set_accessibility_hidden(ui, popup->spacer_top, true);
        set_spacer(popup, popup->spacer_top, 0);
        bind_window(popup, 0);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        renderer_paint(&popup->renderer, window, ui, UI_NONE);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        RECT client;
        GetClientRect(window, &client);
        renderer_resize(&popup->renderer, (UINT)client.right,
            (UINT)client.bottom, (float)popup->dpi);
        ui_invalidate(ui, true);
        rebind_if_scroll_changed(popup);
        return 0;
    }
    case WM_DPICHANGED: {
        popup->dpi = HIWORD(w);
        renderer_resize(&popup->renderer, 0, 0, (float)popup->dpi);
        ui_invalidate(ui, true);
        /* Placement is recomputed around the new scale, so the palette stays
           centered over the owner instead of chasing the suggested rect. */
        place_popup(popup);
        rebind_if_scroll_changed(popup);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    }
    case WM_SETFOCUS:
        ui_set_active(ui, true);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_KILLFOCUS:
        ui_set_active(ui, false);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        ui_set_active(ui, LOWORD(w) != WA_INACTIVE);
        return 0;
    case WM_GETOBJECT: {
        LRESULT result = ui_accessibility_get_object(popup->accessibility,
            w, l);
        if (result) return result;
        break;
    }
    case UI_WM_ACCESSIBILITY_INVOKE:
        ui_accessibility_handle_message(popup->accessibility, message, w);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        /* Keyboard-first: the palette swallows every key so nothing reaches
           the disabled owner while the modal loop is live. */
        if (w == VK_ESCAPE || w == VK_F10 || w == VK_APPS) {
            palette_popup_cancel(popup);
            return 0;
        }
        if (w == VK_RETURN) { palette_popup_accept(popup); return 0; }
        if (w == VK_UP || w == VK_DOWN || w == VK_PRIOR || w == VK_NEXT ||
            w == VK_HOME || w == VK_END) {
            bool moved = false;
            if (w == VK_UP) moved = palette_step(popup->controller, -1);
            else if (w == VK_DOWN) moved = palette_step(popup->controller, 1);
            else if (w == VK_PRIOR) moved = palette_page(popup->controller, -1);
            else if (w == VK_NEXT) moved = palette_page(popup->controller, 1);
            else if (w == VK_HOME) moved = palette_home(popup->controller);
            else moved = palette_end(popup->controller);
            if (moved) {
                ensure_selected_bound(popup, true);
                InvalidateRect(window, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    case WM_CHAR:
        if (w == L'\b') {
            size_t length = wcslen(popup->query);
            if (length) {
                length--;
                /* Never strand the high half of a surrogate pair. */
                if (length && popup->query[length - 1] >= 0xD800 &&
                    popup->query[length - 1] <= 0xDBFF)
                    length--;
                popup->query[length] = 0;
                apply_query(popup);
            }
            return 0;
        }
        if (w >= 32 && w != 127) {
            size_t length = wcslen(popup->query);
            if (length + 1 < PALETTE_QUERY_TEXT) {
                popup->query[length] = (wchar_t)w;
                popup->query[length + 1] = 0;
                apply_query(popup);
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!popup->tracking) {
            TRACKMOUSEEVENT tracking = { sizeof tracking, TME_LEAVE, window, 0 };
            popup->tracking = TrackMouseEvent(&tracking) != FALSE;
        }
        POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ui_pointer_move(ui, (float)point.x * 96.0f / popup->dpi,
            (float)point.y * 96.0f / popup->dpi);
        InvalidateRect(window, NULL, FALSE);
        rebind_if_scroll_changed(popup);
        return 0;
    }
    case WM_MOUSELEAVE:
        popup->tracking = false;
        ui_pointer_leave(ui);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        ui_pointer_down(ui, (float)GET_X_LPARAM(l) * 96.0f / popup->dpi,
            (float)GET_Y_LPARAM(l) * 96.0f / popup->dpi);
        if (ui->pressed) SetCapture(window);
        InvalidateRect(window, NULL, FALSE);
        rebind_if_scroll_changed(popup);
        return 0;
    case WM_LBUTTONUP:
        ui_pointer_up(ui, (float)GET_X_LPARAM(l) * 96.0f / popup->dpi,
            (float)GET_Y_LPARAM(l) * 96.0f / popup->dpi);
        if (GetCapture() == window && !ui->pressed) ReleaseCapture();
        InvalidateRect(window, NULL, FALSE);
        rebind_if_scroll_changed(popup);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(window, &point);
        float step = 3 * ui->theme.control_height;
        ui_scroll(ui, (float)point.x * 96.0f / popup->dpi,
            (float)point.y * 96.0f / popup->dpi,
            -(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * step);
        InvalidateRect(window, NULL, FALSE);
        rebind_if_scroll_changed(popup);
        return 0;
    }
    case WM_CLOSE:
        palette_popup_cancel(popup);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, w, l);
}

static void register_class(void) {
    WNDCLASSW cls;
    memset(&cls, 0, sizeof cls);
    cls.lpfnWndProc = palette_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = PALETTE_CLASS;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
}

static void register_dimmer_class(void) {
    WNDCLASSW cls;
    memset(&cls, 0, sizeof cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = PALETTE_DIMMER_CLASS;
    cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&cls);
}

/* Creates the dimmer overlay owned by the host window: layered black at
   ~12% opacity, click-through and non-activating, so it can never take
   focus or input from the palette. It is parked between the owner and the
   palette and destroyed on every palette exit path. */
static void create_overlay(PalettePopup *popup) {
    register_dimmer_class();
    RECT bounds;
    if (!popup->owner || !IsWindow(popup->owner)) return;
    if (!GetWindowRect(popup->owner, &bounds)) return;
    popup->overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_TOOLWINDOW,
        PALETTE_DIMMER_CLASS, L"", WS_POPUP,
        bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, popup->owner, NULL,
        GetModuleHandleW(NULL), NULL);
    if (popup->overlay)
        SetLayeredWindowAttributes(popup->overlay, 0, PALETTE_DIMMER_ALPHA,
            LWA_ALPHA);
}

static void show_overlay(PalettePopup *popup) {
    if (!popup->overlay || !IsWindow(popup->overlay) ||
        !IsWindow(popup->owner))
        return;
    RECT bounds;
    if (GetWindowRect(popup->owner, &bounds))
        SetWindowPos(popup->overlay, NULL, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(popup->overlay, SW_SHOWNA);
}

static void hide_overlay(PalettePopup *popup) {
    if (popup->overlay && IsWindow(popup->overlay))
        ShowWindow(popup->overlay, SW_HIDE);
}

/* Allocates the controller, renderer and bounded slot pool; the window is
   created separately so its WM_CREATE can bind the source already in place. */
static PalettePopup *popup_new(HWND owner, PaletteMode mode) {
    PalettePopup *popup = (PalettePopup *)calloc(1, sizeof *popup);
    if (!popup) return NULL;
    popup->owner = owner;
    popup->mode = mode;
    popup->dpi = popup_dpi(owner);
    popup->bound_first = (size_t)-1;
    popup->slots = (PalettePopupSlot *)calloc(PALETTE_SLOT_CAPACITY,
        sizeof *popup->slots);
    if (!popup->slots) { free(popup); return NULL; }
    popup->slot_capacity = PALETTE_SLOT_CAPACITY;
    ui_init(&popup->ui, renderer_measure, &popup->renderer);
    popup->ui.on_event = palette_event;
    popup->ui.event_user = popup;
    if (FAILED(renderer_init(&popup->renderer, &popup->ui.theme))) {
        free(popup->slots);
        free(popup);
        return NULL;
    }
    popup->controller = palette_create();
    if (!popup->controller) {
        renderer_dispose(&popup->renderer);
        free(popup->slots);
        free(popup);
        return NULL;
    }
    palette_set_mode(popup->controller, mode);
    return popup;
}

static void popup_release(PalettePopup *popup) {
    if (!popup) return;
    if (popup->window && IsWindow(popup->window))
        DestroyWindow(popup->window);
    if (popup->overlay && IsWindow(popup->overlay))
        DestroyWindow(popup->overlay);
    ui_accessibility_destroy(popup->accessibility);
    palette_dispose(popup->controller);
    renderer_dispose(&popup->renderer);
    free(popup->slots);
    free(popup);
}

static bool popup_make_window(PalettePopup *popup) {
    register_class();
    const wchar_t *title = popup->mode == PALETTE_MODE_MODELS
        ? L"Choose model" : L"Command palette";
    popup->window = CreateWindowExW(WS_EX_TOOLWINDOW, PALETTE_CLASS,
        title, WS_POPUP, 0, 0,
        MulDiv((int)PALETTE_WINDOW_WIDTH, (int)popup->dpi, 96),
        MulDiv((int)PALETTE_WINDOW_HEIGHT, (int)popup->dpi, 96),
        popup->owner, NULL, GetModuleHandleW(NULL), popup);
    if (!popup->window) return false;
    /* Windows 11 rounds top-level popups on request; older systems fail the
       attribute call and keep square corners (silently ignored here). */
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(popup->window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &preference, sizeof preference);
    create_overlay(popup);
    place_popup(popup);
    return true;
}

PalettePopup *palette_popup_create(HWND owner,
    const ChatActionContext *context) {
    if (!owner || !context) return NULL;
    PalettePopup *popup = popup_new(owner, PALETTE_MODE_COMMANDS);
    if (!popup) return NULL;
    if (!palette_set_commands(popup->controller, context)) {
        popup_release(popup);
        return NULL;
    }
    if (!popup_make_window(popup)) {
        popup_release(popup);
        return NULL;
    }
    return popup;
}

PalettePopup *palette_popup_create_models(HWND owner,
    const ChatModelCatalog *catalog, const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    const wchar_t *status, const wchar_t *initial_id) {
    if (!owner) return NULL;
    PalettePopup *popup = popup_new(owner, PALETTE_MODE_MODELS);
    if (!popup) return NULL;
    if (!palette_set_models(popup->controller, catalog, current_id, history,
            history_count)) {
        popup_release(popup);
        return NULL;
    }
    if (initial_id && initial_id[0]) {
        PaletteRowKey key;
        memset(&key, 0, sizeof key);
        key.kind = PALETTE_ROW_MODEL_ALL;
        wcsncpy(key.id, initial_id, CHAT_MODEL_TEXT - 1);
        key.id[CHAT_MODEL_TEXT - 1] = 0;
        palette_select_key(popup->controller, &key);
    }
    if (status) {
        wcsncpy(popup->status, status, CHAT_STATUS_TEXT - 1);
        popup->status[CHAT_STATUS_TEXT - 1] = 0;
    }
    if (!popup_make_window(popup)) {
        popup_release(popup);
        return NULL;
    }
    return popup;
}

void palette_popup_pump(PalettePopup *popup) {
    if (!popup) return;
    if (!popup->done) {
        show_overlay(popup);
        ShowWindow(popup->window, SW_SHOW);
        SetFocus(popup->window);
        if (popup->owner) EnableWindow(popup->owner, FALSE);
        MSG message;
        BOOL got = 1;
        while (!popup->done &&
            (got = GetMessageW(&message, NULL, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (got == 0) PostQuitMessage((int)message.wParam);
    }
    /* Every exit path hides the overlay and re-enables the owner, even when
       the pump was never live for this popup, so a wrapped-pump test seam
       cannot strand the owner disabled or leave the dimmer up. */
    hide_overlay(popup);
    if (popup->owner && IsWindow(popup->owner)) {
        EnableWindow(popup->owner, TRUE);
        SetActiveWindow(popup->owner);
    }
}

void palette_popup_destroy(PalettePopup *popup) {
    popup_release(popup);
}

bool palette_popup_set_models(PalettePopup *popup,
    const ChatModelCatalog *catalog, const wchar_t *current_id,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    const wchar_t *status) {
    if (!popup) return false;
    if (!palette_set_models(popup->controller, catalog, current_id, history,
            history_count))
        return false;
    if (status) {
        wcsncpy(popup->status, status, CHAT_STATUS_TEXT - 1);
        popup->status[CHAT_STATUS_TEXT - 1] = 0;
        if (popup->window) ui_set_text(&popup->ui, popup->status_text,
            popup->status);
    }
    popup->bound_first = (size_t)-1;
    rebind_if_scroll_changed(popup);
    ensure_selected_bound(popup, false);
    return true;
}

HWND palette_popup_window(const PalettePopup *popup) {
    return popup ? popup->window : NULL;
}

HWND palette_popup_overlay(const PalettePopup *popup) {
    return popup ? popup->overlay : NULL;
}

RECT palette_popup_bounds(const PalettePopup *popup) {
    RECT bounds = { 0, 0, 0, 0 };
    if (popup && popup->window) GetWindowRect(popup->window, &bounds);
    return bounds;
}

void palette_popup_set_query(PalettePopup *popup, const wchar_t *query) {
    if (!popup) return;
    wcsncpy(popup->query, query ? query : L"", PALETTE_QUERY_TEXT - 1);
    popup->query[PALETTE_QUERY_TEXT - 1] = 0;
    apply_query(popup);
}

const wchar_t *palette_popup_query(const PalettePopup *popup) {
    return popup ? palette_query(popup->controller) : L"";
}

const wchar_t *palette_popup_status(const PalettePopup *popup) {
    return popup ? popup->status : L"";
}

size_t palette_popup_row_count(const PalettePopup *popup) {
    return popup ? palette_row_count(popup->controller) : 0;
}

const wchar_t *palette_popup_row_label(const PalettePopup *popup,
    size_t index) {
    const PaletteRow *row =
        popup ? palette_row(popup->controller, index) : NULL;
    return row ? row->label : NULL;
}

bool palette_popup_row_key(const PalettePopup *popup, size_t index,
    PaletteRowKey *out) {
    const PaletteRow *row =
        popup ? palette_row(popup->controller, index) : NULL;
    if (!row || !out) return false;
    *out = row->key;
    return true;
}

bool palette_popup_highlighted_model(const PalettePopup *popup,
    wchar_t out[CHAT_MODEL_TEXT]) {
    if (out) out[0] = 0;
    const PaletteRow *row =
        popup ? palette_selected_row(popup->controller) : NULL;
    if (!row || row->key.kind == PALETTE_ROW_COMMAND || !out) return false;
    wcsncpy(out, row->key.id, CHAT_MODEL_TEXT - 1);
    out[CHAT_MODEL_TEXT - 1] = 0;
    return true;
}

void palette_popup_set_selected_model(PalettePopup *popup, const wchar_t *id) {
    if (!popup || !id || !id[0]) return;
    PaletteRowKey key;
    memset(&key, 0, sizeof key);
    key.kind = PALETTE_ROW_MODEL_ALL;
    wcsncpy(key.id, id, CHAT_MODEL_TEXT - 1);
    key.id[CHAT_MODEL_TEXT - 1] = 0;
    palette_select_key(popup->controller, &key);
    ensure_selected_bound(popup, false);
}

UINT palette_popup_dpi(const PalettePopup *popup) {
    return popup ? popup->dpi : 0;
}

float palette_popup_scroll_offset(const PalettePopup *popup) {
    return popup ? ui_scroll_offset(&popup->ui, popup->scroll) : 0;
}

IRawElementProviderSimple *palette_popup_root_provider(PalettePopup *popup) {
    return popup ? ui_accessibility_root_provider(popup->accessibility)
                 : NULL;
}
