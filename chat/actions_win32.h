#ifndef DARKCHAT_ACTIONS_H
#define DARKCHAT_ACTIONS_H
#include "chat.h"
#include "rich_text_win32.h"
enum {
    ACTION_NEW=100, ACTION_RENAME, ACTION_DELETE, ACTION_DELETE_ALL, ACTION_CLEAR,
    ACTION_RETRY, ACTION_REGENERATE, ACTION_EDIT, ACTION_COPY, ACTION_SELECTION,
    ACTION_SYSTEM, ACTION_SIDEBAR, ACTION_MODELS, ACTION_CANCEL_EDIT,
    ACTION_SEARCH,
    ACTION_ROUTING_SORT_DEFAULT, ACTION_ROUTING_SORT_PRICE,
    ACTION_ROUTING_SORT_THROUGHPUT, ACTION_ROUTING_SORT_LATENCY,
    ACTION_ROUTING_ALLOW_FALLBACKS, ACTION_ROUTING_DATA_COLLECTION,
    ACTION_ROUTING_ZDR
};
HMENU chat_actions_menu(const Chat *chat);
/* Refreshes the check/radio marks of the provider-routing items in `menu`
   from live routing state. A no-op for menus without those items, so it is
   safe to call for every popup in WM_INITMENUPOPUP. */
void chat_actions_sync_routing(HMENU menu, const Chat *chat);
bool chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text, size_t capacity, bool multiline);
bool chat_copy_text(HWND owner, const wchar_t *text);
#endif
