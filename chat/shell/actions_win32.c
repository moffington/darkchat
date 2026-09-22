#include "chat/shell/actions_win32.h"
#include "chat/import/import.h"
#include "platform/dark_mode_win32.h"
#include "ui/ui.h"
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    HWND edit, ok, cancel;
    wchar_t *text;
    size_t capacity;
    bool done, accepted, multiline;
    UINT dpi;
    /* Per-instance resources: the window class is registered once and
       reused, so nothing instance-owned may live in the class. */
    HFONT font;
    HBRUSH background, edit_background;
} EditDialog;

static COLORREF theme_color(unsigned role) {
    const UiTheme theme = ui_theme_dark();
    return RGB(theme.colors[role].r, theme.colors[role].g, theme.colors[role].b);
}

/* One Segoe UI face at the theme's body size, scaled to the dialog DPI, so
   the shared dialog reuses the application theme instead of a second one. */
static HFONT dialog_font(UINT dpi) {
    const UiTheme theme = ui_theme_dark();
    return CreateFontW(-MulDiv((int)theme.font_size[UI_BODY], (int)dpi, 96),
        0, 0, 0, (int)theme.font_weight[UI_BODY], FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, theme.font_family);
}

/* Paint-time brush selection: the struct fields hold only independently
   owned brushes (created per dialog, released in chat_edit_dialog), so a
   NULL field means allocation failed and a stock brush is substituted
   here instead of ever being stored or deleted. */
static HBRUSH dialog_brush(const EditDialog *d) {
    return d->background ? d->background : (HBRUSH)GetStockObject(BLACK_BRUSH);
}

static HBRUSH edit_brush(const EditDialog *d) {
    return d->edit_background ? d->edit_background : dialog_brush(d);
}

/* Single geometry source for WM_CREATE and WM_DPICHANGED: the same unit
   math that sized the controls originally, re-applied to the live
   rectangle. */
static void layout_children(EditDialog *d, HWND window) {
    int unit = MulDiv(10, (int)d->dpi, 96);
    RECT r;
    GetClientRect(window, &r);
    MoveWindow(d->edit, unit, unit, r.right - 2 * unit, r.bottom - 6 * unit,
        TRUE);
    MoveWindow(d->ok, r.right - 19 * unit, r.bottom - 4 * unit, 8 * unit,
        3 * unit, TRUE);
    MoveWindow(d->cancel, r.right - 10 * unit, r.bottom - 4 * unit, 9 * unit,
        3 * unit, TRUE);
}

static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    EditDialog *d=(EditDialog *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) {
        d=((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)d);
    }
    if (!d) return DefWindowProcW(window,message,w,l);
    switch (message) {
    case WM_CREATE: {
        d->dpi = GetDpiForWindow(window);
        dark_mode_titlebar_apply(window);
        d->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",d->text,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOVSCROLL|
            (d->multiline ? ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL : ES_AUTOHSCROLL),
            0,0,0,0,window,(HMENU)10,NULL,NULL);
        d->ok=CreateWindowExW(0,L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            0,0,0,0,window,(HMENU)IDOK,NULL,NULL);
        d->cancel=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,0,0,window,(HMENU)IDCANCEL,NULL,NULL);
        SendMessageW(d->edit,EM_SETLIMITTEXT,d->capacity-1,0);
        HFONT font = d->font ? d->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(d->edit,WM_SETFONT,(WPARAM)font,TRUE);
        SendMessageW(d->ok,WM_SETFONT,(WPARAM)font,TRUE);
        SendMessageW(d->cancel,WM_SETFONT,(WPARAM)font,TRUE);
        dark_mode_control_apply(d->edit);
        layout_children(d,window);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w)==IDOK) {
            GetWindowTextW(d->edit,d->text,(int)d->capacity);
            d->accepted=true; d->done=true;
        } else if (LOWORD(w)==IDCANCEL) d->done=true;
        return 0;
    case WM_CLOSE: d->done=true; return 0;
    case WM_ERASEBKGND: {
        /* Explicit client painting: this is an ordinary window class, not a
           dialog-resource window, and the class carries no brush, so only
           this path guarantees the dark background. */
        RECT r;
        GetClipBox((HDC)w,&r);
        FillRect((HDC)w,&r,dialog_brush(d));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(window,&ps);
        if (dc) FillRect(dc,&ps.rcPaint,dialog_brush(d));
        EndPaint(window,&ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc=(HDC)w;
        SetTextColor(dc,theme_color(UI_TEXT));
        SetBkColor(dc,theme_color(UI_TRACK));
        return (LRESULT)edit_brush(d);
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)w;
        SetTextColor(dc,theme_color(UI_TEXT));
        SetBkColor(dc,theme_color(UI_PANEL));
        return (LRESULT)dialog_brush(d);
    }
    case WM_DPICHANGED: {
        UINT dpi=HIWORD(w);
        RECT *suggested=(RECT *)l;
        SetWindowPos(window,NULL,suggested->left,suggested->top,
            suggested->right-suggested->left,suggested->bottom-suggested->top,
            SWP_NOZORDER|SWP_NOACTIVATE);
        /* The new DPI is adopted before any allocation attempt, so child
           geometry always follows the new monitor even when font creation
           below fails and the old font must be retained. */
        d->dpi=dpi;
        HFONT replacement=dialog_font(dpi);
        if (!replacement) {
            /* Allocation failed: every child keeps the existing font, and
               that owned font stays installed and alive for the next
               attempt instead of being deleted out from under them. */
            layout_children(d,window);
            return 0;
        }
        HFONT old=d->font;
        d->font=replacement;
        SendMessageW(d->edit,WM_SETFONT,(WPARAM)d->font,TRUE);
        SendMessageW(d->ok,WM_SETFONT,(WPARAM)d->font,TRUE);
        SendMessageW(d->cancel,WM_SETFONT,(WPARAM)d->font,TRUE);
        /* The old font is released only after every child holds its
           replacement, so no control can render with a dangling font. */
        if (old) DeleteObject(old);
        layout_children(d,window);
        return 0;
    }
    }
    return DefWindowProcW(window,message,w,l);
}
bool chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text, size_t capacity, bool multiline) {
    WNDCLASSW cls={0}; cls.lpfnWndProc=edit_proc; cls.hInstance=GetModuleHandleW(NULL);
    cls.lpszClassName=L"DarkChat.Edit"; cls.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));
    /* No permanent class brush: the class survives every opening of the
       dialog, so instance resources live in EditDialog and the client area
       is painted explicitly above. */
    if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        return false;
    EditDialog d={0}; d.text=text; d.capacity=capacity; d.multiline=multiline;
    d.dpi=GetDpiForWindow(owner);
    d.font=dialog_font(d.dpi);
    /* The fields hold only independently owned handles; failed allocations
       stay NULL and the paint helpers substitute stock brushes at use time,
       so cleanup can never touch a stock object or the same brush twice. */
    d.background=CreateSolidBrush(theme_color(UI_PANEL));
    d.edit_background=CreateSolidBrush(theme_color(UI_TRACK));
    RECT r; GetWindowRect(owner,&r);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,title,
        WS_CAPTION|WS_SYSMENU|WS_POPUP,r.left+40,r.top+60,MulDiv(560,d.dpi,96),
        MulDiv(multiline ? 350 : 150,d.dpi,96),owner,NULL,cls.hInstance,&d);
    if (!window) {
        DeleteObject(d.font);
        DeleteObject(d.background);
        DeleteObject(d.edit_background);
        return false;
    }
    EnableWindow(owner,FALSE); ShowWindow(window,SW_SHOW); SetFocus(d.edit);
    SendMessageW(d.edit,EM_SETSEL,0,-1);
    MSG msg;
    while (!d.done) {
        BOOL got=GetMessageW(&msg,NULL,0,0);
        if (got<=0) { if (!got) PostQuitMessage((int)msg.wParam); break; }
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE) { d.done=true; continue; }
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN &&
            (!multiline || (GetKeyState(VK_CONTROL)&0x8000))) {
            SendMessageW(window,WM_COMMAND,IDOK,0); continue;
        }
        if (!IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner,TRUE); DestroyWindow(window); SetActiveWindow(owner);
    /* GDI releases happen only after DestroyWindow, when no WM_CTLCOLOR
       delivery can still reference the brush or font. */
    DeleteObject(d.font);
    DeleteObject(d.background);
    DeleteObject(d.edit_background);
    return d.accepted;
}
bool chat_copy_text(HWND owner,const wchar_t *text) {
    size_t bytes=(wcslen(text)+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if (!memory) return false;
    void *target=GlobalLock(memory);
    if (!target) { GlobalFree(memory); return false; }
    memcpy(target,text,bytes); GlobalUnlock(memory);
    bool ok=false;
    if (OpenClipboard(owner)) {
        if (EmptyClipboard()) ok=SetClipboardData(CF_UNICODETEXT,memory)!=NULL;
        CloseClipboard();
    }
    if (!ok) GlobalFree(memory);
    return ok;
}
ChatFileDialogResult chat_save_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, const wchar_t *default_ext,
    const wchar_t *default_name, wchar_t *path, size_t capacity) {
    if (!path || capacity == 0) return CHAT_FILE_DIALOG_ERROR;
    path[0] = 0;
    if (default_name && default_name[0]) {
        wcsncpy(path, default_name, capacity - 1);
        path[capacity - 1] = 0;
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)capacity;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = default_ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER |
        OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) return CHAT_FILE_DIALOG_ACCEPTED;
    /* CommDlgExtendedError is zero for a plain cancel and nonzero for a real
       dialog failure (invalid flags, out of memory, ...). */
    return CommDlgExtendedError() == 0 ? CHAT_FILE_DIALOG_CANCELLED
                                       : CHAT_FILE_DIALOG_ERROR;
}

ChatFileDialogResult chat_open_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, wchar_t *path, size_t capacity) {
    if (!path || capacity == 0) return CHAT_FILE_DIALOG_ERROR;
    path[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)capacity;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
        OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return CHAT_FILE_DIALOG_ACCEPTED;
    return CommDlgExtendedError() == 0 ? CHAT_FILE_DIALOG_CANCELLED
                                       : CHAT_FILE_DIALOG_ERROR;
}

ChatFileReadResult chat_read_file_utf8_limited(const wchar_t *path,
    size_t limit, char **data, size_t *length) {
    if (data) *data = NULL;
    if (length) *length = 0;
    if (!path || !path[0] || !data || !length) return CHAT_FILE_READ_IO_ERROR;
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return CHAT_FILE_READ_IO_ERROR;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return CHAT_FILE_READ_IO_ERROR;
    }
    /* Reject an oversized file before allocating, so a huge file cannot cause
       a huge allocation just to be discarded. */
    if ((uint64_t)size.QuadPart > (uint64_t)limit) {
        CloseHandle(file);
        return CHAT_FILE_READ_TOO_LARGE;
    }
    size_t bytes = (size_t)size.QuadPart;
    char *buffer = (char *)malloc(bytes + 1);
    if (!buffer) {
        CloseHandle(file);
        return CHAT_FILE_READ_OOM;
    }
    size_t got = 0;
    while (got < bytes) {
        DWORD chunk = 0;
        size_t remaining = bytes - got;
        DWORD request = remaining > (size_t)MAXDWORD
            ? MAXDWORD : (DWORD)remaining;
        if (!ReadFile(file, buffer + got, request, &chunk, NULL) || chunk == 0) {
            CloseHandle(file);
            free(buffer);
            return CHAT_FILE_READ_IO_ERROR;
        }
        got += chunk;
    }
    CloseHandle(file);
    /* A leading UTF-8 BOM is not part of the JSON document. */
    if (bytes >= 3 && (unsigned char)buffer[0] == 0xEF &&
        (unsigned char)buffer[1] == 0xBB && (unsigned char)buffer[2] == 0xBF) {
        memmove(buffer, buffer + 3, bytes - 3);
        bytes -= 3;
    }
    buffer[bytes] = 0;
    *data = buffer;
    *length = bytes;
    return CHAT_FILE_READ_OK;
}

ChatFileReadResult chat_read_file_utf8(const wchar_t *path, char **data,
    size_t *length) {
    return chat_read_file_utf8_limited(path, CHAT_IMPORT_LIMIT, data, length);
}

bool chat_write_file_utf8(const wchar_t *path, const char *data, size_t length) {
    if (!path || !path[0] || (!data && length)) return false;
    /* WriteFile takes a DWORD byte count; a larger size_t would be truncated
       and could report a short write as success. The exporter is bounded well
       below this, but the declared contract is not. */
    if (length > (size_t)MAXDWORD) return false;
    /* The temporary is a short fixed-shape name inside the destination
       directory, never derived from the destination basename: appending to a
       name already near the component-length limit could fail, and a fixed
       sibling such as "report.json.tmp" could collide with an unrelated
       file. CREATE_NEW guarantees we only ever delete a path we created. */
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *fwd = wcsrchr(path, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    size_t dir_length = slash ? (size_t)(slash - path) : 0;
    wchar_t temp[1024];
    /* Reserve the widest possible suffix (".darkchat-" plus two decimal
       fields up to 10 digits each plus ".tmp") up front, so the directory
       prefix can never leave the generated name truncated. */
    const size_t suffix_reserve = 64;
    size_t temp_capacity = sizeof temp / sizeof *temp;
    if (dir_length + suffix_reserve > temp_capacity) return false;
    if (dir_length) memcpy(temp, path, dir_length * sizeof(wchar_t));
    const wchar_t *separator = L"";
    if (dir_length && path[dir_length - 1] != L'\\' &&
        path[dir_length - 1] != L'/')
        separator = L"\\";
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        wchar_t suffix[64];
        int suffix_length = swprintf(suffix, suffix_reserve,
            L"%ls.darkchat-%lu-%u.tmp", separator,
            (unsigned long)GetCurrentProcessId(), attempt);
        if (suffix_length < 0 ||
            (size_t)suffix_length + 1 > suffix_reserve ||
            dir_length + (size_t)suffix_length + 1 > temp_capacity)
            return false;
        memcpy(temp + dir_length, suffix,
            ((size_t)suffix_length + 1) * sizeof(wchar_t));
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (length) {
        DWORD written = 0;
        /* A single synchronous WriteFile must write the whole bounded
           payload; a short write is a failure, not a partial-file success. */
        ok = WriteFile(file, data, (DWORD)length, &written, NULL) != 0 &&
            written == (DWORD)length;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp);
    return ok;
}

int64_t chat_export_timestamp(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    /* FILETIME counts 100 ns intervals from 1601-01-01; subtract the Unix
       epoch offset and scale to milliseconds. */
    const uint64_t epoch_delta = 116444736000000000ULL;
    return (int64_t)((ticks.QuadPart - epoch_delta) / 10000ULL);
}

/* True when `name` (already trimmed of trailing dots/spaces) begins with a
   reserved Win32 DOS device basename. Windows refuses such a name however it
   is extended, so the caller must prefix it. The check is case-insensitive
   and stops at the first dot: "CON", "CON.txt" and "con.anything" are all
   reserved, while "CONSOLE" and "COM10" are not. */
static bool reserved_device_basename(const wchar_t *name, size_t length) {
    static const wchar_t *const devices[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
        L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
        L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    size_t stem = length;
    for (size_t i = 0; i < length; i++)
        if (name[i] == L'.') { stem = i; break; }
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        const wchar_t *device = devices[i];
        size_t j = 0;
        for (; j < stem && device[j]; j++) {
            wchar_t a = name[j];
            wchar_t b = device[j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) break;
        }
        if (j == stem && device[j] == 0) return true;
    }
    return false;
}

void chat_export_default_name(const wchar_t *title, const wchar_t *ext,
    wchar_t *out, size_t capacity) {
    if (!out || capacity == 0) return;
    const wchar_t *name = (title && title[0]) ? title : L"conversation";
    size_t used = 0;
    for (const wchar_t *p = name; *p && used + 1 < capacity; p++) {
        wchar_t ch = *p;
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' ||
            ch == L'|' || ch < 32)
            ch = L'_';
        out[used++] = ch;
    }
    while (used && (out[used - 1] == L' ' || out[used - 1] == L'.')) used--;
    if (used == 0) {
        const wchar_t *fallback = L"conversation";
        for (const wchar_t *p = fallback; *p && used + 1 < capacity; p++)
            out[used++] = *p;
    }
    /* A device basename is refused however it is extended, so prefix it with
       an underscore. Drop one character first when the prefix would not fit,
       keeping the result terminated. */
    if (reserved_device_basename(out, used)) {
        if (used + 1 >= capacity) used--;
        memmove(out + 1, out, used * sizeof(wchar_t));
        out[0] = L'_';
        used++;
    }
    if (ext)
        for (const wchar_t *p = ext; *p && used + 1 < capacity; p++)
            out[used++] = *p;
    out[used] = 0;
}

static int routing_sort_action(ChatProviderSort sort) {
    switch (sort) {
    case CHAT_PROVIDER_SORT_PRICE: return ACTION_ROUTING_SORT_PRICE;
    case CHAT_PROVIDER_SORT_THROUGHPUT: return ACTION_ROUTING_SORT_THROUGHPUT;
    case CHAT_PROVIDER_SORT_LATENCY: return ACTION_ROUTING_SORT_LATENCY;
    default: return ACTION_ROUTING_SORT_DEFAULT;
    }
}
void chat_actions_sync_routing(HMENU menu, const Chat *chat) {
    if (!menu || !chat) return;
    const ChatProviderRouting *routing = &chat->provider_routing;
    /* Backend radios: a no-op for menus that do not carry them. */
    CheckMenuRadioItem(menu, ACTION_BACKEND_OPENROUTER, ACTION_BACKEND_OLLAMA,
        chat->backend == CHAT_BACKEND_OLLAMA ? ACTION_BACKEND_OLLAMA
                                             : ACTION_BACKEND_OPENROUTER,
        MF_BYCOMMAND);
    CheckMenuRadioItem(menu, ACTION_ROUTING_SORT_DEFAULT,
        ACTION_ROUTING_SORT_LATENCY, routing_sort_action(routing->sort),
        MF_BYCOMMAND);
    CheckMenuItem(menu, ACTION_ROUTING_ALLOW_FALLBACKS,
        MF_BYCOMMAND | (routing->disallow_fallbacks ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(menu, ACTION_ROUTING_DATA_COLLECTION,
        MF_BYCOMMAND | (routing->data_collection == CHAT_DATA_COLLECTION_DENY
            ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(menu, ACTION_ROUTING_ZDR,
        MF_BYCOMMAND | (routing->zdr ? MF_CHECKED : MF_UNCHECKED));
    /* Provider routing is OpenRouter-only: gray the whole group while a local
       Ollama backend is active so the controls cannot imply an effect. The
       host also ignores a stale activation with an explicit status. */
    UINT enable = MF_BYCOMMAND |
        (chat->backend == CHAT_BACKEND_OLLAMA ? MF_GRAYED : MF_ENABLED);
    for (UINT id = ACTION_ROUTING_SORT_DEFAULT; id <= ACTION_ROUTING_ZDR; id++)
        EnableMenuItem(menu, id, enable);
}
void chat_actions_sync(HMENU menu, const ChatActionContext *context) {
    if (!menu || !context) return;
    /* Routing/backend sync owns check and radio marks, but not the enabled
       decision: it runs first so availability, applied last, is authoritative.
       Otherwise an OpenRouter generation would re-enable routing items. */
    chat_actions_sync_routing(menu, context->chat);
    size_t count;
    const ChatActionInfo *table = chat_action_table(&count);
    for (size_t i = 0; i < count; i++) {
        UINT enable = chat_action_available(table[i].id, context)
            ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(menu, (UINT)table[i].id, MF_BYCOMMAND | enable);
    }
    /* Dynamic profile items carry base + index ids, so the same command-id
        sync covers them wherever the submenu carrying them opens. */
    for (int id = CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE;
        id < CHAT_ACTION_DYNAMIC_END; id++) {
        UINT enable = chat_action_available(id, context)
            ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(menu, (UINT)id, MF_BYCOMMAND | enable);
    }
}
static void append_action(HMENU menu, int id) {
    const ChatActionInfo *info = chat_action_info(id);
    if (!info) return;
    if (info->flags & CHAT_ACTION_FLAG_SEPARATOR_BEFORE)
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    wchar_t text[CHAT_ACTION_LABEL_TEXT];
    if (!chat_action_compose_menu_label(id, text, CHAT_ACTION_LABEL_TEXT))
        return;
    AppendMenuW(menu, MF_STRING, (UINT_PTR)id, text);
}

/* Copies a profile name as a menu label with every '&' doubled: names are
    user-controlled and Win32 menu text treats a bare '&' as a mnemonic
    marker, so "R&D" would otherwise display as "RD". Doubling renders the
    literal ampersand; dynamic items carry no mnemonic, which is correct
    for arbitrary user text. */
static void append_escaped_name(HMENU menu, int id, const wchar_t *name) {
    wchar_t text[2 * CHAT_PROFILE_NAME_TEXT];
    size_t used = 0;
    for (const wchar_t *p = name; *p && used + 1 < sizeof text/sizeof *text; p++) {
        if (*p == L'&' && used + 2 < (int)(sizeof text/sizeof *text))
            text[used++] = L'&';
        text[used++] = *p;
    }
    text[used] = 0;
    AppendMenuW(menu, MF_STRING, (UINT_PTR)id, text);
}

/* One dynamic leaf item per live profile, carrying base + profile index.
    The submenu is rebuilt on every menu open, so no stale item can
    outlive the profile it was built from; dispatch still validates the
    index against the live count. */
static void append_profile_items(HMENU menu, const Chat *chat, int base) {
    for (int i = 0; i < chat->profile_count; i++) {
        const ChatPromptProfile *p = chat_profile(chat, i);
        if (!p) break;
        append_escaped_name(menu, base + i, p->name);
    }
}

/* Appends a submenu-only registry entry. MF_POPUP items carry their child
    HMENU where a command id would go, so the registry id is set on the item
    afterwards (MIIM_ID); that is what lets the availability sync enable and
    gray the header by command id like every leaf item. */
static void append_submenu(HMENU menu, int id, HMENU child,
    bool separator_before) {
    if (separator_before)
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)child, chat_action_menu_label(id));
    MENUITEMINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = MIIM_ID;
    info.wID = (UINT)id;
    SetMenuItemInfo(menu, GetMenuItemCount(menu) - 1, TRUE, &info);
}

/* Populates the dynamic profile submenus of one Customization entry. */
static void populate_profile_submenu(HMENU parent, const Chat *chat, int id) {
    if (id == ACTION_PROFILE_APPLY) {
        /* One submenu, two targets: the global slot and the active
            conversation's override, both reachable in two clicks. */
        HMENU global = CreatePopupMenu();
        HMENU here = CreatePopupMenu();
        if (!global || !here) {
            if (global) DestroyMenu(global);
            if (here) DestroyMenu(here);
            return;
        }
        append_profile_items(global, chat,
            CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE);
        append_profile_items(here, chat, CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE);
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)global, L"Global &prompt");
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)here, L"This &conversation");
    } else if (id == ACTION_PROFILE_EDIT) {
        append_profile_items(parent, chat, CHAT_ACTION_DYNAMIC_EDIT_BASE);
    } else if (id == ACTION_PROFILE_DELETE) {
        append_profile_items(parent, chat, CHAT_ACTION_DYNAMIC_DELETE_BASE);
    }
}

HMENU chat_actions_menu(const Chat *chat) {
    /* A popup root, not a menu bar: the only consumer tracks it directly
        with TrackPopupMenu, which does not render a CreateMenu() bar (it
        displays as an empty box). Items, their order and their separators come
        from the registry; only the submenu hierarchy is structural. */
    HMENU group[CHAT_ACTION_GROUP_COUNT];
    for (int g = 0; g < CHAT_ACTION_GROUP_COUNT; g++)
        group[g] = CreatePopupMenu();
    size_t count;
    const ChatActionInfo *table = chat_action_table(&count);
    for (size_t i = 0; i < count; i++) {
        if ((unsigned)table[i].group >= (unsigned)CHAT_ACTION_GROUP_COUNT)
            continue;
        if (table[i].flags & CHAT_ACTION_FLAG_SUBMENU_ONLY) {
            HMENU child = CreatePopupMenu();
            if (!child) continue;
            populate_profile_submenu(child, chat, table[i].id);
            append_submenu(group[table[i].group], table[i].id, child,
                (table[i].flags & CHAT_ACTION_FLAG_SEPARATOR_BEFORE) != 0);
        } else {
            append_action(group[table[i].group], table[i].id);
        }
    }
    chat_actions_sync_routing(group[CHAT_ACTION_GROUP_BACKEND], chat);
    chat_actions_sync_routing(group[CHAT_ACTION_GROUP_ROUTING], chat);
    AppendMenuW(group[CHAT_ACTION_GROUP_SETTINGS], MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_BACKEND], L"&Backend");
    AppendMenuW(group[CHAT_ACTION_GROUP_SETTINGS], MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_ROUTING], L"Provider &routing");
    HMENU bar = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_CONVERSATION], L"&Conversation");
    AppendMenuW(bar, MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_RESPONSE], L"&Response");
    AppendMenuW(bar, MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_SETTINGS], L"&Settings");
    AppendMenuW(bar, MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_CUSTOMIZATION], L"&Customization");
    AppendMenuW(bar, MF_POPUP,
        (UINT_PTR)group[CHAT_ACTION_GROUP_DATA], L"&Data");
    return bar;
}
