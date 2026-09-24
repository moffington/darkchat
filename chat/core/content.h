/* Ordered typed content parts for one message. Pure; no Win32.

   Representation policy (the fixed-residue philosophy applied to parts):
   a text-only message owns NO parts array at all -- `ChatMessage.text` IS its
   content. A parts array exists only when the message carries at least one
   non-text part or more than one text part, so the fixed per-message cost of
   the overwhelmingly common case is unchanged and `sizeof(ChatMessage)` grows
   by one pointer plus two size_t's (24 bytes on x64), which keeps the
   fixed-residue amplification gate well inside its 1 GiB ceiling.

   Logical view invariant: `chat_message_part_count/at` present every message
   as an ordered sequence. A message with parts == NULL presents exactly one
   TEXT part when `text[0] != 0` (or `text_overflow` set) and zero parts when
   the text is empty. When parts != NULL the array is the sole authority for
   content and ordering; `ChatMessage.text` then holds the *plain-text
   projection* (the concatenation of every TEXT part, in order) maintained by
   the setters, so search, copy, titles and Markdown export need no new
   traversal. */
#ifndef DARKCHAT_CONTENT_H
#define DARKCHAT_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Reserved for a future bounded part-text residue. v1 TEXT parts are always
   heap-backed: parts exist only for messages that already left the fast path,
   so a second residue scheme would be dead weight. */
#define CHAT_PART_TEXT_MAX_INLINE 32

typedef enum {
    CHAT_PART_TEXT = 0,
    CHAT_PART_IMAGE = 1
    /* Append-only. A snapshot carrying an unknown kind is rejected as
       corruption at load (fail closed, matching the storage policy for
       unknown record meanings) rather than silently dropped. */
} ChatPartKind;

/* Defined ChatPart.flags bits: append-only, persisted in snapshot format 6.
   Load rejects any bit outside CHAT_PART_FLAG_MASK as corruption (fail
   closed -- the evolution of the old "flags/reserved must be 0" rule as the
   set grows). TEXT parts must carry flags == 0. Mutators reject bits outside
   the mask. */
#define CHAT_PART_FLAG_FIRST_FRAME (1u << 0) /* IMAGE: source animation was
                                                reduced to frame 1 at ingest */
#define CHAT_PART_FLAG_MASK CHAT_PART_FLAG_FIRST_FRAME

typedef struct {
    /* Attachment store identity. Stable across restarts; allocated from the
       same persisted counter as conversation/message ids (storage format 6).
       Opaque in the content layer: commit 1 never interprets it. */
    uint64_t attachment_id;
    /* Normalized pixel size of the stored image, 0 when unknown. Display hint
       only; never used for request sizing. */
    uint32_t pixel_width, pixel_height;
    /* MIME of the *stored managed bytes* ("image/png", "image/jpeg",
       "image/webp", "image/gif"). NUL-terminated, lowercase, bounded. */
    char mime[32];
    /* Original user-facing name at ingest ("photo.jpg"), display only. Empty
       for clipboard pastes. Never a path. */
    wchar_t display_name[64];
} ChatImagePart;

/* Attachment-store record for one managed blob: identity, digest and the
   metadata a request/export/sweep needs without opening the file. Loaded
   from snapshot format 6 `type:"attachment"` records (later commit) and
   filled by attachment_store_put. `id` shares the conversation/message
   counter (CHAT_MAX_ID ceiling). `digest` is 64 lowercase hex characters
   (SHA-256 of the managed bytes) and names the blob file; `bytes` is the
   stored length. `pixel_*` are display hints (0 when unknown before WIC
   ingest). `display_name` is never a path. */
typedef struct {
    uint64_t id;
    char digest[64 + 1];
    char mime[32];
    size_t bytes;
    uint32_t pixel_width, pixel_height;
    int64_t created_at;
    wchar_t display_name[64];
} ChatAttachmentMeta;

typedef struct {
    uint8_t kind;      /* ChatPartKind */
    uint8_t flags;     /* CHAT_PART_FLAG_* (IMAGE only); unknown bits = corruption */
    uint16_t reserved; /* must be 0 */
    union {
        /* CHAT_PART_TEXT: owned NUL-terminated text. No inline residue on
           purpose: parts exist only for messages that already left the fast
           path, so a second residue scheme would be dead weight. Transactional
           set/clone like ChatText. */
        struct { wchar_t *data; size_t length, capacity; } text;
        /* CHAT_PART_IMAGE: value metadata only; never owns blob bytes. */
        ChatImagePart image;
    } u;
} ChatPart;

typedef struct {
    ChatPart *items; /* NULL exactly when count == 0 */
    size_t count;
    size_t capacity; /* growth doubles; never exceeds CHAT_MAX_PARTS */
} ChatContent;

#define CHAT_MAX_PARTS 16

/* A by-value borrowed view of one logical part. There is no ChatPart object
   for a fast-path message (parts == NULL), so the accessor cannot return a
   pointer into parts.items: `out` is filled by value -- IMAGE fields are
   copied, TEXT borrows pointers into the message's own storage (valid until
   that message mutates, the same rule as ChatRequestMessage). i >= count
   returns false and leaves *out zeroed. Callers never alias the live parts
   array, so growth/reorder cannot invalidate a view they hold. */
typedef struct {
    uint8_t kind; /* ChatPartKind */
    uint8_t flags; /* CHAT_PART_FLAG_* */
    union {
        struct { const wchar_t *data; size_t length; } text; /* borrowed */
        ChatImagePart image;                                 /* by value */
    } u;
} ChatPartView;

#endif
