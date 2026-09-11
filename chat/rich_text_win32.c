#include "rich_text_win32.h"
#include <richedit.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

/* Rich Edit 4.1+ ships in Msftedit.dll. */
#define DARKCHAT_RICH_CLASS L"RICHEDIT50W"

/* Older headers may not define these; the values are stable. */
#ifndef EM_AUTOURLDETECT
#define EM_AUTOURLDETECT (WM_USER + 91)
#endif
#ifndef SES_EXTENDBACKCOLOR
#define SES_EXTENDBACKCOLOR 0x00400000
#endif

static HMODULE rich_library;

static COLORREF color_of(UiColor color) { return RGB(color.r, color.g, color.b); }

bool rich_text_library_open(void) {
    if (!rich_library) rich_library = LoadLibraryW(L"Msftedit.dll");
    return rich_library != NULL;
}

void rich_text_library_close(void) {
    if (rich_library) { FreeLibrary(rich_library); rich_library = NULL; }
}

void rich_text_theme(RichTextTheme *theme, const UiTheme *ui) {
    theme->background = color_of(ui->colors[UI_BG]);
    theme->text = color_of(ui->colors[UI_TEXT]);
    theme->muted = color_of(ui->colors[UI_MUTED]);
    theme->header_user = color_of(ui->colors[UI_ACCENT]);
    theme->header_assistant = color_of(ui->colors[UI_BRIGHT]);
    theme->header_system = color_of(ui->colors[UI_MUTED]);
    theme->error = RGB(224, 108, 117);
    theme->code_background = color_of(ui->colors[UI_TRACK]);
    theme->code_text = color_of(ui->colors[UI_BRIGHT]);
    theme->link = color_of(ui->colors[UI_ACCENT]);
    theme->composer_background = color_of(ui->colors[UI_TRACK]);
    theme->ui_family = ui->font_family;
    theme->mono_family = L"Consolas";
    theme->ui_size = ui->font_size[UI_BODY];
    theme->mono_size = ui->font_size[UI_SMALL] + 1.0f;
}

static void apply_format(RichTextControl *control, WPARAM scope, bool bold,
    bool mono, COLORREF color, bool code) {
    const RichTextTheme *theme = &control->theme;
    CHARFORMAT2W format;
    memset(&format, 0, sizeof format);
    format.cbSize = sizeof format;
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_WEIGHT;
    format.dwEffects = bold ? CFE_BOLD : 0;
    format.wWeight = (WORD)(bold ? 700 : 400);
    format.crTextColor = color;
    float size = mono ? theme->mono_size : theme->ui_size;
    /* yHeight is in twips (1/1440 inch), a physical unit the control already
       converts for the monitor DPI. Express the theme's DIP size as twips
       (1 DIP = 0.75 pt = 15 twips) so native text matches DarkUI at any DPI. */
    format.yHeight = (LONG)(size * 15.0f + 0.5f);
    if (format.yHeight < 1) format.yHeight = 1;
    const wchar_t *face = mono ? theme->mono_family : theme->ui_family;
    wcsncpy(format.szFaceName, face, LF_FACESIZE - 1);
    format.szFaceName[LF_FACESIZE - 1] = 0;
    format.bCharSet = DEFAULT_CHARSET;
    if (code) { format.crBackColor = theme->code_background; format.dwMask |= CFM_BACKCOLOR; }
    SendMessageW(control->window, EM_SETCHARFORMAT, scope, (LPARAM)&format);
}

static void caret_end(HWND window) {
    int length = GetWindowTextLengthW(window);
    SendMessageW(window, EM_SETSEL, (WPARAM)length, (LPARAM)length);
}

/* Inserts text using the just-established insertion format. */
static void run(RichTextControl *control, const wchar_t *text, bool bold,
    bool mono, COLORREF color, bool code) {
    apply_format(control, SCF_SELECTION, bold, mono, color, code);
    SendMessageW(control->window, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static LRESULT CALLBACK rich_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    RichTextControl *control =
        (RichTextControl *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!control) return DefWindowProcW(window, message, w, l);
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    if (message == WM_KEYDOWN || message == WM_KEYUP) {
        bool down = message == WM_KEYDOWN;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool control_key = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (control->on_key && control->on_key(control->user, w, shift,
            control_key, down)) return 0;
        if (down && w == VK_RETURN && !shift && control->on_submit) {
            control->on_submit(control->user);
            return 0;
        }
    }
    if (message == WM_CHAR) {
        /* Never insert a tab; plain Enter is swallowed when it submits. */
        if (w == L'\t') return 0;
        if (w == L'\r' && control->on_submit &&
            !(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    }
    if (message == WM_KILLFOCUS && control->on_blur) control->on_blur(control->user);
    return CallWindowProcW(control->previous, window, message, w, l);
}

static void set_margin(RichTextControl *control, int left, int right) {
    int l = (int)(left * control->dpi / 96.0f + 0.5f);
    int r = (int)(right * control->dpi / 96.0f + 0.5f);
    SendMessageW(control->window, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(l, r));
}

static bool create_control(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, bool multiline, bool readonly,
    long limit, const wchar_t *text) {
    memset(control, 0, sizeof *control);
    control->theme = *theme;
    control->dpi = dpi;
    control->multiline = multiline;
    control->readonly = readonly;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    if (multiline) style |= ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL | ES_AUTOVSCROLL;
    else style |= ES_AUTOHSCROLL;
    if (readonly) style |= ES_READONLY;
    control->window = CreateWindowExW(0, DARKCHAT_RICH_CLASS, text ? text : L"",
        style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL),
        NULL);
    if (!control->window) return false;
    SetWindowLongPtrW(control->window, GWLP_USERDATA, (LONG_PTR)control);
    control->previous = (WNDPROC)SetWindowLongPtrW(control->window,
        GWLP_WNDPROC, (LONG_PTR)rich_proc);
    SendMessageW(control->window, EM_SETBKGNDCOLOR, 0,
        (LPARAM)(readonly ? theme->background : theme->composer_background));
    SendMessageW(control->window, EM_EXLIMITTEXT, 0, (LPARAM)limit);
    SendMessageW(control->window, EM_AUTOURLDETECT, TRUE, 0);
    SendMessageW(control->window, EM_SETEVENTMASK, 0, ENM_LINK | ENM_SCROLL);
    SendMessageW(control->window, EM_SETEDITSTYLE, SES_EXTENDBACKCOLOR,
        SES_EXTENDBACKCOLOR);
    if (multiline) SendMessageW(control->window, EM_SETTARGETDEVICE, 0, 0);
    apply_format(control, SCF_DEFAULT, false, false, theme->text, false);
    set_margin(control, multiline ? 10 : 8, multiline ? 10 : 8);
    return true;
}

bool rich_text_create_transcript(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi) {
    return create_control(control, parent, id, theme, dpi, true, true,
        0x7fffffffL, NULL);
}

bool rich_text_create_composer(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi) {
    /* Bound input to what the host can read back, so nothing is silently lost. */
    return create_control(control, parent, id, theme, dpi, true, false,
        (long)(CHAT_MESSAGE_TEXT - 1), NULL);
}

bool rich_text_create_field(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, const wchar_t *text) {
    return create_control(control, parent, id, theme, dpi, false, false,
        (long)(CHAT_MODEL_TEXT - 1), text);
}

void rich_text_set_dpi(RichTextControl *control, float dpi) {
    if (!control->window || dpi <= 0) return;
    control->dpi = dpi;
    apply_format(control, SCF_DEFAULT, false, false, control->theme.text, false);
    set_margin(control, control->multiline ? 10 : 8, control->multiline ? 10 : 8);
}

void rich_text_get_text(const RichTextControl *control, wchar_t *out,
    size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = 0;
    if (!control->window) return;
    GetWindowTextW(control->window, out, (int)capacity);
}

void rich_text_set_text(RichTextControl *control, const wchar_t *text) {
    if (control->window) SetWindowTextW(control->window, text ? text : L"");
}

void rich_text_select_all(RichTextControl *control) {
    if (control->window) SendMessageW(control->window, EM_SETSEL, 0, -1);
}

static void open_link(HWND window, const CHARRANGE *range) {
    int length = (int)(range->cpMax - range->cpMin);
    if (length <= 0 || length > 2048) return;
    wchar_t *buffer = (wchar_t *)malloc((size_t)(length + 1) * sizeof(wchar_t));
    if (!buffer) return;
    TEXTRANGEW text_range;
    text_range.chrg = *range;
    text_range.lpstrText = buffer;
    SendMessageW(window, EM_GETTEXTRANGE, 0, (LPARAM)&text_range);
    buffer[length] = 0;
    ShellExecuteW(NULL, L"open", buffer, NULL, NULL, SW_SHOWNORMAL);
    free(buffer);
}

bool rich_text_pinned(const RichTextControl *control) {
    if (!control->window) return true;
    SCROLLINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SIF_ALL;
    if (!GetScrollInfo(control->window, SB_VERT, &info)) return true;
    if (info.nMax <= (int)info.nPage) return true;
    int maximum = info.nMax - (int)info.nPage + 1;
    return info.nPos >= maximum - 1;
}

void rich_text_scroll_to_end(RichTextControl *control) {
    if (!control->window) return;
    /* Move the caret to the end without leaving a visible selection, then
       pin the view to the bottom. */
    SendMessageW(control->window, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(control->window, EM_SCROLLCARET, 0, 0);
    SendMessageW(control->window, WM_VSCROLL, SB_BOTTOM, 0);
}

void rich_text_clear(RichTextControl *control) {
    if (!control->window) return;
    SendMessageW(control->window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(control->window, L"");
    SendMessageW(control->window, EM_SETREADONLY,
        control->readonly ? TRUE : FALSE, 0);
    control->has_content = false;
    apply_format(control, SCF_DEFAULT, false, false, control->theme.text, false);
}

/* Appends body text one line at a time, switching monospace styling while a
   ``` fence is open. Fence lines themselves are hidden. */
static void append_body(RichTextControl *control, const wchar_t *text,
    ChatRole role) {
    const RichTextTheme *theme = &control->theme;
    COLORREF body = role == CHAT_ROLE_ERROR ? theme->error : theme->text;
    bool code = false;
    const wchar_t *line = text;
    for (;;) {
        const wchar_t *end = line;
        while (*end && *end != L'\n') ++end;
        size_t length = (size_t)(end - line);
        bool fence = length >= 3 && line[0] == L'`' && line[1] == L'`' &&
            line[2] == L'`';
        if (fence) {
            code = !code;
        } else {
            wchar_t *buffer = (wchar_t *)malloc((length + 2) * sizeof(wchar_t));
            if (buffer) {
                if (length) wmemcpy(buffer, line, length);
                buffer[length] = L'\n';
                buffer[length + 1] = 0;
                run(control, buffer, false, code, code ? theme->code_text : body,
                    code);
                free(buffer);
            }
        }
        if (!*end) break;
        line = end + 1;
    }
}

void rich_text_append_message(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control->window) return;
    HWND window = control->window;
    bool pinned = rich_text_pinned(control);
    const RichTextTheme *theme = &control->theme;
    const wchar_t *label;
    COLORREF color;
    switch (role) {
    case CHAT_ROLE_USER: label = L"You"; color = theme->header_user; break;
    case CHAT_ROLE_ASSISTANT: label = L"Assistant"; color = theme->header_assistant; break;
    case CHAT_ROLE_ERROR: label = L"Error"; color = theme->error; break;
    default: label = L"System"; color = theme->header_system; break;
    }
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    caret_end(window);
    if (control->has_content) run(control, L"\n", false, false, theme->text, false);
    run(control, label, true, false, color, false);
    run(control, L"\n", false, false, theme->text, false);
    append_body(control, text ? text : L"", role);
    run(control, L"\n", false, false, theme->text, false);
    SendMessageW(window, EM_SETREADONLY, TRUE, 0);
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    control->has_content = true;
    if (pinned) rich_text_scroll_to_end(control);
}

bool rich_text_handle_notify(RichTextControl *control, LPARAM lparam) {
    NMHDR *header = (NMHDR *)lparam;
    if (header->code == EN_LINK) {
        ENLINK *link = (ENLINK *)lparam;
        if (link->msg == WM_LBUTTONUP) open_link(control->window, &link->chrg);
        return true;
    }
    if (header->code == EN_VSCROLL) return true;
    return false;
}



