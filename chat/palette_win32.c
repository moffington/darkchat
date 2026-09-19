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
   palette while the modal loop is live. */
#define PALETTE_WINDOW_WIDTH 580.0f
#define PALETTE_WINDOW_HEIGHT 420.0f
#define PALETTE_ROW_HEIGHT 32.0f
#define PALETTE_HEADER_HEIGHT 26.0f
#define PALETTE_QUERY_HEIGHT 44.0f
#define PALETTE_EDGE 16.0f        /* clamp margin around the owner */
#define PALETTE_CENTER_BIAS 0.40f /* vertical placement above center */
#define PALETTE_CLASS L"DarkChat.Palette"
#define PALETTE_DIMMER_CLASS L"DarkChat.PaletteDimmer"
#define PALETTE_DIMMER_ALPHA 30   /* ~12% black over the disabled owner */
#define PALETTE_QUERY_HINT L"Type to filter commands"

struct PalettePopup {
    HWND owner, window;
    Ui ui;                          /* retained tree: query row + row list */
    UiRenderer renderer;            /* one HWND target per renderer */
    UiAccessibility *accessibility;
    Palette *controller;            /* pure selection/filter state */
    UiId query_text;                /* shows the live filter query */
    UiId scroll;                    /* row list scroll container */
    /* Row nodes currently mirrored into the tree, in controller visible
       order (section headers are extra nodes and not tracked here). */
    UiId *rows;
    size_t row_count, row_capacity;
    UiId selected_row;
    wchar_t query[PALETTE_QUERY_TEXT];
    HWND overlay;                   /* owned, non-activating dimmer */
    UINT dpi;
    bool done, accepted, tracking;
    int action_id;
};

static LRESULT CALLBACK palette_proc(HWND window, UINT message, WPARAM w,
    LPARAM l);

static UINT popup_dpi(HWND owner) {
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    return dpi ? dpi : 96;
}

/* Ensures the row-node array can hold `needed` ids. On failure the previous
   array and capacity stay intact and false is returned. */
static bool ensure_rows(PalettePopup *popup, size_t needed) {
    if (needed <= popup->row_capacity) return true;
    if (!needed) return true;
    UiId *grown = (UiId *)realloc(popup->rows, needed * sizeof *grown);
    if (!grown) return false;
    popup->rows = grown;
    popup->row_capacity = needed;
    return true;
}

static void clear_rows(PalettePopup *popup) {
    /* Every child of the scroll container is rebuilt together: section
       headers are not tracked in `rows`, so removing only the tracked buttons
       would accumulate headers across queries until the fixed node arena
       overflows. */
    for (;;) {
        UiNode *list = ui_node(&popup->ui, popup->scroll);
        if (!list || !list->first) break;
        if (!ui_remove(&popup->ui, list->first)) break;
    }
    popup->row_count = 0;
    popup->selected_row = UI_NONE;
}

/* Mirrors the controller's highlight into the tree: exactly one selected row
   node, and (for keyboard movement) real retained focus on it so the reveal
   keeps it inside the scroll viewport and UIA sees the focus change. */
static void sync_selection(PalettePopup *popup, bool keyboard) {
    for (size_t i = 0; i < popup->row_count; i++) {
        UiNode *node = ui_node(&popup->ui, popup->rows[i]);
        if (node) node->selected = false;
    }
    popup->selected_row = UI_NONE;
    size_t selected = palette_selected(popup->controller);
    if (selected < popup->row_count) {
        UiId id = popup->rows[selected];
        popup->selected_row = id;
        UiNode *node = ui_node(&popup->ui, id);
        if (node) node->selected = true;
        if (keyboard) ui_focus(&popup->ui, id, true);
    }
    ui_invalidate(&popup->ui, false);
}

/* Mirrors one controller row into the retained tree as a flat row button:
   transparent when idle, soft fill on hover/press/selection, text flush
   left. */
static bool add_row(PalettePopup *popup, const PaletteRow *row) {
    UiId id = ui_add(&popup->ui, popup->scroll, UI_BUTTON, row->label);
    if (!id) return false;
    UiNode *node = ui_node(&popup->ui, id);
    if (node) {
        node->style.height = ui_fixed(PALETTE_ROW_HEIGHT);
        node->style.font = UI_BODY;
        node->style.flat = true;
        node->tag = row->key.kind == PALETTE_ROW_COMMAND
            ? (uintptr_t)row->key.action_id : 0;
    }
    popup->rows[popup->row_count++] = id;
    return true;
}

static void add_header(PalettePopup *popup, const wchar_t *label) {
    UiId header = ui_add(&popup->ui, popup->scroll, UI_LABEL, label);
    if (!header) return;
    UiNode *node = ui_node(&popup->ui, header);
    if (node) {
        node->style.height = ui_fixed(PALETTE_HEADER_HEIGHT);
        node->style.font = UI_SECTION;
        node->style.foreground = UI_MUTED;
    }
}

/* Rebuilds the visible row list from the controller: one header label per
   section run, then every visible row of that run as a button. The row
   array is sized for the full visible list first, so the mirror cannot
   fail halfway. The list is laid out with the established size before the
   selection is mirrored, so the reveal inside ui_focus reads a real
   viewport instead of a degenerate zero-size one. */
static void rebuild_rows(PalettePopup *popup) {
    if (!ensure_rows(popup, palette_row_count(popup->controller))) return;
    clear_rows(popup);
    size_t count = palette_row_count(popup->controller);
    size_t section = 0;
    for (size_t i = 0; i < count; i++) {
        const PaletteRow *row = palette_row(popup->controller, i);
        if (!row) break;
        if (i == 0 || row->group !=
            palette_row(popup->controller, i - 1)->group)
            add_header(popup,
                palette_section_label(popup->controller, section++));
        if (!add_row(popup, row)) break;
    }
    ui_layout(&popup->ui, popup->ui.width, popup->ui.height);
    sync_selection(popup, true);
    ui_invalidate(&popup->ui, false);
}

/* Applies the popup's query buffer to the controller and re-mirrors. */
static void apply_query(PalettePopup *popup) {
    wchar_t shown[PALETTE_QUERY_TEXT];
    if (palette_set_query(popup->controller, popup->query))
        rebuild_rows(popup);
    /* The filter field echoes the query, or the hint when it is empty. */
    swprintf(shown, PALETTE_QUERY_TEXT, L"%ls",
        popup->query[0] ? popup->query : PALETTE_QUERY_HINT);
    ui_set_text(&popup->ui, popup->query_text, shown);
    ui_set_accessible_name(&popup->ui, popup->query_text,
        popup->query[0] ? popup->query : PALETTE_QUERY_HINT);
    InvalidateRect(popup->window, NULL, FALSE);
}

/* Retained activation (mouse click, UIA Invoke) resolves identity through
   the controller, never a row index stored elsewhere. */
static void palette_event(void *user, Ui *ui, UiEvent event) {
    (void)ui;
    PalettePopup *popup = (PalettePopup *)user;
    if (!popup || event.kind != UI_ACTIVATE) return;
    for (size_t i = 0; i < popup->row_count; i++)
        if (popup->rows[i] == event.id) {
            const PaletteRow *row = palette_row(popup->controller, i);
            if (!row) return;
            palette_select_key(popup->controller, &row->key);
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
    popup->action_id = key.kind == PALETTE_ROW_COMMAND ? key.action_id : 0;
}

void palette_popup_cancel(PalettePopup *popup) {
    if (!popup || popup->done) return;
    popup->accepted = false;
    popup->done = true;
    popup->action_id = 0;
}

bool palette_popup_accepted(const PalettePopup *popup) {
    return popup && popup->accepted;
}

int palette_popup_action_id(const PalettePopup *popup) {
    return popup && popup->accepted ? popup->action_id : 0;
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
        UiId root = ui_add(ui, UI_NONE, UI_COLUMN, L"Command palette");
        if (!root) return -1;
        UiNode *node = ui_node(ui, root);
        if (node) {
            node->style.padding = 10;
            node->style.gap = 8;
            node->style.background = UI_PANEL;
            node->style.border = true;
        }
        popup->query_text = ui_add(ui, root, UI_LABEL, PALETTE_QUERY_HINT);
        if (!popup->query_text) return -1;
        node = ui_node(ui, popup->query_text);
        if (node) {
            node->style.height = ui_fixed(PALETTE_QUERY_HEIGHT);
            node->style.background = UI_TRACK;
            node->style.border = true;
            node->style.font = UI_BODY;
            /* Keep the hint off the field's border. */
            node->style.text_inset = 6;
            ui_set_help_text(ui, popup->query_text,
                L"Type to filter commands");
        }
        UiId separator = ui_add(ui, root, UI_SEPARATOR, L"");
        if (!separator) return -1;
        node = ui_node(ui, separator);
        if (node) node->style.height = ui_fixed(1);
        popup->scroll = ui_add(ui, root, UI_SCROLL, L"");
        if (!popup->scroll) return -1;
        node = ui_node(ui, popup->scroll);
        if (node) node->style.height = ui_flex(1);
        popup->accessibility = ui_accessibility_create(window, ui);
        if (!popup->accessibility) return -1;
        /* Establish the layout size before building rows: the selection
           mirror runs a reveal, and a reveal against a zero-size viewport
           would scroll the first section header out of view. */
        ui_layout(ui, PALETTE_WINDOW_WIDTH, PALETTE_WINDOW_HEIGHT);
        rebuild_rows(popup);
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
        return 0;
    }
    case WM_DPICHANGED: {
        popup->dpi = HIWORD(w);
        renderer_resize(&popup->renderer, 0, 0, (float)popup->dpi);
        ui_invalidate(ui, true);
        /* Placement is recomputed around the new scale, so the palette stays
           centered over the owner instead of chasing the suggested rect. */
        place_popup(popup);
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
                sync_selection(popup, true);
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
        return 0;
    case WM_LBUTTONUP:
        ui_pointer_up(ui, (float)GET_X_LPARAM(l) * 96.0f / popup->dpi,
            (float)GET_Y_LPARAM(l) * 96.0f / popup->dpi);
        if (GetCapture() == window && !ui->pressed) ReleaseCapture();
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(window, &point);
        float step = 3 * ui->theme.control_height;
        ui_scroll(ui, (float)point.x * 96.0f / popup->dpi,
            (float)point.y * 96.0f / popup->dpi,
            -(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * step);
        InvalidateRect(window, NULL, FALSE);
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

PalettePopup *palette_popup_create(HWND owner,
    const ChatActionContext *context) {
    if (!owner || !context) return NULL;
    register_class();
    PalettePopup *popup = (PalettePopup *)calloc(1, sizeof *popup);
    if (!popup) return NULL;
    popup->owner = owner;
    popup->dpi = popup_dpi(owner);
    ui_init(&popup->ui, renderer_measure, &popup->renderer);
    popup->ui.on_event = palette_event;
    popup->ui.event_user = popup;
    if (FAILED(renderer_init(&popup->renderer, &popup->ui.theme))) {
        free(popup);
        return NULL;
    }
    popup->controller = palette_create();
    if (!popup->controller ||
        !palette_set_commands(popup->controller, context)) {
        palette_dispose(popup->controller);
        renderer_dispose(&popup->renderer);
        free(popup);
        return NULL;
    }
    popup->window = CreateWindowExW(WS_EX_TOOLWINDOW, PALETTE_CLASS,
        L"Command palette", WS_POPUP, 0, 0,
        MulDiv((int)PALETTE_WINDOW_WIDTH, (int)popup->dpi, 96),
        MulDiv((int)PALETTE_WINDOW_HEIGHT, (int)popup->dpi, 96),
        owner, NULL, GetModuleHandleW(NULL), popup);
    if (!popup->window) {
        ui_accessibility_destroy(popup->accessibility);
        palette_dispose(popup->controller);
        renderer_dispose(&popup->renderer);
        free(popup->rows);
        free(popup);
        return NULL;
    }
    /* Windows 11 rounds top-level popups on request; older systems fail the
       attribute call and keep square corners (silently ignored here). */
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(popup->window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &preference, sizeof preference);
    create_overlay(popup);
    place_popup(popup);
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
        while (!popup->done && (got = GetMessageW(&message, NULL, 0, 0)) > 0) {
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
    if (!popup) return;
    hide_overlay(popup);
    if (popup->overlay && IsWindow(popup->overlay))
        DestroyWindow(popup->overlay);
    popup->overlay = NULL;
    if (popup->window && IsWindow(popup->window))
        DestroyWindow(popup->window);
    ui_accessibility_destroy(popup->accessibility);
    palette_dispose(popup->controller);
    renderer_dispose(&popup->renderer);
    free(popup->rows);
    free(popup);
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

size_t palette_popup_row_count(const PalettePopup *popup) {
    return popup ? palette_row_count(popup->controller) : 0;
}

const wchar_t *palette_popup_row_label(const PalettePopup *popup,
    size_t index) {
    const PaletteRow *row =
        popup ? palette_row(popup->controller, index) : NULL;
    return row ? row->label : NULL;
}

UINT palette_popup_dpi(const PalettePopup *popup) {
    return popup ? popup->dpi : 0;
}

IRawElementProviderSimple *palette_popup_root_provider(PalettePopup *popup) {
    return popup ? ui_accessibility_root_provider(popup->accessibility)
                 : NULL;
}
