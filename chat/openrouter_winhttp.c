#include "openrouter_winhttp.h"
#include "json.h"
#include "sse.h"
#include <winhttp.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define OPENROUTER_HOST L"openrouter.ai"
#define OPENROUTER_PATH L"/api/v1/chat/completions"
#define OPENROUTER_USER_AGENT L"DarkChat/0.3"
#define OPENROUTER_MAX_RESPONSE (16u * 1024u * 1024u)

typedef struct {
    int generation;
    HWND notify;
    UINT message;
    OpenRouterClient *client;
    char *api_key;
    wchar_t *model;
    ChatRole *roles;
    wchar_t **texts;
    int count;
    ChatGeneration metadata;
    ULONGLONG started_tick;
} OpenRouterWork;

typedef struct { char *data; size_t length, capacity; } Response;
typedef struct {
    HANDLE event;
    volatile LONG status;
    DWORD error;
    DWORD bytes;
} AsyncState;
typedef struct {
    OpenRouterWork *work;
    bool done, failed;
    wchar_t *error;
} Stream;
typedef enum { REQUEST_DONE, REQUEST_ERROR, REQUEST_CANCELLED, REQUEST_INTERRUPTED } RequestOutcome;

static wchar_t *copy_wide(const wchar_t *text) {
    if (!text) return NULL;
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof *copy);
    if (copy) memcpy(copy, text, (length + 1) * sizeof *copy);
    return copy;
}

void openrouter_init(OpenRouterClient *client, HWND notify, UINT message) {
    if (!client) return;
    memset(client, 0, sizeof *client);
    client->notify = notify;
    client->message = message;
}

void openrouter_event_free(OpenRouterEvent *event) {
    if (!event) return;
    free(event->text);
    free(event);
}

static bool cancelled(const OpenRouterWork *work) {
    return InterlockedCompareExchange(&work->client->cancelled_generation,
        0, 0) == work->generation;
}

static bool post_event(OpenRouterWork *work, OpenRouterEventType type,
    wchar_t *owned_text) {
    if (type == OPENROUTER_DELTA && cancelled(work)) {
        free(owned_text);
        return false;
    }
    OpenRouterEvent *event = (OpenRouterEvent *)calloc(1, sizeof *event);
    if (!event) { free(owned_text); return false; }
    event->generation = work->generation;
    event->type = type;
    event->text = owned_text;
    event->metadata = work->metadata;
    if (!PostMessageW(work->notify, work->message, (WPARAM)work->generation,
        (LPARAM)event)) {
        openrouter_event_free(event);
        return false;
    }
    return true;
}

static const char *role_name(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_ASSISTANT: return "assistant";
    case CHAT_ROLE_SYSTEM: return "system";
    default: return "user";
    }
}

static bool build_request(const OpenRouterWork *work, JsonBuf *body) {
    json_buf_init(body, 8192);
    if (!json_buf_append_raw(body, "{\"model\":", 9) ||
        !json_buf_append_json_string(body, work->model) ||
        !json_buf_append_raw(body, ",\"messages\":[", 13)) return false;
    bool first = true;
    for (int i = 0; i < work->count; i++) {
        if (work->roles[i] == CHAT_ROLE_ERROR) continue;
        if (!first && !json_buf_append_raw(body, ",", 1)) return false;
        first = false;
        const char *role = role_name(work->roles[i]);
        if (!json_buf_append_raw(body, "{\"role\":\"", 9) ||
            !json_buf_append_raw(body, role, strlen(role)) ||
            !json_buf_append_raw(body, "\",\"content\":", 12) ||
            !json_buf_append_json_string(body, work->texts[i]) ||
            !json_buf_append_raw(body, "}", 1)) return false;
    }
    return json_buf_append_raw(body, "],\"stream\":true}", 16);
}

static wchar_t *build_headers(const char *api_key) {
    size_t length = strlen(api_key);
    wchar_t *key = (wchar_t *)malloc((length + 1) * sizeof *key);
    if (!key) return NULL;
    int converted = MultiByteToWideChar(CP_UTF8, 0, api_key, -1, key,
        (int)(length + 1));
    if (converted <= 0) { free(key); return NULL; }
    static const wchar_t *const prefix =
        L"Content-Type: application/json\r\nAccept: text/event-stream\r\n"
        L"Authorization: Bearer ";
    static const wchar_t *const suffix =
        L"\r\nX-Title: DarkChat\r\nHTTP-Referer: https://localhost/darkchat\r\n";
    size_t total = wcslen(prefix) + (size_t)converted - 1 + wcslen(suffix);
    wchar_t *headers = (wchar_t *)malloc((total + 1) * sizeof *headers);
    if (headers) {
        wcscpy(headers, prefix); wcscat(headers, key); wcscat(headers, suffix);
    }
    SecureZeroMemory(key, (size_t)converted * sizeof *key);
    free(key);
    return headers;
}

static bool response_reserve(Response *response, size_t extra) {
    if (response->length + extra + 1 <= response->capacity) return true;
    size_t capacity = response->capacity ? response->capacity : 4096;
    while (capacity < response->length + extra + 1) capacity *= 2;
    char *data = (char *)realloc(response->data, capacity);
    if (!data) return false;
    response->data = data;
    response->capacity = capacity;
    return true;
}

static void CALLBACK winhttp_callback(HINTERNET handle, DWORD_PTR context,
    DWORD status, LPVOID information, DWORD information_length) {
    (void)handle;
    AsyncState *state = (AsyncState *)context;
    if (!state) return;
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && information &&
        information_length >= sizeof(WINHTTP_ASYNC_RESULT)) {
        state->error = ((WINHTTP_ASYNC_RESULT *)information)->dwError;
    } else if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && information &&
        information_length >= sizeof(DWORD)) {
        state->bytes = *(DWORD *)information;
    } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        state->bytes = information_length;
    }
    InterlockedExchange(&state->status, (LONG)status);
    SetEvent(state->event);
}

static bool wait_status(OpenRouterWork *work, AsyncState *state,
    DWORD expected) {
    for (;;) {
        DWORD waited = WaitForSingleObject(state->event, 100);
        if (waited == WAIT_OBJECT_0) {
            DWORD status = (DWORD)state->status;
            if (status == expected) return true;
            if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return false;
            ResetEvent(state->event);
        } else if (waited != WAIT_TIMEOUT) {
            state->error = GetLastError();
            return false;
        }
        if (cancelled(work)) return false;
    }
}

static void begin_async(AsyncState *state) {
    state->error = ERROR_SUCCESS;
    state->bytes = 0;
    InterlockedExchange(&state->status, 0);
    ResetEvent(state->event);
}

static bool async_started(BOOL result, AsyncState *state) {
    if (result) return true;
    state->error = GetLastError();
    return state->error == ERROR_IO_PENDING;
}

static void close_request(HINTERNET request, AsyncState *state,
    bool context_set) {
    if (!request) return;
    if (!context_set) { WinHttpCloseHandle(request); return; }
    begin_async(state);
    WinHttpCloseHandle(request);
    while ((DWORD)state->status != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        WaitForSingleObject(state->event, INFINITE);
        if ((DWORD)state->status != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
            ResetEvent(state->event);
    }
}

static wchar_t *http_error(DWORD status, const char *body) {
    wchar_t *detail = NULL;
    if (body) {
        size_t size = strlen(body) + 1;
        char *scratch = (char *)malloc(size);
        if (scratch && json_query_string(body, "error.message", scratch, size))
            detail = json_utf8_to_utf16(scratch, strlen(scratch));
        free(scratch);
    }
    wchar_t composed[512];
    if (detail) {
        _snwprintf(composed, 512, L"OpenRouter returned HTTP %lu: %ls",
            (unsigned long)status, detail);
        free(detail);
    } else {
        _snwprintf(composed, 512, L"OpenRouter returned HTTP %lu.",
            (unsigned long)status);
    }
    composed[511] = 0;
    return copy_wide(composed);
}

static bool stream_event(void *user, const char *data, size_t length) {
    Stream *stream = (Stream *)user;
    if (cancelled(stream->work)) return false;
    if (length == 6 && memcmp(data, "[DONE]", 6) == 0) {
        stream->done = true;
        return true;
    }
    char *json = (char *)malloc(length + 1);
    char *decoded = (char *)malloc(length + 1);
    if (!json || !decoded) {
        free(json); free(decoded);
        stream->error = copy_wide(L"Out of memory decoding the stream.");
        stream->failed = true;
        return false;
    }
    memcpy(json, data, length); json[length] = 0;
    if (!json_validate(json)) {
        free(json); free(decoded);
        stream->error = copy_wide(L"Malformed JSON in stream.");
        stream->failed = true;
        return false;
    }
    ChatGeneration *g = &stream->work->metadata;
    if (json_query_string(json, "model", decoded, length + 1)) {
        wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
        if (wide) { wcsncpy(g->actual_model, wide, CHAT_MODEL_TEXT - 1); free(wide); }
    }
    if (json_query_string(json, "choices[0].finish_reason", decoded, length + 1)) {
        wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
        if (wide) { wcsncpy(g->finish_reason, wide, 63); free(wide); }
    }
    double value;
#define USAGE(field) if (json_query_number(json, "usage." #field, &value) && value >= 0) g->field = value
    USAGE(prompt_tokens); USAGE(completion_tokens); USAGE(total_tokens); USAGE(cost);
#undef USAGE

    if (json_query_string(json, "choices[0].delta.content", decoded,
        length + 1)) {
        if (decoded[0] && !g->first_token_at) {
            g->first_token_at = chat_now();
            g->ttft_ms = (double)(GetTickCount64() - stream->work->started_tick);
        }
        wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
        if (!wide || !post_event(stream->work, OPENROUTER_DELTA, wide)) {
            if (!cancelled(stream->work))
                stream->error = copy_wide(L"Could not deliver streamed text.");
            stream->failed = true;
        }
    }
    if (json_query_string(json, "error.message", decoded, length + 1)) {
        stream->error = json_utf8_to_utf16(decoded, strlen(decoded));
        stream->failed = true;
    }
    free(decoded); free(json);
    return !stream->failed;
}

static RequestOutcome perform(OpenRouterWork *work, wchar_t **error) {
    JsonBuf body;
    wchar_t *headers = NULL;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    AsyncState async = {0};
    bool request_context_set = false;
    Response response = {0};
    SseParser parser;
    Stream stream = {work, false, false, NULL};
    DWORD winhttp_error = ERROR_SUCCESS, status = 0;
    RequestOutcome outcome = REQUEST_ERROR;
    sse_init(&parser);
    async.event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!async.event) {
        *error = copy_wide(L"DarkChat could not create a network event.");
        sse_dispose(&parser);
        return outcome;
    }
    if (!build_request(work, &body)) {
        *error = copy_wide(L"DarkChat could not encode the request.");
        json_buf_free(&body); sse_dispose(&parser);
        CloseHandle(async.event);
        return outcome;
    }
    headers = build_headers(work->api_key);
    if (!headers) {
        *error = copy_wide(L"DarkChat could not build request headers.");
        goto cleanup;
    }
    session = WinHttpOpen(OPENROUTER_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session) { winhttp_error = GetLastError(); goto network_error; }
    if (WinHttpSetStatusCallback(session, winhttp_callback,
        WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
        WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
        WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE |
        WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
        WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
        WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        winhttp_error = GetLastError(); goto network_error;
    }
    connect = WinHttpConnect(session, OPENROUTER_HOST,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { winhttp_error = GetLastError(); goto network_error; }
    request = WinHttpOpenRequest(connect, L"POST", OPENROUTER_PATH, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { winhttp_error = GetLastError(); goto network_error; }
    DWORD_PTR context = (DWORD_PTR)&async;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context,
        sizeof context)) { winhttp_error = GetLastError(); goto network_error; }
    request_context_set = true;
    if (cancelled(work)) goto was_cancelled;
    WinHttpSetTimeouts(request, 15000, 15000, 30000, 120000);
    begin_async(&async);
    if (!async_started(WinHttpSendRequest(request, headers, (DWORD)-1,
        (LPVOID)body.data, (DWORD)body.length, (DWORD)body.length, context),
        &async) || !wait_status(work, &async,
            WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) {
        winhttp_error = async.error; goto network_or_cancel;
    }
    begin_async(&async);
    if (!async_started(WinHttpReceiveResponse(request, NULL), &async) ||
        !wait_status(work, &async, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) {
        winhttp_error = async.error; goto network_or_cancel;
    }
    DWORD status_size = sizeof status;
    if (!WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, 0)) {
        winhttp_error = GetLastError(); goto network_error;
    }
    for (;;) {
        if (cancelled(work)) goto was_cancelled;
        begin_async(&async);
        if (!async_started(WinHttpQueryDataAvailable(request, NULL), &async) ||
            !wait_status(work, &async,
                WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)) {
            winhttp_error = async.error; goto network_or_cancel;
        }
        DWORD available = async.bytes;
        if (!available) break;
        if (response.length + available > OPENROUTER_MAX_RESPONSE) {
            *error = copy_wide(L"The OpenRouter response exceeded the 16 MB limit.");
            goto cleanup;
        }
        if (!response_reserve(&response, available)) {
            *error = copy_wide(L"Out of memory reading the OpenRouter response.");
            goto cleanup;
        }
        begin_async(&async);
        if (!async_started(WinHttpReadData(request,
            response.data + response.length, available, NULL), &async) ||
            !wait_status(work, &async, WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) {
            winhttp_error = async.error; goto network_or_cancel;
        }
        DWORD read = async.bytes;
        if (!read) break;
        if (status == 200 || status == 201) {
            if (!sse_feed(&parser, response.data + response.length, read,
                stream_event, &stream)) {
                if (cancelled(work)) goto was_cancelled;
                *error = stream.error ? stream.error :
                    copy_wide(L"The streaming response was malformed.");
                stream.error = NULL;
                goto cleanup;
            }
        }
        response.length += read;
        response.data[response.length] = 0;
    }
    if (status != 200 && status != 201) {
        *error = http_error(status, response.data ? response.data : "");
        goto cleanup;
    }
    if (!sse_finish(&parser, stream_event, &stream) || stream.failed) {
        if (cancelled(work)) goto was_cancelled;
        *error = stream.error ? stream.error :
            copy_wide(L"The streaming response was malformed.");
        stream.error = NULL;
        goto cleanup;
    }
    if (!stream.done) {
        outcome = REQUEST_INTERRUPTED;
        *error = copy_wide(L"The stream ended before OpenRouter sent [DONE].");
        goto cleanup;
    }
    outcome = REQUEST_DONE;
    goto cleanup;
network_or_cancel:
    if (cancelled(work)) goto was_cancelled;
    goto network_error;
network_error:
    if (cancelled(work)) goto was_cancelled;
    if (work->metadata.first_token_at) outcome = REQUEST_INTERRUPTED;
    {
        wchar_t text[160];
        swprintf(text, 160, L"Network request failed (WinHTTP error %lu).",
            (unsigned long)winhttp_error);
        *error = copy_wide(text);
    }
    goto cleanup;
was_cancelled:
    outcome = REQUEST_CANCELLED;
cleanup:
    free(stream.error);
    sse_dispose(&parser);
    free(response.data);
    close_request(request, &async, request_context_set);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    if (async.event) CloseHandle(async.event);
    if (headers) {
        SecureZeroMemory(headers, wcslen(headers) * sizeof *headers);
        free(headers);
    }
    json_buf_free(&body);
    return outcome;
}

static void free_work(OpenRouterWork *work) {
    if (!work) return;
    if (work->api_key) {
        SecureZeroMemory(work->api_key, strlen(work->api_key));
        free(work->api_key);
    }
    free(work->model);
    if (work->texts) for (int i = 0; i < work->count; i++) free(work->texts[i]);
    free(work->texts); free(work->roles); free(work);
}

static unsigned __stdcall worker(void *parameter) {
    OpenRouterWork *work = (OpenRouterWork *)parameter;
    wchar_t *error = NULL;
    RequestOutcome outcome = perform(work, &error);
    OpenRouterEventType type = outcome == REQUEST_DONE ? OPENROUTER_DONE :
        outcome == REQUEST_CANCELLED ? OPENROUTER_CANCELLED :
        outcome == REQUEST_INTERRUPTED ? OPENROUTER_INTERRUPTED : OPENROUTER_ERROR;
    work->metadata.finished_at = chat_now();
    work->metadata.latency_ms = (double)(GetTickCount64() - work->started_tick);
    post_event(work, type, error);
    free_work(work);
    return 0;
}

int openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count) {
    if (!client || !client->notify || !api_key_utf8 || !api_key_utf8[0] ||
        !model || !model[0] || count < 0 || (count > 0 && !messages) ||
        client->thread) return 0;
    LONG generation = InterlockedIncrement(&client->generation);
    InterlockedExchange(&client->cancelled_generation, 0);
    OpenRouterWork *work = (OpenRouterWork *)calloc(1, sizeof *work);
    if (!work) return 0;
    chat_generation_init(&work->metadata);
    work->metadata.started_at = chat_now();
    work->started_tick = GetTickCount64();
    wcsncpy(work->metadata.requested_model, model, CHAT_MODEL_TEXT - 1);
    work->generation = (int)generation;
    work->notify = client->notify;
    work->message = client->message;
    work->client = client;
    work->count = count;
    size_t key_length = strlen(api_key_utf8);
    work->api_key = (char *)calloc(key_length + 1, 1);
    work->model = copy_wide(model);
    work->roles = (ChatRole *)malloc((count ? count : 1) * sizeof *work->roles);
    work->texts = (wchar_t **)calloc(count ? count : 1, sizeof *work->texts);
    bool ok = work->api_key && work->model && work->roles && work->texts;
    if (ok) {
        memcpy(work->api_key, api_key_utf8, key_length + 1);
        for (int i = 0; i < count && ok; i++) {
            work->roles[i] = messages[i].role;
            work->texts[i] = copy_wide(messages[i].text);
            if (!work->texts[i]) ok = false;
        }
    }
    if (ok) {
        uintptr_t thread = _beginthreadex(NULL, 0, worker, work, 0, NULL);
        if (thread) {
            client->thread = (HANDLE)thread;
            return (int)generation;
        }
    }
    free_work(work);
    return 0;
}

bool openrouter_cancel(OpenRouterClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return false;
    InterlockedExchange(&client->cancelled_generation, generation);
    return true;
}

void openrouter_complete(OpenRouterClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return;
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}

void openrouter_shutdown(OpenRouterClient *client) {
    if (!client || !client->thread) return;
    int generation = (int)client->generation;
    openrouter_cancel(client, generation);
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}
