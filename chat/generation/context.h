#ifndef DARKCHAT_CONTEXT_H
#define DARKCHAT_CONTEXT_H

/* Bounded OpenRouter request context. Pure C: no Win32 and no allocation.
   Persisted conversation history and the payload sent to a provider are
   different things. History keeps everything; a request keeps the system
   prompt, the triggering user message and the newest eligible history messages
   that fit an explicit byte budget. Entries borrow text from live Chat state;
   nothing here owns, copies, mutates or persists anything. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "chat/core/chat.h"
#include "chat/json.h"

/* Conservative request-body budget. This is a deliberately small product
   policy: it keeps a request well inside ordinary provider context windows
   without fetching a model's real context length or counting tokens, and it is
   not a token-window guarantee (a provider may still reject a request that
   fits, and DarkChat cannot know a model's window). */
#define CHAT_CONTEXT_BUDGET_BYTES (64u * 1024u)

/* An optional system prompt, every kept history message and the triggering
   user message. */
#define CHAT_CONTEXT_MAX_ENTRIES (CHAT_MAX_MESSAGES + 1)

typedef enum {
    /* A bounded context was built. */
    CHAT_CONTEXT_OK,
    /* Unusable arguments: nothing was built. */
    CHAT_CONTEXT_INVALID,
    /* The system prompt alone cannot fit the budget. */
    CHAT_CONTEXT_OVERSIZE_SYSTEM,
    /* The triggering user message alone cannot fit the budget. */
    CHAT_CONTEXT_OVERSIZE_USER,
    /* Each fits alone; the two indispensable messages together do not. */
    CHAT_CONTEXT_OVERSIZE_COMBINED
} ChatContextResult;

typedef struct {
    /* Borrowed views in request order: the system prompt when set, then the
       oldest kept history message through the newest, then the triggering
       user message. */
    ChatRequestMessage messages[CHAT_CONTEXT_MAX_ENTRIES];
    int count;
    /* Exact encoded size of the request body these entries produce. On OK this
       is at most the budget. */
    size_t bytes;
    /* Conversation index of the oldest kept history message, or -1 when the
       budget kept no history. */
    int first_kept_index;
    /* Eligible history messages dropped, always the oldest ones. */
    int dropped_messages;
    /* Every OVERSIZE result: the complete encoded body size the indispensable
       content (system prompt plus triggering message, with separators) would
       require, which is greater than the budget. */
    size_t required_bytes;
} ChatRequestContext;

/* Builds the bounded request context for conversation `c` as of the triggering
   user message at `user_index`, which must be a CHAT_ROLE_USER message (the
   host passes the pending response index minus one). The system prompt is kept
   whenever it is non-empty, the triggering user message is always kept, and
   eligible history messages are kept newest first while they fit `budget`
   bytes of encoded body. Stops at the first history message that does not fit
   and only counts the older eligible ones, so a dropped huge history never has
   its text measured.

   Eligible history is exactly chat_history_message(): no error notes, no local
   welcome text and no assistant message that did not finish successfully.
   Eligible messages are used whole — including empty ones — and are never
   truncated; only the oldest are dropped.

   Read-only, allocation-free and deterministic. Entries borrow from live Chat
   state and stay valid only until the conversation or the system prompt
   changes, so the caller must hand them to the client (which copies them)
   before any mutation. `out` is zeroed (first_kept_index -1, every other field
   0) before every INVALID return, so a rejected build never leaves a caller
   reading stale or poisoned fields. `budget` must be nonzero; a zero budget is
   invalid arguments rather than an oversize result. On OVERSIZE nothing is
   built and `required_bytes` describes the complete body that would have been
   needed. On OK, `bytes` is the exact encoded body size, derived from the
   budget remainder rather than a second sum, so it always agrees with the
   selection and can never exceed the budget. */
ChatContextResult chat_context_build(const Chat *chat, const ChatConversation *c,
    int user_index, size_t budget, ChatRequestContext *out);

#endif
