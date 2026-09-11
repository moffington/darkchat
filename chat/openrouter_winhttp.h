#ifndef DARKCHAT_OPENROUTER_WINHTTP_H
#define DARKCHAT_OPENROUTER_WINHTTP_H

/* Worker-thread OpenRouter client (Pass 2: non-streaming). A worker builds the
   request body with json.c, POSTs it over WinHTTP to the chat-completions
   endpoint, and posts a heap-owned OpenRouterResult back to the notify window
   with CHAT_WM_OPENROUTER_RESULT (wparam = generation, lparam = result). The
   UI thread owns and frees every result it receives and drops results whose
   generation no longer matches the one openrouter_request returned. The API
   key is carried by value, zeroed after use, and never printed or persisted. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"

/* Distinct from UI_WM_ACCESSIBILITY_INVOKE (WM_APP + 0x4d). */
#define CHAT_WM_OPENROUTER_RESULT (WM_APP + 0x4e)

/* A snapshot of one conversation message. Text is borrowed: openrouter_request
   copies everything before it returns. */
typedef struct {
    ChatRole role;
    const wchar_t *text;
} OpenRouterMessage;

typedef struct {
    int generation;   /* the id openrouter_request returned */
    bool ok;          /* true: assistant content; false: error text */
    wchar_t *text;    /* heap-owned; release with openrouter_result_free */
} OpenRouterResult;

typedef struct OpenRouterClient {
    HWND notify;
    UINT message;
    HANDLE thread;            /* most recent in-flight worker */
    volatile LONG generation;
} OpenRouterClient;

void openrouter_init(OpenRouterClient *client, HWND notify, UINT message);
/* Launches one non-streaming chat-completion request on a worker thread.
   Returns the generation id, or 0 when the request cannot be started (another
   request is already in flight, or allocation failed). */
int openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count);
/* Frees a delivered result. Safe on NULL. */
void openrouter_result_free(OpenRouterResult *result);
/* Reaps the worker that delivered generation, allowing another request. */
void openrouter_complete(OpenRouterClient *client, int generation);
/* Joins an in-flight worker so the process never exits underneath it. */
void openrouter_shutdown(OpenRouterClient *client);

#endif
