#ifndef DARKCHAT_ACTIONS_H
#define DARKCHAT_ACTIONS_H
#include "chat.h"
#include "commands.h"
#include "rich_text_win32.h"
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
#endif
