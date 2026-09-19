#include "chat/persistence/saver.h"
#include <process.h>
#include <stdlib.h>

bool saver_ready(const ChatSaver *saver) {
    return saver && saver->thread && saver->thread!=INVALID_HANDLE_VALUE &&
        saver->store;
}

static void dispose_snapshot(Chat *snapshot) {
    if (!snapshot) return;
    chat_dispose(snapshot);
    free(snapshot);
}

static unsigned __stdcall saver_worker(void *argument);

/* Replaces the pending snapshot with a newer one; the displaced copy is
   disposed here because the replacement was captured later and contains all
   of its state. `attempt` is the caller's freshly assigned submission id. */
static void deposit(ChatSaver *saver, Chat *snapshot, uint64_t attempt,
    uint64_t captured) {
    if (saver->pending) dispose_snapshot(saver->pending);
    saver->pending=snapshot;
    saver->pending_attempt=attempt;
    saver->pending_counter=captured;
}

bool saver_init(ChatSaver *saver, HWND notify, UINT message, ChatStorage *store) {
    memset(saver,0,sizeof *saver);
    if (!notify || !message || !store || !store->writable) return false;
    saver->store=store;
    saver->notify=notify;
    saver->message=message;
    saver->wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    saver->flush_done=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!saver->wake || !saver->flush_done) {
        if (saver->wake) CloseHandle(saver->wake);
        if (saver->flush_done) CloseHandle(saver->flush_done);
        memset(saver,0,sizeof *saver);
        return false;
    }
    InitializeCriticalSection(&saver->lock);
    saver->lock_ready=true;
    uintptr_t thread=_beginthreadex(NULL,0,saver_worker,saver,0,NULL);
    if (!thread) {
        DeleteCriticalSection(&saver->lock);
        CloseHandle(saver->wake);
        CloseHandle(saver->flush_done);
        memset(saver,0,sizeof *saver);
        return false;
    }
    saver->thread=(HANDLE)thread;
    return true;
}

static unsigned __stdcall saver_worker(void *argument) {
    ChatSaver *saver=(ChatSaver *)argument;
    for (;;) {
        Chat *job=NULL;
        uint64_t attempt=0, captured=0;
        bool release_flush=false, exiting=false;
        EnterCriticalSection(&saver->lock);
        for (;;) {
            if (saver->pending) {
                /* Take the newest handoff; its attempt and captured mutation
                   counter travel with the job. */
                job=saver->pending;
                attempt=saver->pending_attempt;
                captured=saver->pending_counter;
                saver->pending=NULL;
                saver->in_flight=job;
                saver->in_flight_attempt=attempt;
                saver->in_flight_counter=captured;
                break;
            }
            if (saver->stop) {
                /* Nothing left to drain. A flush still waiting cannot be
                   satisfied by this thread anymore; release it failed rather
                   than leaving the UI thread parked forever. */
                if (saver->flush_active) {
                    saver->flush_active=false;
                    saver->flush_result=false;
                    release_flush=true;
                }
                break;
            }
            LeaveCriticalSection(&saver->lock);
            WaitForSingleObject(saver->wake,INFINITE);
            EnterCriticalSection(&saver->lock);
        }
        exiting=saver->stop && !job;
        LeaveCriticalSection(&saver->lock);
        if (release_flush) SetEvent(saver->flush_done);
        if (exiting) break;
        if (!job) continue;
        bool ok=storage_save(saver->store,job);
        dispose_snapshot(job);
        EnterCriticalSection(&saver->lock);
        saver->in_flight=NULL;
        /* Attempts complete strictly in submission order, so the newest
           completed attempt only moves forward. */
        if (attempt>saver->done_attempt) saver->done_attempt=attempt;
        /* Flush release keys on the completing job's attempt id: an older
           attempt that merely carries the same mutation counter never
           satisfies a flush. */
        if (saver->flush_active && saver->done_attempt>=saver->flush_attempt) {
            saver->flush_active=false;
            saver->flush_result=ok;
            release_flush=true;
        }
        LeaveCriticalSection(&saver->lock);
        if (release_flush) SetEvent(saver->flush_done);
        /* The result travels with its own attempt id and captured mutation
           counter: wParam = result | (attempt << 1), lParam = counter. If the
           window is already gone the report is dropped; nothing on the UI
           side can consume it anymore. */
        PostMessageW(saver->notify,saver->message,
            (WPARAM)(((UINT_PTR)(ok?1:0))|(attempt<<1)),
            (LPARAM)captured);
    }
    return 0;
}

uint64_t saver_submit(ChatSaver *saver, Chat *snapshot, uint64_t captured) {
    if (!snapshot) return 0;
    EnterCriticalSection(&saver->lock);
    uint64_t attempt=++saver->next_attempt;
    deposit(saver,snapshot,attempt,captured);
    LeaveCriticalSection(&saver->lock);
    if (!saver->stop) SetEvent(saver->wake);
    return attempt;
}

bool saver_flush(ChatSaver *saver, Chat *snapshot, uint64_t captured,
    uint64_t *attempt_id) {
    if (!snapshot) return false;
    ResetEvent(saver->flush_done);
    EnterCriticalSection(&saver->lock);
    uint64_t attempt=++saver->next_attempt;
    saver->flush_active=true;
    saver->flush_attempt=attempt;
    saver->flush_result=false;
    deposit(saver,snapshot,attempt,captured);
    LeaveCriticalSection(&saver->lock);
    if (attempt_id) *attempt_id=attempt;
    SetEvent(saver->wake);
    WaitForSingleObject(saver->flush_done,INFINITE);
    EnterCriticalSection(&saver->lock);
    bool ok=saver->flush_result;
    LeaveCriticalSection(&saver->lock);
    return ok;
}

void saver_shutdown(ChatSaver *saver) {
    if (!saver || !saver->lock_ready) return;
    EnterCriticalSection(&saver->lock);
    saver->stop=true;
    if (saver->flush_active) {
        saver->flush_active=false;
        saver->flush_result=false;
        SetEvent(saver->flush_done);
    }
    LeaveCriticalSection(&saver->lock);
    SetEvent(saver->wake);
    if (saver->thread && saver->thread!=INVALID_HANDLE_VALUE) {
        WaitForSingleObject(saver->thread,INFINITE);
        CloseHandle(saver->thread);
        saver->thread=NULL;
    }
    /* After the join nothing is pending or in flight; this is defense only. */
    dispose_snapshot(saver->pending); saver->pending=NULL;
    dispose_snapshot(saver->in_flight); saver->in_flight=NULL;
    if (saver->wake) CloseHandle(saver->wake);
    if (saver->flush_done) CloseHandle(saver->flush_done);
    DeleteCriticalSection(&saver->lock);
    saver->lock_ready=false;
}
