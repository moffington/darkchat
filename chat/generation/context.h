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
   fits, and DarkChat cannot know a model's window). It bounds the TEXT side of
   the body (envelope, message framing, text terms and separators); image
   payloads are bounded separately below. */
#define CHAT_CONTEXT_BUDGET_BYTES (64u * 1024u)

/* Encoded image-payload budget, independent of the text policy. A 200 KB
   JPEG is ~267 KB of base64: charging it to the 64 KiB text budget would make
   every image-bearing turn oversize, and inflating the text policy to fit
   images would destroy the conservative history dropping above. Measured in
   encoded-body bytes like everything else; charged from the attachment
   record's stored length alone (no blob is ever read to budget a request). */
#define CHAT_ATTACHMENT_BUDGET_BYTES (8u * 1024u * 1024u)

/* An optional system prompt, every kept history message and the triggering
   user message. */
#define CHAT_CONTEXT_MAX_ENTRIES (CHAT_MAX_MESSAGES + 1)

typedef enum {
    /* A bounded context was built. */
    CHAT_CONTEXT_OK,
    /* Unusable arguments, or state no request can trust: nothing was built. */
    CHAT_CONTEXT_INVALID,
    /* The system prompt alone cannot fit the budget. */
    CHAT_CONTEXT_OVERSIZE_SYSTEM,
    /* The triggering user message alone cannot fit the budget. */
    CHAT_CONTEXT_OVERSIZE_USER,
    /* Each fits alone; the two indispensable messages together do not. */
    CHAT_CONTEXT_OVERSIZE_COMBINED,
    /* The triggering user message's images alone exceed
       CHAT_ATTACHMENT_BUDGET_BYTES (its text may fit everything else). */
    CHAT_CONTEXT_OVERSIZE_ATTACHMENTS
} ChatContextResult;

typedef struct {
    /* Borrowed views in request order: the system prompt when set, then the
       oldest kept history message through the newest, then the triggering
       user message. */
    ChatRequestMessage messages[CHAT_CONTEXT_MAX_ENTRIES];
    int count;
    /* Exact encoded size of the request body these entries produce:
       text_bytes + attachment_bytes, derived from the two budget remainders
       rather than a second sum. With no images in the selection this equals
       today's single-remainder size; with images it can exceed the text
       budget (up to CHAT_ATTACHMENT_BUDGET_BYTES more). */
    size_t bytes;
    /* The text side (envelope, message framing, text terms, separators) and
       the encoded image-payload side of `bytes`. */
    size_t text_bytes;
    size_t attachment_bytes;
    /* Conversation index of the oldest kept history message, or -1 when the
       budget kept no history. */
    int first_kept_index;
    /* Eligible history messages dropped, always the oldest ones. */
    int dropped_messages;
    /* Every OVERSIZE result: the complete encoded body size the indispensable
       content (system prompt plus triggering message, with separators and the
       trigger's images) would require, which is greater than the budget it
       failed. Zero on OK and INVALID -- the host's text-side diagnostics
       subtract required_attachment_bytes from this and compare against the
       text policy. */
    size_t required_bytes;
    /* Every OVERSIZE result: the image share of `required_bytes` (the
       triggering message's encoded image payload; 0 when it has none). The
       diagnostic compares this against CHAT_ATTACHMENT_BUDGET_BYTES. Zero on
       OK and INVALID. */
    size_t required_attachment_bytes;
    /* Explicitly owned backing storage for every ChatRequestMessage.parts run
       (the view is borrowed; the run lives here). Sized to the hard worst
       case -- CHAT_CONTEXT_MAX_ENTRIES messages x CHAT_MAX_PARTS parts -- so
       the builder can never fail for lack of a slot: a message carries at
       most CHAT_MAX_PARTS parts and at most CHAT_CONTEXT_MAX_ENTRIES entries
       are placed, while text-only messages consume no slots. Allocation-free
       contract preserved: the builder fills caller-owned storage only.
       sizeof(ChatRequestContext) is roughly 350 KiB with this pool, so
       callers must not stack-allocate it. */
    ChatRequestPart part_scratch[CHAT_CONTEXT_MAX_ENTRIES * CHAT_MAX_PARTS];
    int part_slots_used;
} ChatRequestContext;

/* Builds the bounded request context for conversation `c` as of the triggering
   user message at `user_index`, which must be a CHAT_ROLE_USER message (the
   host passes the pending response index minus one). The system prompt is kept
   whenever it is non-empty, the triggering user message is always kept, and
   eligible history messages are kept newest first while they fit both budget
   remainders: `budget` bytes of text side (envelope, framing, text terms,
   separators) and CHAT_ATTACHMENT_BUDGET_BYTES of encoded image payload. Stops
   at the first history message that does not fit and only counts the older
   eligible ones, so a dropped huge history never has its text measured.

   Eligible history is exactly chat_history_message(): no error notes, no local
   welcome text and no assistant message that did not finish successfully.
   Eligible messages are used whole -- including empty ones -- and are never
   truncated or stripped: a kept message keeps all of its parts (only the
   oldest are dropped).

   Attachment costing is metadata-only: byte lengths come from the
   chat->attachments table and no blob is ever opened. A history candidate the
   text side alone rules out is dropped without any attachment lookup (a
   dangling image in dropped history must not block the send); every other
   candidate's images are resolved as the budget is charged.

   Missing attachment records fail closed. For the triggering message the
   lookup runs unconditionally -- even after a text-budget failure, because
   `required_bytes` must include the trigger's images to be meaningful -- so a
   missing trigger record yields INVALID before any OVERSIZE result and no
   diagnostic is reported: an oversize number computed from untrustworthy
   metadata would lie. A missing record on a history candidate whose text side
   fits is likewise INVALID (the run could not be built with a trustworthy
   byte_length).

   Read-only, allocation-free and deterministic. Entries borrow from live Chat
   state and stay valid only until the conversation or the system prompt
   changes, so the caller must hand them to the client (which copies them)
   before any mutation. `out` is zeroed (first_kept_index -1, every other field
   0) before every INVALID return, so a rejected build never leaves a caller
   reading stale or poisoned fields. `budget` must be nonzero; a zero budget is
   invalid arguments rather than an oversize result. On OVERSIZE nothing is
   built; `required_bytes` describes the complete body that would have been
   needed and `required_attachment_bytes` its image share. On OK, `bytes` is
   the exact encoded body size, derived from the budget remainders rather than
   a second sum, so it always agrees with the selection and can never exceed
   the two budgets. */
ChatContextResult chat_context_build(const Chat *chat, const ChatConversation *c,
    int user_index, size_t budget, ChatRequestContext *out);

#endif
