#ifndef DARKCHAT_ACTIONS_H
#define DARKCHAT_ACTIONS_H
#include <stdint.h>
#include "chat/core/chat.h"
#include "chat/core/commands.h"
#include "chat/transcript/rich_text_win32.h"
#include <shellapi.h>

/* The tray icon's stable notification identifier. */
#define CHAT_TRAY_ICON_ID 1

/* Low-level seam around Shell_NotifyIconW. Kept in this translation unit so
   the host's tray helpers (a different translation unit) call it through an
   external reference that -Wl,--wrap can reliably intercept in the tests. */
bool chat_shell_notify(NOTIFYICONDATAW *data, DWORD operation);

/* Foreground-window query. Another dllimport seam wrapped behind a normal
   function so the notification trigger is deterministically testable. */
HWND chat_foreground_window(void);

/* Requests the window be brought to the foreground. Seamed for the same
   reason: a hidden test window must not drive the real foreground/IME path. */
bool chat_set_foreground(HWND window);

/* Whether the host thread's active input locale is an IME. When it is,
   Ctrl+Space is the OS/IME on/off toggle and the host must not consume it;
   the keyboard-layout query is wrapped so a hidden test is deterministic on
   any developer machine. */
bool chat_ime_active(void);

/* Tray lifecycle. Defined in the host translation unit (again, so calls to
   chat_shell_notify are cross-unit and wrap-reliable). show performs NIM_ADD
   and then NIM_SETVERSION(NOTIFYICON_VERSION_4), rolling back with NIM_DELETE
   when the version call fails; balloon performs NIM_MODIFY with NIF_INFO. */
bool chat_tray_show(HWND window, HICON icon, UINT callback_message,
    const wchar_t *tip);
bool chat_tray_hide(HWND window);
bool chat_tray_balloon(HWND window, const wchar_t *title, const wchar_t *text);

HMENU chat_actions_menu(const Chat *chat);
/* Applies the availability predicate and the live routing/backend check state
   to every command the menu carries. Absent items are ignored, so it is safe
   to call for every popup in WM_INITMENUPOPUP. */
void chat_actions_sync(HMENU menu, const ChatActionContext *context);
/* Refreshes the check/radio marks of the provider-routing and backend items in
   `menu` from live state, and grays out the OpenRouter-only routing controls
   while Ollama is active. A no-op for menus without those items. */
void chat_actions_sync_routing(HMENU menu, const Chat *chat);
bool chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text, size_t capacity, bool multiline);
bool chat_copy_text(HWND owner, const wchar_t *text);

/* Clipboard image seam for the composer's paste interception. Defined here
   (not in the Rich Edit module) so the host's calls are cross-unit and
   -Wl,--wrap can serve synthetic clipboard content in tests without touching
   the real clipboard. has_image reports whether an image paste should be
   intercepted (CF_DIBV5, CF_DIB or CF_BITMAP); read_dib copies the best
   available representation into a freshly allocated packed DIB
   (BITMAPINFOHEADER-based, the exact input image_ingest_dib accepts),
   converting a GDI CF_BITMAP through GetDIBits. The caller frees *out with
   free(); both functions zero the out parameters on failure. */
bool chat_clipboard_has_image(void);
bool chat_clipboard_read_dib(unsigned char **out, size_t *out_n);

/* Exact byte span of a packed CF_DIB/CF_DIBV5 payload (header, optional
   BITFIELDS masks, pixels), or false for anything image_ingest_dib would
   reject (layout, planes, formats, a header size past the defined DIB
   headers, dimensions past CHAT_ATTACHMENT_MAX_PIXELS, or pixel data that
   runs past the buffer). The clipboard reader runs this BEFORE allocating:
   the span it reports is the only copy the ingest needs, so an oversized or
   lying clipboard entry costs a header read and nothing more. Exposed for
   the reader's boundary tests. */
bool chat_clipboard_dib_span(const void *dib, size_t bytes, size_t *out_span);

/* Native file-dialog result: a dialog error is kept distinct from a user
   cancellation, so an export can stay silent on cancel but report a real
   failure. GetSaveFileNameW returns FALSE for both. */
typedef enum {
    CHAT_FILE_DIALOG_ACCEPTED,
    CHAT_FILE_DIALOG_CANCELLED,
    CHAT_FILE_DIALOG_ERROR
} ChatFileDialogResult;

/* Save-file dialog. `filter` is a double-NUL-terminated Win32 filter string
   (e.g. L"Markdown (*.md)\0*.md\0All files (*.*)\0*.*\0\0"); `default_name`
   seeds the file name and may be empty. On ACCEPTED `path` holds the chosen
   path. */
ChatFileDialogResult chat_save_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, const wchar_t *default_ext,
    const wchar_t *default_name, wchar_t *path, size_t capacity);

/* Open-file dialog. Same result contract as chat_save_dialog: a real dialog
    failure is distinct from a user cancellation. On ACCEPTED `path` holds the
    chosen existing file. */
ChatFileDialogResult chat_open_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, wchar_t *path, size_t capacity);

/* Native UTF-8 file read. A too-large file is kept distinct from an
    unreadable one and from an allocation failure, so a caller can report the
    right reason (and so the importer's own size classification stays
    reachable through the UI). */
typedef enum {
    CHAT_FILE_READ_OK,
    CHAT_FILE_READ_TOO_LARGE,
    CHAT_FILE_READ_OOM,
    CHAT_FILE_READ_IO_ERROR
} ChatFileReadResult;

/* Reads `path` as raw UTF-8 with an upper bound of `limit` bytes. On OK, `*data`
    owns a NUL-terminated buffer (release with free) and `*length` is its byte
    count; a leading UTF-8 BOM is stripped. On every failure `*data` is NULL and
    `*length` is 0. The bytes are not validated as UTF-8 here: the importer owns
    that check. */
ChatFileReadResult chat_read_file_utf8_limited(const wchar_t *path,
    size_t limit, char **data, size_t *length);
/* Whole-file BINARY read with the same size preflight and result mapping. No
   BOM is stripped and no UTF-8 is validated: the caller is reading opaque
   attachment bytes whose integrity is proved by the digest the export
   declares. On success `*data` owns a buffer (release with free) and `*length`
   is its exact byte count; on every failure `*data` is NULL and `*length` is
   0. */
ChatFileReadResult chat_read_file_bytes_limited(const wchar_t *path,
    size_t limit, unsigned char **data, size_t *length);
ChatFileReadResult chat_read_file_utf8(const wchar_t *path, char **data,
    size_t *length);

/* Writes `length` UTF-8 bytes to `path` atomically. A uniquely named
   same-directory temporary is created with CREATE_NEW, flushed with
   FlushFileBuffers, then moved over `path` with MOVEFILE_REPLACE_EXISTING |
   MOVEFILE_WRITE_THROUGH. On any failure only that owned temporary is
   deleted: no partial target is left and no unrelated file is touched.
   `length` must fit the Win32 DWORD byte count; larger values are rejected
   rather than truncated. Returns false on any failure. */
bool chat_write_file_utf8(const wchar_t *path, const char *data, size_t length);

/* Current time as signed Unix milliseconds, for export metadata. */
int64_t chat_export_timestamp(void);

/* Builds a filesystem-safe default file name from a conversation title and
   extension (e.g. L".md"): reserved characters become '_', trailing spaces
   and dots are trimmed, reserved DOS device basenames (CON, NUL, COM1...)
   are prefixed with '_', and an empty result falls back to "conversation".
   Always NUL-terminates within `capacity`. */
void chat_export_default_name(const wchar_t *title, const wchar_t *ext,
    wchar_t *out, size_t capacity);
#endif
