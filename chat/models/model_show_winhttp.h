#ifndef DARKCHAT_MODEL_SHOW_WINHTTP_H
#define DARKCHAT_MODEL_SHOW_WINHTTP_H

/* Ollama capability probe: one `POST /api/show` per model, asked on demand to
   learn whether a local model accepts image input. The OpenRouter-compatible
   `GET /v1/models` payload carries no capability field, so this endpoint is
   the only honest source for Ollama -- which is also why there is no backend
   parameter here and why the endpoint can never carry credentials: it is a
   loopback address, and no API key is ever sent to it.

   One probe is in flight at a time. The completion event is heap-owned by the
   UI thread and carries the id it belongs to plus the generation it was
   started with, so a stale completion can never be read as an answer about
   the current model. The event carries the *classified* capability rather than
   the response body: a show document is dominated by `model_info` the client
   has no use for, and classification is a pure function the worker can run
   itself (chat_model_capability_from_show).

   Every failure -- transport, HTTP status, oversize body, a body that does not
   classify, a lost post, cancellation -- leaves the reported capability
   CHAT_CAP_UNKNOWN. That is the whole safety contract of this module: a
   failed probe must never be able to produce CHAT_CAP_NO, which downstream
   would treat as a proven refusal. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include "chat/models/model_catalog.h"

/* Free slot after UI_WM_ACCESSIBILITY_INVOKE (0x4d), CHAT_WM_COMPLETION_EVENT
   (0x4e), CHAT_WM_SAVER_RESULT (0x4f) and CHAT_WM_CATALOG_EVENT (0x50). */
#define CHAT_WM_MODEL_SHOW_EVENT (WM_APP + 0x51)

typedef enum {
    MODEL_SHOW_OK,
    MODEL_SHOW_HTTP_ERROR,
    MODEL_SHOW_NETWORK_ERROR,
    MODEL_SHOW_TOO_LARGE
} ModelShowResult;

typedef struct {
    int generation;
    ModelShowResult result;
    wchar_t *id;               /* owned; the model this result is about */
    ChatCapability capability; /* meaningful only for MODEL_SHOW_OK */
    wchar_t *error;            /* owned; non-NULL for every failure result */
} ModelShowEvent;

typedef struct {
    HWND notify;
    UINT message;
    HANDLE thread;
    int generation;
    volatile LONG cancelled_generation;
} ModelShowClient;

void model_show_client_init(ModelShowClient *client, HWND notify,
    UINT message);
/* True while a probe is in flight. Reaps an already-exited worker, so a lost
   completion does not read as busy forever. */
bool model_show_busy(ModelShowClient *client);
/* Starts one probe for `model_id`, which must be a non-empty id that fits
   CHAT_MODEL_TEXT. A second request while one is in flight is refused rather
   than queued or restarted: the transport only observes cancellation between
   chunks, so cancelling and joining in place would block the caller for up to
   the receive timeout. The caller re-drives after a completion, exactly as it
   re-drives a catalog fetch. Returns the generation (> 0) or 0. */
int model_show_request(ModelShowClient *client, const wchar_t *model_id);
/* Joins the worker for `generation`; safe when it already finished. */
void model_show_complete(ModelShowClient *client, int generation);
/* Cancels an in-flight probe and joins the worker before returning. */
void model_show_shutdown(ModelShowClient *client);
void model_show_event_free(ModelShowEvent *event);

#endif
