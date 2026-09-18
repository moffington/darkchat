/* Deterministic worker tests. The module is included so the transport seam
   and the internal response constants are reachable without touching the
   network; malloc is wrapped so the allocation-failure path is exercised. */
#include "../chat/model_catalog_winhttp.c"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static long alloc_fail_countdown = -1;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_countdown >= 0) {
        if (alloc_fail_countdown == 0) return NULL;
        --alloc_fail_countdown;
    }
    return __real_malloc(size);
}

enum { MODE_OK, MODE_HTTP_ERROR, MODE_TOO_LARGE, MODE_BLOCK, MODE_CANCEL };
static int transport_mode;
static HANDLE transport_release;
static const char *transport_payload =
    "{\"data\":[{\"id\":\"openai/gpt-4\",\"name\":\"GPT-4\","
    "\"context_length\":8192}]}";

static bool fake_transport(ChatBackend backend,
    const CatalogEndpoint *endpoint, const wchar_t *headers,
    const ModelCatalogCancel *cancel, char **body, size_t *length,
    DWORD *status, wchar_t **error) {
    (void)backend; (void)endpoint; (void)headers; (void)error;
    *body = NULL; *length = 0; *status = 0;
    if (transport_mode == MODE_BLOCK)
        WaitForSingleObject(transport_release, INFINITE);
    if (transport_mode == MODE_CANCEL) {
        while (!InterlockedCompareExchange((LONG *)cancel->cancelled, 0, 0))
            Sleep(5);
        return false;
    }
    if (transport_mode == MODE_TOO_LARGE) {
        *length = MODEL_CATALOG_MAX_RESPONSE + 1;
        *status = 200;
        return true;
    }
    if (transport_mode == MODE_HTTP_ERROR) {
        *status = 503;
        return true;
    }
    size_t size = strlen(transport_payload);
    char *copy = (char *)malloc(size + 1);
    if (!copy) return false;
    memcpy(copy, transport_payload, size + 1);
    *body = copy;
    *length = size;
    *status = 200;
    return true;
}

static ModelCatalogEvent *received;
static LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    if (message == CHAT_WM_CATALOG_EVENT) {
        received = (ModelCatalogEvent *)l;
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
    if (received) { model_catalog_event_free(received); received = NULL; }
}

int main(void) {
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = test_proc;
    cls.lpszClassName = L"DarkChat.CatalogTest";
    CHECK(RegisterClassW(&cls));
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, NULL, NULL);
    CHECK(window);
    transport_release = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(transport_release);
    model_catalog_transport = fake_transport;
    ModelCatalogClient client;
    model_catalog_client_init(&client, window, CHAT_WM_CATALOG_EVENT);
    int generation;

    /* Missing key: no OpenRouter worker is started. Ollama needs no key. */
    CHECK(model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, NULL) == 0);
    CHECK(model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "") == 0);
    CHECK(!model_catalog_busy(&client));
    transport_mode = MODE_OK;
    generation = model_catalog_request(&client, CHAT_BACKEND_OLLAMA, NULL);
    CHECK(generation > 0 && model_catalog_busy(&client));
    CHECK(await_event(5000));
    CHECK(received->result == MODEL_CATALOG_OK && received->json);
    free_received();
    model_catalog_complete(&client, generation);
    CHECK(!model_catalog_busy(&client));
    /* An out-of-range backend is rejected. */
    CHECK(model_catalog_request(&client, (ChatBackend)7, "key") == 0);

    /* Successful fetch. */
    transport_mode = MODE_OK;
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0 && model_catalog_busy(&client));
    CHECK(await_event(5000));
    CHECK(received->result == MODEL_CATALOG_OK && received->json);
    {
        ChatModelCatalog catalog;
        chat_model_catalog_init(&catalog);
        ChatModelParseStats stats;
        CHECK(chat_model_catalog_parse(&catalog, received->json, &stats));
        CHECK(catalog.count == 1 && !wcscmp(catalog.items[0].id, L"openai/gpt-4"));
        chat_model_catalog_dispose(&catalog);
    }
    free_received();
    model_catalog_complete(&client, generation);
    CHECK(!model_catalog_busy(&client));

    /* Non-2xx is rejected before parsing. */
    transport_mode = MODE_HTTP_ERROR;
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_CATALOG_HTTP_ERROR && !received->json &&
        received->error);
    free_received();
    model_catalog_complete(&client, generation);

    /* Oversized body is refused. */
    transport_mode = MODE_TOO_LARGE;
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0 && await_event(5000));
    CHECK(received->result == MODEL_CATALOG_TOO_LARGE && !received->json);
    free_received();
    model_catalog_complete(&client, generation);

    /* A second fetch is refused while one is in flight. */
    transport_mode = MODE_BLOCK;
    ResetEvent(transport_release);
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0);
    CHECK(model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key") == 0);
    SetEvent(transport_release);
    CHECK(await_event(5000));
    free_received();
    model_catalog_complete(&client, generation);

    /* Allocation failure while building headers refuses to start. */
    alloc_fail_countdown = 0;
    CHECK(model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key") == 0);
    alloc_fail_countdown = -1;

    /* A failed completion post must not wedge the client: the worker exits and
       can be reaped. */
    transport_mode = MODE_OK;
    client.notify = (HWND)1;
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0);
    model_catalog_complete(&client, generation);
    CHECK(!model_catalog_busy(&client));
    client.notify = window;

    /* Shutdown cancels and joins an in-flight fetch. */
    transport_mode = MODE_CANCEL;
    received = NULL;
    generation = model_catalog_request(&client, CHAT_BACKEND_OPENROUTER, "key");
    CHECK(generation > 0);
    Sleep(50);
    model_catalog_shutdown(&client);
    CHECK(!model_catalog_busy(&client));
    pump();
    CHECK(!received);

    DestroyWindow(window);
    puts("Model catalog worker success/error/limit/duplicate/shutdown passed");
    return 0;
}
