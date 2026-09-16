#ifndef DARKCHAT_MODEL_CATALOG_WINHTTP_H
#define DARKCHAT_MODEL_CATALOG_WINHTTP_H

/* Worker-thread fetch of the OpenRouter model catalog. One fetch is in flight
   at a time; the completion event is heap-owned by the UI thread and carries
   the generation it belongs to, so a stale completion can never replace newer
   state. The API key is embedded in an owned header buffer and cleared before
   release, exactly like the streaming request worker. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include "model_catalog.h"

#define CHAT_WM_CATALOG_EVENT (WM_APP + 0x50)

typedef enum {
    MODEL_CATALOG_OK,
    MODEL_CATALOG_NO_KEY,
    MODEL_CATALOG_HTTP_ERROR,
    MODEL_CATALOG_NETWORK_ERROR,
    MODEL_CATALOG_TOO_LARGE,
    MODEL_CATALOG_OOM
} ModelCatalogResult;

typedef struct {
    int generation;
    ModelCatalogResult result;
    char *json;       /* owned; non-NULL only for MODEL_CATALOG_OK */
    wchar_t *error;   /* owned; non-NULL for failure results */
} ModelCatalogEvent;

typedef struct {
    HWND notify;
    UINT message;
    HANDLE thread;
    int generation;
    volatile LONG cancelled_generation;
} ModelCatalogClient;

void model_catalog_client_init(ModelCatalogClient *client, HWND notify,
    UINT message);
/* True while a fetch is in flight. Reaps an already-exited worker, so a lost
   completion does not read as busy forever. */
bool model_catalog_busy(ModelCatalogClient *client);
/* Starts a fetch when a key is present and none is in flight. Returns the
   generation (> 0) or 0. */
int model_catalog_request(ModelCatalogClient *client, const char *api_key_utf8);
/* Joins the worker for `generation`; safe when it already finished. */
void model_catalog_complete(ModelCatalogClient *client, int generation);
/* Cancels an in-flight fetch and joins the worker before returning. */
void model_catalog_shutdown(ModelCatalogClient *client);
void model_catalog_event_free(ModelCatalogEvent *event);

#endif
