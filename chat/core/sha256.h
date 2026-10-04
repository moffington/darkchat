#ifndef DARKCHAT_SHA256_H
#define DARKCHAT_SHA256_H

#include <stddef.h>

/* SHA-256 (FIPS 180-4) as 64 lowercase hex characters plus a NUL. Pure C: no
   Win32, no allocation, no third-party code.

   This lives outside the attachment store because the digest is a content
   identity, not a storage concern. The importer must re-hash an exported
   attachment and compare it against the digest the document itself declares,
   before any blob is written -- and the importer is a pure module with no
   Win32 dependency. Keeping the hash here lets both sides share one
   definition, so a digest can never mean two different things. */

#ifdef __cplusplus
extern "C" {
#endif

/* `data` may be NULL only when n == 0. Writes exactly 65 bytes: 64 lowercase
   hex characters and a NUL. */
void sha256_hex(const void *data, size_t n, char out[65]);

#ifdef __cplusplus
}
#endif

#endif