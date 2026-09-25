#ifndef DARKCHAT_STORAGE_H
#define DARKCHAT_STORAGE_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat/core/chat.h"
/* One writer per directory. A failed load disables writes to preserve evidence. */
typedef struct {
    wchar_t path[1024], backup[1024], temporary[1024];
    HANDLE lock;
    bool writable, primary_valid, recovered;
} ChatStorage;
bool storage_open(ChatStorage *store, const wchar_t *directory);
/* 1 loaded, 0 new store, -1 unreadable/unsupported (writes disabled). */
int storage_load(ChatStorage *store, Chat *chat);
bool storage_save(ChatStorage *store, const Chat *chat);
void storage_close(ChatStorage *store);
/* Reports one attachment digest (64 lowercase hex, NUL-terminated) to the
   callback; returning false aborts the scan. */
typedef bool (*StorageDigestFn)(void *user, const char digest[65]);
/* Reports every digest named by a `type:"attachment"` record of the
   recoverable snapshot at `path` (format 6). The per-file member of the
   mark/sweep live set: the sweep's caller scans all three state files and
   adds the live message parts and the pending/staged attachments before
   invoking attachment_store_collect. No blob is opened.

   The whole file is validated first (the exact grammar storage_load
   applies: checksum, record grammar, version gate) and its digests are
   staged before any callback runs, so a snapshot the loader would reject
   contributes no digest at all and nothing is ever reported out of a
   partially validated file. A missing file (file-not-found only) is an
   empty contribution and reports true. A file that exists but cannot be
   read or inspected -- access denied, sharing violation, a missing parent
   directory -- or is not a valid snapshot reports false and contributes
   nothing. When any scan reports false the caller's live set is
   necessarily incomplete and the sweep must not run. */
bool storage_scan_attachment_digests(const wchar_t *path,
    StorageDigestFn fn, void *user);
#endif
