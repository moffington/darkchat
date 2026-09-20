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
    Snapshot format 3 raised the per-conversation message bound from 64 to 512.
    The storage decode accepts formats 1-4 and enforces these bounds; older
    builds reject newer snapshots as unsupported. The downgrade contract is
    documented in docs/CHAT.md. */
#define CHAT_MAX_CONVERSATIONS 128
#define CHAT_MAX_MESSAGES 512
/* Every persisted identity (conversation and stable message ids) comes from
   one counter that the storage format encodes as an exact double. This is the
   shared allocation ceiling: exhausting it is treated as corruption on load
   and blocks further allocation at runtime. */
#define CHAT_MAX_ID 9007199254740000ULL
/* Composer input, per-conversation drafts and the system prompt keep the
   16,383-code-unit product limit (the composer enforces one less through
   EM_LIMITTEXT). These buffers are never message storage. */
#define CHAT_COMPOSER_TEXT 16384
/* A message keeps only a small inline residue: at most CHAT_MESSAGE_INLINE - 1
   stored code units plus the terminator. Growth past it promotes to the
   heap-backed overflow representation below, so the fixed per-message cost
   stays small and streamed output pays only for the text it actually holds. */
#define CHAT_MESSAGE_INLINE 256
#define CHAT_REASONING_INLINE CHAT_MESSAGE_INLINE
#define CHAT_TITLE_TEXT 64
#define CHAT_MODEL_TEXT 96
#define CHAT_STATUS_TEXT 160
#define CHAT_MODEL_HISTORY 16
/* Prompt profiles: a bounded inline library of named system prompts. A name
   holds at most CHAT_PROFILE_NAME_TEXT - 1 stored UTF-16 code units plus the
   terminator (63 stored units). The prompt keeps the CHAT_COMPOSER_TEXT
   product limit and is heap-backed through ChatText, so an unused profile
   costs only its inline name slot; the array itself is inline so structural
   copies stay proportional to the shipped bound, not to content. */
#define CHAT_MAX_PROMPT_PROFILES 24
#define CHAT_PROFILE_NAME_TEXT 64
/* Shared composition metric: the centered transcript and composer columns
   never exceed this width in DIPs. Kept here so the DarkUI chrome and the
   native transcript container agree on the readable measure. */
#define CHAT_CONTENT_WIDTH_DIPS 780.0f

/* The active provider. OpenRouter is the historical default and keeps the
   zero value, so an existing snapshot that has no backend field and a zeroed
   or freshly initialized Chat both mean OpenRouter. Ollama is first-class:
   its OpenAI-compatible endpoints are used, never the native /api/chat. */
typedef enum {
    CHAT_BACKEND_OPENROUTER = 0,
    CHAT_BACKEND_OLLAMA = 1
} ChatBackend;

#define CHAT_BACKEND_COUNT 2

typedef enum {
    CHAT_GENERATION_NONE, CHAT_GENERATION_RUNNING, CHAT_GENERATION_COMPLETE,
    CHAT_GENERATION_CANCELLED, CHAT_GENERATION_INTERRUPTED, CHAT_GENERATION_FAILED
} ChatGenerationState;

typedef struct {
    ChatGenerationState state;
    /* Which backend produced this turn. Persisted with the message; an older
       snapshot without the field decodes as OpenRouter. */
    ChatBackend backend;
    wchar_t requested_model[CHAT_MODEL_TEXT], actual_model[CHAT_MODEL_TEXT];
    wchar_t finish_reason[64], error[512];
    int64_t started_at, finished_at, first_token_at;
    /* -1 means unavailable, including usage/cost after cancellation. */
    double ttft_ms, latency_ms, prompt_tokens, completion_tokens, total_tokens, cost;
    /* Duration of the visible reasoning for this turn in milliseconds, or -1
       when the turn supplied no reasoning. Persisted with the message. */
    double reasoning_ms;
} ChatGeneration;

/* Heap-backed owned text for the few customization strings that must not
    inflate every structural copy: per-conversation system-prompt overrides
    and prompt-profile prompts. `data` is NULL exactly when the text is
    unset, which reads as the empty string; setting an empty value therefore
    disposes back to unset. A non-NULL data always owns exactly
    (length + 1) wide characters. The set and clone operations are
    transactional: an allocation failure leaves the previous content
    untouched, and clone builds its copy before releasing the destination,
    so a failed clone can never strand the destination empty. */
typedef struct {
    wchar_t *data;
    size_t length, capacity;
} ChatText;

bool chat_text_set(ChatText *text, const wchar_t *value);
/* Overwrites `destination` with an owned copy of `source`; on failure the
   destination keeps its previous content. Cloning an unset source leaves the
   destination unset. */
bool chat_text_clone(ChatText *destination, const ChatText *source);
void chat_text_dispose(ChatText *text);
/* Borrowed view, never NULL; L"" for unset text. */
const wchar_t *chat_text_value(const ChatText *text);

/* One named system prompt: the prompt is owned heap text (unset or empty
   means "apply no system prompt"); the name is inline and holds at most
   CHAT_PROFILE_NAME_TEXT - 1 stored code units. */
typedef struct {
    wchar_t name[CHAT_PROFILE_NAME_TEXT];
    ChatText prompt;
} ChatPromptProfile;

typedef enum {
    CHAT_ROLE_USER, CHAT_ROLE_ASSISTANT, CHAT_ROLE_SYSTEM, CHAT_ROLE_ERROR
} ChatRole;

/* A borrowed role/text view of one message for a request payload: the shape a
   provider client consumes. It points into live Chat state and is never owned,
   copied into persistent state or persisted. */
typedef struct { ChatRole role; const wchar_t *text; } ChatRequestMessage;

typedef enum {
    CHAT_PROVIDER_SORT_DEFAULT = 0,   /* OpenRouter's balanced load balancing */
    CHAT_PROVIDER_SORT_PRICE,
    CHAT_PROVIDER_SORT_THROUGHPUT,
    CHAT_PROVIDER_SORT_LATENCY
} ChatProviderSort;

typedef enum {
    CHAT_DATA_COLLECTION_ALLOW = 0,   /* OpenRouter's default */
    CHAT_DATA_COLLECTION_DENY
} ChatDataCollection;

/* Global OpenRouter provider-routing preferences, applied to every request
   unless a control is left at its OpenRouter default. The all-zero value is
   exactly OpenRouter's default routing (balanced load balancing, fallbacks
   allowed, data collection allowed, no request-level ZDR requirement), so a
   zeroed or freshly initialized Chat sends no `provider` object at all and
   OpenRouter defaults are preserved. `disallow_fallbacks` is stored inverted
   so that zero means the OpenRouter default `allow_fallbacks: true`. */
typedef struct {
    ChatProviderSort sort;
    bool disallow_fallbacks;
    ChatDataCollection data_collection;
    bool zdr;
} ChatProviderRouting;

typedef struct {
    ChatRole role;
    /* Inline residue; growth past it promotes to text_overflow below. */
    wchar_t text[CHAT_MESSAGE_INLINE];
    /* Optional streamed model reasoning for this turn; empty when the provider
       supplied none. Never fabricated locally. */
    wchar_t reasoning[CHAT_REASONING_INLINE];
    /* Heap-backed promotion of the text/reasoning residue. Access through
       chat_message_* helpers. */
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
    wchar_t draft[CHAT_COMPOSER_TEXT];
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
    /* Per-conversation customization, persisted from format 4 (empty or
       NULL = inherit the global slot). The model overrides are inline —
       empty means inherit, and models are bounded by CHAT_MODEL_TEXT. The
       system-prompt override is heap-backed ChatText: NULL inherits the
       global prompt. Like title/renamed, an override survives Clear
       (chat_clear) and is released only when the conversation itself is
       deleted or the whole Chat is disposed. Ownership follows the same
       bytewise-move rules as the messages allocation: deleting a
       conversation disposes its own override before the move, and a moved
       conversation transfers its override pointer to the vacated-and-zeroed
       scheme exactly once. */
    wchar_t model[CHAT_MODEL_TEXT];
    wchar_t ollama_model[CHAT_MODEL_TEXT];
    ChatText system_prompt;
} ChatConversation;

typedef struct {
    ChatConversation conversations[CHAT_MAX_CONVERSATIONS];
    int conversation_count;
    int active;
    /* Active provider and one remembered last-used model per backend. `model`
       is OpenRouter's slot; `ollama_model` is Ollama's. chat_active_model()
       resolves the slot the active backend sends. An older snapshot without
       the backend field decodes as OpenRouter and keeps its `model`. */
    ChatBackend backend;
    wchar_t model[CHAT_MODEL_TEXT];
    wchar_t ollama_model[CHAT_MODEL_TEXT];
    wchar_t status[CHAT_STATUS_TEXT];
    unsigned replies;
    uint64_t next_id;
    wchar_t system_prompt[CHAT_COMPOSER_TEXT];
    ChatProviderRouting provider_routing;
    wchar_t model_history[CHAT_MODEL_HISTORY][CHAT_MODEL_TEXT];
    /* Backend tag for each history entry, parallel to model_history, so one
       backend's recent models never appear in another backend's picker. An
       older snapshot without the tag decodes every entry as OpenRouter. */
    ChatBackend model_history_backend[CHAT_MODEL_HISTORY];
    int model_history_count;
    /* Named system-prompt library (format 4). Only [0, profile_count) is
       live; the tail slots are zeroed backing storage. Profile order is
       meaningful (save order and menu order) and `prompt` is owned heap
       text released by chat_dispose exactly once. Names are unique up to
       ordinal case-insensitive comparison. Profiles are library data:
       chat_delete_all preserves them, only chat_dispose releases them. */
    ChatPromptProfile profiles[CHAT_MAX_PROMPT_PROFILES];
    int profile_count;
    int window_x, window_y, window_width, window_height, maximized, sidebar_width;
    /* Explicit user preference for the collapsible conversation sidebar:
       0 = expanded (the historical default and the meaning of an absent
       snapshot field), 1 = collapsed. The temporary narrow-width drawer is
       session state and is never persisted. Additive at snapshot format 3. */
    int sidebar_collapsed;
} Chat;

int64_t chat_now(void);
/* Display name for a backend: L"OpenRouter" or L"Ollama". */
const wchar_t *chat_backend_name(ChatBackend backend);
/* The remembered last-used model of the active backend. Borrowed, never NULL
   (an unset Ollama slot is the empty string). */
const wchar_t *chat_active_model(const Chat *chat);
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

/* Prompt-profile library. Names are unique up to locale-independent ordinal
   case-insensitive comparison (the comparison the profile picker reuses), so
   adding or renaming to a name that differs from an existing profile only by
   case fails. A name is required, non-empty and bounded by
   CHAT_PROFILE_NAME_TEXT - 1 stored code units; the prompt may be empty
   (clears to unset) and is bounded by CHAT_COMPOSER_TEXT. Setters are
   transactional: on failure neither the profile list nor the edited profile
   changes. */
int chat_profile_count(const Chat *chat);
const ChatPromptProfile *chat_profile(const Chat *chat, int index);
/* Appends a profile and returns its index, or -1 at the cap, on a duplicate
   name, an invalid name or an allocation failure. */
int chat_profile_add(Chat *chat, const wchar_t *name, const wchar_t *prompt);
/* Replaces the name and prompt of `index` after both new values validate;
   a failure changes nothing. A rename colliding with a different profile's
   name (case-insensitively) fails. */
bool chat_profile_set(Chat *chat, int index, const wchar_t *name,
    const wchar_t *prompt);
/* Removes a profile, shifting the survivors down; false when the index is
   out of range. The removed profile's owned prompt is disposed exactly
   once and the vacated tail slot is zeroed. */
bool chat_profile_remove(Chat *chat, int index);

/* Per-conversation customization setters. An empty or NULL value clears the
   override back to inherit (the inline model arrays become empty, the
   ChatText becomes unset); a value is validated against the same bounds the
   storage format enforces. The setters never touch anything else. */
bool chat_conversation_set_system_prompt(Chat *chat, int conversation,
    const wchar_t *text);
bool chat_conversation_set_model(Chat *chat, int conversation,
    ChatBackend backend, const wchar_t *text);

void chat_init(Chat *chat);
/* Creates an empty conversation, selects it and returns its index, or -1. */
int chat_new_conversation(Chat *chat);
bool chat_select_conversation(Chat *chat, int index);
/* Index of the conversation with this stable id, or -1 when unknown. */
int chat_index_of_id(const Chat *chat, uint64_t id);
/* Index of the message with this stable id inside one conversation, or -1.
   Pure stable-identity resolution for transcript anchoring and reveal; ids
   are unique within a conversation and ordered by array position. */
int chat_message_index_by_id(const Chat *chat, int conversation,
    uint64_t id);
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
