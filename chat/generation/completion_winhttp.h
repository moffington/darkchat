#ifndef DARKCHAT_COMPLETION_WINHTTP_H
#define DARKCHAT_COMPLETION_WINHTTP_H

/* Worker-thread OpenAI-compatible streaming client for both backends. The
   request bytes come from the shared pure builder (chat/generation/completion_request.h);
   this module only selects a hard-coded endpoint descriptor, opens the
   matching WinHTTP session and reuses one SSE lifecycle for OpenRouter and
   Ollama. Each posted event is heap-owned by the UI thread. Generation IDs let
   the host ignore stale queued deltas. Answer content and model reasoning
   arrive as separate event types. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat/core/chat.h"

#define CHAT_WM_COMPLETION_EVENT (WM_APP + 0x4e)

/* The client consumes the same borrowed role/text view the request context
   builder produces (chat/generation/context.h), so a built context needs no conversion. */
typedef ChatRequestMessage CompletionMessage;

typedef enum {
    COMPLETION_DELTA, COMPLETION_REASONING, COMPLETION_DONE, COMPLETION_ERROR,
    COMPLETION_CANCELLED, COMPLETION_INTERRUPTED
} CompletionEventType;

typedef struct {
    int generation;
    CompletionEventType type;
    wchar_t *text;
    ChatGeneration metadata;
} CompletionEvent;

typedef struct CompletionClient {
    HWND notify;
    UINT message;
    HANDLE thread;
    volatile LONG generation;
    volatile LONG cancelled_generation;
} CompletionClient;

void completion_init(CompletionClient *client, HWND notify, UINT message);
/* Starts one streamed request against `backend`. An API key is required only
   for OpenRouter: Ollama needs none, so a missing OPENROUTER_API_KEY never
   blocks it. `routing` applies to OpenRouter only. Returns the generation
   (> 0) or 0 when the request could not start. */
int completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing);
bool completion_cancel(CompletionClient *client, int generation);
void completion_event_free(CompletionEvent *event);
void completion_complete(CompletionClient *client, int generation);
void completion_shutdown(CompletionClient *client);

#endif
