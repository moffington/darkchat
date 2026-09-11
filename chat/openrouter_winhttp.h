#ifndef DARKCHAT_OPENROUTER_WINHTTP_H
#define DARKCHAT_OPENROUTER_WINHTTP_H

/* Worker-thread OpenRouter streaming client. Each posted event is heap-owned
   by the UI thread. Generation IDs let the host ignore stale queued deltas. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"

#define CHAT_WM_OPENROUTER_EVENT (WM_APP + 0x4e)

typedef struct { ChatRole role; const wchar_t *text; } OpenRouterMessage;

typedef enum {
    OPENROUTER_DELTA, OPENROUTER_DONE, OPENROUTER_ERROR,
    OPENROUTER_CANCELLED
} OpenRouterEventType;

typedef struct {
    int generation;
    OpenRouterEventType type;
    wchar_t *text;
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
    const wchar_t *model, const OpenRouterMessage *messages, int count);
bool openrouter_cancel(OpenRouterClient *client, int generation);
void openrouter_event_free(OpenRouterEvent *event);
void openrouter_complete(OpenRouterClient *client, int generation);
void openrouter_shutdown(OpenRouterClient *client);

#endif
