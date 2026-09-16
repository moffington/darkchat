#include "rich_text_win32.h"
#include "markdown.h"
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
#ifndef EM_REQUESTRESIZE
#define EM_REQUESTRESIZE (WM_USER + 81)
#endif
#ifndef EN_REQUESTRESIZE
#define EN_REQUESTRESIZE 0x0701
#endif
#ifndef ENM_REQUESTRESIZE
#define ENM_REQUESTRESIZE 0x00010000
#endif
#ifndef ENM_SELCHANGE
#define ENM_SELCHANGE 0x00080000
#endif
#ifndef EN_SETFOCUS
#define EN_SETFOCUS 0x0700
#endif
#ifndef EN_KILLFOCUS
#define EN_KILLFOCUS 0x0702
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
    theme->reasoning_background = color_of(ui->colors[UI_PANEL]);
    theme->ui_family = ui->font_family;
    theme->mono_family = L"Consolas";
    theme->ui_size = ui->font_size[UI_BODY];
    theme->mono_size = ui->font_size[UI_SMALL] + 1.0f;
    theme->small_size = ui->font_size[UI_SMALL];
}

static void apply_format(RichTextControl *control, WPARAM scope, bool bold,
    bool italic, bool mono, COLORREF color, bool code, float size) {
    const RichTextTheme *theme = &control->theme;
    CHARFORMAT2W format;
    memset(&format, 0, sizeof format);
    format.cbSize = sizeof format;
    /* Every field is set on every call: runs never inherit stale formatting,
       so emphasis or the code tint cannot bleed into following text. */
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_WEIGHT |
        CFM_ITALIC | CFM_BACKCOLOR;
    format.dwEffects = (bold ? CFE_BOLD : 0) | (italic ? CFE_ITALIC : 0);
    format.wWeight = (WORD)(bold ? 700 : 400);
    format.crTextColor = color;
    /* yHeight is in twips (1/1440 inch), a physical unit the control already
       converts for the monitor DPI. Express the theme's DIP size as twips
       (1 DIP = 0.75 pt = 15 twips) so native text matches DarkUI at any DPI. */
    format.yHeight = (LONG)(size * 15.0f + 0.5f);
    if (format.yHeight < 1) format.yHeight = 1;
    const wchar_t *face = mono ? theme->mono_family : theme->ui_family;
    wcsncpy(format.szFaceName, face, LF_FACESIZE - 1);
    format.szFaceName[LF_FACESIZE - 1] = 0;
    format.bCharSet = DEFAULT_CHARSET;
    format.crBackColor = code ? theme->code_background :
        control->surface_background;
    SendMessageW(control->window, EM_SETCHARFORMAT, scope, (LPARAM)&format);
}

static void caret_end(HWND window) {
    int length = GetWindowTextLengthW(window);
    SendMessageW(window, EM_SETSEL, (WPARAM)length, (LPARAM)length);
}

/* Inserts text at the caret using an explicit point size. */
static void run_at(RichTextControl *control, const wchar_t *text, bool bold,
    bool mono, COLORREF color, bool code, float size) {
    apply_format(control, SCF_SELECTION, bold, false, mono, color, code, size);
    SendMessageW(control->window, EM_REPLACESEL, FALSE, (LPARAM)text);
}

/* Inserts text at the caret using the default face for its role. */
static void run(RichTextControl *control, const wchar_t *text, bool bold,
    bool mono, COLORREF color, bool code) {
    run_at(control, text, bold, mono, color, code,
        mono ? control->theme.mono_size : control->theme.ui_size);
}

static LRESULT CALLBACK rich_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    RichTextControl *control =
        (RichTextControl *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!control) return DefWindowProcW(window, message, w, l);
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    /* Read-only transcript blocks never consume the wheel: the transcript
       container decides whether to scroll a reasoning viewport or itself, so
       the target no longer depends on which control holds focus. */
    if (message == WM_MOUSEWHEEL && control->readonly) {
        HWND parent = GetParent(window);
        if (parent) { SendMessageW(parent, WM_MOUSEWHEEL, w, l); return 0; }
    }
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
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) &&
        control->on_line_click && control->readonly) {
        POINTL point;
        point.x = (LONG)(short)LOWORD(l);
        point.y = (LONG)(short)HIWORD(l);
        LONG character = (LONG)SendMessageW(window, EM_CHARFROMPOS, 0,
            (LPARAM)&point);
        int line = 0;
        if (character >= 0)
            line = (int)SendMessageW(window, EM_EXLINEFROMCHAR, 0,
                (LPARAM)character);
        if (control->on_line_click(control->user, control, line,
                message == WM_LBUTTONDOWN)) return 0;
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
    bool scrollable, long limit, COLORREF background, const wchar_t *text) {
    memset(control, 0, sizeof *control);
    control->theme = *theme;
    control->surface_background = background;
    control->dpi = dpi;
    control->multiline = multiline;
    control->readonly = readonly;
    control->scrollable = scrollable;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    if (multiline) {
        style |= ES_MULTILINE | ES_WANTRETURN;
        if (scrollable) style |= WS_VSCROLL | ES_AUTOVSCROLL;
    } else style |= ES_AUTOHSCROLL;
    if (readonly) style |= ES_READONLY;
    control->window = CreateWindowExW(0, DARKCHAT_RICH_CLASS, text ? text : L"",
        style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL),
        NULL);
    if (!control->window) return false;
    SetWindowLongPtrW(control->window, GWLP_USERDATA, (LONG_PTR)control);
    control->previous = (WNDPROC)SetWindowLongPtrW(control->window,
        GWLP_WNDPROC, (LONG_PTR)rich_proc);
    SendMessageW(control->window, EM_SETBKGNDCOLOR, 0, (LPARAM)background);
    SendMessageW(control->window, EM_EXLIMITTEXT, 0, (LPARAM)limit);
    SendMessageW(control->window, EM_AUTOURLDETECT, TRUE, 0);
    /* Read-only blocks size themselves to their content, so they ask their
        parent for the required height whenever their text or width changes.
        They also report selection changes so the transcript can defer
        destructive rebuilds while a selection is held. Focus notifications
        (EN_SETFOCUS/EN_KILLFOCUS) need no event mask: rich edit controls
        send them regardless, so no ENM_FOCUS bit is set. */
    DWORD events = ENM_LINK | ENM_SCROLL;
    if (readonly) events |= ENM_SELCHANGE;
    if (readonly && !scrollable) events |= ENM_REQUESTRESIZE;
    SendMessageW(control->window, EM_SETEVENTMASK, 0, events);
    SendMessageW(control->window, EM_SETEDITSTYLE, SES_EXTENDBACKCOLOR,
        SES_EXTENDBACKCOLOR);
    if (multiline) SendMessageW(control->window, EM_SETTARGETDEVICE, 0, 0);
    apply_format(control, SCF_DEFAULT, false, false, false, theme->text,
        false, theme->ui_size);
    set_margin(control, multiline ? 10 : 8, multiline ? 10 : 8);
    return true;
}

bool rich_text_create_block(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi) {
    /* Model output grows on demand; the network/storage layers provide the
       practical safety bounds instead of this display control. */
    return create_control(control, parent, id, theme, dpi, true, true, false,
        0x7ffffffeL, theme->background, NULL);
}

bool rich_text_create_viewport(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi) {
    return create_control(control, parent, id, theme, dpi, true, true, true,
        0x7ffffffeL, theme->reasoning_background, NULL);
}

bool rich_text_create_composer(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi) {
    /* Bound input to what the host can read back, so nothing is silently lost. */
    return create_control(control, parent, id, theme, dpi, true, false, true,
        (long)(CHAT_COMPOSER_TEXT - 1), theme->composer_background, NULL);
}

bool rich_text_create_field(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, const wchar_t *text) {
    return rich_text_create_field_limit(control, parent, id, theme, dpi,
        (long)(CHAT_MODEL_TEXT - 1), text);
}

bool rich_text_create_field_limit(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, long limit, const wchar_t *text) {
    if (limit < 1) return false;
    return create_control(control, parent, id, theme, dpi, false, false, false,
        limit, theme->composer_background, text);
}

void rich_text_set_dpi(RichTextControl *control, float dpi) {
    if (!control->window || dpi <= 0) return;
    control->dpi = dpi;
    apply_format(control, SCF_DEFAULT, false, false, false,
        control->theme.text, false, control->theme.ui_size);
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

bool rich_text_has_selection(const RichTextControl *control) {
    if (!control || !control->window) return false;
    CHARRANGE selection;
    SendMessageW(control->window, EM_EXGETSEL, 0, (LPARAM)&selection);
    return selection.cpMax > selection.cpMin;
}

void rich_text_scroll_to_end(RichTextControl *control) {
    if (!control->window) return;
    /* Move the caret to the end without leaving a visible selection, then
       pin the view to the bottom. */
    SendMessageW(control->window, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(control->window, EM_SCROLLCARET, 0, 0);
    SendMessageW(control->window, WM_VSCROLL, SB_BOTTOM, 0);
}

static const wchar_t *role_label(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_USER: return L"You";
    case CHAT_ROLE_ASSISTANT: return L"Assistant";
    case CHAT_ROLE_ERROR: return L"Error";
    default: return L"System";
    }
}

static COLORREF role_color(const RichTextTheme *theme, ChatRole role) {
    switch (role) {
    case CHAT_ROLE_USER: return theme->header_user;
    case CHAT_ROLE_ASSISTANT: return theme->header_assistant;
    case CHAT_ROLE_ERROR: return theme->error;
    default: return theme->header_system;
    }
}

/* Clears the control and leaves it writable with the insertion point at the
   start, ready for a batch of runs. */
static void begin_write(RichTextControl *control) {
    SendMessageW(control->window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(control->window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(control->window, L"");
    caret_end(control->window);
}

static void end_write(RichTextControl *control) {
    SendMessageW(control->window, EM_SETREADONLY,
        control->readonly ? TRUE : FALSE, 0);
    control->has_content = GetWindowTextLengthW(control->window) > 0;
    SendMessageW(control->window, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(control->window, NULL, NULL, RDW_INVALIDATE);
}

/* Writes text verbatim, one line at a time; CR LF pairs become one paragraph
   break. No markdown interpretation of any kind: user, system and error
   messages and running assistant output stay exactly as stored, including
   fence markers. */
static void write_literal(RichTextControl *control, const wchar_t *text,
    COLORREF color) {
    bool first = true;
    const wchar_t *line = text ? text : L"";
    for (;;) {
        const wchar_t *end = line;
        while (*end && *end != L'\n' && *end != L'\r') ++end;
        size_t length = (size_t)(end - line);
        if (!first) run(control, L"\n", false, false, color, false);
        wchar_t *buffer = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
        if (buffer) {
            if (length) wmemcpy(buffer, line, length);
            buffer[length] = 0;
            run(control, buffer, false, false, color, false);
            free(buffer);
        }
        first = false;
        if (!*end) break;
        line = end + 1;
        if (*end == L'\r' && *line == L'\n') ++line;
    }
}

/* Terminal assistant body: parse once, insert the normalized document, then
   style each run's exact range. Every character belongs to a run, so no
   format survives past its run. */
void rich_text_set_markdown(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control || !control->window) return;
    const RichTextTheme *theme = &control->theme;
    COLORREF body = role == CHAT_ROLE_ERROR ? theme->error : theme->text;
    begin_write(control);
    MdDocument document;
    if (!markdown_render(text, &document)) {
        /* Transactional failure: fall back to the verbatim body. */
        write_literal(control, text, body);
    } else {
        SendMessageW(control->window, EM_REPLACESEL, FALSE,
            (LPARAM)document.text);
        for (int i = 0; i < document.run_count; i++) {
            const MdRun *r = &document.runs[i];
            if (!r->length) continue;
            CHARRANGE range;
            range.cpMin = (LONG)r->offset;
            range.cpMax = (LONG)(r->offset + r->length);
            SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
            COLORREF color = r->code ? theme->code_text :
                r->muted ? theme->muted : body;
            float size = r->mono ? theme->mono_size :
                r->heading == 1 ? theme->ui_size + 3.0f :
                r->heading == 2 ? theme->ui_size + 2.0f :
                r->heading == 3 ? theme->ui_size + 1.0f : theme->ui_size;
            apply_format(control, SCF_SELECTION, r->bold, r->italic, r->mono,
                color, r->code, size);
        }
        markdown_dispose(&document);
        caret_end(control->window);
    }
    end_write(control);
}

void rich_text_set_body(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control || !control->window) return;
    const RichTextTheme *theme = &control->theme;
    begin_write(control);
    write_literal(control, text,
        role == CHAT_ROLE_ERROR ? theme->error : theme->text);
    end_write(control);
}

void rich_text_set_head(RichTextControl *control, ChatRole role,
    const wchar_t *row) {
    if (!control || !control->window) return;
    const RichTextTheme *theme = &control->theme;
    begin_write(control);
    run(control, role_label(role), true, false, role_color(theme, role), false);
    if (row && row[0]) {
        run(control, L"\n", false, false, theme->text, false);
        run(control, row, false, false, theme->muted, false);
    }
    end_write(control);
}

void rich_text_set_block(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control || !control->window) return;
    const RichTextTheme *theme = &control->theme;
    begin_write(control);
    run(control, role_label(role), true, false, role_color(theme, role), false);
    if (text && text[0]) {
        run(control, L"\n", false, false, theme->text, false);
        write_literal(control, text,
            role == CHAT_ROLE_ERROR ? theme->error : theme->text);
    }
    end_write(control);
}

void rich_text_append_body(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window || !text || !text[0]) return;
    HWND window = control->window;
    CHARRANGE selection;
    SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    caret_end(window);
    run(control, text, false, false, control->theme.text, false);
    SendMessageW(window, EM_SETREADONLY, control->readonly ? TRUE : FALSE, 0);
    control->has_content = true;
    /* The transcript owns answer scrolling. Keep the block at its origin and
       let the host finish sizing/positioning it before the next paint. */
    SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&selection);
    POINT origin = {0, 0};
    SendMessageW(window, EM_SETSCROLLPOS, 0, (LPARAM)&origin);
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE);
}

void rich_text_set_meta(RichTextControl *control, const wchar_t *text,
    const wchar_t *error) {
    if (!control || !control->window) return;
    begin_write(control);
    if (text && text[0])
        run_at(control, text, false, false, control->theme.muted, false,
            control->theme.small_size);
    if (error && error[0]) {
        if (text && text[0])
            run_at(control, L"\n", false, false, control->theme.text, false,
                control->theme.small_size);
        run_at(control, error, false, false, control->theme.error, false,
            control->theme.small_size);
    }
    end_write(control);
}

void rich_text_set_reasoning(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window) return;
    HWND window = control->window;
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(window, L"");
    caret_end(window);
    run(control, text ? text : L"", false, false, control->theme.muted, false);
    SendMessageW(window, EM_SETREADONLY, control->readonly ? TRUE : FALSE, 0);
    control->has_content = text && text[0];
    rich_text_scroll_to_end(control);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

void rich_text_append_reasoning(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window || !text || !text[0]) return;
    HWND window = control->window;
    CHARRANGE selection;
    SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
    bool pinned = rich_text_pinned(control);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    caret_end(window);
    run(control, text, false, false, control->theme.muted, false);
    SendMessageW(window, EM_SETREADONLY, control->readonly ? TRUE : FALSE, 0);
    control->has_content = true;
    /* Follow the stream only while the reader stays pinned to the bottom;
       restore the reader's selection after scrolling so it survives appends. */
    if (pinned) rich_text_scroll_to_end(control);
    SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&selection);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
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



