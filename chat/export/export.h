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
   timestamp, so golden tests are byte-exact and a caller owns the result.

   Two formats share one authoritative JSON payload:
   - JSON    : the payload itself.
   - Markdown: a leading `<!-- darkchat.export:` comment holding that exact
               payload (with `<`/`>` escaped as \u003c/\u003e so the closing
               `-->` cannot be forged), followed by a presentation-only
               Markdown body. The body is never authoritative: an importer
               reads the payload at offset zero only, so a message body that
               contains marker-like text can never be mistaken for metadata. */

/* Broad resource guard mirroring the storage limit. Exceeding it fails the
   export cleanly before the cap is crossed. */
#define CHAT_EXPORT_LIMIT (128u * 1024u * 1024u)

/* Serializes one conversation (index into chat->conversations) or every
   conversation when `all` is true, in storage order. `exported_at` is written
   verbatim as a signed 64-bit Unix-millisecond value.

   `*out` must not currently own a buffer: the callee overwrites it without
   reading it and never leaks a caller allocation. On success it owns a
   NUL-terminated UTF-8 buffer (free with json_buf_free). On any failure (bad
   arguments, an invalid conversation index, an out-of-domain id, an unknown
   role/backend enum, a non-finite metric, the size cap, or allocation
   failure) the buffer is freed and set to (JsonBuf){0}, and false is
   returned. Passing `out == NULL` returns false without writing. */
bool chat_export_json(const Chat *chat, int conversation, bool all,
    int64_t exported_at, JsonBuf *out);
bool chat_export_markdown(const Chat *chat, int conversation, bool all,
    int64_t exported_at, JsonBuf *out);

#endif
