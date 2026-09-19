#include "chat/transcript/rich_text_win32.h"
#include "chat/transcript/markdown.h"
#include "chat/transcript/table_layout.h"
#include <richedit.h>
#include <shellapi.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Rich Edit 4.1+ ships in Msftedit.dll. */
#define DARKCHAT_RICH_CLASS L"RICHEDIT50W"

/* The paragraph format owns at most 32 tab stops; the table subset never
   exceeds MD_MAX_TABLE_COLUMNS (24). */
#ifndef MAX_TAB_STOPS
#define MAX_TAB_STOPS 32
#endif
#ifndef PFM_TABSTOPS
#define PFM_TABSTOPS 0x00008000
#endif

/* Older headers may not define these; the values are stable. */
#ifndef EM_AUTOURLDETECT
#define EM_AUTOURLDETECT (WM_USER + 91)
#endif
/* Re-enabling detection without an initial document scan keeps link effects
   this module already applied; the flag is documented but absent from older
   headers. */
#ifndef AURL_NOINITIALSCAN
#define AURL_NOINITIALSCAN 256
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
    /* The assistant label is deliberately quiet: the answer carries the
       hierarchy, the role name only anchors it. */
    theme->header_assistant = color_of(ui->colors[UI_MUTED]);
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

static void apply_format(RichTextControl *control, WPARAM scope, unsigned style,
    COLORREF color, float size) {
    const RichTextTheme *theme = &control->theme;
    CHARFORMAT2W format;
    memset(&format, 0, sizeof format);
    format.cbSize = sizeof format;
    /* Every field is set on every call: runs never inherit stale formatting,
       so emphasis, strikethrough or the code tint cannot bleed into following
       text. */
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_WEIGHT |
        CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR;
    format.dwEffects = (style & MD_STYLE_BOLD ? CFE_BOLD : 0) |
        (style & MD_STYLE_ITALIC ? CFE_ITALIC : 0) |
        (style & MD_STYLE_STRIKE ? CFE_STRIKEOUT : 0);
    format.wWeight = (WORD)(style & MD_STYLE_BOLD ? 700 : 400);
    format.crTextColor = color;
    /* yHeight is in twips (1/1440 inch), a physical unit the control already
       converts for the monitor DPI. Express the theme's DIP size as twips
       (1 DIP = 0.75 pt = 15 twips) so native text matches DarkUI at any DPI. */
    format.yHeight = (LONG)(size * 15.0f + 0.5f);
    if (format.yHeight < 1) format.yHeight = 1;
    const wchar_t *face = style & MD_STYLE_MONO ? theme->mono_family :
        theme->ui_family;
    wcsncpy(format.szFaceName, face, LF_FACESIZE - 1);
    format.szFaceName[LF_FACESIZE - 1] = 0;
    format.bCharSet = DEFAULT_CHARSET;
    format.crBackColor = style & MD_STYLE_CODE ? theme->code_background :
        control->surface_background;
    SendMessageW(control->window, EM_SETCHARFORMAT, scope, (LPARAM)&format);
}

/* Marks one label range as a link. The effect is set on its own mask, so the
   run formatting that precedes it cannot clear it; the destination is resolved
   through the control's transferred metadata when EN_LINK arrives. */
static void apply_link(RichTextControl *control, LONG cpMin, LONG cpMax) {
    CHARFORMAT2W format;
    memset(&format, 0, sizeof format);
    format.cbSize = sizeof format;
    format.dwMask = CFM_LINK;
    format.dwEffects = CFE_LINK;
    CHARRANGE range;
    range.cpMin = cpMin;
    range.cpMax = cpMax;
    SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
    SendMessageW(control->window, EM_SETCHARFORMAT, SCF_SELECTION,
        (LPARAM)&format);
}

/* Layout columns to twips. Twips are DIP * 15 (the unit yHeight already uses),
   so the control converts them for the monitor DPI; one column is half an em,
   roughly the advance of the synthesized marker glyphs. */
static LONG indent_twips(const RichTextControl *control, int columns) {
    return (LONG)(columns * control->theme.ui_size * 15.0f * 0.5f + 0.5f);
}

/* Paragraph format for the current selection. Every field is set on every
   call, mirroring apply_format, so a paragraph never inherits a stale indent.
   dxStartIndent moves the first line; a positive dxOffset moves the following
   lines further in, which is the hanging indent a synthesized marker needs.
   Tab stops are always part of the mask: a flattened table paragraph owns the
   complete monotonic stop array for its table, and every ordinary paragraph
   explicitly clears its stops (cTabCount = 0) so a previous table's stops can
   never survive into following text (TABLES_PLAN.md I6). */
typedef struct {
    int column_count;
    int x[MD_MAX_TABLE_COLUMNS];        /* pixel anchors, device px */
    unsigned char align[MD_MAX_TABLE_COLUMNS];
} MdTableFormat;

static void apply_paragraph_tabs(RichTextControl *control, int first,
    int hanging, const MdTableFormat *table, int dpi) {
    PARAFORMAT2 format;
    memset(&format, 0, sizeof format);
    format.cbSize = sizeof format;
    format.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT |
        PFM_TABSTOPS;
    format.dxStartIndent = indent_twips(control, first);
    format.dxOffset = indent_twips(control, hanging);
    format.dxRightIndent = 0;
    format.cTabCount = 0;
    if (table) {
        /* A table paragraph has no indentation; its tab stops are the layout. */
        format.dxStartIndent = 0;
        format.dxOffset = 0;
        int count = table->column_count;
        if (count < 0) count = 0;
        if (count > MAX_TAB_STOPS) count = MAX_TAB_STOPS;
        int used = 0;
        for (int i = 0; i < count; i++) {
            LONG twip = (LONG)MulDiv(table->x[i], 1440, dpi);
            /* The stop position must fit the low 24 bits; the alignment nibble
               occupies bits 24-27 and the leader bits 28-31 stay zero. */
            if (twip < 0 || twip > 0x00FFFFFFL) break;
            format.rgxTabs[i] = twip |
                (((LONG)table->align[i] & 0xF) << 24);
            used++;
        }
        format.cTabCount = (WORD)used;
    }
    SendMessageW(control->window, EM_SETPARAFORMAT, 0, (LPARAM)&format);
}

static void apply_paragraph(RichTextControl *control, int first, int hanging) {
    apply_paragraph_tabs(control, first, hanging, NULL, 96);
}

/* Clears the paragraph format of the whole current selection: a verbatim body,
   a literal fallback or a rebuilt document must never inherit an indent or a
   stale tab-stop array. */
static void reset_paragraphs(RichTextControl *control) {
    apply_paragraph(control, 0, 0);
}

/* Applies each paragraph's layout. Adjacent paragraphs sharing one layout are
   formatted with a single selection, so a long fenced block costs one message. */
static void apply_blocks(RichTextControl *control, const MdDocument *document) {
    for (int i = 0; i < document->block_count;) {
        const MdBlock *block = &document->blocks[i];
        int j = i + 1;
        while (j < document->block_count &&
            document->blocks[j].first_indent == block->first_indent &&
            document->blocks[j].continuation_indent ==
                block->continuation_indent) ++j;
        const MdBlock *last = &document->blocks[j - 1];
        CHARRANGE range;
        range.cpMin = (LONG)block->offset;
        range.cpMax = (LONG)(last->offset + last->length);
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
        apply_paragraph(control, block->first_indent,
            block->continuation_indent - block->first_indent);
        i = j;
    }
}

static void caret_end(HWND window) {
    /* EM_SETSEL positions count one unit per paragraph mark, so the internal
       length (not the CRLF-expanded GetWindowTextLengthW) is the caret end. */
    GETTEXTLENGTHEX measure;
    measure.flags = GTL_NUMCHARS;
    measure.codepage = 1200;
    LONG length = (LONG)SendMessageW(window, EM_GETTEXTLENGTHEX,
        (WPARAM)&measure, 0);
    SendMessageW(window, EM_SETSEL, (WPARAM)length, (LPARAM)length);
}

/* Inserts text at the caret using an explicit point size. */
static void run_at(RichTextControl *control, const wchar_t *text, unsigned style,
    COLORREF color, float size) {
    apply_format(control, SCF_SELECTION, style, color, size);
    SendMessageW(control->window, EM_REPLACESEL, FALSE, (LPARAM)text);
}

/* Inserts text at the caret using the default face for its role. */
static void run(RichTextControl *control, const wchar_t *text, unsigned style,
    COLORREF color) {
    run_at(control, text, style, color,
        style & MD_STYLE_MONO ? control->theme.mono_size : control->theme.ui_size);
}

/* Releases the control's Markdown metadata: link label ranges with their
   destination arena, and the rendered code-block ranges. Every content
   mutation calls it so stale metadata can never survive a write, and
   WM_NCDESTROY calls it so nothing outlives the window. */
static void clear_links(RichTextControl *control) {
    free(control->links);
    free(control->link_targets);
    control->links = NULL;
    control->link_count = 0;
    control->link_targets = NULL;
    control->link_targets_length = 0;
    free(control->code_blocks);
    control->code_blocks = NULL;
    control->code_block_count = 0;
}

static LRESULT CALLBACK rich_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    RichTextControl *control =
        (RichTextControl *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!control) return DefWindowProcW(window, message, w, l);
    if (message == WM_NCDESTROY) {
        clear_links(control);
        control->window = NULL;
        return CallWindowProcW(control->previous, window, message, w, l);
    }
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    /* A read-only transcript surface always presents the text-selection
       cursor. Streaming rebuilds make the control transiently writable
       (EM_SETREADONLY) and a child resize under the pointer delivers
       WM_SETCURSOR inside that window; the cursor must never depend on
       transient write state, so it is installed here and the message is
       consumed. Editable controls (composer, fields) keep native behavior. */
    if (message == WM_SETCURSOR && control->readonly &&
        LOWORD(l) == HTCLIENT) {
        SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32513)));
        return TRUE;
    }
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
    apply_format(control, SCF_DEFAULT, 0, theme->text, theme->ui_size);
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
    apply_format(control, SCF_DEFAULT, 0, control->theme.text,
        control->theme.ui_size);
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
    if (control->window) {
        clear_links(control);
        SetWindowTextW(control->window, text ? text : L"");
    }
}

static void open_url_default(const wchar_t *url) {
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* Launch seam: production uses the shell; the integration suite substitutes a
   capture so EN_LINK can be driven without starting a browser. */
static void (*open_url_hook)(const wchar_t *url) = open_url_default;

void rich_text_test_set_open(void (*open)(const wchar_t *url)) {
    open_url_hook = open ? open : open_url_default;
}

/* Only HTTP(S) destinations are ever handed to the shell. */
static bool is_http_url(const wchar_t *s, size_t n) {
    static const wchar_t *schemes[2] = {L"https://", L"http://"};
    for (int k = 0; k < 2; k++) {
        size_t length = wcslen(schemes[k]);
        if (n < length) continue;
        size_t i = 0;
        for (; i < length; i++) {
            wchar_t c = s[i];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (c != schemes[k][i]) break;
        }
        if (i == length) return true;
    }
    return false;
}

/* Opens one stored destination through its NUL-terminated arena entry. The
   arena holds no length limit, so a long semantic URL stays usable; the range
   is bounds-checked against the stored arena length and the scheme revalidated
   before the shell is asked to open it. */
static void open_target(const RichTextControl *control, const MdLink *link) {
    size_t offset = link->target_offset;
    size_t length = link->target_length;
    if (length + 1 > control->link_targets_length) return;
    if (offset > control->link_targets_length - (length + 1)) return;
    const wchar_t *target = control->link_targets + offset;
    if (target[length] != L'\0' || !is_http_url(target, length)) return;
    open_url_hook(target);
}

/* Fallback for links the control detected itself (EM_AUTOURLDETECT): the URL
   is visible text, so the range is read back and opened verbatim. */
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
    open_url_hook(buffer);
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

int rich_text_code_block_count(const RichTextControl *control) {
    return control ? control->code_block_count : 0;
}

bool rich_text_code_block_range(const RichTextControl *control, int index,
    size_t *start, size_t *length) {
    if (!control || index < 0 || index >= control->code_block_count) return false;
    if (start) *start = control->code_blocks[index].offset;
    if (length) *length = control->code_blocks[index].length;
    return true;
}

int rich_text_code_block_at_char(const RichTextControl *control, size_t cp) {
    if (!control) return -1;
    for (int i = 0; i < control->code_block_count; i++) {
        const MdCodeFence *block = &control->code_blocks[i];
        if (cp >= block->offset && cp - block->offset < block->length) return i;
    }
    return -1;
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
    clear_links(control);
    SendMessageW(control->window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(control->window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(control->window, L"");
    /* Restore normal auto-URL scanning for the text about to be inserted: a
       previous Markdown write may have re-enabled detection with no initial
       scan, which skips recognizing URLs in later insertions. */
    SendMessageW(control->window, EM_AUTOURLDETECT, TRUE, 0);
    caret_end(control->window);
    /* Paragraph defaults are cleared only after the control is empty: the one
       paragraph that remains is the one later text inherits from. */
    reset_paragraphs(control);
}

static void end_write(RichTextControl *control) {
    SendMessageW(control->window, EM_SETREADONLY,
        control->readonly ? TRUE : FALSE, 0);
    control->has_content = GetWindowTextLengthW(control->window) > 0;
    SendMessageW(control->window, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(control->window, NULL, NULL, RDW_INVALIDATE);
}

/* Number of code units a literal write deposits: every LF, CRLF or lone CR
   becomes exactly one paragraph break (LF), every other code unit is kept. */
static size_t literal_code_units(const wchar_t *text) {
    size_t count = 0;
    const wchar_t *p = text ? text : L"";
    while (*p) {
        if (*p == L'\r') {
            ++count;
            ++p;
            if (*p == L'\n') ++p;
        } else {
            ++count;
            ++p;
        }
    }
    return count;
}

/* Internal Rich Edit character count (one unit per paragraph mark), the
   coordinate space EM_SETSEL and link ranges use. GetWindowTextLengthW would
   expand every break to CRLF and is not that space. */
static LONG text_length(HWND window) {
    GETTEXTLENGTHEX measure;
    measure.flags = GTL_NUMCHARS;
    measure.codepage = 1200;
    return (LONG)SendMessageW(window, EM_GETTEXTLENGTHEX, (WPARAM)&measure, 0);
}

/* Writes text verbatim, one logical line per paragraph; LF, CRLF and lone CR
   each become one paragraph break. No markdown interpretation of any kind:
   user, system and error messages and running assistant output stay exactly as
   stored, including fence markers.

   Allocation-free and complete: the normalized stream is built into a fixed
   stack chunk and flushed in large pieces, so a failed heap allocation can
   never drop content and many small insertions cannot make the write
   quadratic. Returns true only when the whole normalized source landed. */
static bool write_literal(RichTextControl *control, const wchar_t *text,
    COLORREF color) {
    HWND window = control->window;
    LONG start = text_length(window);
    const wchar_t *p = text ? text : L"";
    wchar_t chunk[2048];
    size_t used = 0;
    const size_t capacity = sizeof chunk / sizeof *chunk - 2;
    while (*p) {
        wchar_t character;
        if (*p == L'\r') {
            character = L'\n';
            ++p;
            if (*p == L'\n') ++p;
        } else if (*p == L'\n') {
            character = L'\n';
            ++p;
        } else {
            character = *p++;
        }
        chunk[used++] = character;
        if (used == capacity) {
            wchar_t held = 0;
            size_t flush = used;
            /* Never split a surrogate pair across insertions. */
            if (chunk[used - 1] >= (wchar_t)0xD800 &&
                chunk[used - 1] <= (wchar_t)0xDBFF) {
                held = chunk[used - 1];
                flush = used - 1;
            }
            chunk[flush] = 0;
            if (flush) SendMessageW(window, EM_REPLACESEL, FALSE, (LPARAM)chunk);
            if (held) {
                chunk[0] = held;
                used = 1;
            } else {
                used = 0;
            }
        }
    }
    if (used) {
        chunk[used] = 0;
        SendMessageW(window, EM_REPLACESEL, FALSE, (LPARAM)chunk);
    }
    LONG finish = text_length(window);
    if (finish > start) {
        CHARRANGE range;
        range.cpMin = start;
        range.cpMax = finish;
        SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&range);
        apply_format(control, SCF_SELECTION, 0, color, control->theme.ui_size);
        /* Leave an empty selection at the end, exactly like the other
           writers: a lingering selection would make the surface read as
           reader-selected and defer every later rebuild. */
        caret_end(window);
    }
    return (size_t)(finish - start) == literal_code_units(text);
}

/* Takes ownership of a rendered document's link metadata: the label ranges and
   the single destination arena move to the control, so the document no longer
   owns them when it is disposed. */
static void take_links(RichTextControl *control, MdDocument *document) {
    control->links = document->links;
    control->link_count = document->link_count;
    control->link_targets = document->targets;
    control->link_targets_length = document->targets_length;
    document->links = NULL;
    document->link_count = 0;
    document->link_capacity = 0;
    document->targets = NULL;
    document->targets_length = 0;
    document->targets_capacity = 0;
}

/* Takes ownership of a rendered document's code-block metadata. The document
   text is inserted verbatim on the no-table path, so the fence ranges already
   address control characters and transfer as-is. */
static void take_code_fences(RichTextControl *control, MdDocument *document) {
    control->code_blocks = document->fences;
    control->code_block_count = document->fence_count;
    document->fences = NULL;
    document->fence_count = 0;
    document->fence_capacity = 0;
}

/* Face size (DIPs) for one run, shared by the direct path and the table plan so
   a measured span and its rendered run always agree. */
static float run_size(const RichTextTheme *theme, unsigned style, int heading) {
    if (style & MD_STYLE_MONO) return theme->mono_size;
    if (heading == 1) return theme->ui_size + 3.0f;
    if (heading == 2) return theme->ui_size + 2.0f;
    if (heading == 3) return theme->ui_size + 1.0f;
    return theme->ui_size;
}

static COLORREF run_color(const RichTextTheme *theme, unsigned style,
    COLORREF body) {
    if (style & MD_STYLE_CODE) return theme->code_text;
    if (style & MD_STYLE_MUTED) return theme->muted;
    return body;
}

/* Applies a rendered document's runs, paragraphs and links over the text most
   recently inserted (the no-table direct path). Every character belongs to a
   run, so no format survives past its run. */
static void apply_document(RichTextControl *control, const MdDocument *document,
    COLORREF body) {
    const RichTextTheme *theme = &control->theme;
    CHARRANGE whole;
    whole.cpMin = 0;
    whole.cpMax = (LONG)document->length;
    SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&whole);
    reset_paragraphs(control);
    apply_blocks(control, document);
    for (int i = 0; i < document->run_count; i++) {
        const MdRun *r = &document->runs[i];
        if (!r->length) continue;
        CHARRANGE range;
        range.cpMin = (LONG)r->offset;
        range.cpMax = (LONG)(r->offset + r->length);
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
        apply_format(control, SCF_SELECTION, r->style,
            run_color(theme, r->style, body),
            run_size(theme, r->style, r->heading));
    }
    if (document->link_count) {
        /* Detection would strip a link effect that is not an autoURL, so
           custom labels are applied with detection off and it is restored
           without re-scanning, leaving both the labels and any bare URL the
           insertion recognized. */
        SendMessageW(control->window, EM_AUTOURLDETECT, FALSE, 0);
        for (int i = 0; i < document->link_count; i++)
            apply_link(control, (LONG)document->links[i].offset,
                (LONG)(document->links[i].offset + document->links[i].length));
        SendMessageW(control->window, EM_AUTOURLDETECT,
            (WPARAM)(AURL_ENABLEURL | AURL_NOINITIALSCAN), 0);
    }
    caret_end(control->window);
}

/* ---- Transactional flatten plan (TABLES_PLAN.md 3.2, 4, 5) --------------- */

typedef struct {
    size_t offset, length;              /* paragraph range in plan text */
    unsigned char first_indent, continuation_indent;
    int table_format;                   /* index into formats[], or -1 */
} MdParagraph;

typedef struct {
    size_t src, length;                 /* source range copied verbatim */
    size_t plan;                        /* plan offset the range maps to */
} PlanSegment;

typedef struct {
    wchar_t *text; size_t length, text_capacity;
    MdRun *runs; int run_count, run_capacity;
    MdParagraph *paragraphs; int paragraph_count, paragraph_capacity;
    MdTableFormat *table_formats; int table_format_count, table_format_capacity;
    MdLink *links; int link_count, link_capacity;   /* control coords */
    MdCodeFence *code_blocks; int code_block_count, code_block_capacity;
    PlanSegment *segments; int segment_count, segment_capacity;
    wchar_t *link_targets;              /* adopted document target arena */
    size_t link_targets_length;
    bool owns_targets;
    bool failed, overflow;
    int dpi;                            /* rounded device DPI */
} MarkdownPlan;

/* Test-only allocation seam: every permanent and temporary plan allocation
   (text, runs, paragraphs, formats, links, spans, cell geometry, wrapped
   lines) goes through these. `allocation_index` 0 fails the first attempt and
   every attempt after it, so a countdown walk exercises each failure point. */
static int plan_fail_after = -1;
static int plan_alloc_index;

void markdown_plan_test_fail_after(int allocation_index) {
    plan_fail_after = allocation_index;
}

/* Test-only metric seam: forces the GDI measurement adapter to report a
   failure, so the document-wide transactional fallback can be exercised. */
static bool test_fail_metrics;

void rich_text_test_fail_metrics(bool enable) {
    test_fail_metrics = enable;
}

static bool plan_should_fail(void) {
    int index = plan_alloc_index++;
    return plan_fail_after >= 0 && index >= plan_fail_after;
}

static void *plan_malloc(size_t bytes) {
    if (bytes == 0) bytes = 1;
    if (plan_should_fail()) return NULL;
    return malloc(bytes);
}

static void *plan_realloc(void *memory, size_t bytes) {
    if (bytes == 0) bytes = 1;
    if (plan_should_fail()) return NULL;
    return realloc(memory, bytes);
}

static bool plan_reserve_text(MarkdownPlan *p, size_t need) {
    if (need <= p->text_capacity) return true;
    size_t capacity = p->text_capacity ? p->text_capacity : 256;
    while (capacity < need) {
        if (capacity > (SIZE_MAX / 2)) { capacity = need; break; }
        capacity *= 2;
    }
    wchar_t *grown = (wchar_t *)plan_realloc(p->text,
        capacity * sizeof(wchar_t));
    if (!grown) { p->failed = true; return false; }
    p->text = grown;
    p->text_capacity = capacity;
    return true;
}

static bool plan_append(MarkdownPlan *p, const wchar_t *source, size_t length) {
    if (p->failed) return false;
    if (length == 0) return true;
    /* Document-wide flatten cap: overflow renders the whole body verbatim. */
    if (length > (size_t)MD_MAX_FLATTEN_CHARS ||
        p->length > (size_t)MD_MAX_FLATTEN_CHARS - length) {
        p->overflow = true;
        p->failed = true;
        return false;
    }
    if (!plan_reserve_text(p, p->length + length + 1)) return false;
    memcpy(p->text + p->length, source, length * sizeof(wchar_t));
    p->length += length;
    p->text[p->length] = 0;
    return true;
}

static bool plan_append_run(MarkdownPlan *p, size_t offset, size_t length,
    unsigned style, int heading) {
    if (p->failed || length == 0) return true;
    /* Coalesce adjacent identical runs: synthesized tabs and line breaks then
       merge with the plain text they touch, while still covering every
       emitted code unit (TABLES_PLAN.md requirement 2). */
    if (p->run_count > 0) {
        MdRun *last = &p->runs[p->run_count - 1];
        if (last->offset + last->length == offset && last->style == style &&
            last->heading == heading) {
            last->length += length;
            return true;
        }
    }
    if (p->run_count == p->run_capacity) {
        int capacity = p->run_capacity ? p->run_capacity * 2 : 64;
        MdRun *grown = (MdRun *)plan_realloc(p->runs,
            (size_t)capacity * sizeof(MdRun));
        if (!grown) { p->failed = true; return false; }
        p->runs = grown;
        p->run_capacity = capacity;
    }
    MdRun *run = &p->runs[p->run_count++];
    run->offset = offset;
    run->length = length;
    run->style = style;
    run->heading = heading;
    return true;
}

static bool plan_append_paragraph(MarkdownPlan *p, size_t offset, size_t length,
    unsigned char first, unsigned char continuation, int format) {
    if (p->failed) return false;
    if (p->paragraph_count == p->paragraph_capacity) {
        int capacity = p->paragraph_capacity ? p->paragraph_capacity * 2 : 64;
        MdParagraph *grown = (MdParagraph *)plan_realloc(p->paragraphs,
            (size_t)capacity * sizeof(MdParagraph));
        if (!grown) { p->failed = true; return false; }
        p->paragraphs = grown;
        p->paragraph_capacity = capacity;
    }
    MdParagraph *paragraph = &p->paragraphs[p->paragraph_count++];
    paragraph->offset = offset;
    paragraph->length = length;
    paragraph->first_indent = first;
    paragraph->continuation_indent = continuation;
    paragraph->table_format = format;
    return true;
}

static bool plan_append_format(MarkdownPlan *p, const MdTableFormat *format,
    int *out_index) {
    if (p->failed) return false;
    if (p->table_format_count == p->table_format_capacity) {
        int capacity = p->table_format_capacity ? p->table_format_capacity * 2
            : 8;
        MdTableFormat *grown = (MdTableFormat *)plan_realloc(p->table_formats,
            (size_t)capacity * sizeof(MdTableFormat));
        if (!grown) { p->failed = true; return false; }
        p->table_formats = grown;
        p->table_format_capacity = capacity;
    }
    p->table_formats[p->table_format_count] = *format;
    *out_index = p->table_format_count;
    p->table_format_count++;
    return true;
}

static bool plan_append_code_block(MarkdownPlan *p, const MdCodeFence *fence) {
    if (p->failed) return false;
    if (p->code_block_count == p->code_block_capacity) {
        int capacity = p->code_block_capacity ? p->code_block_capacity * 2 : 8;
        MdCodeFence *grown = (MdCodeFence *)plan_realloc(p->code_blocks,
            (size_t)capacity * sizeof(MdCodeFence));
        if (!grown) { p->failed = true; return false; }
        p->code_blocks = grown;
        p->code_block_capacity = capacity;
    }
    p->code_blocks[p->code_block_count++] = *fence;
    return true;
}

static bool plan_append_link(MarkdownPlan *p, const MdLink *link) {
    if (p->failed) return false;
    if (p->link_count == p->link_capacity) {
        int capacity = p->link_capacity ? p->link_capacity * 2 : 8;
        MdLink *grown = (MdLink *)plan_realloc(p->links,
            (size_t)capacity * sizeof(MdLink));
        if (!grown) { p->failed = true; return false; }
        p->links = grown;
        p->link_capacity = capacity;
    }
    p->links[p->link_count++] = *link;
    return true;
}

static bool plan_append_segment(MarkdownPlan *p, size_t src, size_t length,
    size_t plan) {
    if (p->failed) return false;
    if (p->segment_count == p->segment_capacity) {
        int capacity = p->segment_capacity ? p->segment_capacity * 2 : 64;
        PlanSegment *grown = (PlanSegment *)plan_realloc(p->segments,
            (size_t)capacity * sizeof(PlanSegment));
        if (!grown) { p->failed = true; return false; }
        p->segments = grown;
        p->segment_capacity = capacity;
    }
    PlanSegment *segment = &p->segments[p->segment_count++];
    segment->src = src;
    segment->length = length;
    segment->plan = plan;
    return true;
}

static void plan_dispose(MarkdownPlan *p) {
    if (!p) return;
    free(p->text);
    free(p->runs);
    free(p->paragraphs);
    free(p->table_formats);
    free(p->links);
    free(p->code_blocks);
    free(p->segments);
    if (p->owns_targets) free(p->link_targets);
    memset(p, 0, sizeof *p);
}

/* ---- Metric adapter (TABLES_PLAN.md 5.2, I8) ------------------------------ */

typedef struct {
    wchar_t family[LF_FACESIZE];
    LONG twips;
    int weight;
    bool italic;
    HFONT font;
} MetricFont;

typedef struct {
    RichTextControl *control;
    HDC dc;
    int dpi;                            /* rounded device DPI */
    MetricFont fonts[32];
    int font_count;
    HFONT selected;                     /* font currently selected in `dc` */
    HGDIOBJ previous;                   /* the DC's own font, restored at end */
    bool metric_failed;
} MetricCtx;

static void metric_begin(MetricCtx *ctx, RichTextControl *control) {
    memset(ctx, 0, sizeof *ctx);
    ctx->control = control;
    int dpi = (int)(control->dpi + 0.5f);
    ctx->dpi = dpi > 0 ? dpi : 96;
    if (test_fail_metrics) {
        ctx->metric_failed = true;
        return;
    }
    /* GetDC is paired with ReleaseDC in metric_end on every exit path. */
    ctx->dc = GetDC(control->window);
    if (!ctx->dc) ctx->metric_failed = true;
}

static void metric_end(MetricCtx *ctx) {
    if (ctx->dc && ctx->selected)
        SelectObject(ctx->dc, ctx->previous);       /* deselect before delete */
    for (int i = 0; i < ctx->font_count; i++)
        if (ctx->fonts[i].font) DeleteObject(ctx->fonts[i].font);
    ctx->font_count = 0;
    if (ctx->dc && ctx->control) ReleaseDC(ctx->control->window, ctx->dc);
    ctx->dc = NULL;
}

/* Selects (creating lazily) the face matching the span's metric-relevant
   style. The cache is per plan, keyed by copied family, twips, weight and
   italic; theme_epoch is deliberately not a key. */
static HFONT metric_font(MetricCtx *ctx, const TableSpan *span) {
    const RichTextTheme *theme = &ctx->control->theme;
    bool mono = (span->style & MD_STYLE_MONO) != 0;
    const wchar_t *family = mono ? theme->mono_family : theme->ui_family;
    float dip = run_size(theme, span->style, span->heading);
    LONG twips = (LONG)(dip * 15.0f + 0.5f);
    int weight = (span->style & MD_STYLE_BOLD) ? 700 : 400;
    bool italic = (span->style & MD_STYLE_ITALIC) != 0;
    for (int i = 0; i < ctx->font_count; i++) {
        MetricFont *entry = &ctx->fonts[i];
        if (entry->twips == twips && entry->weight == weight &&
            entry->italic == italic && !wcscmp(entry->family, family))
            return entry->font;
    }
    if (ctx->font_count >= (int)(sizeof ctx->fonts / sizeof *ctx->fonts)) {
        ctx->metric_failed = true;
        return NULL;
    }
    LOGFONTW logfont;
    memset(&logfont, 0, sizeof logfont);
    logfont.lfHeight = -(LONG)MulDiv((int)twips, ctx->dpi, 1440);
    if (logfont.lfHeight == 0) logfont.lfHeight = -1;
    logfont.lfWeight = weight;
    logfont.lfItalic = italic ? TRUE : FALSE;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfQuality = DEFAULT_QUALITY;
    wcsncpy(logfont.lfFaceName, family, LF_FACESIZE - 1);
    HFONT font = CreateFontIndirectW(&logfont);
    if (!font) { ctx->metric_failed = true; return NULL; }
    MetricFont *entry = &ctx->fonts[ctx->font_count++];
    wcsncpy(entry->family, family, LF_FACESIZE - 1);
    entry->family[LF_FACESIZE - 1] = 0;
    entry->twips = twips;
    entry->weight = weight;
    entry->italic = italic;
    entry->font = font;
    return font;
}

/* TableSpanWidth: text-only width of one styled substring. Gutters are owned
   solely by table_layout, so nothing is added here. */
static int metric_span_width(void *user, const TableSpan *span) {
    MetricCtx *ctx = (MetricCtx *)user;
    if (!ctx || ctx->metric_failed || !ctx->dc || !span || span->length == 0)
        return 0;
    if (span->length > (size_t)INT_MAX) { ctx->metric_failed = true; return 0; }
    HFONT font = metric_font(ctx, span);
    if (!font) return 0;
    if (ctx->selected != font) {
        HGDIOBJ previous = SelectObject(ctx->dc, font);
        if (!previous || previous == HGDI_ERROR) {
            ctx->metric_failed = true;
            return 0;
        }
        if (!ctx->selected) ctx->previous = previous;   /* the DC's own font */
        ctx->selected = font;
    }
    SIZE size;
    memset(&size, 0, sizeof size);
    if (!GetTextExtentPoint32W(ctx->dc, span->text, (int)span->length, &size)) {
        ctx->metric_failed = true;
        return 0;
    }
    return (int)size.cx;
}

/* ---- Flatten emission ----------------------------------------------------- */

typedef struct {
    const MdDocument *document;
    MarkdownPlan *plan;
} PlanEmit;

/* First source run whose end lies past `src` (runs are ordered by offset).
   A per-slice lookup, not a forward-only cursor: table emission emits the
   wrapped lines of one row column by column, so source ranges are revisited
   out of order and a monotonic cursor would skip their styles. */
static int first_run_past(const MdDocument *document, size_t src) {
    int low = 0, high = document->run_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        const MdRun *run = &document->runs[mid];
        if (run->offset + run->length <= src) low = mid + 1;
        else high = mid;
    }
    return low;
}

/* Copies a source range into the plan and remaps every intersecting source-run
   slice, so plan formatting is deterministic over every emitted code unit.
   Gaps, the final tail, ordinary blocks and table cell slices all go through
   here. */
static void emit_source_span(PlanEmit *emit, size_t src, size_t length) {
    MarkdownPlan *p = emit->plan;
    const MdDocument *document = emit->document;
    if (p->failed || length == 0) return;
    size_t base = p->length;
    if (!plan_append(p, document->text + src, length)) return;
    if (!plan_append_segment(p, src, length, base)) return;
    size_t end = src + length;
    int i = first_run_past(document, src);
    size_t covered = 0;
    while (i < document->run_count) {
        const MdRun *run = &document->runs[i];
        if (run->offset >= end) break;
        size_t low = run->offset > src ? run->offset : src;
        size_t high = run->offset + run->length;
        if (high > end) high = end;
        if (low < high) {
            if (low > src + covered)
                plan_append_run(p, base + covered, low - (src + covered), 0, 0);
            plan_append_run(p, base + (low - src), high - low, run->style,
                run->heading);
            covered = high - src;
        }
        if (run->offset + run->length <= end) i++;
        else break;                         /* continues into the next span */
    }
    if (covered < length)
        plan_append_run(p, base + covered, length - covered, 0, 0);
}

/* Synthesized table characters (tabs and line breaks) carry an explicit plain
   run so no surrounding style can bleed onto them. */
static void emit_plain_char(PlanEmit *emit, wchar_t character) {
    MarkdownPlan *p = emit->plan;
    if (p->failed) return;
    size_t base = p->length;
    if (!plan_append(p, &character, 1)) return;
    plan_append_run(p, base, 1, 0, 0);
}

/* A failed table is rendered literally from its stored normalized source slice:
   plain runs and ordinary paragraphs with table_format = -1. */
static bool emit_table_literal(PlanEmit *emit, const MdTable *table) {
    MarkdownPlan *p = emit->plan;
    const wchar_t *source = emit->document->literals + table->literal_offset;
    size_t length = table->literal_length;
    size_t base = p->length;
    if (!plan_append(p, source, length)) return false;
    if (length) plan_append_run(p, base, length, 0, 0);
    size_t line = 0;
    for (;;) {
        size_t end = line;
        while (end < length && source[end] != L'\n') end++;
        if (!plan_append_paragraph(p, base + line, end - line, 0, 0, -1))
            return false;
        if (end >= length) break;
        line = end + 1;
    }
    return !p->failed;
}

/* Fills each cell's wrapped flat line ranges into one flat array, reusing the
   line counts already computed by the fit pass (no redundant wrapping). */
static bool table_cell_lines(const TableLayoutInput *input,
    const TableColumns *columns, TableSpanWidth measure, void *user,
    const int *counts, int *firsts, TableCellLine *lines, long total_entries) {
    int cell = 0;
    long cursor = 0;
    for (int r = 0; r < input->row_count; r++) {
        for (int c = 0; c < input->columns; c++, cell++) {
            int count = counts[cell];
            firsts[cell] = (int)cursor;
            if (cursor + count > total_entries) return false;
            int written = count > 0 ? table_layout_cell_breaks(input, r, c,
                columns->width[c], measure, user, lines + cursor, count) : 0;
            if (written != count) return false;
            cursor += count;
        }
    }
    return true;
}

/* Builds the TableLayoutInput overlays for one table from the document's
   run/cell/row metadata. All temporary arrays are plan allocations so the
   failure seam covers them. */
static bool build_table_input(const MdDocument *document, const MdTable *table,
    TableLayoutInput *input, TableLayoutRow **rows_out,
    TableLayoutCell **cells_out, TableSpan **spans_out) {
    int row_count = table->row_count;
    int cell_count = table->cell_count;
    if (row_count <= 0 || cell_count <= 0) return false;
    TableLayoutRow *rows = (TableLayoutRow *)plan_malloc(
        (size_t)row_count * sizeof(TableLayoutRow));
    TableLayoutCell *cells = (TableLayoutCell *)plan_malloc(
        (size_t)cell_count * sizeof(TableLayoutCell));
    size_t span_cap = (size_t)document->run_count + (size_t)cell_count + 1;
    TableSpan *spans = (TableSpan *)plan_malloc(span_cap * sizeof(TableSpan));
    if (!rows || !cells || !spans) {
        /* Free every partial allocation: a failed call must not leak. */
        free(rows);
        free(cells);
        free(spans);
        return false;
    }

    int span_count = 0;
    int cell = 0;
    int run_cursor = 0;                 /* runs are ordered: never rescan them */
    for (int r = 0; r < row_count; r++) {
        const MdTableRow *source_row = &document->rows[table->first_row + r];
        rows[r].first_cell = cell;
        rows[r].cell_count = table->columns;
        for (int c = 0; c < table->columns; c++) {
            cells[cell].first_span = span_count;
            if (c < source_row->cell_count) {
                const MdTableCell *source_cell =
                    &document->cells[source_row->first_cell + c];
                size_t from = source_cell->offset;
                size_t to = source_cell->offset + source_cell->length;
                while (run_cursor < document->run_count &&
                    document->runs[run_cursor].offset +
                        document->runs[run_cursor].length <= from)
                    run_cursor++;
                int ri = run_cursor;
                while (ri < document->run_count && from < to) {
                    const MdRun *run = &document->runs[ri];
                    size_t run_from = run->offset;
                    size_t run_to = run->offset + run->length;
                    if (run_from >= to) break;
                    size_t low = run_from > from ? run_from : from;
                    size_t high = run_to < to ? run_to : to;
                    if (low < high && (size_t)span_count < span_cap) {
                        TableSpan *span = &spans[span_count++];
                        span->text = document->text + low;
                        span->length = high - low;
                        unsigned style = run->style &
                            (MD_STYLE_MONO | MD_STYLE_BOLD | MD_STYLE_ITALIC);
                        /* Header spans are bold before measurement (I8). */
                        if (r == 0) style |= MD_STYLE_BOLD;
                        span->style = (unsigned char)style;
                        span->heading = (unsigned char)run->heading;
                    }
                    from = high;
                    if (run_to <= to) ri++;         /* fully consumed */
                    else break;                     /* spans this cell and on */
                }
                run_cursor = ri;
            }
            cells[cell].span_count = span_count - cells[cell].first_span;
            cell++;
        }
    }
    input->spans = spans;
    input->span_count = span_count;
    input->cells = cells;
    input->cell_count = cell_count;
    input->rows = rows;
    input->row_count = row_count;
    input->columns = table->columns;
    input->align = table->align;
    *rows_out = rows;
    *cells_out = cells;
    *spans_out = spans;
    return true;
}

/* Emits one table: resolves columns, wraps every cell, and writes physical
   lines with tab stops. Fit failure, table-wide line overflow or an
   out-of-range tab anchor renders only this table literally. Any allocation
   failure fails the whole plan (whole-body verbatim). */
static bool emit_table(PlanEmit *emit, MetricCtx *ctx, int control_width_px,
    const MdTable *table) {
    const MdDocument *document = emit->document;
    MarkdownPlan *p = emit->plan;
    /* Every row is at least one physical line, so a table already over the
       line cap can fall back to literal without measuring a single cell. */
    if (table->row_count > TABLE_MAX_PHYSICAL_LINES)
        return emit_table_literal(emit, table);
    TableLayoutInput input;
    TableLayoutRow *rows = NULL;
    TableLayoutCell *cells = NULL;
    TableSpan *spans = NULL;
    int *counts = NULL;
    int *firsts = NULL;
    TableCellLine *lines = NULL;
    bool fatal = false;                     /* whole-body fallback */
    bool literal = false;                   /* this table only */

    if (!build_table_input(document, table, &input, &rows, &cells, &spans)) {
        fatal = true;
        goto done;
    }
    counts = (int *)plan_malloc((size_t)input.cell_count * sizeof(int));
    firsts = (int *)plan_malloc((size_t)input.cell_count * sizeof(int));
    if (!counts || !firsts) { fatal = true; goto done; }

    int dpi = ctx->dpi > 0 ? ctx->dpi : 96;
    int minimum_px = MulDiv(TABLE_MIN_COLUMN_DIP, dpi, 96);
    int gutter_px = MulDiv(TABLE_GUTTER_DIP, dpi, 96);
    int margin = (int)(10.0f * ctx->control->dpi / 96.0f + 0.5f);
    int available = control_width_px - 2 * margin;
    if (minimum_px < 1) minimum_px = 1;
    if (gutter_px < 0) gutter_px = 0;
    if (available < 0) available = 0;

    TableColumns columns;
    if (!table_layout_columns(&input, available, minimum_px, gutter_px,
            metric_span_width, ctx, &columns))
        literal = true;

    long total_entries = 0;
    long physical = 0;
    if (!literal) {
        for (int r = 0; r < input.row_count; r++) {
            int row_lines = 1;
            for (int c = 0; c < input.columns; c++) {
                int index = r * input.columns + c;
                int count = table_layout_cell_breaks(&input, r, c,
                    columns.width[c], metric_span_width, ctx, NULL, 0);
                if (count < 0) { literal = true; break; }
                counts[index] = count;
                if (count > row_lines) row_lines = count;
                total_entries += count;
            }
            if (literal) break;
            physical += row_lines;
            /* Table-wide physical-line cap: only this table falls back (4.2). */
            if (physical > TABLE_MAX_PHYSICAL_LINES) { literal = true; break; }
        }
    }
    if (ctx->metric_failed) { fatal = true; goto done; }

    if (!literal && total_entries > 0) {
        lines = (TableCellLine *)plan_malloc((size_t)total_entries *
            sizeof(TableCellLine));
        if (!lines) { fatal = true; goto done; }
        if (!table_cell_lines(&input, &columns, metric_span_width, ctx,
                counts, firsts, lines, total_entries)) {
            if (ctx->metric_failed) { fatal = true; goto done; }
            literal = true;
        }
    }
    if (ctx->metric_failed) { fatal = true; goto done; }

    int format_index = -1;
    if (!literal) {
        MdTableFormat format;
        memset(&format, 0, sizeof format);
        format.column_count = table->columns;
        for (int c = 0; c < table->columns; c++) {
            format.align[c] = table->align[c];
            int anchor = table_layout_anchor(&columns, c, table->align[c]);
            LONG twip = (LONG)MulDiv(anchor, 1440, dpi);
            if (anchor < 0 || twip < 0 || twip > 0x00FFFFFFL) literal = true;
            format.x[c] = anchor;
        }
        if (!literal && !plan_append_format(p, &format, &format_index)) {
            fatal = true;
            goto done;
        }
    }
    if (literal) goto done;

    bool first_line = true;
    for (int r = 0; r < input.row_count && !p->failed; r++) {
        int index = r * input.columns;
        int row_lines = 1;
        for (int c = 0; c < input.columns; c++)
            if (counts[index + c] > row_lines) row_lines = counts[index + c];
        const MdTableRow *source_row = &document->rows[table->first_row + r];
        for (int l = 0; l < row_lines; l++) {
            if (!first_line) emit_plain_char(emit, L'\n');
            first_line = false;
            size_t paragraph_start = p->length;
            for (int c = 0; c < input.columns; c++) {
                unsigned char align = table->align[c];
                /* Tab rule (I6): before a cell iff column > 0 or the cell is
                   not left-aligned. Empty cells still emit their tab. */
                if (c > 0 || align != MD_ALIGN_LEFT)
                    emit_plain_char(emit, L'\t');
                int cell_index = r * input.columns + c;
                if (l >= counts[cell_index]) continue;
                const TableCellLine *line = &lines[firsts[cell_index] + l];
                if (line->end <= line->start) continue;
                int doc_cell = source_row->first_cell + c;
                if (c >= source_row->cell_count ||
                    doc_cell >= document->cell_count) continue;
                const MdTableCell *source_cell = &document->cells[doc_cell];
                emit_source_span(emit,
                    source_cell->offset + (size_t)line->start,
                    (size_t)(line->end - line->start));
            }
            if (!plan_append_paragraph(p, paragraph_start,
                    p->length - paragraph_start, 0, 0, format_index))
                break;
        }
    }
    if (p->failed) fatal = true;

done:
    free(cells);
    free(rows);
    free(spans);
    free(counts);
    free(firsts);
    free(lines);
    if (fatal) {
        p->failed = true;
        return false;
    }
    if (literal) return emit_table_literal(emit, table);
    return !p->failed;
}

/* Maps one document fence through the plan's segments. A multiline fence is
   emitted as one span per code paragraph plus one for each separator gap, so
   the source range must be covered completely and every mapped piece must be
   adjacent to the previous one; the pieces then coalesce into a single control
   range. Returns false when the coverage or adjacency invariant breaks (never
   expected: a table cannot occur inside a fence), letting the caller drop only
   this record. */
static bool map_fence(const MarkdownPlan *p, const MdCodeFence *fence,
    MdCodeFence *mapped) {
    size_t to = fence->offset + fence->length;
    size_t cursor = fence->offset;
    bool have = false;
    size_t plan_start = 0, plan_end = 0;
    for (int s = 0; s < p->segment_count && cursor < to; s++) {
        const PlanSegment *segment = &p->segments[s];
        size_t seg_from = segment->src;
        size_t seg_to = segment->src + segment->length;
        if (seg_to <= cursor) continue;
        if (seg_from > cursor) return false;            /* source gap */
        size_t high = to < seg_to ? to : seg_to;
        size_t piece_start = segment->plan + (cursor - seg_from);
        size_t piece_end = segment->plan + (high - seg_from);
        if (!have) {
            plan_start = piece_start;
            have = true;
        } else if (piece_start != plan_end) {
            return false;                               /* not adjacent */
        }
        plan_end = piece_end;
        cursor = high;
    }
    if (!have || cursor != to) return false;
    mapped->offset = plan_start;
    mapped->length = plan_end - plan_start;
    return true;
}

/* Builds the transactional flatten plan. Returns false for every
   document-wide failure (allocation, metric, character overflow): the caller
   then writes the entire original source verbatim. A per-table failure is
   handled inside emit_table and never fails the whole plan. */
static bool build_plan(RichTextControl *control, const MdDocument *document,
    int control_width_px, MarkdownPlan *plan) {
    memset(plan, 0, sizeof *plan);
    plan_alloc_index = 0;
    int dpi = (int)(control->dpi + 0.5f);
    plan->dpi = dpi > 0 ? dpi : 96;
    MetricCtx ctx;
    metric_begin(&ctx, control);

    PlanEmit emit;
    emit.document = document;
    emit.plan = plan;

    size_t cursor = 0;
    for (int i = 0; i < document->block_count && !plan->failed; i++) {
        const MdBlock *block = &document->blocks[i];
        if (block->offset > cursor)
            emit_source_span(&emit, cursor, block->offset - cursor);
        cursor = block->offset;
        if (block->kind == MD_BLOCK_TABLE && block->table_index >= 0 &&
            block->table_index < document->table_count) {
            if (!emit_table(&emit, &ctx, control_width_px,
                    &document->tables[block->table_index]))
                break;
        } else {
            size_t base = plan->length;
            emit_source_span(&emit, block->offset, block->length);
            plan_append_paragraph(plan, base, plan->length > base
                ? plan->length - base : 0, block->first_indent,
                block->continuation_indent, -1);
        }
        cursor += block->length;
    }
    if (!plan->failed && document->length > cursor)
        emit_source_span(&emit, cursor, document->length - cursor);

    /* Map every document link through the emitted segments; a link spanning a
       wrapped slice becomes one plan link per slice, sharing its target. */
    if (!plan->failed) {
        for (int i = 0; i < document->link_count && !plan->failed; i++) {
            const MdLink *link = &document->links[i];
            size_t from = link->offset;
            size_t to = link->offset + link->length;
            for (int s = 0; s < plan->segment_count && !plan->failed; s++) {
                const PlanSegment *segment = &plan->segments[s];
                size_t seg_from = segment->src;
                size_t seg_to = segment->src + segment->length;
                size_t low = from > seg_from ? from : seg_from;
                size_t high = to < seg_to ? to : seg_to;
                if (low >= high) continue;
                MdLink mapped;
                mapped.offset = segment->plan + (low - seg_from);
                mapped.length = high - low;
                mapped.target_offset = link->target_offset;
                mapped.target_length = link->target_length;
                plan_append_link(plan, &mapped);
            }
        }
    }

    /* Map every document fence through the emitted segments. The mapped range
       is in plan (control) coordinates; an unmappable fence is dropped, which
       leaves body rendering unaffected. */
    if (!plan->failed) {
        for (int i = 0; i < document->fence_count && !plan->failed; i++) {
            MdCodeFence mapped;
            if (map_fence(plan, &document->fences[i], &mapped))
                plan_append_code_block(plan, &mapped);
        }
    }

    if (ctx.metric_failed) plan->failed = true;
    metric_end(&ctx);
    return !plan->failed && !plan->overflow;
}

/* ---- Plan application ----------------------------------------------------- */

static void apply_plan_paragraphs(RichTextControl *control,
    const MarkdownPlan *plan) {
    for (int i = 0; i < plan->paragraph_count;) {
        const MdParagraph *first = &plan->paragraphs[i];
        int j = i + 1;
        while (j < plan->paragraph_count &&
            plan->paragraphs[j].first_indent == first->first_indent &&
            plan->paragraphs[j].continuation_indent ==
                first->continuation_indent &&
            plan->paragraphs[j].table_format == first->table_format) j++;
        const MdParagraph *last = &plan->paragraphs[j - 1];
        CHARRANGE range;
        range.cpMin = (LONG)first->offset;
        range.cpMax = (LONG)(last->offset + last->length);
        if (range.cpMax < range.cpMin) range.cpMax = range.cpMin;
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
        const MdTableFormat *format = first->table_format >= 0 &&
            first->table_format < plan->table_format_count
            ? &plan->table_formats[first->table_format] : NULL;
        apply_paragraph_tabs(control, first->first_indent,
            first->continuation_indent - first->first_indent, format,
            plan->dpi);
        i = j;
    }
}

static void take_plan_links(RichTextControl *control, MarkdownPlan *plan) {
    control->links = plan->links;
    control->link_count = plan->link_count;
    control->link_targets = plan->link_targets;
    control->link_targets_length = plan->link_targets_length;
    plan->links = NULL;
    plan->link_count = 0;
    plan->link_capacity = 0;
    plan->link_targets = NULL;
    plan->link_targets_length = 0;
    plan->owns_targets = false;
}

static void take_plan_code_blocks(RichTextControl *control, MarkdownPlan *plan) {
    control->code_blocks = plan->code_blocks;
    control->code_block_count = plan->code_block_count;
    plan->code_blocks = NULL;
    plan->code_block_count = 0;
    plan->code_block_capacity = 0;
}

/* Formats the plan text already inserted at the control's caret into the
   control's current selection. The insertion and its completeness check are
   the caller's job, so a partial insert is never formatted or linked. */
static void apply_plan_format(RichTextControl *control, MarkdownPlan *plan,
    COLORREF body) {
    const RichTextTheme *theme = &control->theme;
    CHARRANGE whole;
    whole.cpMin = 0;
    whole.cpMax = (LONG)plan->length;
    SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&whole);
    reset_paragraphs(control);
    apply_plan_paragraphs(control, plan);
    for (int i = 0; i < plan->run_count; i++) {
        const MdRun *run = &plan->runs[i];
        if (!run->length) continue;
        CHARRANGE range;
        range.cpMin = (LONG)run->offset;
        range.cpMax = (LONG)(run->offset + run->length);
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&range);
        apply_format(control, SCF_SELECTION, run->style,
            run_color(theme, run->style, body),
            run_size(theme, run->style, run->heading));
    }
    if (plan->link_count) {
        SendMessageW(control->window, EM_AUTOURLDETECT, FALSE, 0);
        for (int i = 0; i < plan->link_count; i++)
            apply_link(control, (LONG)plan->links[i].offset,
                (LONG)(plan->links[i].offset + plan->links[i].length));
        SendMessageW(control->window, EM_AUTOURLDETECT,
            (WPARAM)(AURL_ENABLEURL | AURL_NOINITIALSCAN), 0);
    }
    caret_end(control->window);
}

/* True when the control now holds exactly `expected` internal code units; the
   insertion succeeded whole. Any shortfall means the write is incomplete and
   must be replaced by the verbatim source. */
static bool inserted_whole(const RichTextControl *control, size_t expected) {
    return (size_t)text_length(control->window) == expected;
}

/* Terminal assistant body at an explicit control width. The width is asserted
   before content so the plan's usable-width arithmetic matches the surface the
   reader sees; the plan is built off-control and inserted only when complete,
   and the inserted length is verified before any formatting is applied.
   Returns false only when the control/window is unavailable or cannot be
   sized, or when the verbatim fallback itself is incomplete (a completed
   verbatim fallback still returns true). */
bool rich_text_set_markdown_width(RichTextControl *control, ChatRole role,
    const wchar_t *text, int control_width_px) {
    if (!control || !control->window) return false;
    if (control_width_px > 0) {
        RECT bounds;
        if (!GetWindowRect(control->window, &bounds)) return false;
        if (bounds.right - bounds.left != control_width_px) {
            if (!SetWindowPos(control->window, NULL, 0, 0, control_width_px,
                    bounds.bottom - bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
                return false;               /* width not assertable */
        }
    }
    const RichTextTheme *theme = &control->theme;
    COLORREF body = role == CHAT_ROLE_ERROR ? theme->error : theme->text;
    begin_write(control);
    MdDocument document;
    bool ok = true;
    if (!markdown_render(text, &document)) {
        /* Transactional parser failure: the whole body is written verbatim. */
        ok = write_literal(control, text, body);
    } else if (document.table_count == 0) {
        SendMessageW(control->window, EM_REPLACESEL, FALSE,
            (LPARAM)document.text);
        if (!inserted_whole(control, document.length)) {
            /* A partial insert is never formatted or linked: replace it with
               the complete original source. */
            begin_write(control);
            ok = write_literal(control, text, body);
        } else {
            apply_document(control, &document, body);
            take_links(control, &document);
            take_code_fences(control, &document);
        }
    } else {
        MarkdownPlan plan;
        if (!build_plan(control, &document, control_width_px, &plan)) {
            /* Plan/layout/allocation/metric failure: whole body verbatim,
               no links transferred. */
            plan_dispose(&plan);
            ok = write_literal(control, text, body);
        } else {
            if (plan.link_count > 0) {
                plan.link_targets = document.targets;
                plan.link_targets_length = document.targets_length;
                plan.owns_targets = true;
                document.targets = NULL;
                document.targets_length = 0;
                document.targets_capacity = 0;
            }
            SendMessageW(control->window, EM_REPLACESEL, FALSE,
                (LPARAM)(plan.text ? plan.text : L""));
            if (!inserted_whole(control, plan.length)) {
                begin_write(control);
                ok = write_literal(control, text, body);
            } else {
                apply_plan_format(control, &plan, body);
                take_plan_links(control, &plan);
                take_plan_code_blocks(control, &plan);
            }
            plan_dispose(&plan);
        }
    }
    markdown_dispose(&document);
    end_write(control);
    return ok;
}

/* Compatibility wrapper: renders at the control's current width. Production
   transcript paths use the explicit-width writer. */
void rich_text_set_markdown(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control || !control->window) return;
    RECT bounds;
    int width = GetWindowRect(control->window, &bounds)
        ? bounds.right - bounds.left : 0;
    rich_text_set_markdown_width(control, role, text, width);
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
    run(control, role_label(role), MD_STYLE_BOLD, role_color(theme, role));
    if (row && row[0]) {
        run(control, L"\n", 0, theme->text);
        run(control, row, 0, theme->muted);
    }
    end_write(control);
}

void rich_text_set_block(RichTextControl *control, ChatRole role,
    const wchar_t *text) {
    if (!control || !control->window) return;
    const RichTextTheme *theme = &control->theme;
    begin_write(control);
    run(control, role_label(role), MD_STYLE_BOLD, role_color(theme, role));
    if (text && text[0]) {
        run(control, L"\n", 0, theme->text);
        write_literal(control, text,
            role == CHAT_ROLE_ERROR ? theme->error : theme->text);
    }
    end_write(control);
}

void rich_text_append_body(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window || !text || !text[0]) return;
    /* Appending only extends the end, so existing label ranges stay valid and
       the transferred metadata is kept; the appended text starts no link. */
    HWND window = control->window;
    CHARRANGE selection;
    SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    caret_end(window);
    run(control, text, 0, control->theme.text);
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
        run_at(control, text, 0, control->theme.muted, control->theme.small_size);
    if (error && error[0]) {
        if (text && text[0])
            run_at(control, L"\n", 0, control->theme.text,
                control->theme.small_size);
        run_at(control, error, 0, control->theme.error, control->theme.small_size);
    }
    end_write(control);
}

void rich_text_set_reasoning(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window) return;
    clear_links(control);
    HWND window = control->window;
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(window, L"");
    caret_end(window);
    run(control, text ? text : L"", 0, control->theme.muted);
    SendMessageW(window, EM_SETREADONLY, control->readonly ? TRUE : FALSE, 0);
    control->has_content = text && text[0];
    rich_text_scroll_to_end(control);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

void rich_text_append_reasoning(RichTextControl *control, const wchar_t *text) {
    if (!control || !control->window || !text || !text[0]) return;
    /* Appending only extends the end, so existing metadata stays valid. */
    HWND window = control->window;
    CHARRANGE selection;
    SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
    bool pinned = rich_text_pinned(control);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    caret_end(window);
    run(control, text, 0, control->theme.muted);
    SendMessageW(window, EM_SETREADONLY, control->readonly ? TRUE : FALSE, 0);
    control->has_content = true;
    /* Follow the stream only while the reader stays pinned to the bottom;
       restore the reader's selection after scrolling so it survives appends. */
    if (pinned) rich_text_scroll_to_end(control);
    SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&selection);
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}
/* Resolves a notification range to a stored Markdown destination, or NULL when
   the control detected the link itself (bare URL) and the visible text is the
   destination. */
static const MdLink *link_at(const RichTextControl *control,
    const CHARRANGE *range) {
    for (int i = 0; i < control->link_count; i++) {
        const MdLink *link = &control->links[i];
        if ((LONG)link->offset == range->cpMin &&
            (LONG)(link->offset + link->length) == range->cpMax) return link;
    }
    return NULL;
}

bool rich_text_handle_notify(RichTextControl *control, LPARAM lparam) {
    NMHDR *header = (NMHDR *)lparam;
    if (header->code == EN_LINK) {
        ENLINK *link = (ENLINK *)lparam;
        if (link->msg == WM_LBUTTONUP) {
            const MdLink *stored = link_at(control, &link->chrg);
            if (stored) open_target(control, stored);
            else open_link(control->window, &link->chrg);
        }
        return true;
    }
    if (header->code == EN_VSCROLL) return true;
    return false;
}
