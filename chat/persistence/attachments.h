#ifndef DARKCHAT_ATTACHMENTS_H
#define DARKCHAT_ATTACHMENTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "chat/core/content.h"

/* Managed attachment blob store: content-addressed files under
   <directory>\attachments\ beside state.jsonl (same directory storage_open
   uses; NULL = %LOCALAPPDATA%\DarkChat):

     attachments\<64-hex>     managed blobs (only these collect touches)
     attachments\.tmp\        unique staging names for atomic writes
     attachments\corrupt\     quarantined digest mismatches

   Blob-then-state ordering: attachment_store_put completes the durable
   rename before it returns true, so a caller cannot reference a digest whose
   bytes are not yet on disk. A crash after put and before any snapshot
   references the id leaves an orphan blob (reclaimable by collect) — never
   a dangling reference.

   Filename rule: the sole managed name for a digest is exactly 64 lowercase
   hex characters (SHA-256 of the managed bytes) and **nothing else** — no
   extension, no MIME hint, never display_name. MIME lives only in
   ChatAttachmentMeta / the message part. One file per digest is structural:
   every put of the same bytes targets the same path, so concurrent puts
   (even with different MIME arguments) serialize through atomic rename onto
   one file and cannot delete each other's only copy. display_name is
   display-only and must never appear in a path.

   Dedup: put of bytes whose digest already exists re-hashes the on-disk
   file first. A matching blob is reused (fresh attachment_id, out_created =
   false). A mismatching blob (including a zero-byte file) is quarantined to
   corrupt\ and rewritten (out_created = true).

   Ids: allocated from the caller-borrowed shared counter (CHAT_MAX_ID
   ceiling, same space as conversation/message ids). Capacity is checked
   before any write; the counter increments only after the blob is durable
   or verified, so a failed put consumes no id and leaves no new file.

   out_created reports whether *this call* wrote new bytes (true) versus
   verified reuse of an existing blob (false). It is informational only —
   not permission to delete on abort. A repair sets true yet may rewrite a
   blob other messages already reference, and a fresh write may be
   deduplicated by a later put before this import aborts. Import rollback
   must always use the complete live-set sweep (attachment_store_collect
   with the full §4.4.4 set: live parts, all three recovery snapshots,
   pending/staged attachments, this session's puts). Never unlink a file
   based on out_created alone.

   Concurrency: staging names are unique per write with exclusive creation
   (CREATE_NEW), then renamed with MOVEFILE_REPLACE_EXISTING onto the single
   digest path — concurrent identical writes are benign. The store is not
   itself a lock; state.jsonl still serializes through writer.lock. */

/* SHA-256 as 64 lowercase hex characters plus NUL. Pure; data may be NULL
   only when n is 0. Direct hash entry point (put rejects n == 0, so empty
   and other vectors are tested here rather than through put). */
void attachment_digest_hex(const void *data, size_t n, char out[65]);

/* Per-store file-I/O seam. Defaults are Win32 implementations; tests (and
   only tests) replace individual pointers on the store after open. There is
   no process-global setter: two stores never share mutable I/O behavior.
   All paths are absolute; find_blob resolves <dir>\<digest> at the top level
   only (never .tmp\ or corrupt\). */
typedef struct {
    bool (*mkdir)(const wchar_t *path);
    /* Durable publish: unique temp under <blob-dir>\.tmp\ with exclusive
       creation, write+flush, then rename onto `path`. On failure no file
       remains at `path` and no staging file is left behind. */
    bool (*write_atomic)(const wchar_t *path, const void *data, size_t n);
    /* Whole-file read into a malloc'd buffer (caller frees). */
    bool (*read)(const wchar_t *path, unsigned char **out, size_t *out_n);
    bool (*remove)(const wchar_t *path);
    bool (*rename_file)(const wchar_t *from, const wchar_t *to);
    /* True path of the sole top-level blob for `digest` when it exists.
       False when absent. */
    bool (*find_blob)(const wchar_t *dir, const char *digest,
        wchar_t *out_path, size_t out_cap);
} AttachmentIo;

typedef struct {
    wchar_t dir[1024]; /* <directory>\attachments */
    uint64_t *next_id; /* borrowed shared id counter; must outlive store */
    AttachmentIo io;   /* per-store seam; replace fields for fault injection */
    bool open;
} ChatAttachmentStore;

/* Creates <dir>\attachments\{,.tmp\,corrupt\} and installs default I/O.
   `directory` is the state directory (NULL = %LOCALAPPDATA%\DarkChat);
   `next_id` is required. */
bool attachment_store_open(ChatAttachmentStore *store,
    const wchar_t *directory, uint64_t *next_id);
/* Idempotent; clears the store so a closed store fails every operation. */
void attachment_store_close(ChatAttachmentStore *store);

/* Hashes bytes, publishes the blob (or verifies+reuses per the dedup rule
   above), then allocates a fresh id into *out_meta. `out_created` is
   optional; when supplied it receives informational write-vs-reuse status
   (see the out_created rule above — never a delete permission).
   Rejects n == 0, oversized mime/display_name and an exhausted counter
   before any I/O. */
bool attachment_store_put(ChatAttachmentStore *store,
    const void *bytes, size_t n, const char *mime,
    const wchar_t *display_name, ChatAttachmentMeta *out_meta,
    bool *out_created);

/* Reads the blob and re-hashes it. Missing → false. Digest mismatch —
   including a zero-byte file — → quarantine of the path read to corrupt\
   and false. *out_data is malloc'd for non-empty blobs (caller frees);
   n == 0 never returns true for a managed blob (put rejects empty input). */
bool attachment_store_get(const ChatAttachmentStore *store,
    const char *digest, unsigned char **out_data, size_t *out_n);

/* Moves <digest> into corrupt\ (keeps the filename). False when the
   digest is invalid, no top-level blob exists, or the rename fails. */
bool attachment_store_quarantine(const ChatAttachmentStore *store,
    const char *digest);

/* Mark/sweep over top-level managed blobs only (exactly 64 lowercase hex,
   no extension). Every entry in [live, live_count) must itself be a valid
   64-hex digest or the call fails closed before deleting anything — the
   caller owns the full §4.4.4 live set (live parts, all three recovery
   snapshots, pending/staged attachments, this session's puts), not message
   parts alone. Foreign files, .tmp\ and corrupt\ are never touched.
   Best-effort deletes: continues after a failure, returns false if any
   intended delete failed. */
bool attachment_store_collect(ChatAttachmentStore *store,
    const char *const *live, size_t live_count);

#endif
