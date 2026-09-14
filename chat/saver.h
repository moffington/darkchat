#ifndef DARKCHAT_SAVER_H
#define DARKCHAT_SAVER_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include "storage.h"

#define CHAT_WM_SAVER_RESULT (WM_APP + 0x4f)

/* One background snapshot writer per process. The UI thread builds an owned
   immutable Chat snapshot (chat_snapshot) and hands it over; the writer alone
   calls storage_save, so the single-writer lock, backup rotation and recovery
   semantics of storage.c are unchanged and stay single-threaded.

   Coalescing: the pending slot is latest-wins. A snapshot that has not started
   writing is displaced outright by the next handoff and disposed, because the
   replacing snapshot was taken later and contains all of its state. At most
   one snapshot is in flight and one pending, so a burst of handoffs can never
   queue unbounded work.

   Ownership: pending and in-flight snapshots belong to exactly one side at a
   time. saver_submit/saver_flush transfer ownership under the lock; the
   receiver disposes what it received. The writer never touches the live Chat.

   Submission identity: every accepted handoff gets a monotonically increasing
   `attempt` id, independent of the host's mutation counter. Two handoffs can
   carry the same mutation counter (no mutation between them), so the counter
   alone cannot identify a save attempt; flush and result matching key on the
   attempt id, and the mutation counter rides along only for the dirty-vs-
   durable decision. Each pending and in-flight job retains its own attempt
   and counter.

   Results: the writer posts CHAT_WM_SAVER_RESULT per completed job with
   wParam = (ok ? 1 : 0) | (attempt << 1) and lParam = the captured mutation
   counter. A flush is released only when a job whose attempt is at least its
   own completes — its own attempt or a genuinely later covering one — never
   by an older attempt that merely shares the same mutation counter. Every
   accepted submission either completes with exactly one posted result or is
   displaced before starting; a displaced snapshot is disposed without a
   result, which is safe because the displacing snapshot was captured later
   and contains all of its state, and that displacement's own result then
   reports authoritatively.

   Shutdown: saver_shutdown drains pending jobs, joins the thread and is
   idempotent. It must run before the storage lock is released, and releases
   a stranded flush with a false result. */
typedef struct {
    ChatStorage *store;                 /* borrowed; outlives the saver */
    HWND notify;
    UINT message;
    HANDLE thread, wake, flush_done;
    CRITICAL_SECTION lock;
    bool lock_ready, stop;
    Chat *pending, *in_flight;          /* owned; latest-wins pending slot */
    uint64_t next_attempt;
    uint64_t pending_attempt, pending_counter;
    uint64_t in_flight_attempt, in_flight_counter;
    uint64_t done_attempt;              /* attempt of the newest completed job */
    bool flush_active, flush_result;
    uint64_t flush_attempt;
} ChatSaver;

bool saver_init(ChatSaver *saver, HWND notify, UINT message, ChatStorage *store);
bool saver_ready(const ChatSaver *saver);
/* Transfers ownership of `snapshot`; never blocks. Returns the attempt id
   assigned to this submission. */
uint64_t saver_submit(ChatSaver *saver, Chat *snapshot, uint64_t captured);
/* Deposits `snapshot` as a new attempt and blocks until a job whose attempt
   id is at least this attempt completes; returns that job's durability
   result. The same mutation counter on an older in-flight job does not
   satisfy this wait. `*attempt` receives this submission's attempt id. */
bool saver_flush(ChatSaver *saver, Chat *snapshot, uint64_t captured,
    uint64_t *attempt);
void saver_shutdown(ChatSaver *saver);
#endif
