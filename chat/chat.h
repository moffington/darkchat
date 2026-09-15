#ifndef DARKCHAT_CHAT_H
#define DARKCHAT_CHAT_H

/* DarkChat conversation state. No Win32 or graphics dependencies.
   Bounded conversation state is owned by the UI thread; the storage and
   network modules serialize explicit snapshots, never raw struct memory. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include <stdint.h>

/* Up to 128 conversations; snapshot format 2 raised this bound from 16.
   The storage decode accepts both format versions and enforces this bound;
   older 16-conversation builds reject format 2 as unsupported. The
   downgrade contract is documented in docs/CHAT.md. */
#define CHAT_MAX_CONVERSATIONS 128
#define CHAT_MAX_MESSAGES 64
/* Every persisted identity (conversation and stable message ids) comes from
   one counter that the storage format encodes as an exact double. This is the
   shared allocation ceiling: exhausting it is treated as corruption on load
   and blocks further allocation at runtime. */
#define CHAT_MAX_ID 9007199254740000ULL
/* Fixed inline space covers prompts and ordinary replies without allocations.
   Streamed model output grows onto the heap when it exceeds this size. */
#define CHAT_MESSAGE_TEXT 16384
#define CHAT_REASONING_TEXT CHAT_MESSAGE_TEXT
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
    /* Duration of the visible reasoning for this turn in milliseconds, or -1
       when the turn supplied no reasoning. Persisted with the message. */
    double reasoning_ms;
} ChatGeneration;

typedef enum {
    CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_SYSTEM, CHAT_ROLE_ERROR
} ChatRole;

/* A borrowed role/text view of one message for a request payload: the shape a
   provider client consumes. It points into live Chat state and is never owned,
   copied into persistent state or persisted. */
typedef struct { ChatRole role; const wchar_t *text; } ChatRequestMessage;

typedef struct {
    ChatRole role;
    wchar_t text[CHAT_MESSAGE_TEXT];
    /* Optional streamed model reasoning for this turn; empty when the provider
       supplied none. Never fabricated locally. */
    wchar_t reasoning[CHAT_REASONING_TEXT];
    /* Oversized model output is stored here instead of reserving its worst-case
       size in every message slot. Access through chat_message_* helpers. */
    wchar_t *text_overflow, *reasoning_overflow;
    size_t text_length, reasoning_length;
    size_t text_capacity, reasoning_capacity;
    /* Stable message identity, allocated from the same persisted counter as
       conversation ids and saved in the snapshot: a message keeps its id
       across restarts, and retry/regenerate/edit-and-resend give the new
       assistant turn a fresh id. `revision` counts every observable change;
       `body_revision` counts answer-text changes only, so a metadata-only
       generation update does not invalidate the rendered body. The counters
       and the expansion flag below are per-session view bookkeeping and are
       never persisted. */
    uint64_t id, revision, body_revision;
    /* View state: whether this turn's reasoning viewport is expanded. Owned by
       the rendered turn; reset with the message, not persisted. */
    bool reasoning_open;
    int64_t created_at, modified_at;
    ChatGeneration generation;
} ChatMessage;

typedef struct {
    wchar_t title[CHAT_TITLE_TEXT];
    uint64_t id;
    int64_t created_at, modified_at;
    bool renamed;
    wchar_t draft[CHAT_MESSAGE_TEXT];
    /* Dynamic message storage. `messages` points to `message_capacity`
       slots and is NULL exactly when `message_capacity` is 0; the invariants

           0 <= message_count <= message_capacity <= CHAT_MAX_MESSAGES
           message_count > 0  =>  messages != NULL

       hold at all times. Only [0, message_count) holds live messages; slots in
       [message_count, message_capacity) are private backing storage that
       callers must never inspect as meaningful state, cache, serialize or
       dispose independently. A never-used conversation has messages == NULL,
       message_count == 0 and message_capacity == 0; an empty conversation may
       still retain allocated capacity for reuse (trimming never reallocs
       merely because message_count decreased; chat_clear releases the storage
       entirely, matching its contract to wipe message content). Growth
       doubles the capacity up to the CHAT_MAX_MESSAGES hard cap and is
       transactional: a failed growth leaves the original allocation valid.

       Ownership and pointer-lifetime rules:
       - Element pointers (&messages[i]) are not stable across operations that
         can grow the array: any growth realloc invalidates every element
         pointer of that conversation. Re-derive messages from a conversation
         pointer plus index or stable ID after every mutating call; never
         cache a ChatMessage * across chat_append*, chat_begin_response or a
         load. The transcript stores rendered identity by value and never
         owns live ChatMessage pointers; preserve that property.
       - A bytewise move of a whole ChatConversation (conversation deletion)
         transfers ownership of the messages allocation and every live
         message's overflow allocations; the vacated slot must be zeroed so
         the same pointers cannot be freed twice.
       - Never byte-copy a live ChatMessage that owns overflow pointers into
         another independently owned slot; the copies would alias the same
         allocations and free them twice.
       - Before a conversation's message storage is discarded or overwritten,
         dispose every live message in [0, message_count), then free the
         messages allocation and reset pointer/count/capacity (chat_dispose
         and the storage decode quarantine do this).
       - Chat and its message arrays are UI-thread-owned. Worker threads never
         borrow the array or pointers into it; asynchronous persistence work
         must use an immutable/deep snapshot instead. */
    ChatMessage *messages;
    size_t message_count;
    size_t message_capacity;
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
/* Deep, transactional copy for asynchronous persistence: the result is fully
   owned by the caller, shares no allocation with `chat`, and is safe to hand
   to a background writer while the UI thread keeps mutating the original.
   Overflow storage is reallocated per message (never aliased), so the copy
   can be disposed independently. On any allocation failure the partial copy
   is disposed and NULL is returned; the source is never modified. */
Chat *chat_snapshot(const Chat *chat);
void chat_generation_init(ChatGeneration *generation);
const wchar_t *chat_generation_name(ChatGenerationState state);
void chat_remember_model(Chat *chat);
bool chat_rename(Chat *chat, const wchar_t *title);
bool chat_delete(Chat *chat);
/* Removes every conversation and leaves one fresh empty conversation active. */
bool chat_delete_all(Chat *chat);
void chat_clear(Chat *chat);
/* Replacement actions remove only the latest user turn's response, never
   duplicate its user message. Edit replaces that user text at send time. */
typedef enum { CHAT_SEND, CHAT_RETRY, CHAT_REGENERATE, CHAT_EDIT_RESEND } ChatSendMode;
int chat_begin_response(Chat *chat, ChatSendMode mode, const wchar_t *prompt);
int chat_latest_user(const ChatConversation *conversation);
bool chat_history_message(const ChatMessage *message);
const wchar_t *chat_message_text(const ChatMessage *message);
const wchar_t *chat_message_reasoning(const ChatMessage *message);
bool chat_message_set_text(ChatMessage *message, const wchar_t *text);
bool chat_message_set_reasoning(ChatMessage *message, const wchar_t *text);
bool chat_message_append_text(ChatMessage *message, const wchar_t *text);
bool chat_message_append_reasoning(ChatMessage *message, const wchar_t *text);
/* Marks a message as observably changed for view bookkeeping after a direct
   mutation the setters do not cover (generation metadata written in place).
   Bumping must be limited to real changes; never call it speculatively. */
void chat_message_touch(ChatMessage *message);
void chat_message_dispose(ChatMessage *message);
void chat_dispose(Chat *chat);

void chat_init(Chat *chat);
/* Creates an empty conversation, selects it and returns its index, or -1. */
int chat_new_conversation(Chat *chat);
bool chat_select_conversation(Chat *chat, int index);
/* Index of the conversation with this stable id, or -1 when unknown. */
int chat_index_of_id(const Chat *chat, uint64_t id);
/* Appends a message to the active conversation. Returns its index, or -1. */
int chat_append(Chat *chat, ChatRole role, const wchar_t *text);
/* Appends a message to a specific conversation, whichever is active or not;
   late replies land in the conversation they were asked for. Returns the
   message index, or -1 when the conversation is unknown or full. The append
   is transactional: an allocation or construction failure leaves the
   conversation semantically unchanged, consumes no id and bumps no
   timestamp. */
int chat_append_at(Chat *chat, int conversation, ChatRole role,
    const wchar_t *text);
/* Reserves backing storage for `count` live messages without changing any
   live message or message_count. Returns false when the request exceeds the
   CHAT_MAX_MESSAGES hard limit or the transactional growth failed. Storage
   decode uses this before populating a decoded conversation. */
bool chat_reserve_messages(ChatConversation *conversation, size_t count);
/* Further messages the active conversation can still store. */
int chat_remaining(const Chat *chat);
const ChatConversation *chat_active(const Chat *chat);
/* Writes a locally-generated reply that responds to prompt into out. */
void chat_fake_reply(Chat *chat, const wchar_t *prompt, wchar_t *out, size_t capacity);

#endif
