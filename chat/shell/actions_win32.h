#ifndef DARKCHAT_ACTIONS_H
#define DARKCHAT_ACTIONS_H
#include <stdint.h>
#include "chat/core/chat.h"
#include "chat/core/commands.h"
#include "chat/transcript/rich_text_win32.h"
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
