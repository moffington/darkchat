#ifndef DARKCHAT_COMPLETION_WINHTTP_H
#define DARKCHAT_COMPLETION_WINHTTP_H

/* Worker-thread OpenAI-compatible streaming client for both backends. The
   request bytes come from the shared pure builder (chat/generation/completion_request.h);
   this module only selects a hard-coded endpoint descriptor, opens the
   matching WinHTTP session and reuses one SSE lifecycle for OpenRouter and
   Ollama. Events are queued under the client lock and delivered to the UI
   thread through one outstanding wake message: the wake handler takes the
   whole queued batch and then returns to the normal pump, so stream
   delivery can never starve input or outgrow one bounded batch per wake.
   Generation IDs let the host ignore stale queued deltas. Answer content
   and model reasoning arrive as separate event types. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat/core/chat.h"

#define CHAT_WM_COMPLETION_EVENT (WM_APP + 0x4e)

/* The client consumes the same borrowed role/text view the request context
   builder produces (chat/generation/context.h), so a built context needs no
   conversion. The view is deep-copied onto the worker before the thread
   starts: texts copy like today, and a message carrying a part run copies
   that run as owned parts (including the image bytes the host loaded into
   the view), so a request can outlive the per-send scope that built it. */
typedef ChatRequestMessage CompletionMessage;

typedef enum {
    COMPLETION_DELTA, COMPLETION_REASONING, COMPLETION_DONE, COMPLETION_ERROR,
    COMPLETION_CANCELLED, COMPLETION_INTERRUPTED
} CompletionEventType;

typedef struct CompletionEvent {
    int generation;
    CompletionEventType type;
    wchar_t *text;
    ChatGeneration metadata;
    /* Delivery-queue link: events are queued under the client lock and
       handed to the UI thread as one FIFO batch per wake. */
    struct CompletionEvent *next;
} CompletionEvent;

typedef struct CompletionClient {
    HWND notify;
    UINT message;
    HANDLE thread;
    volatile LONG generation;
    volatile LONG cancelled_generation;
    /* Single-outstanding-wake delivery: the worker appends events under
       `lock` and posts `message` to `notify` only when no wake is
       outstanding (`wake_pending`). completion_take clears that flag under
       the same lock while removing the batch, so an event is never
       stranded without a wake and never posted twice. `alive` gates drains
       after shutdown (the wake message may outlive the queue). */
    CRITICAL_SECTION lock;
    CompletionEvent *head, *tail;
    volatile LONG wake_pending;
    bool alive;
} CompletionClient;

void completion_init(CompletionClient *client, HWND notify, UINT message);
/* Takes every queued completion event as one FIFO batch (ownership
   transfers to the caller); NULL when nothing is queued. Call from the
   wake-message handler on the UI thread; each wake delivers exactly the
   events queued since the previous take. */
CompletionEvent *completion_take(CompletionClient *client);
/* Starts one streamed request against `backend`. An API key is required only
   for OpenRouter: Ollama needs none, so a missing OPENROUTER_API_KEY never
   blocks it. `routing` applies to OpenRouter only. `reasoning` gates
   OpenRouter's reasoning object and is ignored for Ollama. Returns the
   generation (> 0) or 0 when the request could not start. */
int completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning);
bool completion_cancel(CompletionClient *client, int generation);
void completion_event_free(CompletionEvent *event);
/* Frees a linked batch returned by completion_take (single events too). */
void completion_events_free(CompletionEvent *batch);
void completion_complete(CompletionClient *client, int generation);
void completion_shutdown(CompletionClient *client);

#endif
