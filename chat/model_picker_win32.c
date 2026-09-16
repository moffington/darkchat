#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "model_picker_win32.h"
#include <stdlib.h>
#include <string.h>

#define MODEL_PICKER_FILTER 128
#define MODEL_PICKER_CLASS L"DarkChat.ModelPicker"
#define MODEL_PICKER_MARGIN 10

struct ModelPicker {
    HWND owner, window, filter, list, status, ok, cancel;
    WNDPROC filter_proc, list_proc;
    ChatModelCatalog source;          /* owned snapshot of the merged list */
    const ChatModelInfo **found;      /* filter output; owned */
    size_t found_capacity;
    wchar_t filter_text[MODEL_PICKER_FILTER];
    wchar_t selected_id[CHAT_MODEL_TEXT];
    wchar_t status_text[CHAT_STATUS_TEXT];
    bool done, accepted;
};

static bool source_copy(ChatModelCatalog *destination,
    const ChatModelCatalog *source) {
    ChatModelCatalog temp;
    chat_model_catalog_init(&temp);
    if (source && source->count) {
        temp.items = (ChatModelInfo *)malloc(source->count * sizeof *temp.items);
        if (!temp.items) return false;
        memcpy(temp.items, source->items, source->count * sizeof *temp.items);
        temp.count = temp.capacity = source->count;
    }
    chat_model_catalog_dispose(destination);
    *destination = temp;
    return true;
}

static void set_text(HWND control, const wchar_t *text) {
    SetWindowTextW(control, text ? text : L"");
}

/* Grows the filter-output pointer array. Returns false on allocation failure,
   leaving the old array and capacity intact. */
static bool ensure_found(ModelPicker *picker, size_t needed) {
    if (needed <= picker->found_capacity) return true;
    if (!needed) return true;
    const ChatModelInfo **grown = (const ChatModelInfo **)realloc(
        picker->found, needed * sizeof *grown);
    if (!grown) return false;
    picker->found = grown;
    picker->found_capacity = needed;
    return true;
}

/* Rebuilds the listbox from the source snapshot using the current filter text
   and re-selects the stored id (nearest entry when it is gone). */
static void refilter(ModelPicker *picker) {
    /* A failed growth leaves the previous list untouched; the source snapshot
       must never be paired with a too-small pointer array. */
    if (!ensure_found(picker, picker->source.count)) return;
    size_t count = chat_model_catalog_filter(&picker->source,
        picker->filter_text, picker->found, picker->found_capacity);
    SendMessageW(picker->list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(picker->list, LB_RESETCONTENT, 0, 0);
    int select = -1;
    for (size_t i = 0; i < count; i++) {
        const ChatModelInfo *info = picker->found[i];
        wchar_t label[CHAT_MODEL_NAME_TEXT + CHAT_MODEL_TEXT + 4];
        if (info->name[0]) {
            swprintf(label, sizeof label / sizeof *label,
                L"%ls \u2014 %ls", info->name, info->id);
        } else {
            wcsncpy(label, info->id, sizeof label / sizeof *label - 1);
            label[sizeof label / sizeof *label - 1] = 0;
        }
        int item = (int)SendMessageW(picker->list, LB_ADDSTRING, 0,
            (LPARAM)label);
        if (item >= 0 &&
            !wcscmp(info->id, picker->selected_id) && select < 0) select = item;
    }
    if (select < 0 && count > 0) {
        select = 0;
        wcsncpy(picker->selected_id, picker->found[0]->id,
            CHAT_MODEL_TEXT - 1);
        picker->selected_id[CHAT_MODEL_TEXT - 1] = 0;
    }
    if (select >= 0) SendMessageW(picker->list, LB_SETCURSEL, select, 0);
    SendMessageW(picker->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(picker->list, NULL, TRUE);
}

static void picker_move(ModelPicker *picker, int delta) {
    LRESULT count = SendMessageW(picker->list, LB_GETCOUNT, 0, 0);
    if (count <= 0) return;
    LRESULT current = SendMessageW(picker->list, LB_GETCURSEL, 0, 0);
    long next = current < 0 ? 0 : (long)current + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    SendMessageW(picker->list, LB_SETCURSEL, next, 0);
    /* List order equals the filter output order, so the index resolves
       directly; refilter always sizes `found` to the full source count. */
    if ((size_t)next < picker->found_capacity) {
        wcsncpy(picker->selected_id, picker->found[next]->id,
            CHAT_MODEL_TEXT - 1);
        picker->selected_id[CHAT_MODEL_TEXT - 1] = 0;
    }
}

void model_picker_accept(ModelPicker *picker) {
    if (!picker || picker->accepted) return;
    if (!picker->selected_id[0]) return;
    picker->accepted = true;
    picker->done = true;
}

void model_picker_cancel(ModelPicker *picker) {
    if (!picker) return;
    picker->accepted = false;
    picker->done = true;
}

void model_picker_set_filter(ModelPicker *picker, const wchar_t *filter) {
    if (!picker) return;
    wcsncpy(picker->filter_text, filter ? filter : L"",
        MODEL_PICKER_FILTER - 1);
    picker->filter_text[MODEL_PICKER_FILTER - 1] = 0;
    if (picker->window) refilter(picker);
}

void model_picker_set_selected(ModelPicker *picker, const wchar_t *id) {
    if (!picker || !id) return;
    wcsncpy(picker->selected_id, id, CHAT_MODEL_TEXT - 1);
    picker->selected_id[CHAT_MODEL_TEXT - 1] = 0;
    if (picker->window) refilter(picker);
}

const wchar_t *model_picker_filter(const ModelPicker *picker) {
    return picker ? picker->filter_text : L"";
}

size_t model_picker_match_count(const ModelPicker *picker) {
    if (!picker) return 0;
    return chat_model_catalog_filter(&picker->source, picker->filter_text,
        picker->found, picker->found_capacity);
}

const wchar_t *model_picker_match_id(const ModelPicker *picker, size_t index) {
    if (!picker || index >= picker->found_capacity) return NULL;
    size_t count = model_picker_match_count(picker);
    return index < count ? picker->found[index]->id : NULL;
}

bool model_picker_accepted(const ModelPicker *picker) {
    return picker && picker->accepted;
}

const wchar_t *model_picker_selected_id(const ModelPicker *picker) {
    return picker ? picker->selected_id : L"";
}

static LRESULT CALLBACK child_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    ModelPicker *picker = (ModelPicker *)GetWindowLongPtrW(window,
        GWLP_USERDATA);
    WNDPROC original = picker && window == picker->filter
        ? picker->filter_proc : picker ? picker->list_proc : NULL;
    if (picker && message == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { model_picker_cancel(picker); return 0; }
        if (w == VK_DOWN) { picker_move(picker, 1); return 0; }
        if (w == VK_UP) { picker_move(picker, -1); return 0; }
        if (w == VK_NEXT) { picker_move(picker, 8); return 0; }
        if (w == VK_PRIOR) { picker_move(picker, -8); return 0; }
        if (w == VK_RETURN) { model_picker_accept(picker); return 0; }
    }
    return original ? CallWindowProcW(original, window, message, w, l)
                    : DefWindowProcW(window, message, w, l);
}

static void select_from_list(ModelPicker *picker) {
    LRESULT item = SendMessageW(picker->list, LB_GETCURSEL, 0, 0);
    size_t count = model_picker_match_count(picker);
    if (item >= 0 && (size_t)item < count) {
        wcsncpy(picker->selected_id, picker->found[item]->id,
            CHAT_MODEL_TEXT - 1);
        picker->selected_id[CHAT_MODEL_TEXT - 1] = 0;
    }
}

static LRESULT CALLBACK picker_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    ModelPicker *picker = (ModelPicker *)GetWindowLongPtrW(window,
        GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        picker = (ModelPicker *)((CREATESTRUCTW *)l)->lpCreateParams;
        picker->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)picker);
    }
    if (!picker) return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(window);
        int margin = MulDiv(MODEL_PICKER_MARGIN, (int)dpi, 96);
        int unit = MulDiv(10, (int)dpi, 96);
        RECT bounds;
        GetClientRect(window, &bounds);
        int width = bounds.right - 2 * margin;
        picker->filter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            margin, margin, width, 2 * unit, window, (HMENU)10, NULL, NULL);
        picker->status = CreateWindowExW(0, L"STATIC", picker->status_text,
            WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin + 2 * unit + 4,
            width, 2 * unit, window, NULL, NULL, NULL);
        int list_top = margin + 4 * unit + 10;
        picker->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
            margin, list_top, width,
            bounds.bottom - list_top - 5 * unit - margin, window, (HMENU)11,
            NULL, NULL);
        int button_y = bounds.bottom - 4 * unit - margin;
        picker->ok = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            bounds.right - 2 * margin - 18 * unit, button_y, 9 * unit,
            3 * unit, window, (HMENU)IDOK, NULL, NULL);
        picker->cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            bounds.right - margin - 9 * unit, button_y, 9 * unit, 3 * unit,
            window, (HMENU)IDCANCEL, NULL, NULL);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(picker->filter, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(picker->status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(picker->list, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(picker->ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(picker->cancel, WM_SETFONT, (WPARAM)font, TRUE);
        SetWindowLongPtrW(picker->filter, GWLP_USERDATA, (LONG_PTR)picker);
        SetWindowLongPtrW(picker->list, GWLP_USERDATA, (LONG_PTR)picker);
        picker->filter_proc = (WNDPROC)SetWindowLongPtrW(picker->filter,
            GWLP_WNDPROC, (LONG_PTR)child_proc);
        picker->list_proc = (WNDPROC)SetWindowLongPtrW(picker->list,
            GWLP_WNDPROC, (LONG_PTR)child_proc);
        set_text(picker->status, picker->status_text);
        refilter(picker);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { model_picker_accept(picker); return 0; }
        if (LOWORD(w) == IDCANCEL) { model_picker_cancel(picker); return 0; }
        if ((HWND)l == picker->filter && HIWORD(w) == EN_CHANGE) {
            GetWindowTextW(picker->filter, picker->filter_text,
                MODEL_PICKER_FILTER);
            refilter(picker);
            return 0;
        }
        if ((HWND)l == picker->list && HIWORD(w) == LBN_SELCHANGE) {
            select_from_list(picker);
            return 0;
        }
        if ((HWND)l == picker->list && HIWORD(w) == LBN_DBLCLK) {
            select_from_list(picker);
            model_picker_accept(picker);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        model_picker_cancel(picker);
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
    cls.lpfnWndProc = picker_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = MODEL_PICKER_CLASS;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&cls);
}

ModelPicker *model_picker_create(HWND owner, const ChatModelCatalog *source,
    const wchar_t *status, const wchar_t *initial_id) {
    register_class();
    ModelPicker *picker = (ModelPicker *)calloc(1, sizeof *picker);
    if (!picker) return NULL;
    chat_model_catalog_init(&picker->source);
    if (!source_copy(&picker->source, source)) {
        free(picker);
        return NULL;
    }
    picker->owner = owner;
    if (initial_id) {
        wcsncpy(picker->selected_id, initial_id, CHAT_MODEL_TEXT - 1);
        picker->selected_id[CHAT_MODEL_TEXT - 1] = 0;
    }
    if (status) {
        wcsncpy(picker->status_text, status, CHAT_STATUS_TEXT - 1);
        picker->status_text[CHAT_STATUS_TEXT - 1] = 0;
    }
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    RECT anchor;
    if (owner) GetWindowRect(owner, &anchor);
    else SetRect(&anchor, 0, 0, 0, 0);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, MODEL_PICKER_CLASS,
        L"Choose model", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        anchor.left + MulDiv(40, (int)dpi, 96),
        anchor.top + MulDiv(80, (int)dpi, 96), MulDiv(520, (int)dpi, 96),
        MulDiv(430, (int)dpi, 96), owner, NULL, GetModuleHandleW(NULL),
        picker);
    if (!window) {
        chat_model_catalog_dispose(&picker->source);
        free(picker);
        return NULL;
    }
    return picker;
}

void model_picker_pump(ModelPicker *picker) {
    if (!picker || picker->done) return;
    ShowWindow(picker->window, SW_SHOW);
    SetFocus(picker->filter);
    if (picker->owner) EnableWindow(picker->owner, FALSE);
    MSG message;
    BOOL got = 1;
    while (!picker->done &&
        (got = GetMessageW(&message, NULL, 0, 0)) > 0) {
        /* Native dialog navigation (Tab, group arrows) is handled here; a
           message for another window (a posted host event, for example) is not
           a dialog message and still reaches DispatchMessageW. */
        if (!IsDialogMessageW(picker->window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (got == 0) PostQuitMessage((int)message.wParam);
    if (picker->owner) {
        EnableWindow(picker->owner, TRUE);
        SetActiveWindow(picker->owner);
    }
}

void model_picker_source_updated(ModelPicker *picker,
    const ChatModelCatalog *source, const wchar_t *status) {
    if (!picker) return;
    /* Transactional: grow the filter-output capacity before replacing the
       snapshot, so an allocation failure at either step leaves the previous
       source and its matching list fully usable, with no dangling pointers. */
    if (!ensure_found(picker, source ? source->count : 0)) return;
    if (!source_copy(&picker->source, source)) return;
    if (status) {
        wcsncpy(picker->status_text, status, CHAT_STATUS_TEXT - 1);
        picker->status_text[CHAT_STATUS_TEXT - 1] = 0;
        if (picker->status) set_text(picker->status, picker->status_text);
    }
    if (picker->window) refilter(picker);
}

void model_picker_destroy(ModelPicker *picker) {
    if (!picker) return;
    if (picker->window && IsWindow(picker->window))
        DestroyWindow(picker->window);
    chat_model_catalog_dispose(&picker->source);
    free(picker->found);
    free(picker);
}
