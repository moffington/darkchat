#ifndef DARKCHAT_CHAT_UI_H
#define DARKCHAT_CHAT_UI_H

/* Retained DarkUI chrome for DarkChat: toolbar, conversation sidebar, model
   field placeholder, transcript/composer placeholders, send button and status
   line. The native Rich Edit surfaces are positioned over the placeholders by
   the host, which queries their arranged rectangles here. */
#include "../ui/ui.h"
#include "chat.h"

typedef enum {
    CHAT_COMMAND_NONE,
    CHAT_COMMAND_SEND,
    CHAT_COMMAND_NEW_CONVERSATION,
    CHAT_COMMAND_SELECT
} ChatCommand;

typedef void (*ChatCommandFn)(void *user, ChatCommand command, int index);

typedef struct {
    Ui *ui;
    Chat *chat;
    UiId root, heading, model, search, search_status, new_conversation, list,
        transcript, composer, send, status;
    UiId conversations[CHAT_MAX_CONVERSATIONS];
    UiId spacer, sidebar;
    ChatCommandFn command;
    void *command_user;
} ChatUi;

bool chat_ui_init(ChatUi *chat_ui, Ui *ui, Chat *chat);
/* Reflects conversation count, selection, title and status into the tree. */
void chat_ui_sync(ChatUi *chat_ui);
/* Changes the primary action between Send, Stop, and the disabled stopping
   state without rebuilding the retained tree. */
void chat_ui_set_generation(ChatUi *chat_ui, bool generating, bool stopping);
void chat_ui_set_search_status(ChatUi *chat_ui, const wchar_t *text);
void chat_ui_resize(ChatUi *chat_ui, float width, float height);
void chat_ui_event(void *user, Ui *ui, UiEvent event);
/* Arranged rectangle for a laid-out node, or an empty rectangle. */
UiRect chat_ui_rect(const ChatUi *chat_ui, UiId id);

#endif
