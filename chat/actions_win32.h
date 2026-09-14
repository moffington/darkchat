#ifndef DARKCHAT_ACTIONS_H
#define DARKCHAT_ACTIONS_H
#include "rich_text_win32.h"
enum {
    ACTION_NEW=100, ACTION_RENAME, ACTION_DELETE, ACTION_DELETE_ALL, ACTION_CLEAR,
    ACTION_RETRY, ACTION_REGENERATE, ACTION_EDIT, ACTION_COPY, ACTION_SELECTION,
    ACTION_SYSTEM, ACTION_SIDEBAR, ACTION_MODELS, ACTION_CANCEL_EDIT,
    ACTION_SEARCH
};
HMENU chat_actions_menu(void);
bool chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text, size_t capacity, bool multiline);
bool chat_copy_text(HWND owner, const wchar_t *text);
#endif
