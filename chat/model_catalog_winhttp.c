#include "model_catalog_winhttp.h"
#include <winhttp.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_CATALOG_HOST L"openrouter.ai"
#define MODEL_CATALOG_PATH L"/api/v1/models"
#define MODEL_CATALOG_USER_AGENT L"DarkChat/0.4"
#define MODEL_CATALOG_MAX_RESPONSE (16u * 1024u * 1024u)

typedef struct {
    int generation;
    HWND notify;
    UINT message;
    ModelCatalogClient *client;
    wchar_t *headers;   /* owned; contains the bearer key */
} ModelCatalogWork;

/* Cancellation view handed to the transport so a seam can abandon an in-flight
   fetch when shutdown asks for it. */
typedef struct {
    const volatile LONG *cancelled;
    int generation;
} ModelCatalogCancel;

typedef bool (*ModelCatalogTransport)(const wchar_t *headers,
    const ModelCatalogCancel *cancel, char **body, size_t *length,
    DWORD *status, wchar_t **error);

static wchar_t *copy_wide(const wchar_t *text) {
    if (!text) return NULL;
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof *copy);
    if (copy) memcpy(copy, text, (length + 1) * sizeof *copy);
    return copy;
}

static bool cancelled(const ModelCatalogCancel *cancel) {
    if (!cancel || !cancel->cancelled) return false;
    return InterlockedCompareExchange((volatile LONG *)cancel->cancelled, 0, 0)
        == cancel->generation;
}

static bool winhttp_transport(const wchar_t *headers,
    const ModelCatalogCancel *cancel, char **body, size_t *length,
    DWORD *status, wchar_t **error);

/* Indirection so the worker suite can drive deterministic network states. */
static ModelCatalogTransport model_catalog_transport = winhttp_transport;

void model_catalog_client_init(ModelCatalogClient *client, HWND notify,
    UINT message) {
    if (!client) return;
    memset(client, 0, sizeof *client);
    client->notify = notify;
    client->message = message;
}

/* Reaps a worker that already exited but was never joined (for example when
   the completion event could not be allocated or posted, so no event will ever
   arrive to complete it). */
static void reap_finished(ModelCatalogClient *client) {
    if (client->thread &&
        WaitForSingleObject(client->thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(client->thread);
        client->thread = NULL;
    }
}

bool model_catalog_busy(ModelCatalogClient *client) {
    if (!client) return false;
    /* A finished worker must not read as busy forever; the host relies on this
       to retry a fetch whose completion was lost. */
    reap_finished(client);
    return client->thread != NULL;
}

static wchar_t *build_headers(const char *api_key_utf8) {
    size_t length = strlen(api_key_utf8);
    wchar_t *key = (wchar_t *)malloc((length + 1) * sizeof *key);
    if (!key) return NULL;
    int converted = MultiByteToWideChar(CP_UTF8, 0, api_key_utf8, -1, key,
        (int)(length + 1));
    if (converted <= 0) { free(key); return NULL; }
    static const wchar_t *const prefix =
        L"Accept: application/json\r\nAuthorization: Bearer ";
    static const wchar_t *const suffix =
        L"\r\nX-Title: DarkChat\r\nHTTP-Referer: https://localhost/darkchat\r\n";
    size_t total = wcslen(prefix) + (size_t)converted - 1 + wcslen(suffix);
    wchar_t *headers = (wchar_t *)malloc((total + 1) * sizeof *headers);
    if (headers) {
        wcscpy(headers, prefix);
        wcscat(headers, key);
        wcscat(headers, suffix);
    }
    SecureZeroMemory(key, (size_t)converted * sizeof *key);
    free(key);
    return headers;
}

static bool reserve(char **data, size_t *capacity, size_t needed) {
    if (needed <= *capacity) return true;
    size_t grown = *capacity ? *capacity : 4096;
    while (grown < needed) grown *= 2;
    char *next = (char *)realloc(*data, grown);
    if (!next) return false;
    *data = next;
    *capacity = grown;
    return true;
}

static bool winhttp_transport(const wchar_t *headers,
    const ModelCatalogCancel *cancel, char **body, size_t *length,
    DWORD *status, wchar_t **error) {
    *body = NULL; *length = 0; *status = 0; *error = NULL;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    bool ok = false;
    session = WinHttpOpen(MODEL_CATALOG_USER_AGENT,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = copy_wide(L"Could not open a network session."); goto done; }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    connect = WinHttpConnect(session, MODEL_CATALOG_HOST,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { *error = copy_wide(L"Could not connect to OpenRouter."); goto done; }
    request = WinHttpOpenRequest(connect, L"GET", MODEL_CATALOG_PATH, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { *error = copy_wide(L"Could not create the catalog request."); goto done; }
    if (!WinHttpSendRequest(request, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA,
        0, 0, 0)) {
        *error = copy_wide(L"Could not send the catalog request.");
        goto done;
    }
    if (cancelled(cancel)) goto done;
    if (!WinHttpReceiveResponse(request, NULL)) {
        *error = copy_wide(L"No response from OpenRouter for the catalog.");
        goto done;
    }
    DWORD code = 0, size = sizeof code;
    if (!WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, 0)) {
        *error = copy_wide(L"Could not read the catalog HTTP status.");
        goto done;
    }
    *status = code;
    if (code == 200) {
        char *data = NULL;
        size_t capacity = 0, used = 0;
        for (;;) {
            if (cancelled(cancel)) { free(data); return false; }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                free(data);
                *error = copy_wide(L"Could not read the catalog response.");
                goto done;
            }
            if (!available) break;
            if (used + available + 1 > MODEL_CATALOG_MAX_RESPONSE) {
                /* The caller validates length before reading the body. */
                free(data);
                *length = used + available;
                ok = true;
                goto done;
            }
            if (!reserve(&data, &capacity, used + available + 1)) {
                free(data);
                *error = copy_wide(L"Out of memory reading the catalog.");
                goto done;
            }
            DWORD read = 0;
            if (!WinHttpReadData(request, data + used, available, &read)) {
                free(data);
                *error = copy_wide(L"Could not read the catalog response.");
                goto done;
            }
            used += read;
            data[used] = 0;
            if (!read) break;
        }
        *body = data;
        *length = used;
        ok = true;
    } else {
        ok = true;
    }
done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

static void free_work(ModelCatalogWork *work) {
    if (!work) return;
    if (work->headers) {
        SecureZeroMemory(work->headers, wcslen(work->headers) * sizeof(wchar_t));
        free(work->headers);
    }
    free(work);
}

void model_catalog_event_free(ModelCatalogEvent *event) {
    if (!event) return;
    free(event->json);
    free(event->error);
    free(event);
}

static void post_result(ModelCatalogWork *work, ModelCatalogResult result,
    char *json, wchar_t *error) {
    ModelCatalogEvent *event = (ModelCatalogEvent *)calloc(1, sizeof *event);
    if (!event) { free(json); free(error); return; }
    event->generation = work->generation;
    event->result = result;
    event->json = json;
    event->error = error;
    if (!PostMessageW(work->notify, work->message, (WPARAM)work->generation,
        (LPARAM)event))
        model_catalog_event_free(event);
}

static unsigned __stdcall worker(void *parameter) {
    ModelCatalogWork *work = (ModelCatalogWork *)parameter;
    ModelCatalogCancel cancel = { &work->client->cancelled_generation,
        work->generation };
    char *body = NULL, *json = NULL;
    size_t length = 0;
    DWORD status = 0;
    wchar_t *error = NULL, *final_error = NULL;
    ModelCatalogResult result = MODEL_CATALOG_NETWORK_ERROR;
    bool ok = model_catalog_transport(work->headers, &cancel, &body, &length,
        &status, &error);
    if (!ok) {
        if (cancelled(&cancel)) { free(error); free(body); free_work(work); return 0; }
        final_error = error ? error : copy_wide(L"Model catalog request failed.");
        error = NULL;
    } else if (status != 200) {
        wchar_t composed[128];
        swprintf(composed, 128, L"OpenRouter returned HTTP %lu for the catalog.",
            (unsigned long)status);
        final_error = copy_wide(composed);
        result = MODEL_CATALOG_HTTP_ERROR;
        free(body);
    } else if (length > MODEL_CATALOG_MAX_RESPONSE) {
        final_error = copy_wide(L"The catalog response exceeded the 16 MB limit.");
        result = MODEL_CATALOG_TOO_LARGE;
        free(body);
    } else {
        json = body;
        result = MODEL_CATALOG_OK;
    }
    free(error);
    post_result(work, result, json, final_error);
    free_work(work);
    return 0;
}

int model_catalog_request(ModelCatalogClient *client, const char *api_key_utf8) {
    if (!client || !client->notify) return 0;
    if (!api_key_utf8 || !api_key_utf8[0]) return 0;
    reap_finished(client);
    if (client->thread) return 0;
    int generation = (int)InterlockedIncrement((LONG *)&client->generation);
    InterlockedExchange(&client->cancelled_generation, 0);
    ModelCatalogWork *work = (ModelCatalogWork *)calloc(1, sizeof *work);
    if (!work) return 0;
    work->generation = generation;
    work->notify = client->notify;
    work->message = client->message;
    work->client = client;
    work->headers = build_headers(api_key_utf8);
    if (work->headers) {
        uintptr_t thread = _beginthreadex(NULL, 0, worker, work, 0, NULL);
        if (thread) {
            client->thread = (HANDLE)thread;
            return generation;
        }
    }
    free_work(work);
    return 0;
}

void model_catalog_complete(ModelCatalogClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return;
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}

void model_catalog_shutdown(ModelCatalogClient *client) {
    if (!client || !client->thread) return;
    int generation = client->generation;
    InterlockedExchange(&client->cancelled_generation, generation);
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}
