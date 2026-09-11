#ifndef DARKCHAT_RICH_TEXT_WIN32_H
#define DARKCHAT_RICH_TEXT_WIN32_H

/* Native text surfaces for the chat host: a read-only transcript, a multiline
   composer and a single-line field, all built on the Windows Rich Edit control
   (Msftedit.dll / RICHEDIT50W). Windows owns selection, caret movement,
   clipboard, undo and IME; this module owns dark styling, role formatting,
   fenced-code blocks, automatic URL detection and transcript auto-scroll. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../ui/ui.h"
#include "chat.h"

/* Colors and type derived from the DarkUI theme. */
typedef struct {
    COLORREF background, text, muted;
    COLORREF header_user, header_assistant, header_system, error;
    COLORREF code_background, code_text, link, composer_background;
    const wchar_t *ui_family, *mono_family;
    float ui_size, mono_size; /* DIPs */
} RichTextTheme;

/* One native surface. The host positions the window; callbacks let the host
   intercept submit and navigation keys without subclassing. */
typedef struct RichTextControl RichTextControl;
struct RichTextControl {
    HWND window;
    WNDPROC previous;
    RichTextTheme theme;
    float dpi;
    bool readonly, multiline, has_content;
    /* Enter without Shift. Return true to consume the key. */
    bool (*on_submit)(void *user);
    /* Down/up translation of any key. Return true to consume it. */
    bool (*on_key)(void *user, WPARAM key, bool shift, bool control, bool down);
    /* Called when the surface loses keyboard focus (e.g. to validate a field). */
    void (*on_blur)(void *user);
    void *user;
};

/* Load/unload Msftedit.dll once per process. */
bool rich_text_library_open(void);
void rich_text_library_close(void);

void rich_text_theme(RichTextTheme *theme, const UiTheme *ui);

bool rich_text_create_transcript(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi);
bool rich_text_create_composer(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi);
bool rich_text_create_field(RichTextControl *control, HWND parent, int id,
    const RichTextTheme *theme, float dpi, const wchar_t *text);

void rich_text_set_dpi(RichTextControl *control, float dpi);
void rich_text_get_text(const RichTextControl *control, wchar_t *out,
    size_t capacity);
void rich_text_set_text(RichTextControl *control, const wchar_t *text);
void rich_text_select_all(RichTextControl *control);

void rich_text_clear(RichTextControl *control);
void rich_text_append_message(RichTextControl *control, ChatRole role,
    const wchar_t *text);
/* Lightweight live-response path. The completed transcript is re-rendered
   through append_message so fenced-code formatting is applied at the end. */
void rich_text_begin_stream(RichTextControl *control);
void rich_text_append_stream(RichTextControl *control, const wchar_t *text);
void rich_text_end_stream(RichTextControl *control);
void rich_text_scroll_to_end(RichTextControl *control);
bool rich_text_pinned(const RichTextControl *control);
/* Handles EN_LINK (opens the target) and EN_VSCROLL. Returns true if consumed.
   lparam is the WM_NOTIFY lParam; the caller checks the source handle. */
bool rich_text_handle_notify(RichTextControl *control, LPARAM lparam);

#endif
