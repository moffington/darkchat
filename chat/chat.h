#ifndef DARKCHAT_CHAT_H
#define DARKCHAT_CHAT_H

/* DarkChat conversation state. No Win32 or graphics dependencies.
   Fixed-capacity storage keeps the app branch allocation-free while the
   architecture is still moving; persistence arrives in a later pass. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define CHAT_MAX_CONVERSATIONS 16
#define CHAT_MAX_MESSAGES 64
#define CHAT_MESSAGE_TEXT 4096
#define CHAT_TITLE_TEXT 64
#define CHAT_MODEL_TEXT 96
#define CHAT_STATUS_TEXT 160

typedef enum {
    CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_SYSTEM, CHAT_ROLE_ERROR
} ChatRole;

typedef struct {
    ChatRole role;
    wchar_t text[CHAT_MESSAGE_TEXT];
} ChatMessage;

typedef struct {
    wchar_t title[CHAT_TITLE_TEXT];
    ChatMessage messages[CHAT_MAX_MESSAGES];
    int message_count;
} ChatConversation;

typedef struct {
    ChatConversation conversations[CHAT_MAX_CONVERSATIONS];
    int conversation_count;
    int active;
    wchar_t model[CHAT_MODEL_TEXT];
    wchar_t status[CHAT_STATUS_TEXT];
    unsigned replies;
} Chat;

void chat_init(Chat *chat);
/* Creates an empty conversation, selects it and returns its index, or -1. */
int chat_new_conversation(Chat *chat);
bool chat_select_conversation(Chat *chat, int index);
/* Appends a message to the active conversation. Returns its index, or -1. */
int chat_append(Chat *chat, ChatRole role, const wchar_t *text);
/* Appends a message to a specific conversation, whichever is active or not;
   late replies land in the conversation they were asked for. Returns the
   message index, or -1 when the conversation is unknown or full. */
int chat_append_at(Chat *chat, int conversation, ChatRole role,
    const wchar_t *text);
/* Further messages the active conversation can still store. */
int chat_remaining(const Chat *chat);
const ChatConversation *chat_active(const Chat *chat);
/* Writes a locally-generated reply that responds to prompt into out. */
void chat_fake_reply(Chat *chat, const wchar_t *prompt, wchar_t *out, size_t capacity);

#endif
