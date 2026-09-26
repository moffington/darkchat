#include "chat/generation/completion_winhttp.h"
#include "chat/generation/completion_request.h"
#include "chat/generation/sse.h"
#include <winhttp.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hard-coded endpoint descriptors. The OpenAI-compatible request shape is
   identical for both; only the network specifics and headers differ, and only
   OpenRouter authenticates or carries a provider object. A small, fixed table
   (never caller-configurable) is the whole selection mechanism. */
typedef struct {
    const wchar_t *host;
    INTERNET_PORT port;
    const wchar_t *path;
    const wchar_t *user_agent;
    DWORD access_type;   /* WINHTTP_ACCESS_TYPE_* */
    DWORD request_flags; /* WINHTTP_FLAG_SECURE for https, 0 for local http */
    bool authorize;      /* whether the API key and OpenRouter headers are sent */
} CompletionEndpoint;

static const CompletionEndpoint ENDPOINTS[CHAT_BACKEND_COUNT] = {
    { L"openrouter.ai", INTERNET_DEFAULT_HTTPS_PORT,
      L"/api/v1/chat/completions", L"DarkChat/0.3",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_FLAG_SECURE, true },
    { L"localhost", 11434, L"/v1/chat/completions", L"DarkChat/0.4",
      WINHTTP_ACCESS_TYPE_NO_PROXY, 0, false },
};

#define COMPLETION_MAX_RESPONSE (16u * 1024u * 1024u)

/* Owned copy of one request content part, parallel to the borrowed
   ChatRequestPart the pure encoder consumes. TEXT parts hold their own
   text; IMAGE parts hold an owned copy of the managed bytes plus an owned
   copy of the attachment record -- the record carrier is what lets
   build_request re-wrap a trustworthy ChatRequestPart (the encoder reads
   rec.mime and fails closed without one) with no reference into live Chat
   state. `n` is the copied length and always equals rec.bytes for a term
   the host loaded. */
typedef struct {
    uint8_t kind;      /* ChatPartKind */
    uint8_t flags;     /* CHAT_PART_FLAG_* of the source part */
    union {
        wchar_t *text;
        struct {
            ChatAttachmentMeta rec;
            unsigned char *bytes;
            size_t n;
        } image;
    } u;
} CompletionPart;

typedef struct {
    int generation;
    HWND notify;
    UINT message;
    CompletionClient *client;
    ChatBackend backend;
    char *api_key;
    wchar_t *model;
    ChatRole *roles;
    wchar_t **texts;
    /* Per-message owned part runs, parallel to roles/texts. A fast-path
       message has a NULL entry and copies its text into texts[]; a message
       with a run leaves texts[] NULL (the run is its content, and the
       encoder never reads the projection of a content array). NULL arrays
       when count == 0. */
    CompletionPart **parts;
    int *part_counts;
    int count;
    ChatProviderRouting routing;
    bool reasoning;
    ChatGeneration metadata;
    ULONGLONG started_tick;
} CompletionWork;

typedef struct { char *data; size_t length, capacity; } Response;

/* Pending stream fragments awaiting delivery. Tiny reasoning and content
   deltas arrive as separate SSE events (often one word each); concatenating
   them and enqueueing one event per flush keeps the delivered text and its
   order identical to the original per-fragment stream. */
typedef struct { wchar_t *data; size_t length, capacity; } PendingFragments;

/* Delivery threshold: a single fragment flush never grows past this, so a
   fast burst inside one network chunk still reaches the UI promptly. */
#define STREAM_COALESCE_CHARS 1024u

/* Delivery cadence, in milliseconds: fragments never sit in the buffers
   longer than this regardless of read boundaries, so one SSE event per
   network read still coalesces into at most one enqueue per interval. */
#define STREAM_FLUSH_MS 100u

typedef struct {
    HANDLE event;
    volatile LONG status;
    DWORD error;
    DWORD bytes;
} AsyncState;
typedef struct {
    CompletionWork *work;
    bool done, failed;
    wchar_t *error;
    PendingFragments delta, reason;   /* coalescing buffers, NUL-terminated */
    ULONGLONG flush_tick;             /* last real delivery, any type */
} Stream;
typedef enum { REQUEST_DONE, REQUEST_ERROR, REQUEST_CANCELLED, REQUEST_INTERRUPTED } RequestOutcome;

/* Checked multiplication: false instead of wrapping on overflow, so an
   allocation size can never disagree with the count it came from. */
static bool size_mul(size_t a, size_t b, size_t *out) {
    if (b && a > (size_t)-1 / b) return false;
    *out = a * b;
    return true;
}

static wchar_t *copy_wide(const wchar_t *text) {
    if (!text) return NULL;
    size_t length = wcslen(text);
    if (length > ((size_t)-1 / sizeof(wchar_t)) - 1) return NULL;
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof *copy);
    if (copy) memcpy(copy, text, (length + 1) * sizeof *copy);
    return copy;
}

static bool cancelled(const CompletionWork *work) {
    return InterlockedCompareExchange(&work->client->cancelled_generation,
        0, 0) == work->generation;
}

/* Enqueues one event for UI delivery. The worker appends under the client
   lock and posts exactly one outstanding wake message: a wake is sent only
   when none is pending (`wake_pending` cleared by the UI thread inside
   completion_take), so the number of posted messages is bounded by the
   number of takes, never by the number of fragments. */
static bool post_event(CompletionWork *work, CompletionEventType type,
    wchar_t *owned_text) {
    if (type == COMPLETION_DELTA && cancelled(work)) {
        free(owned_text);
        return false;
    }
    CompletionEvent *event = (CompletionEvent *)calloc(1, sizeof *event);
    if (!event) { free(owned_text); return false; }
    event->generation = work->generation;
    event->type = type;
    event->text = owned_text;
    event->metadata = work->metadata;
    CompletionClient *client = work->client;
    bool alive = false, wake = false;
    EnterCriticalSection(&client->lock);
    alive = client->alive;
    if (alive) {
        if (client->tail) client->tail->next = event;
        else client->head = event;
        client->tail = event;
        wake = !InterlockedExchange(&client->wake_pending, 1);
    }
    LeaveCriticalSection(&client->lock);
    if (!alive) {
        /* Defensive: the worker is joined before shutdown closes the
           queue, so this never fires in production; a late post frees its
           event and reports failure. */
        completion_event_free(event);
        return false;
    }
    if (wake && !PostMessageW(client->notify, client->message,
        (WPARAM)work->generation, 0)) {
        /* The event stays queued; clearing the flag makes the next event
           (the terminal one, at the latest) retry the wake, and the host's
           1 Hz sweep drains the batch as a backstop once the worker exits. */
        InterlockedExchange(&client->wake_pending, 0);
        return false;
    }
    return true;
}

static bool fragments_append(PendingFragments *buffer, const wchar_t *text) {
    size_t added = wcslen(text);
    if (buffer->length + added + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < buffer->length + added + 1) capacity *= 2;
        wchar_t *grown = (wchar_t *)realloc(buffer->data,
            capacity * sizeof *grown);
        if (!grown) return false;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    wmemcpy(buffer->data + buffer->length, text, added + 1);
    buffer->length += added;
    return true;
}

static wchar_t *fragments_release(PendingFragments *buffer) {
    wchar_t *data = buffer->data;
    buffer->data = NULL;
    buffer->length = buffer->capacity = 0;
    return data;
}

static void fragments_free(PendingFragments *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = buffer->capacity = 0;
}

/* Posts the pending fragments as one event per type, reasoning before
   content (the order the stream emits them). Delivery failure keeps the
   current failure semantics of the direct-post path; cancellation frees
   the text inside post_event. */
static bool stream_flush(Stream *stream) {
    bool ok = true;
    if (stream->reason.length) {
        wchar_t *text = fragments_release(&stream->reason);
        if (!post_event(stream->work, COMPLETION_REASONING, text)) {
            if (!cancelled(stream->work)) {
                stream->error =
                    copy_wide(L"Could not deliver streamed reasoning.");
                stream->failed = true;
            }
            ok = false;
        }
    }
    if (stream->delta.length) {
        wchar_t *text = fragments_release(&stream->delta);
        if (!post_event(stream->work, COMPLETION_DELTA, text)) {
            if (!cancelled(stream->work)) {
                stream->error = copy_wide(L"Could not deliver streamed text.");
                stream->failed = true;
            }
            ok = false;
        }
    }
    if (ok) stream->flush_tick = GetTickCount64();
    return ok;
}

/* Adds one decoded fragment to a coalescing buffer (taking ownership).
   Delivery is independent of read boundaries: buffers drain only when the
   cadence interval has elapsed or the size threshold is crossed, so
   packetization (one SSE event per read) cannot force one enqueue per
   token. */
static bool stream_push(Stream *stream, PendingFragments *buffer,
    wchar_t *owned) {
    if (!owned) return false;
    if (!fragments_append(buffer, owned)) {
        free(owned);
        stream->error = copy_wide(L"Out of memory decoding the stream.");
        stream->failed = true;
        return false;
    }
    free(owned);
    if (stream->failed || cancelled(stream->work)) return !stream->failed;
    if (buffer->length >= STREAM_COALESCE_CHARS) return stream_flush(stream);
    if (GetTickCount64() - stream->flush_tick >= STREAM_FLUSH_MS)
        return stream_flush(stream);
    return true;
}

void completion_init(CompletionClient *client, HWND notify, UINT message) {
    if (!client) return;
    memset(client, 0, sizeof *client);
    client->notify = notify;
    client->message = message;
    client->alive = true;
    InitializeCriticalSection(&client->lock);
}

CompletionEvent *completion_take(CompletionClient *client) {
    /* `alive` gates before the lock: after shutdown the critical section is
       deleted, and this runs on the UI thread that also wrote the flag. */
    if (!client || !client->alive) return NULL;
    CompletionEvent *batch = NULL;
    EnterCriticalSection(&client->lock);
    batch = client->head;
    client->head = client->tail = NULL;
    InterlockedExchange(&client->wake_pending, 0);
    LeaveCriticalSection(&client->lock);
    return batch;
}

/* Single-event ownership: the event's `next` link, if any, is NOT followed
   (a batch must be detached node-by-node before each free, or use
   completion_events_free). */
void completion_event_free(CompletionEvent *event) {
    if (!event) return;
    free(event->text);
    free(event);
}

void completion_events_free(CompletionEvent *batch) {
    while (batch) {
        CompletionEvent *next = batch->next;
        completion_event_free(batch);
        batch = next;
    }
}

/* Deep-copies one borrowed view into the work arrays (the owned carry the
   worker needs once the per-send scope is gone). Two passes: every part is
   validated before anything is copied -- kind, image record presence,
   counts and allocation-size arithmetic -- because copying an IMAGE term
   dereferences its record and an untrustworthy term must fail the request
   before it starts, exactly as the encoder rejects it later. On failure the
   partially filled work stays free_work-safe (every array is zeroed and
   part_counts is published only together with its run). A fast-path message
   (no run) copies its text like today; a message with a run copies the run
   only. */
static bool copy_work_messages(CompletionWork *work,
    const CompletionMessage *messages, int count) {
    if (!work || count < 0 || (count > 0 && !messages)) return false;
    work->count = count;
    if (count <= 0) return true;
    size_t bytes = 0;
    if (!size_mul((size_t)count, sizeof *work->roles, &bytes) ||
        !size_mul((size_t)count, sizeof *work->texts, &bytes) ||
        !size_mul((size_t)count, sizeof *work->parts, &bytes) ||
        !size_mul((size_t)count, sizeof *work->part_counts, &bytes))
        return false;
    /* Validation pass. */
    for (int i = 0; i < count; i++) {
        int part_count = messages[i].part_count;
        if (part_count < 0 || part_count > CHAT_MAX_PARTS) return false;
        if (part_count > 0 && !messages[i].parts) return false;
        if (!size_mul((size_t)part_count, sizeof(CompletionPart), &bytes))
            return false;
        for (int j = 0; j < part_count; j++) {
            const ChatRequestPart *part = &messages[i].parts[j];
            if (part->kind == CHAT_PART_TEXT) continue;
            if (part->kind != CHAT_PART_IMAGE) return false;
            /* The rec is copied below, so it must exist; and an image term
               must carry real bytes to encode -- a zero-length term is
               untrustworthy, not an empty image to send. */
            if (!part->u.image.rec || !part->u.image.byte_length ||
                !part->u.image.bytes)
                return false;
        }
    }
    /* Copy pass. */
    work->roles = (ChatRole *)malloc((size_t)count * sizeof *work->roles);
    work->texts = (wchar_t **)calloc((size_t)count, sizeof *work->texts);
    work->parts = (CompletionPart **)calloc((size_t)count,
        sizeof *work->parts);
    work->part_counts = (int *)calloc((size_t)count,
        sizeof *work->part_counts);
    if (!work->roles || !work->texts || !work->parts || !work->part_counts)
        return false;
    for (int i = 0; i < count; i++) {
        work->roles[i] = messages[i].role;
        int part_count = messages[i].part_count;
        if (part_count <= 0) {
            work->texts[i] = copy_wide(messages[i].text);
            if (!work->texts[i]) return false;
            continue;
        }
        CompletionPart *run = (CompletionPart *)calloc((size_t)part_count,
            sizeof *run);
        if (!run) return false;
        work->parts[i] = run;
        work->part_counts[i] = part_count;
        for (int j = 0; j < part_count; j++) {
            const ChatRequestPart *part = &messages[i].parts[j];
            CompletionPart *copy = &run[j];
            copy->kind = (uint8_t)part->kind;
            copy->flags = part->flags;
            if (part->kind == CHAT_PART_TEXT) {
                if (part->u.text) {
                    copy->u.text = copy_wide(part->u.text);
                    if (!copy->u.text) return false;
                }
            } else {
                copy->u.image.rec = *part->u.image.rec;
                copy->u.image.n = part->u.image.byte_length;
                copy->u.image.bytes = (unsigned char *)malloc(
                    part->u.image.byte_length);
                if (!copy->u.image.bytes) return false;
                memcpy(copy->u.image.bytes, part->u.image.bytes,
                    part->u.image.byte_length);
            }
        }
    }
    return true;
}

/* Real encoder: adapts the work arrays into the shared borrowed view. Kept as
   a thin adapter so the exact bytes live in one place. Owned parts are
   re-wrapped as borrowed runs pointing back into the work arrays (the record
   carrier supplies rec; the copied bytes supply the payload), so the body is
   byte-identical to the pure encoder's over the original view. */
static bool build_request(const CompletionWork *work, JsonBuf *body) {
    /* Initialize before any fallible step so a caller can always free it. */
    json_buf_init(body, 0);
    ChatRequestMessage *messages = NULL;
    ChatRequestPart *run_storage = NULL;
    if (work->count > 0) {
        messages = (ChatRequestMessage *)malloc(
            (size_t)work->count * sizeof *messages);
        if (!messages) return false;
    }
    size_t total = 0;
    for (int i = 0; i < work->count; i++) {
        int part_count = work->part_counts ? work->part_counts[i] : 0;
        if (part_count > 0) {
            if (!work->parts || !work->parts[i]) { free(messages); return false; }
            total += (size_t)part_count;
        }
    }
    /* The re-wrap pool is allocated only when a run exists at all. */
    if (total > 0) {
        size_t bytes = 0;
        if (!size_mul(total, sizeof *run_storage, &bytes)) {
            free(messages);
            return false;
        }
        run_storage = (ChatRequestPart *)malloc(bytes);
        if (!run_storage) { free(messages); return false; }
    }
    size_t used = 0;
    for (int i = 0; i < work->count; i++) {
        int part_count = work->part_counts ? work->part_counts[i] : 0;
        messages[i].role = work->roles[i];
        messages[i].text = work->texts[i] ? work->texts[i] : L"";
        messages[i].parts = NULL;
        messages[i].part_count = 0;
        if (part_count <= 0) continue;
        messages[i].parts = &run_storage[used];
        messages[i].part_count = part_count;
        for (int j = 0; j < part_count; j++) {
            const CompletionPart *src = &work->parts[i][j];
            ChatRequestPart *dst = &run_storage[used + (size_t)j];
            dst->kind = (ChatPartKind)src->kind;
            dst->flags = src->flags;
            if (src->kind == CHAT_PART_TEXT) {
                dst->u.text = src->u.text;
            } else {
                dst->u.image.meta = NULL;
                dst->u.image.rec = &src->u.image.rec;
                dst->u.image.bytes = src->u.image.bytes;
                dst->u.image.byte_length = src->u.image.n;
            }
        }
        used += (size_t)part_count;
    }
    bool ok = chat_completion_request_build(body, work->backend, work->model,
        messages, work->count, &work->routing, work->reasoning);
    free(run_storage);
    free(messages);
    return ok;
}

static wchar_t *build_headers(const CompletionWork *work) {
    if (!ENDPOINTS[work->backend].authorize) {
        static const wchar_t *const plain =
            L"Content-Type: application/json\r\n"
            L"Accept: text/event-stream\r\n";
        return copy_wide(plain);
    }
    const char *api_key = work->api_key;
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

static bool wait_status(CompletionWork *work, AsyncState *state,
    DWORD expected, Stream *stream) {
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
        /* Between waits the network is quiet: the 100 ms delivery cadence
           is enforced here, not only when the next fragment arrives, so a
           paused stream never holds decoded text past the latency bound. */
        if (stream && !stream->failed && !cancelled(work) &&
            (stream->delta.length || stream->reason.length) &&
            GetTickCount64() - stream->flush_tick >= STREAM_FLUSH_MS &&
            !stream_flush(stream))
            return false;
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

static wchar_t *http_error(ChatBackend backend, DWORD status, const char *body) {
    const wchar_t *name = chat_backend_name(backend);
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
        _snwprintf(composed, 512, L"%ls returned HTTP %lu: %ls",
            name, (unsigned long)status, detail);
        free(detail);
    } else {
        _snwprintf(composed, 512, L"%ls returned HTTP %lu.",
            name, (unsigned long)status);
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

    /* A reasoning_details delta may contain several displayable objects. Emit
       every fragment in array order; dropping all but the first makes the
       reasoning pane appear frozen while the model is still generating. */
    bool has_reasoning = false;
    size_t detail_count = 0;
    json_query_array_length(json,
        "choices[0].delta.reasoning_details", &detail_count);
    for (size_t i = 0; i < detail_count && !stream->failed; i++) {
        char path[80];
        snprintf(path, sizeof path,
            "choices[0].delta.reasoning_details[%zu].text", i);
        bool has_fragment = json_query_string(json, path, decoded,
            length + 1) && decoded[0];
        if (!has_fragment) {
            snprintf(path, sizeof path,
                "choices[0].delta.reasoning_details[%zu].summary", i);
            has_fragment = json_query_string(json, path, decoded,
                length + 1) && decoded[0];
        }
        if (has_fragment) {
            has_reasoning = true;
            wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
            if (!stream_push(stream, &stream->reason, wide) &&
                !stream->failed && !cancelled(stream->work)) {
                stream->error = copy_wide(
                    L"Could not deliver streamed reasoning.");
                stream->failed = true;
            }
        }
    }
    /* Plain fields are compatibility fallbacks used by some providers,
       including Ollama's OpenAI-compatible reasoning output. */
    bool has_plain_reasoning = false;
    if (!has_reasoning && !stream->failed)
        has_plain_reasoning = json_query_string(json,
            "choices[0].delta.reasoning",
            decoded, length + 1) && decoded[0];
    if (!has_reasoning && !has_plain_reasoning && !stream->failed)
        has_plain_reasoning = json_query_string(json,
            "choices[0].delta.reasoning_content", decoded, length + 1) &&
            decoded[0];
    if (has_plain_reasoning && !stream->failed) {
        wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
        if (!stream_push(stream, &stream->reason, wide) &&
            !stream->failed && !cancelled(stream->work)) {
            stream->error = copy_wide(L"Could not deliver streamed reasoning.");
            stream->failed = true;
        }
    }
    if (!stream->failed && json_query_string(json,
        "choices[0].delta.content", decoded,
        length + 1)) {
        if (decoded[0] && !g->first_token_at) {
            g->first_token_at = chat_now();
            g->ttft_ms = (double)(GetTickCount64() - stream->work->started_tick);
        }
        wchar_t *wide = json_utf8_to_utf16(decoded, strlen(decoded));
        if (!stream_push(stream, &stream->delta, wide) &&
            !stream->failed && !cancelled(stream->work)) {
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

static RequestOutcome perform(CompletionWork *work, wchar_t **error) {
    const CompletionEndpoint *endpoint = &ENDPOINTS[work->backend];
    JsonBuf body;
    wchar_t *headers = NULL;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    AsyncState async = {0};
    bool request_context_set = false;
    Response response = {0};
    SseParser parser;
    Stream stream = {work, false, false, NULL, {0}, {0}, GetTickCount64()};
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
    headers = build_headers(work);
    if (!headers) {
        *error = copy_wide(L"DarkChat could not build request headers.");
        goto cleanup;
    }
    session = WinHttpOpen(endpoint->user_agent, endpoint->access_type,
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
    connect = WinHttpConnect(session, endpoint->host, endpoint->port, 0);
    if (!connect) { winhttp_error = GetLastError(); goto network_error; }
    request = WinHttpOpenRequest(connect, L"POST", endpoint->path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        endpoint->request_flags);
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
            WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, NULL)) {
        winhttp_error = async.error; goto network_or_cancel;
    }
    begin_async(&async);
    if (!async_started(WinHttpReceiveResponse(request, NULL), &async) ||
        !wait_status(work, &async, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
            NULL)) {
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
                WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &stream)) {
            if (stream.failed) goto stream_failed;
            winhttp_error = async.error; goto network_or_cancel;
        }
        DWORD available = async.bytes;
        if (!available) break;
        if (response.length + available > COMPLETION_MAX_RESPONSE) {
            *error = copy_wide(work->backend == CHAT_BACKEND_OLLAMA ?
                L"The Ollama response exceeded the 16 MB limit." :
                L"The OpenRouter response exceeded the 16 MB limit.");
            goto cleanup;
        }
        if (!response_reserve(&response, available)) {
            *error = copy_wide(L"Out of memory reading the response.");
            goto cleanup;
        }
        begin_async(&async);
        if (!async_started(WinHttpReadData(request,
            response.data + response.length, available, NULL), &async) ||
            !wait_status(work, &async, WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
                &stream)) {
            if (stream.failed) goto stream_failed;
            winhttp_error = async.error; goto network_or_cancel;
        }
        DWORD read = async.bytes;
        if (!read) break;
        if (status == 200 || status == 201) {
            /* Fragments are buffered independently of read boundaries (the
                cadence/threshold logic in stream_push); nothing is flushed
                per read. */
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
        *error = http_error(work->backend, status,
            response.data ? response.data : "");
        goto cleanup;
    }
    if (!sse_finish(&parser, stream_event, &stream) || stream.failed ||
        !stream_flush(&stream)) {
        if (cancelled(work)) goto was_cancelled;
        *error = stream.error ? stream.error :
            copy_wide(L"The streaming response was malformed.");
        stream.error = NULL;
        goto cleanup;
    }
    if (!stream.done) {
        outcome = REQUEST_INTERRUPTED;
        *error = copy_wide(work->backend == CHAT_BACKEND_OLLAMA ?
            L"The stream ended before Ollama sent [DONE]." :
            L"The stream ended before OpenRouter sent [DONE].");
        goto cleanup;
    }
    outcome = REQUEST_DONE;
    goto cleanup;
network_or_cancel:
    if (cancelled(work)) goto was_cancelled;
    goto network_error;
stream_failed:
    if (cancelled(work)) goto was_cancelled;
    *error = stream.error ? stream.error :
        copy_wide(L"Could not deliver streamed text.");
    stream.error = NULL;
    goto cleanup;
network_error:
    if (cancelled(work)) goto was_cancelled;
    if (work->metadata.first_token_at) outcome = REQUEST_INTERRUPTED;
    {
        wchar_t text[160];
        if (work->backend == CHAT_BACKEND_OLLAMA)
            swprintf(text, 160, L"Ollama is not reachable at localhost:11434.");
        else
            swprintf(text, 160, L"Network request failed (WinHTTP error %lu).",
                (unsigned long)winhttp_error);
        *error = copy_wide(text);
    }
    goto cleanup;
was_cancelled:
    outcome = REQUEST_CANCELLED;
cleanup:
    /* Deliver decoded-but-undelivered fragments (success, malformed tail
       and network failure all flush before erroring so no text is lost);
       cancellation drops them instead. */
    if (outcome != REQUEST_CANCELLED) stream_flush(&stream);
    fragments_free(&stream.reason);
    fragments_free(&stream.delta);
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

static void free_work(CompletionWork *work) {
    if (!work) return;
    if (work->api_key) {
        SecureZeroMemory(work->api_key, strlen(work->api_key));
        free(work->api_key);
    }
    free(work->model);
    if (work->texts) for (int i = 0; i < work->count; i++) free(work->texts[i]);
    if (work->parts) {
        for (int i = 0; i < work->count; i++) {
            CompletionPart *run = work->parts[i];
            if (!run) continue;
            int part_count = work->part_counts ? work->part_counts[i] : 0;
            for (int j = 0; j < part_count; j++) {
                if (run[j].kind == CHAT_PART_IMAGE) {
                    /* Image bytes are user content, not secrets, but the
                       scrub keeps stale heap residue out of freed blocks --
                       consistent with the api-key and header scrubbing. */
                    if (run[j].u.image.bytes) {
                        SecureZeroMemory(run[j].u.image.bytes,
                            run[j].u.image.n);
                        free(run[j].u.image.bytes);
                    }
                } else {
                    free(run[j].u.text);
                }
            }
            free(run);
        }
    }
    free(work->parts); free(work->part_counts);
    free(work->texts); free(work->roles); free(work);
}

static unsigned __stdcall worker(void *parameter) {
    CompletionWork *work = (CompletionWork *)parameter;
    wchar_t *error = NULL;
    RequestOutcome outcome = perform(work, &error);
    CompletionEventType type = outcome == REQUEST_DONE ? COMPLETION_DONE :
        outcome == REQUEST_CANCELLED ? COMPLETION_CANCELLED :
        outcome == REQUEST_INTERRUPTED ? COMPLETION_INTERRUPTED : COMPLETION_ERROR;
    work->metadata.finished_at = chat_now();
    work->metadata.latency_ms = (double)(GetTickCount64() - work->started_tick);
    post_event(work, type, error);
    free_work(work);
    return 0;
}

int completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning) {
    if (!client || !client->notify || backend < 0 ||
        backend >= CHAT_BACKEND_COUNT || !model || !model[0] || count < 0 ||
        (count > 0 && !messages) || client->thread) return 0;
    /* Only OpenRouter needs a key; Ollama must work without OPENROUTER_API_KEY. */
    if (backend == CHAT_BACKEND_OPENROUTER &&
        (!api_key_utf8 || !api_key_utf8[0])) return 0;
    LONG generation = InterlockedIncrement(&client->generation);
    InterlockedExchange(&client->cancelled_generation, 0);
    CompletionWork *work = (CompletionWork *)calloc(1, sizeof *work);
    if (!work) return 0;
    chat_generation_init(&work->metadata);
    work->metadata.backend = backend;
    work->metadata.started_at = chat_now();
    work->started_tick = GetTickCount64();
    wcsncpy(work->metadata.requested_model, model, CHAT_MODEL_TEXT - 1);
    work->generation = (int)generation;
    work->notify = client->notify;
    work->message = client->message;
    work->client = client;
    work->backend = backend;
    work->count = count;
    /* The work item is calloc-zeroed, so a NULL routing keeps OpenRouter's
       defaults; otherwise the caller's routing is copied for this request. */
    if (routing) work->routing = *routing;
    work->reasoning = reasoning;
    bool ok = true;
    if (api_key_utf8 && api_key_utf8[0]) {
        size_t key_length = strlen(api_key_utf8);
        work->api_key = (char *)calloc(key_length + 1, 1);
        if (work->api_key) memcpy(work->api_key, api_key_utf8, key_length + 1);
        else ok = false;
    }
    work->model = copy_wide(model);
    ok = ok && work->model && copy_work_messages(work, messages, count);
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

bool completion_cancel(CompletionClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return false;
    InterlockedExchange(&client->cancelled_generation, generation);
    return true;
}

void completion_complete(CompletionClient *client, int generation) {
    if (!client || !client->thread || generation != client->generation) return;
    WaitForSingleObject(client->thread, INFINITE);
    CloseHandle(client->thread);
    client->thread = NULL;
}

void completion_shutdown(CompletionClient *client) {
    if (!client || !client->alive) return;
    /* `alive` is the once-guard: the first shutdown joins the (optional)
       worker, closes and drains the queue, and destroys the critical
       section exactly once; later calls return here. */
    if (client->thread) {
        int generation = (int)client->generation;
        completion_cancel(client, generation);
        WaitForSingleObject(client->thread, INFINITE);
        CloseHandle(client->thread);
        client->thread = NULL;
    }
    /* Close the queue and free anything the worker left: a wake message
       still pending in the pump finds the queue closed (completion_take
       answers NULL) and delivers nothing. */
    EnterCriticalSection(&client->lock);
    client->alive = false;
    CompletionEvent *queued = client->head;
    client->head = client->tail = NULL;
    LeaveCriticalSection(&client->lock);
    InterlockedExchange(&client->wake_pending, 0);
    completion_events_free(queued);
    DeleteCriticalSection(&client->lock);
}
