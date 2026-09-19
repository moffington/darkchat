#ifndef DARKCHAT_RICH_TEXT_WIN32_H
#define DARKCHAT_RICH_TEXT_WIN32_H

/* Native text surfaces for the chat host, all built on the Windows Rich Edit
   control (Msftedit.dll / RICHEDIT50W). Windows owns selection, caret movement,
   clipboard, undo and IME; this module owns dark styling, role formatting,
   Markdown rendering for terminal assistant output, automatic URL detection
   and per-viewport auto-follow.

   Two read-only shapes exist so every transcript turn can own its own views
   instead of sharing one monolithic control: a block whose height tracks its
   content and has no scrollbar, and a viewport whose height the caller chooses
   and which scrolls itself (used for a single turn's reasoning). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ui/ui.h"
#include "chat/core/chat.h"
#include "chat/transcript/markdown.h"

/* Colors and type derived from the DarkUI theme. */
typedef struct {
    COLORREF background, text, muted;
    COLORREF header_user, header_assistant, header_system, error;
    COLORREF code_background, code_text, link, composer_background;
    /* Persistent tint behind an expanded reasoning viewport so it reads as a
       nested sub-panel rather than part of the answer. */
    COLORREF reasoning_background;
    const wchar_t *ui_family, *mono_family;
    float ui_size, mono_size, small_size; /* DIPs */
} RichTextTheme;

/* One native surface. The host positions the window; callbacks let the host
   intercept submit and navigation keys without subclassing. */
typedef struct RichTextControl RichTextControl;
struct RichTextControl {
    HWND window;
    WNDPROC previous;
    RichTextTheme theme;
    /* The surface's own background; Markdown code highlighting sets this
       explicitly on plain runs so the code tint cannot bleed past its runs. */
    COLORREF surface_background;
    float dpi;
    bool readonly, multiline, scrollable, has_content;
    /* Link metadata for the current Markdown body, transferred from the
       document that produced it and owned by the control: MdLink label ranges
       address control characters, and every destination lives in the single
       link_targets arena. Cleared on every content mutation and freed on
       WM_NCDESTROY. */
    MdLink *links;
    int link_count;
    wchar_t *link_targets;      /* arena: NUL-terminated destinations */
    size_t link_targets_length; /* wchar units in link_targets, NULs included */
    /* Rendered fenced-code-block metadata for the current Markdown body,
       transferred from the document that produced it and owned by the control:
       each entry is a half-open character range in the control's LF-only
       coordinate space, marker lines and the separator before the fence
       excluded. Cleared on every content mutation and freed on WM_NCDESTROY,
       exactly like the link metadata. Only the stable range is stored; the
       current display line for a position is derived from the control at the
       point of use, so wrapping, width, DPI and font changes cannot stale it. */
    MdCodeFence *code_blocks;
    int code_block_count;
    /* Enter without Shift. Return true to consume the key. */
    bool (*on_submit)(void *user);
    /* Down/up translation of any key. Return true to consume it. */
    bool (*on_key)(void *user, WPARAM key, bool shift, bool control, bool down);
    /* A line of a read-only block was clicked. Return true to consume the
       click; the host uses this for a turn's reasoning row. */
    bool (*on_line_click)(void *user, RichTextControl *control, int line,
        bool down);
    /* Called when the surface loses keyboard focus (e.g. to validate a field). */
    void (*on_blur)(void *user);
    void *user;
};

/* Load/unload Msftedit.dll once per process. */
bool rich_text_library_open(void);
void rich_text_library_close(void);

void rich_text_theme(RichTextTheme *theme, const UiTheme *ui);

/* Read-only multiline block whose height tracks its content (no scrollbar). */
bool rich_text_create_block(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi);
/* Read-only multiline viewport with its own vertical scrollbar, used for one
   turn's reasoning. */
bool rich_text_create_viewport(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi);
bool rich_text_create_composer(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi);
bool rich_text_create_field(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, const wchar_t *text);
bool rich_text_create_field_limit(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, long limit, const wchar_t *text);

void rich_text_set_dpi(RichTextControl *control, float dpi);
void rich_text_get_text(const RichTextControl *control, wchar_t *out,
    size_t capacity);
void rich_text_set_text(RichTextControl *control, const wchar_t *text);

/* Role header line, followed by an optional clickable reasoning row line. */
void rich_text_set_head(RichTextControl *control, ChatRole role,
    const wchar_t *row);
/* Role header and body together, for messages that have no reasoning row. */
void rich_text_set_block(RichTextControl *control, ChatRole role,
    const wchar_t *text);
/* Body text only, written verbatim (user, system and error turns, and
   explicit clears); no markdown interpretation, fence markers visible. */
void rich_text_set_body(RichTextControl *control, ChatRole role,
    const wchar_t *text);
/* Live answer text appended verbatim; the completed body is re-rendered. */
void rich_text_append_body(RichTextControl *control, const wchar_t *text);
/* Body text only, rendered as Markdown: headings 1-3, bold, italic,
   strikethrough, inline and fenced code, nested lists with unordered task
   markers, nested blockquotes, HTTP(S) links and GFM tables. Paragraph
   indentation and hanging indentation come from the parser's paragraph
   records and are cleared before every write, so a verbatim or failed body
   never inherits an indent. Tables flatten into physical lines that drive Rich
   Edit tab stops; when a table cannot fit, only that table falls back to
   literal source, and any document-wide plan failure writes the whole body
   verbatim. Falls back to verbatim text when parsing or allocation fails. */
void rich_text_set_markdown(RichTextControl *control, ChatRole role,
    const wchar_t *text);
/* Authoritative Markdown body write at an explicit control width. Asserts the
   HWND width before content, derives the usable width after scaled margins,
   builds and inserts a complete transactional flatten plan, and returns false
   only when the control/window is unavailable (a completed verbatim fallback
   still returns true). Production transcript paths use this. */
bool rich_text_set_markdown_width(RichTextControl *control, ChatRole role,
    const wchar_t *text, int control_width_px);
/* Compact terminal-state metadata footer (status, timings, tokens, cost,
   model), rendered in the small muted face. */
void rich_text_set_meta(RichTextControl *control, const wchar_t *text,
    const wchar_t *error);
/* Reasoning is rendered muted and never re-formatted. It auto-follows the
   stream unless the reader has scrolled that viewport away from the bottom. */
void rich_text_set_reasoning(RichTextControl *control, const wchar_t *text);
void rich_text_append_reasoning(RichTextControl *control, const wchar_t *text);

void rich_text_scroll_to_end(RichTextControl *control);
bool rich_text_pinned(const RichTextControl *control);
/* True when the surface currently holds a non-empty selection. */
bool rich_text_has_selection(const RichTextControl *control);

/* Rendered code-block metadata for the current body, valid from the last
   completed write and cleared whenever the link metadata is. Count is zero for
   bodies with no rendered fence, verbatim writes and failed renders. */
int rich_text_code_block_count(const RichTextControl *control);
/* Half-open control character range of code block `index`; false when the
   control is NULL or the index is out of range. Either output may be NULL when
   the caller needs only the other, so a valid index with both NULL still
   reports true. */
bool rich_text_code_block_range(const RichTextControl *control, int index,
    size_t *start, size_t *length);
/* Index of the code block whose range contains `cp`, or -1. Membership is
   half-open, so neither the separator newline between adjacent fences nor the
   character after a block belongs to it. */
int rich_text_code_block_at_char(const RichTextControl *control, size_t cp);
/* Handles EN_LINK (opens the target) and EN_VSCROLL. Returns true if consumed.
   lparam is the WM_NOTIFY lParam; the caller checks the source handle. */
bool rich_text_handle_notify(RichTextControl *control, LPARAM lparam);

/* Test-only hook: substitutes URL launching so the integration suite can drive
   EN_LINK without starting the shell. Pass NULL to restore shell launching. */
void rich_text_test_set_open(void (*open)(const wchar_t *url));

/* Test-only hook: makes the Nth (0-based) transaction-plan allocation and every
   allocation after it fail, so the whole-body fallback and recovery can be
   exercised through every arena. Pass a negative value to disable. */
void markdown_plan_test_fail_after(int allocation_index);

/* Test-only hook: while enabled the styled-span measurement adapter reports a
   failure, forcing the document-wide transactional fallback. */
void rich_text_test_fail_metrics(bool enable);

#endif
