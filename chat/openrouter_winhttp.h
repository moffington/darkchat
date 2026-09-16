#ifndef DARKCHAT_OPENROUTER_WINHTTP_H
#define DARKCHAT_OPENROUTER_WINHTTP_H

/* Worker-thread OpenRouter streaming client. Each posted event is heap-owned
   by the UI thread. Generation IDs let the host ignore stale queued deltas.
   Answer content and model reasoning arrive as separate event types. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"

#define CHAT_WM_OPENROUTER_EVENT (WM_APP + 0x4e)

/* The client consumes the same borrowed role/text view the request context
   builder produces (chat/context.h), so a built context needs no conversion. */
typedef ChatRequestMessage OpenRouterMessage;

typedef enum {
    OPENROUTER_DELTA, OPENROUTER_REASONING, OPENROUTER_DONE, OPENROUTER_ERROR,
    OPENROUTER_CANCELLED, OPENROUTER_INTERRUPTED
} OpenRouterEventType;

typedef struct {
    int generation;
    OpenRouterEventType type;
    wchar_t *text;
    ChatGeneration metadata;
} OpenRouterEvent;

typedef struct OpenRouterClient {
    HWND notify;
    UINT message;
    HANDLE thread;
    volatile LONG generation;
    volatile LONG cancelled_generation;
} OpenRouterClient;

void openrouter_init(OpenRouterClient *client, HWND notify, UINT message);
int openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count,
    const ChatProviderRouting *routing);
bool openrouter_cancel(OpenRouterClient *client, int generation);
void openrouter_event_free(OpenRouterEvent *event);
void openrouter_complete(OpenRouterClient *client, int generation);
void openrouter_shutdown(OpenRouterClient *client);

#endif
