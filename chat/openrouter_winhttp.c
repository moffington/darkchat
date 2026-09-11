#include "openrouter_winhttp.h"
#include "json.h"
#include <winhttp.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#define OPENROUTER_HOST L"openrouter.ai"
#define OPENROUTER_PATH L"/api/v1/chat/completions"
#define OPENROUTER_USER_AGENT L"DarkChat/0.2"
#define OPENROUTER_MAX_RESPONSE (16u * 1024u * 1024u)

/* Everything the worker needs, deep-copied so the UI thread stays free to
   mutate the conversation while the request runs. */
typedef struct {
    int generation;
    HWND notify;
    UINT message;
    char *api_key;      /* UTF-8; zeroed and freed after the request */
    wchar_t *model;
    ChatRole *roles;
    wchar_t **texts;
    int count;
    OpenRouterResult *result;
} OpenRouterWork;

typedef struct {
    char *data;
    size_t length, capacity;
} Response;

typedef enum {
    RESPONSE_READ_OK,
    RESPONSE_READ_WINHTTP_ERROR,
    RESPONSE_READ_TOO_LARGE,
    RESPONSE_READ_OUT_OF_MEMORY
} ResponseRead;

void openrouter_init(OpenRouterClient *client, HWND notify, UINT message) {
    if (!client) return;
    memset(client, 0, sizeof *client);
    client->notify = notify;
    client->message = message;
}

void openrouter_result_free(OpenRouterResult *result) {
    if (!result) return;
    free(result->text);
    free(result);
}

static wchar_t *copy_wide(const wchar_t *text) {
    if (!text) return NULL;
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof *copy);
    if (copy) { memcpy(copy, text, length * sizeof *copy); copy[length] = 0; }
    return copy;
}

static void set_error(OpenRouterResult *result, const wchar_t *text) {
    wchar_t *copy = copy_wide(text);
    if (!copy) return;  /* result->text stays NULL; the UI treats it as an error */
    result->ok = false;
    result->text = copy;
}

/* --- Request body -------------------------------------------------------- */

static const char *role_name(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_ASSISTANT: return "assistant";
    case CHAT_ROLE_SYSTEM: return "system";
    default: return "user";
    }
}

static bool build_request(const OpenRouterWork *work, JsonBuf *body) {
    json_buf_init(body, 8192);
    if (!json_buf_append_raw(body, "{\"model\":", 9)) return false;
    if (!json_buf_append_json_string(body, work->model)) return false;
    if (!json_buf_append_raw(body, ",\"messages\":[", 13)) return false;
    bool first = true;
    for (int i = 0; i < work->count; i++) {
        /* Error notes are local annotations, not conversation history. */
        if (work->roles[i] == CHAT_ROLE_ERROR) continue;
        if (!first && !json_buf_append_raw(body, ",", 1)) return false;
        first = false;
        const char *role = role_name(work->roles[i]);
        if (!json_buf_append_raw(body, "{\"role\":\"", 9)) return false;
        if (!json_buf_append_raw(body, role, strlen(role))) return false;
        if (!json_buf_append_raw(body, "\",\"content\":", 12)) return false;
        if (!json_buf_append_json_string(body, work->texts[i])) return false;
        if (!json_buf_append_raw(body, "}", 1)) return false;
    }
    /* Non-streaming on purpose: Pass 2 is the end-to-end proof before the SSE
       parser arrives. */
    return json_buf_append_raw(body, "],\"stream\":false}", 17);
}

/* --- Request headers ----------------------------------------------------- */

static wchar_t *build_headers(const char *api_key) {
    size_t length = strlen(api_key);
    wchar_t *key = (wchar_t *)malloc((length + 1) * sizeof *key);
    if (!key) return NULL;
    int converted = MultiByteToWideChar(CP_UTF8, 0, api_key, -1, key,
        (int)(length + 1));
    if (converted <= 0) { free(key); return NULL; }
    static const wchar_t *const prefix =
        L"Content-Type: application/json\r\nAuthorization: Bearer ";
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

/* --- Response reading ----------------------------------------------------- */

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

static void response_free(Response *response) { free(response->data); }

static ResponseRead read_response(HINTERNET request, Response *response,
    DWORD *winhttp_error) {
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            *winhttp_error = GetLastError();
            return RESPONSE_READ_WINHTTP_ERROR;
        }
        if (!available) break;
        if (response->length + available > OPENROUTER_MAX_RESPONSE)
            return RESPONSE_READ_TOO_LARGE;
        if (!response_reserve(response, available))
            return RESPONSE_READ_OUT_OF_MEMORY;
        DWORD read = 0;
        if (!WinHttpReadData(request, response->data + response->length,
            available, &read)) {
            *winhttp_error = GetLastError();
            return RESPONSE_READ_WINHTTP_ERROR;
        }
        if (!read) break;
        response->length += read;
    }
    if (response->data) response->data[response->length] = 0;
    return RESPONSE_READ_OK;
}

/* Turns one decoded string value into a fresh UTF-16 result text. */
static void set_from_utf8(OpenRouterResult *result, bool ok,
    const char *decoded) {
    wchar_t *text = json_utf8_to_utf16(decoded, strlen(decoded));
    if (!text) return;
    result->ok = ok;
    result->text = text;
}

/* Extracts the reply or the API error out of one full response body. */
static void finish(OpenRouterResult *result, DWORD status,
    const char *response) {
    size_t size = strlen(response) + 1;
    char *scratch = (char *)malloc(size);
    if (!scratch) { set_error(result, L"Out of memory reading the response."); return; }
    if (status == 200 || status == 201) {
        if (json_query_string(response, "choices[0].message.content", scratch,
            size)) {
            set_from_utf8(result, true, scratch);
            if (result->text && result->text[0]) { free(scratch); return; }
            bool empty = result->text != NULL;
            free(result->text);
            result->text = NULL;
            set_error(result, empty ? L"The response had no message content."
                                    : L"Could not decode the response text.");
        } else if (json_query_string(response, "error.message", scratch,
            size)) {
            set_from_utf8(result, false, scratch);
            if (result->text) { free(scratch); return; }
            set_error(result, L"Could not decode the API error.");
        } else {
            set_error(result, L"The response had no message content.");
        }
        free(scratch);
        return;
    }
    wchar_t *detail = NULL;
    if (json_query_string(response, "error.message", scratch, size))
        detail = json_utf8_to_utf16(scratch, strlen(scratch));
    wchar_t composed[512];
    if (detail) {
        _snwprintf(composed, 512, L"OpenRouter returned HTTP %lu: %s",
            (unsigned long)status, detail);
        free(detail);
    } else {
        _snwprintf(composed, 512, L"OpenRouter returned HTTP %lu.",
            (unsigned long)status);
    }
    composed[511] = 0;
    set_error(result, composed);
    free(scratch);
}

/* --- The request itself ----------------------------------------------------- */

static void perform(OpenRouterWork *work, OpenRouterResult *result) {
    JsonBuf body;
    if (!build_request(work, &body)) {
        json_buf_free(&body);
        set_error(result, L"DarkChat could not encode the request.");
        return;
    }
    wchar_t *headers = build_headers(work->api_key);
    if (!headers) {
        set_error(result, L"DarkChat could not build the request headers.");
        json_buf_free(&body);
        return;
    }
    HINTERNET session = NULL, connect = NULL, request = NULL;
    Response response = { 0 };
    bool delivered = false;
    DWORD winhttp_error = ERROR_SUCCESS;
    do {
        session = WinHttpOpen(OPENROUTER_USER_AGENT,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { winhttp_error = GetLastError(); break; }
        connect = WinHttpConnect(session, OPENROUTER_HOST,
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { winhttp_error = GetLastError(); break; }
        request = WinHttpOpenRequest(connect, L"POST", OPENROUTER_PATH, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request) { winhttp_error = GetLastError(); break; }
        /* Generous receive timeout: non-streaming completions can be slow. */
        WinHttpSetTimeouts(request, 15000, 15000, 30000, 120000);
        if (!WinHttpSendRequest(request,
            headers, (DWORD)-1, (LPVOID)body.data, (DWORD)body.length,
            (DWORD)body.length, 0)) {
            winhttp_error = GetLastError();
            break;
        }
        if (!WinHttpReceiveResponse(request, NULL)) {
            winhttp_error = GetLastError();
            break;
        }
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, 0)) {
            winhttp_error = GetLastError();
            break;
        }
        ResponseRead read = read_response(request, &response, &winhttp_error);
        if (read == RESPONSE_READ_TOO_LARGE) {
            set_error(result, L"The OpenRouter response exceeded the 16 MB limit.");
            delivered = true;
            break;
        }
        if (read == RESPONSE_READ_OUT_OF_MEMORY) {
            set_error(result, L"Out of memory reading the OpenRouter response.");
            delivered = true;
            break;
        }
        if (read != RESPONSE_READ_OK) break;
        finish(result, status, response.data ? response.data : "");
        delivered = true;
    } while (0);
    if (!delivered && !result->text) {
        wchar_t text[160];
        swprintf(text, 160, L"Network request failed (WinHTTP error %lu).",
            (unsigned long)winhttp_error);
        set_error(result, text);
    }
    response_free(&response);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    if (headers) {
        SecureZeroMemory(headers, wcslen(headers) * sizeof *headers);
        free(headers);
    }
    json_buf_free(&body);
}

static void free_work(OpenRouterWork *work) {
    if (!work) return;
    if (work->api_key) {
        SecureZeroMemory(work->api_key, strlen(work->api_key));
        free(work->api_key);
    }
    free(work->model);
    for (int i = 0; i < work->count; i++) free(work->texts[i]);
    free(work->texts);
    free(work->roles);
}

static unsigned __stdcall worker(void *parameter) {
    OpenRouterWork *work = (OpenRouterWork *)parameter;
    OpenRouterResult *result = work->result;
    HWND notify = work->notify;
    UINT message = work->message;
    result->generation = work->generation;
    perform(work, result);
    free_work(work);
    free(work);
    if (!PostMessageW(notify, message, (WPARAM)result->generation,
        (LPARAM)result))
        openrouter_result_free(result);  /* window is gone */
    return 0;
}

int openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count) {
    if (!client || !client->notify || !api_key_utf8 || !api_key_utf8[0] ||
        !model || count < 0 || (count > 0 && !messages)) return 0;
    if (client->thread) return 0;  /* one request in flight in this pass */
    LONG generation = InterlockedIncrement(&client->generation);
    OpenRouterWork *work = (OpenRouterWork *)calloc(1, sizeof *work);
    if (!work) return 0;
    work->generation = (int)generation;
    work->notify = client->notify;
    work->message = client->message;
    work->count = count;
    size_t key_length = strlen(api_key_utf8);
    work->api_key = (char *)malloc(key_length + 1);
    work->model = copy_wide(model);
    work->roles = (ChatRole *)malloc((count ? count : 1) * sizeof *work->roles);
    work->texts = (wchar_t **)calloc(count ? count : 1, sizeof *work->texts);
    work->result = (OpenRouterResult *)calloc(1, sizeof *work->result);
    bool ok = work->api_key && work->model && work->roles && work->texts &&
        work->result;
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
    /* Failure: tear the snapshot down the same way the worker would. */
    openrouter_result_free(work->result);
    free_work(work);
    free(work);
    return 0;
}

void openrouter_complete(OpenRouterClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return;
    /* The result is posted only after the worker has released its snapshot.
       Waiting here is therefore normally just a scheduler handoff. */
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}

void openrouter_shutdown(OpenRouterClient *client) {
    if (!client || !client->thread) return;
    HANDLE thread = client->thread;
    client->thread = NULL;
    /* Pass 2 has no cancellation yet, so shutdown must genuinely join the
       worker rather than letting process teardown race it. */
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
