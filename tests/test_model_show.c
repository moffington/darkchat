/* Deterministic worker tests. The module is included so the transport seam and
   the internal endpoint descriptor are reachable without touching the network;
   malloc and realloc are wrapped so the allocation-failure paths are
   exercised. The contract under test is as much about what the probe must NOT
   report as about what it reports: every failure leaves CHAT_CAP_UNKNOWN, and
   no request may ever carry a credential. */
#include "chat/models/model_show_winhttp.c"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static long alloc_fail_countdown = -1;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    return __real_realloc(pointer, size);
}


enum { MODE_OK, MODE_TEXT, MODE_HTTP_ERROR, MODE_TOO_LARGE,
       MODE_NETWORK_ERROR, MODE_BLOCK, MODE_CANCEL, MODE_GARBAGE };
static int transport_mode;
static HANDLE transport_release;
static const char *vision_payload =
    "{\"model\":\"qwen3-vl:8b\",\"capabilities\":[\"completion\","
    "\"thinking\",\"vision\"],\"model_info\":{\"general.architecture\":\"qwen3vl\"}}";
static const char *text_payload =
    "{\"model\":\"llama3.2:latest\",\"capabilities\":[\"completion\"]}";
static const char *garbage_payload = "not json at all";
/* Recorded request, so the test can pin the exact bytes and headers that would
   have gone over the wire. */
static char recorded_body[512];
static size_t recorded_body_length;
static wchar_t recorded_headers[256];
static int transport_calls;

static bool fake_transport(const ModelShowEndpoint *endpoint,
    const wchar_t *headers, const void *body, size_t body_length,
    const ModelShowCancel *cancel, char **out_body, size_t *out_length,
    DWORD *status, wchar_t **error) {
    *out_body = NULL; *out_length = 0; *status = 0; ++transport_calls;
    (void)error;  /* the fake supplies its own outcomes, never an error string */
    recorded_body_length = body_length < sizeof recorded_body - 1
        ? body_length : sizeof recorded_body - 1;
    memcpy(recorded_body, body, recorded_body_length);
    recorded_body[recorded_body_length] = 0;
    wcscpy(recorded_headers, headers ? headers : L"");
    /* The loopback /api/show endpoint is the only thing this module can reach. */
    if (!endpoint || wcscmp(endpoint->path, L"/api/show") ||
        wcscmp(endpoint->host, L"localhost") || endpoint->port != 11434)
        return false;
    if (transport_mode == MODE_BLOCK)
        WaitForSingleObject(transport_release, INFINITE);
    if (transport_mode == MODE_CANCEL) {
        while (!InterlockedCompareExchange((LONG *)cancel->cancelled, 0, 0))
            Sleep(5);
        return false;
    }
    if (transport_mode == MODE_NETWORK_ERROR) return false;
    if (transport_mode == MODE_HTTP_ERROR) { *status = 404; return true; }
    if (transport_mode == MODE_TOO_LARGE) {
        /* The real transport reports the length and leaves the body unread. */
        *out_length = MODEL_SHOW_MAX_RESPONSE + 1;
        *status = 200;
        return true;
    }
    const char *payload = transport_mode == MODE_TEXT ? text_payload
        : transport_mode == MODE_OK ? vision_payload : garbage_payload;
    size_t size = strlen(payload);
    char *copy = (char *)malloc(size + 1);
    if (!copy) return false;
    memcpy(copy, payload, size + 1);
    *out_body = copy;
    *out_length = size;
    *status = 200;
    return true;
}

static ModelShowEvent *received;
static LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    if (message == CHAT_WM_MODEL_SHOW_EVENT) {
        received = (ModelShowEvent *)l;
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static void pump(void) {
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static bool await_event(DWORD timeout) {
    ULONGLONG deadline = GetTickCount64() + timeout;
    while (!received && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        pump();
    }
    return received != NULL;
}

static void free_received(void) {
    if (received) { model_show_event_free(received); received = NULL; }
}

/* WinHTTP handle accounting is not observable from here: MinGW resolves
   WinHTTP calls through the import address table, so the linker's --wrap never
   sees them. What the real transport can still be asked for, against a port
   nothing listens on, is its failure contract: it reports the failure with an
   owned error string and hands back no buffer, which is the same single exit
   the cancellation path uses. */
static int transport_failure_contract(void) {
    static const ModelShowEndpoint dead = { L"localhost", 1, L"/api/show",
        L"DarkChat/0.4" };
    char *body = NULL;
    size_t length = 0;
    DWORD status = 0;
    wchar_t *error = NULL;
    ModelShowCancel not_cancelled = { NULL, 0 };
    CHECK(!winhttp_transport(&dead, SHOW_HEADERS, "{}", 2, &not_cancelled,
        &body, &length, &status, &error));
    CHECK(error != NULL);
    CHECK(body == NULL && length == 0);
    free(error);
    return 0;
}

int main(void) {
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = test_proc;
    cls.lpszClassName = L"DarkChat.ModelShowTest";
    CHECK(RegisterClassW(&cls));
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, NULL, NULL);
    CHECK(window);
    transport_release = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(transport_release);
    /* The slot after the other completion messages; a collision would make a
       catalog result read as a capability result. */
    CHECK(CHAT_WM_MODEL_SHOW_EVENT == WM_APP + 0x51);
    model_show_transport = fake_transport;
    ModelShowClient client;
    model_show_client_init(&client, window, CHAT_WM_MODEL_SHOW_EVENT);
    int generation;

    /* A request that cannot name a model is refused before anything starts. */
    CHECK(model_show_request(&client, NULL) == 0);
    CHECK(model_show_request(&client, L"") == 0);
    wchar_t over_long[CHAT_MODEL_TEXT + 4];
    for (size_t i = 0; i < CHAT_MODEL_TEXT + 3; i++) over_long[i] = L'a';
    over_long[CHAT_MODEL_TEXT + 3] = 0;
    CHECK(model_show_request(&client, over_long) == 0);
    CHECK(model_show_request(NULL, L"qwen3-vl:8b") == 0);
    CHECK(!model_show_busy(&client) && transport_calls == 0);

    /* A successful probe reports the classified capability and the id. */
    transport_mode = MODE_OK;
    generation = model_show_request(&client, L"qwen3-vl:8b");
    CHECK(generation > 0 && model_show_busy(&client));
    CHECK(await_event(5000));
    CHECK(received->result == MODEL_SHOW_OK);
    CHECK(received->capability == CHAT_CAP_YES && received->error == NULL);
    CHECK(received->generation == generation);
    CHECK(received->id && !wcscmp(received->id, L"qwen3-vl:8b"));
    CHECK(!strcmp(recorded_body, "{\"model\":\"qwen3-vl:8b\"}"));
    CHECK(recorded_body_length == strlen(recorded_body));
    /* No credential may ever reach a loopback request. */
    CHECK(!wcsstr(recorded_headers, L"Authorization") &&
        !wcsstr(recorded_headers, L"Bearer"));
    free_received();
    model_show_complete(&client, generation);
    CHECK(!model_show_busy(&client));

    /* The body is encoded, not pasted: a quote, a backslash and non-ASCII in
       the id cannot produce a malformed request. */
    generation = model_show_request(&client, L"we\"ird\\mo\u00e9del");
    CHECK(generation > 0 && await_event(5000));
    CHECK(!strcmp(recorded_body,
        "{\"model\":\"we\\\"ird\\\\mo\xc3\xa9" "del\"}"));
    free_received();
    model_show_complete(&client, generation);

    /* A readable capability list without vision is a proven refusal. */
    transport_mode = MODE_TEXT;
    generation = model_show_request(&client, L"llama3.2:latest");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_SHOW_OK &&
        received->capability == CHAT_CAP_NO);
    CHECK(received->id && !wcscmp(received->id, L"llama3.2:latest"));
    free_received();
    model_show_complete(&client, generation);

    /* Every failure result reports UNKNOWN, never a refusal. */
    transport_mode = MODE_HTTP_ERROR;
    generation = model_show_request(&client, L"gpt-oss:20b");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_SHOW_HTTP_ERROR);
    CHECK(received->capability == CHAT_CAP_UNKNOWN && received->error);
    CHECK(wcsstr(received->error, L"404") != NULL);
    free_received();
    model_show_complete(&client, generation);

    transport_mode = MODE_NETWORK_ERROR;
    generation = model_show_request(&client, L"gpt-oss:20b");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_SHOW_NETWORK_ERROR);
    CHECK(received->capability == CHAT_CAP_UNKNOWN && received->error);
    free_received();
    model_show_complete(&client, generation);

    transport_mode = MODE_TOO_LARGE;
    generation = model_show_request(&client, L"gpt-oss:20b");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_SHOW_TOO_LARGE);
    CHECK(received->capability == CHAT_CAP_UNKNOWN && received->error);
    free_received();
    model_show_complete(&client, generation);

    /* A 200 whose body does not classify is a successful probe with an honest
       answer, not a transport failure. */
    transport_mode = MODE_GARBAGE;
    generation = model_show_request(&client, L"mystery:1b");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_SHOW_OK);
    CHECK(received->capability == CHAT_CAP_UNKNOWN && received->error == NULL);
    free_received();
    model_show_complete(&client, generation);

    /* One probe at a time; the refused one asks nothing of the network. */
    transport_mode = MODE_BLOCK;
    ResetEvent(transport_release);
    int before = transport_calls;
    generation = model_show_request(&client, L"blocked:1b");
    CHECK(generation > 0);
    CHECK(model_show_request(&client, L"other:1b") == 0);
    SetEvent(transport_release);
    CHECK(await_event(5000));
    CHECK(transport_calls == before + 1);
    free_received();
    model_show_complete(&client, generation);
    /* Generations advance, so a stale completion can never match a later
       request's id. */
    transport_mode = MODE_OK;
    int next = model_show_request(&client, L"later:1b");
    CHECK(next > generation);
    CHECK(await_event(5000));
    free_received();
    model_show_complete(&client, next);
    CHECK(!model_show_busy(&client));

    /* Allocation failure while starting refuses the request outright: the id
       copy first, then the encoded body. */
    alloc_fail_countdown = 0;
    CHECK(model_show_request(&client, L"oom:1b") == 0);
    alloc_fail_countdown = 1;
    CHECK(model_show_request(&client, L"oom:1b") == 0);
    alloc_fail_countdown = -1;
    /* (two requests succeeded since `before`: the blocked one and `later`) */
    CHECK(!model_show_busy(&client) && transport_calls == before + 2);

    /* A failed completion post must not wedge the client. */
    client.notify = (HWND)1;
    generation = model_show_request(&client, L"unposted:1b");
    CHECK(generation > 0);
    model_show_complete(&client, generation);
    CHECK(!model_show_busy(&client));
    client.notify = window;

    /* Shutdown cancels and joins an in-flight probe without delivering it. */
    transport_mode = MODE_CANCEL;
    received = NULL;
    generation = model_show_request(&client, L"cancelled:1b");
    CHECK(generation > 0);
    Sleep(50);
    model_show_shutdown(&client);
    CHECK(!model_show_busy(&client));
    pump();
    CHECK(!received);
    /* A shutdown with nothing in flight is a no-op, not a crash. */
    model_show_shutdown(&client);

    /* The real transport, against a port nothing listens on. */
    CHECK(transport_failure_contract() == 0);

    DestroyWindow(window);
    puts("Model show probe body/headers/capability/failure/cancel passed");
    return 0;
}
