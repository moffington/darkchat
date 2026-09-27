#include "chat/models/model_show_winhttp.h"
#include "chat/json.h"
#include <winhttp.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one endpoint this module talks to: loopback, no TLS, no proxy, no
   credentials. Mirrors the completion and catalog clients' timeouts. */
typedef struct {
    const wchar_t *host;
    INTERNET_PORT port;
    const wchar_t *path;
    const wchar_t *user_agent;
} ModelShowEndpoint;

static const ModelShowEndpoint MODEL_SHOW_ENDPOINT = {
    L"localhost", 11434, L"/api/show", L"DarkChat/0.4"
};

/* Same ceiling as the catalog worker: /api/show answers with a `model_info`
   dump, so it is larger than a catalog entry but still far below this. */
#define MODEL_SHOW_MAX_RESPONSE (16u * 1024u * 1024u)

typedef struct {
    int generation;
    HWND notify;
    UINT message;
    ModelShowClient *client;
    wchar_t *model;      /* owned; the id this probe is about */
    char *body;          /* owned; the encoded request body */
    size_t body_length;
} ModelShowWork;

/* Cancellation view handed to the transport, the same interlocked generation
   check the catalog worker uses. */
typedef struct {
    const volatile LONG *cancelled;
    int generation;
} ModelShowCancel;

typedef bool (*ModelShowTransport)(const ModelShowEndpoint *endpoint,
    const wchar_t *headers, const void *body, size_t body_length,
    const ModelShowCancel *cancel, char **out_body, size_t *out_length,
    DWORD *status, wchar_t **error);

static wchar_t *copy_wide(const wchar_t *text) {
    if (!text) return NULL;
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof *copy);
    if (copy) memcpy(copy, text, (length + 1) * sizeof *copy);
    return copy;
}

static bool cancelled(const ModelShowCancel *cancel) {
    if (!cancel || !cancel->cancelled) return false;
    return InterlockedCompareExchange((volatile LONG *)cancel->cancelled, 0, 0)
        == cancel->generation;
}

static bool winhttp_transport(const ModelShowEndpoint *endpoint,
    const wchar_t *headers, const void *body, size_t body_length,
    const ModelShowCancel *cancel, char **out_body, size_t *out_length,
    DWORD *status, wchar_t **error);

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

static bool winhttp_transport(const ModelShowEndpoint *endpoint,
    const wchar_t *headers, const void *body, size_t body_length,
    const ModelShowCancel *cancel, char **out_body, size_t *out_length,
    DWORD *status, wchar_t **error) {
    *out_body = NULL; *out_length = 0; *status = 0; *error = NULL;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    /* Owned by this function until the success path hands it to *out_body, so
       that every exit -- including a cancellation part-way through the body --
       releases the buffer and the three WinHTTP handles through `done`. */
    char *data = NULL;
    size_t capacity = 0, used = 0;
    bool ok = false;
    session = WinHttpOpen(endpoint->user_agent, WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = copy_wide(L"Could not open a network session."); goto done; }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    connect = WinHttpConnect(session, endpoint->host, endpoint->port, 0);
    if (!connect) {
        *error = copy_wide(L"Ollama is not reachable at localhost:11434.");
        goto done;
    }
    request = WinHttpOpenRequest(connect, L"POST", endpoint->path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) { *error = copy_wide(L"Could not create the model capability request."); goto done; }
    /* WinHTTP declares the optional body non-const but does not write to it;
       the request body is const here so no caller can mutate it in flight. */
    if (!WinHttpSendRequest(request, headers, (DWORD)-1, (LPVOID)body,
        (DWORD)body_length, (DWORD)body_length, 0)) {
        *error = copy_wide(L"Could not send the model capability request.");
        goto done;
    }
    if (cancelled(cancel)) goto done;
    if (!WinHttpReceiveResponse(request, NULL)) {
        *error = copy_wide(L"Ollama is not reachable at localhost:11434.");
        goto done;
    }
    DWORD code = 0, size = sizeof code;
    if (!WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, 0)) {
        *error = copy_wide(L"Could not read the model capability HTTP status.");
        goto done;
    }
    *status = code;
    if (code == 200) {
        for (;;) {
            /* A cancelled probe abandons the body it has read so far; the
               handles and the partial buffer are released by `done` like every
               other exit. */
            if (cancelled(cancel)) goto done;
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                *error = copy_wide(L"Could not read the model capability response.");
                goto done;
            }
            if (!available) break;
            if (used + available + 1 > MODEL_SHOW_MAX_RESPONSE) {
                /* The caller validates length before reading the body. */
                *out_length = used + available;
                ok = true;
                goto done;
            }
            if (!reserve(&data, &capacity, used + available + 1)) {
                *error = copy_wide(L"Out of memory reading the model capability response.");
                goto done;
            }
            DWORD read = 0;
            if (!WinHttpReadData(request, data + used, available, &read)) {
                *error = copy_wide(L"Could not read the model capability response.");
                goto done;
            }
            used += read;
            data[used] = 0;
            if (!read) break;
        }
        *out_body = data;
        data = NULL;  /* ownership transferred to the caller */
        *out_length = used;
        ok = true;
    } else {
        ok = true;
    }
done:
    free(data);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

/* Indirection so the worker suite can drive deterministic network states. */
static ModelShowTransport model_show_transport = winhttp_transport;

void model_show_client_init(ModelShowClient *client, HWND notify,
    UINT message) {
    if (!client) return;
    memset(client, 0, sizeof *client);
    client->notify = notify;
    client->message = message;
}

/* Reaps a worker that already exited but was never joined (for example when
   the completion event could not be allocated or posted, so no event will ever
   arrive to complete it). */
static void reap_finished(ModelShowClient *client) {
    if (client->thread &&
        WaitForSingleObject(client->thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(client->thread);
        client->thread = NULL;
    }
}

bool model_show_busy(ModelShowClient *client) {
    if (!client) return false;
    reap_finished(client);
    return client->thread != NULL;
}

/* The request body is the one shape Ollama documents. Built through the shared
   encoder so an id containing a quote, a backslash or non-ASCII cannot produce
   a malformed request. Ownership of buf.data transfers to the caller. */
static char *build_show_body(const wchar_t *model_id, size_t *out_length) {
    JsonBuf buf;
    json_buf_init(&buf, 0);
    if (!json_buf_append_raw(&buf, "{\"model\":", 9) ||
        !json_buf_append_json_string(&buf, model_id) ||
        !json_buf_append_raw(&buf, "}", 1)) {
        json_buf_free(&buf);
        return NULL;
    }
    *out_length = buf.length;
    return buf.data;
}

static void free_work(ModelShowWork *work) {
    if (!work) return;
    free(work->model);
    free(work->body);
    free(work);
}

void model_show_event_free(ModelShowEvent *event) {
    if (!event) return;
    free(event->id);
    free(event->error);
    free(event);
}

/* No credentials, ever: this request never leaves the loopback interface. */
static const wchar_t *const SHOW_HEADERS =
    L"Accept: application/json\r\nContent-Type: application/json\r\n";

/* Consumes `response` and `error` either way. Only a classified answer may
   leave the worker as anything but UNKNOWN: every failure path reaches here
   with result != MODEL_SHOW_OK, and a 200 whose body does not classify is an
   OK result carrying UNKNOWN rather than a transport failure. */
static void post_result(ModelShowWork *work, ModelShowResult result,
    char *response, wchar_t *error) {
    ModelShowEvent *event = (ModelShowEvent *)calloc(1, sizeof *event);
    if (!event) { free(response); free(error); return; }
    event->generation = work->generation;
    event->result = result;
    event->id = work->model;
    event->capability = (result == MODEL_SHOW_OK && response)
        ? chat_model_capability_from_show(response) : CHAT_CAP_UNKNOWN;
    event->error = error;
    work->model = NULL;  /* ownership moved into the event */
    free(response);
    if (!PostMessageW(work->notify, work->message, (WPARAM)work->generation,
        (LPARAM)event))
        model_show_event_free(event);
}

static unsigned __stdcall worker(void *parameter) {
    ModelShowWork *work = (ModelShowWork *)parameter;
    ModelShowCancel cancel = { &work->client->cancelled_generation,
        work->generation };
    char *body = NULL;
    size_t length = 0;
    DWORD status = 0;
    wchar_t *error = NULL, *final_error = NULL;
    ModelShowResult result = MODEL_SHOW_NETWORK_ERROR;
    bool ok = model_show_transport(&MODEL_SHOW_ENDPOINT, SHOW_HEADERS,
        work->body, work->body_length, &cancel, &body, &length, &status,
        &error);
    if (!ok) {
        if (cancelled(&cancel)) { free(error); free(body); free_work(work); return 0; }
        final_error = error ? error : copy_wide(L"Model capability request failed.");
        error = NULL;
    } else if (status != 200) {
        wchar_t composed[128];
        swprintf(composed, 128, L"Ollama returned HTTP %lu for %ls.",
            (unsigned long)status, work->model);
        final_error = copy_wide(composed);
        result = MODEL_SHOW_HTTP_ERROR;
        free(body);
    } else if (length > MODEL_SHOW_MAX_RESPONSE) {
        final_error = copy_wide(L"The model capability response exceeded the 16 MB limit.");
        result = MODEL_SHOW_TOO_LARGE;
        free(body);
    } else {
        result = MODEL_SHOW_OK;
    }
    free(error);
    post_result(work, result, body, final_error);
    free_work(work);
    return 0;
}

int model_show_request(ModelShowClient *client, const wchar_t *model_id) {
    if (!client || !client->notify) return 0;
    /* An empty or over-long id can never name a model, and neither should be
       sent: the model field bound is the same one the catalog enforces. */
    if (!model_id || !model_id[0] || wcslen(model_id) >= CHAT_MODEL_TEXT)
        return 0;
    reap_finished(client);
    if (client->thread) return 0;
    int generation = (int)InterlockedIncrement((LONG *)&client->generation);
    InterlockedExchange(&client->cancelled_generation, 0);
    ModelShowWork *work = (ModelShowWork *)calloc(1, sizeof *work);
    if (!work) return 0;
    work->generation = generation;
    work->notify = client->notify;
    work->message = client->message;
    work->client = client;
    work->model = copy_wide(model_id);
    work->body = work->model
        ? build_show_body(model_id, &work->body_length) : NULL;
    if (work->model && work->body) {
        uintptr_t thread = _beginthreadex(NULL, 0, worker, work, 0, NULL);
        if (thread) {
            client->thread = (HANDLE)thread;
            return generation;
        }
    }
    free_work(work);
    return 0;
}

void model_show_complete(ModelShowClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return;
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}

void model_show_shutdown(ModelShowClient *client) {
    if (!client || !client->thread) return;
    int generation = client->generation;
    InterlockedExchange(&client->cancelled_generation, generation);
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}
