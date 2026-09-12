#ifndef DARKCHAT_CHAT_H
#define DARKCHAT_CHAT_H

/* DarkChat conversation state. No Win32 or graphics dependencies.
   Bounded conversation state is owned by the UI thread; the storage and
   network modules serialize explicit snapshots, never raw struct memory. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include <stdint.h>

#define CHAT_MAX_CONVERSATIONS 16
#define CHAT_MAX_MESSAGES 64
#define CHAT_MESSAGE_TEXT 4096
#define CHAT_TITLE_TEXT 64
#define CHAT_MODEL_TEXT 96
#define CHAT_STATUS_TEXT 160
#define CHAT_MODEL_HISTORY 16

typedef enum {
    CHAT_GENERATION_NONE, CHAT_GENERATION_RUNNING, CHAT_GENERATION_COMPLETE,
    CHAT_GENERATION_CANCELLED, CHAT_GENERATION_INTERRUPTED, CHAT_GENERATION_FAILED
} ChatGenerationState;

typedef struct {
    ChatGenerationState state;
    wchar_t requested_model[CHAT_MODEL_TEXT], actual_model[CHAT_MODEL_TEXT];
    wchar_t finish_reason[64], error[512];
    int64_t started_at, finished_at, first_token_at;
    /* -1 means unavailable, including usage/cost after cancellation. */
    double ttft_ms, latency_ms, prompt_tokens, completion_tokens, total_tokens, cost;
} ChatGeneration;

typedef enum {
    CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_SYSTEM, CHAT_ROLE_ERROR
} ChatRole;

typedef struct {
    ChatRole role;
    wchar_t text[CHAT_MESSAGE_TEXT];
    int64_t created_at, modified_at;
    ChatGeneration generation;
} ChatMessage;

typedef struct {
    wchar_t title[CHAT_TITLE_TEXT];
    uint64_t id;
    int64_t created_at, modified_at;
    bool renamed;
    wchar_t draft[CHAT_MESSAGE_TEXT];
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
    uint64_t next_id;
    wchar_t system_prompt[CHAT_MESSAGE_TEXT];
    wchar_t model_history[CHAT_MODEL_HISTORY][CHAT_MODEL_TEXT];
    int model_history_count;
    int window_x, window_y, window_width, window_height, maximized, sidebar_width;
} Chat;

int64_t chat_now(void);
void chat_generation_init(ChatGeneration *generation);
const wchar_t *chat_generation_name(ChatGenerationState state);
void chat_remember_model(Chat *chat);
bool chat_rename(Chat *chat, const wchar_t *title);
bool chat_delete(Chat *chat);
void chat_clear(Chat *chat);
/* Replacement actions remove only the latest user turn's response, never
   duplicate its user message. Edit replaces that user text at send time. */
typedef enum { CHAT_SEND, CHAT_RETRY, CHAT_REGENERATE, CHAT_EDIT_RESEND } ChatSendMode;
int chat_begin_response(Chat *chat, ChatSendMode mode, const wchar_t *prompt);
int chat_latest_user(const ChatConversation *conversation);
bool chat_history_message(const ChatMessage *message);

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
