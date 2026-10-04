#ifndef DARKCHAT_EXPORT_H
#define DARKCHAT_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "chat/core/chat.h"
#include "chat/json.h"

/* Deterministic, pure conversation export. No Win32, no file I/O, no global
   state: the serializers are a function of the live Chat plus an explicit
   timestamp and, for messages that carry images, the blobs resolved through
   an explicit callback, so golden tests are byte-exact and a caller owns the
   result.

   Two formats share one authoritative JSON payload:
   - JSON    : the payload itself.
   - Markdown: a leading `<!-- darkchat.export:` comment holding that exact
               payload (with `<`/`>` escaped as \u003c/\u003e so the closing
               `-->` cannot be forged), followed by a presentation-only
               Markdown body. The body is never authoritative: an importer
               reads the payload at offset zero only, so a message body that
               contains marker-like text can never be mistaken for metadata.

   Version 2 is content-derived, exactly as the snapshot's format version is:
   a conversation whose messages carry no content parts emits byte-for-byte
   what version 1 emitted, so an uncustomized text-only export is unchanged
   and every earlier build still imports it. Only the presence of an image
   part raises the version, because only then does the grammar mean something
   an earlier reader would silently drop. */

/* Broad resource guard mirroring the storage limit. Exceeding it fails the
   export cleanly before the cap is crossed. */
#define CHAT_EXPORT_LIMIT (128u * 1024u * 1024u)

/* Blob resolution for an export. Each format needs a different thing, so this
   struct is asymmetric and each serializer uses one half:

     chat_export_json      calls read/discard, while appending base64. One
                           blob is resident at a time and it is released
                           before the next part.
     chat_export_markdown  calls reference, and never reads a byte: a sidecar
                           filename is derived from the attachment record's
                           digest, extension and length. Reading the blobs is
                           the caller's job when it writes the package.

   Neither callback's failure is ever absorbed. The serializers refuse to emit
   a reference they cannot fill, so a document is either complete or absent. */
typedef struct {
    void *user;
    /* JSON only. *out_bytes receives a malloc'd buffer that the CALLER owns
       and releases through discard(). The serializer never retains it and may
       call read again for the same digest. False fails the whole export. */
    bool (*read)(void *user, const char digest[65],
        unsigned char **out_bytes, size_t *out_length);
    void (*discard)(void *user, unsigned char *bytes, size_t length);
    /* Markdown only. Reports one referenced attachment in deterministic
       first-reference order, once per IMAGE part including repeats of the
       same digest, so a caller materializing files can key its file set on
       (digest, extension) rather than on the digest alone: one blob may
       legitimately back two attachment records that differ only in MIME,
       since the store's filename carries no MIME hint.
       `extension` is already validated -- exactly one of "png", "jpg",
       "webp", "gif". False fails the whole export, which is what lets a
       caller report its own allocation failure rather than losing a sidecar
       silently. */
    bool (*reference)(void *user, const char digest[65],
        const char *extension, size_t bytes);
} ChatExportBlobs;

/* Serializes one conversation (index into chat->conversations) or every
   conversation when `all` is true, in storage order. `exported_at` is written
   verbatim as a signed 64-bit Unix-millisecond value.

   `blobs` and `sidecar_stem` are required exactly when the selection contains
   a message with content parts:

     JSON      `blobs` must carry read and discard.
     Markdown  `blobs` must carry reference, and `sidecar_stem` must be a
               name chat/export/package.h accepts. A NULL stem cannot produce
               a reference a reader could resolve, so it fails the export
               rather than emitting an unopenable package.

   When no selected message has parts, both may be NULL and the output is
   byte-for-byte what it has always been.

   `*out` must not currently own a buffer: the callee overwrites it without
   reading it and never leaks a caller allocation. On success it owns a
   NUL-terminated UTF-8 buffer (free with json_buf_free). On any failure (bad
   arguments, an invalid conversation index, an out-of-domain id, an unknown
   role/backend enum, a non-finite metric, a content part whose attachment
   record is missing, an image MIME no sidecar can be named for, the size cap,
   or allocation failure) the buffer is freed and set to (JsonBuf){0}, and
   false is returned. Passing `out == NULL` returns false without writing. */
bool chat_export_json(const Chat *chat, int conversation, bool all,
    int64_t exported_at, const ChatExportBlobs *blobs, JsonBuf *out);
bool chat_export_markdown(const Chat *chat, int conversation, bool all,
    int64_t exported_at, const ChatExportBlobs *blobs,
    const wchar_t *sidecar_stem, JsonBuf *out);

#endif
