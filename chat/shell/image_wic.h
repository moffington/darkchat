/* Native image ingest: turn a local file or a packed clipboard DIB into the
   managed, API-ready bytes and display metadata a ChatImagePart carries.
   This is the only Win32/Windows Imaging Component (WIC) translation unit for
   image input; the accept-vs-normalize decision itself is pure and lives in
   chat/core/image_policy.h.

   COM must already be initialized on the calling thread (the host does this
   before storage/UI; tests initialize once). Ingest never calls
   CoInitializeEx/CoUninitialize.

   Output contract: on success `out->bytes` is a malloc'd owned buffer whose
   length is `out->length`, `out->meta.mime` is the lowercase MIME of those
   stored bytes, `out->meta.pixel_width/height` are the post-transform size,
   and `out->meta.attachment_id` is 0 until the caller persists the bytes
   through attachment_store_put. `out->meta.display_name` is the file's base
   name for a file source and empty for a clipboard DIB. On every failure
   `*out` is fully zeroed. Release owned bytes with image_ingest_dispose. */
#ifndef DARKCHAT_IMAGE_WIC_H
#define DARKCHAT_IMAGE_WIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "chat/core/content.h"

typedef struct {
    ChatImagePart meta;      /* attachment_id stays 0; store assigns it */
    uint8_t flags;           /* CHAT_PART_FLAG_* for chat_message_add_image */
    unsigned char *bytes;    /* owned; free with image_ingest_dispose */
    size_t length;
} ChatImageIngest;

typedef enum {
    IMAGE_INGEST_OK = 0,
    IMAGE_INGEST_IO,          /* file read or encode-stream failure */
    IMAGE_INGEST_TOO_LARGE,   /* raw source over CHAT_ATTACHMENT_MAX_BYTES */
    IMAGE_INGEST_DIMENSIONS,  /* long edge over CHAT_ATTACHMENT_MAX_PIXELS */
    IMAGE_INGEST_UNSUPPORTED, /* undecodable or an unsupported DIB layout */
    IMAGE_INGEST_OOM,
    IMAGE_INGEST_EMPTY        /* zero-byte input */
} ChatImageIngestResult;

/* Reads `path`, enforces the byte cap before decoding, sniffs the container
   and either stores the source bytes unchanged or normalizes them per
   image_policy. display_name is the path's base name. */
ChatImageIngestResult image_ingest_file(const wchar_t *path,
    ChatImageIngest *out);

/* Parses a packed CF_DIB/CF_DIBV5 (BITMAPINFOHEADER + palette + pixels).
   Only BI_RGB 24bpp and BI_RGB/BI_BITFIELDS 32bpp are accepted; every
   clipboard source takes the normalize path and becomes PNG. */
ChatImageIngestResult image_ingest_dib(const void *dib, size_t dib_size,
    ChatImageIngest *out);

/* Frees owned bytes and zeroes the whole result. Safe on a zeroed struct and
   safe to call twice. */
void image_ingest_dispose(ChatImageIngest *out);

/* User-facing text for a result. Never NULL; IMAGE_INGEST_OK maps to L"". */
const wchar_t *image_ingest_error_text(ChatImageIngestResult result);

#endif
