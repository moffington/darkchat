/* Pure image-ingest policy: sniffed facts in, a storage/normalization plan
   out. No Win32, so the decision table is testable without Windows Imaging
   Component or COM. The native WIC ingest layer in chat/shell/image_wic.c is
   the only producer of ChatImageFacts and the only consumer of the plan.

   The caps below are the single source of truth for both the policy and the
   native decode path. Exceeding either hard cap is a user-visible rejection;
   the normalize path never rescues an over-cap input (one limit, one rule,
   one check). Sizes are checked from raw byte length and decoder-reported
   dimensions only, never from a full decode. */
#ifndef DARKCHAT_IMAGE_POLICY_H
#define DARKCHAT_IMAGE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chat/core/content.h"

/* Raw source byte cap, checked before any decode. */
#define CHAT_ATTACHMENT_MAX_BYTES (20u * 1024u * 1024u)
/* Long-edge pixel cap, checked from decoder GetSize before a full decode.
   Doubles as the decompression-bomb guard. */
#define CHAT_ATTACHMENT_MAX_PIXELS 4096u
/* Normalize path output long edge. Applies only to in-bounds sources whose
   normalization triggers fired; passthrough bytes are never rescaled. */
#define CHAT_ATTACHMENT_NORMALIZE_EDGE 2048u
/* Per-message attachment bound; must stay <= CHAT_MAX_PARTS. */
#define CHAT_ATTACHMENT_MAX_PER_MESSAGE 8

/* Sniffed container. OTHER is any decodable container that is not one of the
   four API-ready formats (BMP/TIFF/ICO/...); it normalizes to PNG. */
typedef enum {
    CHAT_IMAGE_FORMAT_PNG = 0,
    CHAT_IMAGE_FORMAT_JPEG,
    CHAT_IMAGE_FORMAT_WEBP,
    CHAT_IMAGE_FORMAT_GIF,
    CHAT_IMAGE_FORMAT_OTHER
} ChatImageFormat;

typedef struct {
    size_t byte_length;      /* raw source bytes */
    ChatImageFormat format;  /* sniffed container */
    uint32_t width, height;  /* decoder-reported before any transform */
    unsigned frame_count;    /* decoder frame count; > 1 = animated */
    unsigned exif_orientation; /* 0 absent, 1..8 */
    bool from_clipboard;     /* DIB path: always normalize */
} ChatImageFacts;

typedef enum {
    CHAT_IMAGE_STORE_AS_IS = 0,
    CHAT_IMAGE_NORMALIZE_PNG,
    CHAT_IMAGE_NORMALIZE_JPEG,
    CHAT_IMAGE_REJECT_TOO_LARGE,
    CHAT_IMAGE_REJECT_DIMENSIONS
} ChatImagePlan;

/* Decides what to do with one source. `out_flags`, when non-NULL, receives 0
   or CHAT_PART_FLAG_FIRST_FRAME (a multi-frame source is reduced to frame
   one). Rejection decisions take precedence over format decisions and over
   each other in the order documented above. A NULL facts pointer is a
   dimensions rejection (defensive; callers never pass NULL). */
ChatImagePlan chat_image_plan(const ChatImageFacts *facts, uint8_t *out_flags);

/* Structural integrity gate for an as-is passthrough. WIC decoders can report
   success for a damaged or truncated payload, so the source bytes must also
   satisfy their own container framing before they are stored unchanged:
   PNG chunk lengths/CRCs plus IEND, JPEG SOI/EOI, GIF trailer, WebP RIFF
   size. Purely structural; it never decodes. CHAT_IMAGE_FORMAT_OTHER has no
   defined framing here and returns true (those sources normalize anyway).
   A NULL buffer or zero length is not intact. */
bool chat_image_bytes_intact(const void *bytes, size_t length,
    ChatImageFormat format);

#endif
