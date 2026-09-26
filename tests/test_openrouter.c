/* Includes the client to test its actual request encoder and SSE event decoder
   for both backends. */
#include "chat/generation/completion_winhttp.c"
#include "chat/generation/completion_request.h"
#include "chat/generation/context.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int deltas, reasons, terminal, wakes;
static wchar_t reasoning_text[256];
static wchar_t delta_text[4096];
/* Terminal event text, captured for the live probes: the non-vision image
   send records the provider's ACTUAL error rather than assuming one. */
static wchar_t terminal_text[512];
/* Free watch for the scrub proof: the test registers the owned image buffer
   (readable from the work item) before free_work, and the wrap proves the
   bytes were already zeroed when free saw them. */
static unsigned char *free_watch_ptr;
static size_t free_watch_n;
static bool free_watch_seen, free_watch_zeroed;
void __real_free(void *pointer);
void __wrap_free(void *pointer) {
    if (pointer && pointer == free_watch_ptr) {
        free_watch_seen = true;
        free_watch_zeroed = true;
        for (size_t i = 0; i < free_watch_n; i++)
            if (free_watch_ptr[i]) { free_watch_zeroed = false; break; }
        free_watch_ptr = NULL;
    }
    __real_free(pointer);
}
static CompletionEventType order[16];
static int order_count;
static CompletionEventType outcome;
static ChatGeneration metadata;
static int generation;
/* File scope: the wake handler drains this client's queue. */
static CompletionClient client;
static LRESULT CALLBACK test_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
    if (msg==CHAT_WM_COMPLETION_EVENT) {
        /* The wake carries no payload: take the whole queued batch. */
        ++wakes;
        CompletionEvent *batch=completion_take(&client);
        while (batch) {
            CompletionEvent *e=batch;
            batch=batch->next;
            e->next=NULL;
            if (e->generation==generation) {
                if (order_count<16) order[order_count++]=e->type;
                if (e->type==COMPLETION_DELTA) {
                    if (e->text && e->text[0]) {
                        ++deltas;
                        wcsncat(delta_text,e->text,
                            (sizeof delta_text/sizeof *delta_text)-1
                                -wcslen(delta_text));
                    }
                }
                else if (e->type==COMPLETION_REASONING) {
                    if (e->text && e->text[0]) {
                        ++reasons;
                        wcsncat(reasoning_text,e->text,
                            (sizeof reasoning_text/sizeof *reasoning_text)-1
                                -wcslen(reasoning_text));
                    }
                }
                else {
                    ++terminal; outcome=e->type;
                    if (e->text) {
                        wcsncpy(terminal_text,e->text,
                            (sizeof terminal_text/sizeof *terminal_text)-1);
                        terminal_text[(sizeof terminal_text/
                            sizeof *terminal_text)-1]=0;
                    }
                }
                metadata=e->metadata;
            }
            completion_event_free(e);
        }
        return 0;
    }
    return DefWindowProcW(window,msg,w,l);
}
static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
static bool await_terminal(DWORD timeout) {
    ULONGLONG deadline=GetTickCount64()+timeout;
    while (!terminal && GetTickCount64()<deadline) {
        MsgWaitForMultipleObjects(0,NULL,FALSE,100,QS_ALLINPUT); pump();
    }
    return terminal==1;
}
/* Flushes a stream's coalescing buffers and pumps the wake through. Every
   decode assertion runs after this; buffering means stream_event alone no
   longer delivers. */
#define DELIVER(s) do { CHECK(stream_flush(s)); pump(); } while (0)
/* Drops undelivered fragments after a forced failure reset, so pending
   text from the failing event cannot leak into later counts. */
static void drop_pending(Stream *s) {
    fragments_free(&s->delta);
    fragments_free(&s->reason);
}
typedef struct { AsyncState *state; DWORD status, delay; } DelayedSignal;
static unsigned __stdcall signal_after(void *parameter) {
    DelayedSignal *signal=(DelayedSignal *)parameter;
    Sleep(signal->delay);
    InterlockedExchange(&signal->state->status,(LONG)signal->status);
    SetEvent(signal->state->event);
    return 0;
}
/* Builds a request body through the real work adapter. */
static bool encode(CompletionWork *work, JsonBuf *body) {
    return build_request(work, body);
}
/* The host's commit-7 job: point every run's image slot at fixture bytes of
   exactly byte_length so the pure encoder can be exercised against a built
   view. The calculator only ever reads the metadata. */
static void supply_bytes(ChatRequestContext *context, unsigned char *blob,
    size_t n) {
    for (int i=0;i<context->part_slots_used;i++) {
        ChatRequestPart *part=&context->part_scratch[i];
        if (part->kind==CHAT_PART_IMAGE && part->u.image.byte_length<=n)
            part->u.image.bytes=blob;
    }
}
int main(int argc,char **argv) {
    WNDCLASSW cls={0}; cls.lpfnWndProc=test_proc; cls.lpszClassName=L"DarkChat.NetworkTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,NULL,NULL); CHECK(window);
    completion_init(&client,window,CHAT_WM_COMPLETION_EVENT);
    CompletionWork work={0}; work.notify=window; work.message=CHAT_WM_COMPLETION_EVENT;
    work.client=&client; work.generation=7; generation=7;
    work.backend=CHAT_BACKEND_OPENROUTER;
    work.reasoning=true;
    work.started_tick=GetTickCount64(); chat_generation_init(&work.metadata);
    work.metadata.backend=CHAT_BACKEND_OPENROUTER;
    Stream stream={0}; stream.work=&work; stream.flush_tick=GetTickCount64();
    const char *chunk="{\"model\":\"actual/model\",\"choices\":[{\"delta\":{\"content\":\"Hi\\uD83D\\uDE80\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    /* Below the burst threshold and cadence, the fragment stays buffered
       ("Hi" plus the U+1F680 surrogate pair: 4 UTF-16 units). */
    CHECK(stream.delta.length==4 && deltas==0 && terminal==0);
    DELIVER(&stream); CHECK(deltas==1 &&
        !wcscmp(delta_text,L"Hi" L"\xD83D" L"\xDE80"));
    CHECK(work.metadata.ttft_ms>=0 && work.metadata.first_token_at>0);
    chunk="{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5,\"cost\":0.00002}}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    CHECK(work.metadata.prompt_tokens==3 && work.metadata.completion_tokens==2 && work.metadata.total_tokens==5);
    CHECK(work.metadata.cost==0.00002 && !wcscmp(work.metadata.actual_model,L"actual/model"));
    CHECK(stream_event(&stream,"[DONE]",6) && stream.done);
    chunk="{\"choices\":[{\"delta\":{\"content\":\"partial\"}}],\"error\":{\"message\":\"provider failed\"}}";
    CHECK(!stream_event(&stream,chunk,strlen(chunk)) && stream.failed && stream.error);
    free(stream.error); stream.error=NULL; stream.failed=false; drop_pending(&stream);
    CHECK(!stream_event(&stream,"{bad}",5)); free(stream.error); drop_pending(&stream); pump();
    /* Reasoning is parsed from structured details and compatibility fallbacks;
       an entry with no displayable text never fabricates one. */
    stream.error=NULL; stream.failed=false; reasons=0; deltas=0;
    reasoning_text[0]=0;
    stream.flush_tick=GetTickCount64();
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"step one \"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream); CHECK(reasons==1);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.summary\",\"summary\":\"summary\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream); CHECK(reasons==2);
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"plain text\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream); CHECK(reasons==3);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream); CHECK(reasons==3);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"multi one \"},{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"},{\"type\":\"reasoning.summary\",\"summary\":\"multi two\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream);
    CHECK(reasons==4 && wcsstr(reasoning_text,L"multi one multi two"));
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}],\"reasoning_content\":\"alias text\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream);
    CHECK(reasons==5 && wcsstr(reasoning_text,L"alias text"));
    chunk="{\"choices\":[{\"delta\":{\"content\":\"answer only\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); DELIVER(&stream); CHECK(reasons==5 && deltas==1);
    /* Ollama's OpenAI-compatible reasoning output arrives through the same
       parser fallbacks (reasoning, then reasoning_content). */
    stream.error=NULL; stream.failed=false; reasons=0; deltas=0; reasoning_text[0]=0;
    CompletionWork ollama=work; ollama.backend=CHAT_BACKEND_OLLAMA;
    ollama.metadata.backend=CHAT_BACKEND_OLLAMA;
    Stream ollama_stream={0}; ollama_stream.work=&ollama;
    ollama_stream.flush_tick=GetTickCount64();
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"local step \"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); DELIVER(&ollama_stream); CHECK(reasons==1);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_content\":\"more\"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); DELIVER(&ollama_stream);
    CHECK(reasons==2 && wcsstr(reasoning_text,L"local step more"));
    chunk="{\"model\":\"llama3\",\"choices\":[{\"delta\":{\"content\":\"local answer\"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); DELIVER(&ollama_stream); CHECK(deltas==1);
    chunk="{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":4,\"total_tokens\":11}}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk)));
    CHECK(ollama.metadata.prompt_tokens==7 && ollama.metadata.completion_tokens==4 &&
        ollama.metadata.total_tokens==11);
    CHECK(!wcscmp(ollama.metadata.actual_model,L"llama3"));
    CHECK(!stream_event(&ollama_stream,"{oops",5) && ollama_stream.failed);
    free(ollama_stream.error); ollama_stream.error=NULL; ollama_stream.failed=false;
    drop_pending(&ollama_stream);
    CHECK(stream_event(&ollama_stream,"[DONE]",6) && ollama_stream.done);
    /* Backend identity travels with the generation metadata. */
    CHECK(ollama.metadata.backend==CHAT_BACKEND_OLLAMA &&
        work.metadata.backend==CHAT_BACKEND_OPENROUTER);

    /* Coalescing: fragments concatenate per type in arrival order, one
       flush enqueues reasoning before content, and a single outstanding
       wake delivers the whole batch. Nothing is delivered by pushes alone
       (no read boundary exists in this harness, and the cadence is reset). */
    stream.error=NULL; stream.failed=false; stream.done=false;
    drop_pending(&stream);
    reasons=0; deltas=0; wakes=0; order_count=0;
    reasoning_text[0]=0; delta_text[0]=0;
    stream.flush_tick=GetTickCount64();
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"one \"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"two\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    chunk="{\"choices\":[{\"delta\":{\"content\":\"alpha \"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    chunk="{\"choices\":[{\"delta\":{\"content\":\"beta\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    CHECK(stream.reason.length==7 && stream.delta.length==10);
    CHECK(wakes==0 && reasons==0 && deltas==0);
    CHECK(stream_flush(&stream)); pump();
    CHECK(wakes==1 && reasons==1 && deltas==1);
    CHECK(!wcscmp(reasoning_text,L"one two") && !wcscmp(delta_text,L"alpha beta"));
    CHECK(order_count==2 && order[0]==COMPLETION_REASONING &&
        order[1]==COMPLETION_DELTA);
    /* A burst past the coalesce threshold delivers mid-stream without an
       explicit flush, still as one concatenated event per type. */
    reasons=0; deltas=0; wakes=0; reasoning_text[0]=0; delta_text[0]=0;
    stream.flush_tick=GetTickCount64();
    {
        static wchar_t big[1200];
        static char burst[16000];
        for (int i=0;i<1199;i++) big[i]=L'x';
        big[1199]=0;
        snprintf(burst,sizeof burst,
            "{\"choices\":[{\"delta\":{\"content\":\"%ls\"}}]}",big);
        CHECK(stream_event(&stream,burst,strlen(burst)));
        CHECK(stream.delta.length==0 && stream.reason.length==0);
        pump();
        CHECK(deltas==1 && (int)wcslen(delta_text)==1199 && wakes==1);
        CHECK(stream_event(&stream,burst,strlen(burst)));
        CHECK(stream.delta.length==0 && stream.reason.length==0);
        pump();
        CHECK(deltas==2 && (int)wcslen(delta_text)==2398 && wakes==2);
    }
    /* A quiet network wait enforces the cadence even when no later fragment
       arrives to call stream_push. The delayed expected callback keeps
       wait_status blocked long enough for its timeout path to flush. */
    deltas=0; wakes=0; delta_text[0]=0;
    drop_pending(&stream);
    stream.flush_tick=GetTickCount64();
    chunk="{\"choices\":[{\"delta\":{\"content\":\"quiet\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)) && stream.delta.length==5);
    {
        AsyncState state={0};
        state.event=CreateEventW(NULL,TRUE,FALSE,NULL);
        CHECK(state.event);
        DelayedSignal signal={&state,
            WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,STREAM_FLUSH_MS*2+50};
        HANDLE thread=(HANDLE)_beginthreadex(NULL,0,signal_after,&signal,0,NULL);
        CHECK(thread);
        CHECK(wait_status(&work,&state,
            WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,&stream));
        WaitForSingleObject(thread,INFINITE);
        CloseHandle(thread); CloseHandle(state.event);
    }
    pump();
    CHECK(stream.delta.length==0 && deltas==1 && wakes==1 &&
        !wcscmp(delta_text,L"quiet"));
    /* A cancelled generation drops content at delivery, keeps the queue
       empty, and leaves the stream's failure state clean. */
    deltas=0; wakes=0; delta_text[0]=0; stream.done=false;
    drop_pending(&stream);
    chunk="{\"choices\":[{\"delta\":{\"content\":\"gone\"}}]}";
    stream.flush_tick=GetTickCount64();
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    CHECK(stream.delta.length==4);
    InterlockedExchange(&client.cancelled_generation,7);
    CHECK(!stream_flush(&stream) && !stream.failed && !stream.error);
    CHECK(deltas==0 && wakes==0 && stream.delta.length==0);
    InterlockedExchange(&client.cancelled_generation,0);

    work.model=L"test/model";
    ChatRole roles[]={CHAT_ROLE_SYSTEM,CHAT_ROLE_USER,CHAT_ROLE_ERROR};
    wchar_t *texts[]={L"system",L"question \"quoted\"",L"local error"};
    work.roles=roles; work.texts=texts; work.count=3;
    JsonBuf body; CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"reasoning\":{\"enabled\":true}")!=NULL);
    char value[128]; CHECK(json_query_string(body.data,"messages[1].content",value,sizeof value));
    CHECK(!json_query_string(body.data,"messages[2].content",value,sizeof value));
    /* OpenRouter bytes are unchanged: the all-error-skipping fixture encodes to
       exactly this pinned body, with no provider object at default routing. */
    CHECK(!strcmp(body.data,
        "{\"model\":\"test/model\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"system\"},"
        "{\"role\":\"user\",\"content\":\"question \\\"quoted\\\"\"}],"
        "\"stream\":true,\"reasoning\":{\"enabled\":true}}"));
    /* Suppressed reasoning omits the object entirely and keeps the body
       otherwise identical. */
    work.reasoning=false;
    JsonBuf plain; CHECK(encode(&work,&plain)); CHECK(json_validate(plain.data));
    CHECK(strstr(plain.data,"\"reasoning\"")==NULL);
    CHECK(!strcmp(plain.data,
        "{\"model\":\"test/model\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"system\"},"
        "{\"role\":\"user\",\"content\":\"question \\\"quoted\\\"\"}],"
        "\"stream\":true}"));
    json_buf_free(&plain);
    work.reasoning=true;
    /* OpenRouter credentials and attribution headers are unchanged. */
    {
        static char key[]="secret-key";
        work.api_key=key;
        wchar_t *headers=build_headers(&work);
        CHECK(headers && wcscmp(headers,L"Content-Type: application/json\r\n"
            L"Accept: text/event-stream\r\nAuthorization: Bearer secret-key\r\n"
            L"X-Title: DarkChat\r\nHTTP-Referer: https://localhost/darkchat\r\n")==0);
        free(headers);
        work.api_key=NULL;
    }
    json_buf_free(&body);
    /* Each control serializes to its exact request JSON shape and only the
       non-default keys are emitted, in a fixed order. */
    work.routing.sort=CHAT_PROVIDER_SORT_THROUGHPUT;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"provider\":{\"sort\":\"throughput\"}")!=NULL);
    json_buf_free(&body);
    work.routing.sort=CHAT_PROVIDER_SORT_DEFAULT;
    work.routing.disallow_fallbacks=true;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"provider\":{\"allow_fallbacks\":false}")!=NULL);
    json_buf_free(&body);
    work.routing.disallow_fallbacks=false;
    work.routing.data_collection=CHAT_DATA_COLLECTION_DENY;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"provider\":{\"data_collection\":\"deny\"}")!=NULL);
    json_buf_free(&body);
    work.routing.data_collection=CHAT_DATA_COLLECTION_ALLOW;
    work.routing.zdr=true;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"provider\":{\"zdr\":true}")!=NULL);
    json_buf_free(&body);
    /* All four together, in the documented fixed order. */
    work.routing.sort=CHAT_PROVIDER_SORT_PRICE;
    work.routing.disallow_fallbacks=true;
    work.routing.data_collection=CHAT_DATA_COLLECTION_DENY;
    work.routing.zdr=true;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,
        "\"provider\":{\"sort\":\"price\",\"allow_fallbacks\":false,"
        "\"data_collection\":\"deny\",\"zdr\":true}")!=NULL);
    json_buf_free(&body);
    chat_provider_routing_init(&work.routing);
    /* Ollama: streamed usage is requested, and no reasoning object, provider
       object, credential or attribution header is ever sent. */
    work.backend=CHAT_BACKEND_OLLAMA;
    work.routing.sort=CHAT_PROVIDER_SORT_PRICE;
    work.routing.disallow_fallbacks=true;
    work.routing.data_collection=CHAT_DATA_COLLECTION_DENY;
    work.routing.zdr=true;
    CHECK(encode(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"stream_options\":{\"include_usage\":true}")!=NULL);
    CHECK(strstr(body.data,"\"reasoning\"")==NULL);
    CHECK(strstr(body.data,"\"provider\"")==NULL);
    CHECK(strstr(body.data,"\"stream\":true")!=NULL);
    {
        wchar_t *headers=build_headers(&work);
        CHECK(headers && !wcsstr(headers,L"Authorization") &&
            !wcsstr(headers,L"X-Title") && !wcsstr(headers,L"HTTP-Referer") &&
            !wcsstr(headers,L"Bearer"));
        free(headers);
        /* A key set for another backend must still not leak into Ollama. */
        work.api_key=(char *)"leak";
        headers=build_headers(&work);
        CHECK(headers && !wcsstr(headers,L"leak") && !wcsstr(headers,L"Authorization"));
        free(headers);
        work.api_key=NULL;
    }
    json_buf_free(&body);
    /* The Ollama envelope constant is exactly what the encoder writes. */
    CHECK(chat_completion_envelope_bytes(CHAT_BACKEND_OLLAMA,work.model,NULL,
        true)==78 + json_encoded_string_size(work.model));
    work.backend=CHAT_BACKEND_OPENROUTER;
    chat_provider_routing_init(&work.routing);
    /* The request context's measured size must equal the body the real encoder
       produces for both backends. This is the invariant the budget policy
       depends on: the context decides what to drop by measuring exactly what
       will be sent. */
    Chat *context_chat=(Chat *)calloc(1,sizeof *context_chat); CHECK(context_chat);
    chat_init(context_chat); chat_clear(context_chat);
    wcscpy(context_chat->system_prompt,L"Answer in one short sentence.");
    for (int turn=0;turn<3;turn++) {
        wchar_t question[64];
        swprintf(question,64,L"question %d \u2014 unicode \u00e9",turn);
        chat_append(context_chat,CHAT_ROLE_USER,question);
        int index=chat_append(context_chat,CHAT_ROLE_ASSISTANT,
            L"answer with \"quotes\", a\ttab and a newline\nsecond line");
        context_chat->conversations[0].messages[index].generation.state=CHAT_GENERATION_COMPLETE;
    }
    int failed=chat_append(context_chat,CHAT_ROLE_ASSISTANT,L"partial");
    context_chat->conversations[0].messages[failed].generation.state=CHAT_GENERATION_FAILED;
    chat_append(context_chat,CHAT_ROLE_USER,L"final \U0001f600 question");
    int trigger=(int)context_chat->conversations[0].message_count-1;
    /* Static: the context carries its part_scratch pool and is far past the
       safe stack budget for a test frame. */
    static ChatRequestContext context;
    CHECK(chat_context_build(context_chat,&context_chat->conversations[0],trigger,
        CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
    CHECK(context.count==8 && context.dropped_messages==0);
    CompletionWork sized={0};
    sized.reasoning=true;   /* the default conversation asks for reasoning */
    ChatRole sized_roles[CHAT_CONTEXT_MAX_ENTRIES];
    wchar_t *sized_texts[CHAT_CONTEXT_MAX_ENTRIES];
    for (int i=0;i<context.count;i++) {
        sized_roles[i]=context.messages[i].role;
        sized_texts[i]=(wchar_t *)context.messages[i].text;
    }
    sized.model=context_chat->model; sized.roles=sized_roles; sized.texts=sized_texts;
    sized.count=context.count;
    JsonBuf measured; CHECK(encode(&sized,&measured));
    CHECK(json_validate(measured.data));
    CHECK(measured.length==context.bytes);
    size_t messages_in_body=0;
    CHECK(json_query_array_length(measured.data,"messages",&messages_in_body) &&
        messages_in_body==(size_t)context.count);
    CHECK(json_query_string(measured.data,"messages[0].content",value,sizeof value) &&
        !strcmp(value,"Answer in one short sentence."));
    json_buf_free(&measured);
    /* The budget also accounts for a non-default provider object; the measured
       size must still equal the bytes the encoder actually writes. */
    context_chat->provider_routing.sort=CHAT_PROVIDER_SORT_LATENCY;
    context_chat->provider_routing.disallow_fallbacks=true;
    context_chat->provider_routing.data_collection=CHAT_DATA_COLLECTION_DENY;
    context_chat->provider_routing.zdr=true;
    CHECK(chat_context_build(context_chat,&context_chat->conversations[0],trigger,
        CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
    sized.routing=context_chat->provider_routing;
    JsonBuf routed; CHECK(encode(&sized,&routed));
    CHECK(json_validate(routed.data));
    CHECK(routed.length==context.bytes);
    CHECK(strstr(routed.data,"\"provider\":{")!=NULL);
    json_buf_free(&routed);
    /* The same equality holds for Ollama, whose envelope is different and
       carries neither the provider object nor a reasoning control. */
    chat_provider_routing_init(&context_chat->provider_routing);
    context_chat->backend=CHAT_BACKEND_OLLAMA;
    wcscpy(context_chat->ollama_model,L"local/model:latest");
    CHECK(chat_context_build(context_chat,&context_chat->conversations[0],trigger,
        CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
    sized.backend=CHAT_BACKEND_OLLAMA;
    sized.model=context_chat->ollama_model;
    json_buf_free(&measured);
    CHECK(encode(&sized,&measured));
    CHECK(json_validate(measured.data));
    CHECK(measured.length==context.bytes);
    CHECK(strstr(measured.data,"\"stream_options\":{\"include_usage\":true}")!=NULL);
    CHECK(strstr(measured.data,"\"provider\"")==NULL);
    json_buf_free(&measured);
    /* Reasoning suppressed: the object leaves both the body and the measured
       envelope, and the budget parity invariant still holds. */
    context_chat->backend=CHAT_BACKEND_OPENROUTER;
    context_chat->conversations[0].reasoning_disabled=true;
    CHECK(chat_context_build(context_chat,&context_chat->conversations[0],trigger,
        CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
    sized.backend=CHAT_BACKEND_OPENROUTER;
    sized.model=context_chat->model;
    sized.routing=context_chat->provider_routing;
    sized.reasoning=false;
    JsonBuf plain_body; CHECK(encode(&sized,&plain_body));
    CHECK(json_validate(plain_body.data));
    CHECK(plain_body.length==context.bytes);
    CHECK(strstr(plain_body.data,"\"reasoning\"")==NULL);
    json_buf_free(&plain_body);
    context_chat->conversations[0].reasoning_disabled=false;
    /* Commit-6: content-array encoding. The host's commit-7 job of loading
       blob bytes is simulated by pointing each run's image slot at fixture
       bytes of exactly byte_length; the calculator must agree with the
       encoder either way. */
    {
        static unsigned char blob[70000];
        for (size_t i=0;i<sizeof blob;i++) blob[i]=(unsigned char)(i&0xff);
        unsigned char one_byte=0x41;   /* base64 "QQ==" */
        ChatAttachmentMeta rec={0};
        rec.id=1;
        for (int i=0;i<64;i++) rec.digest[i]='a';
        rec.digest[64]=0;
        strcpy(rec.mime,"image/png");
        rec.bytes=1000;
        rec.created_at=1;
        wcscpy(rec.display_name,L"photo.png");
        CHECK(chat_attachment_add(context_chat,&rec));
        ChatImagePart meta={0};
        meta.attachment_id=1;
        strcpy(meta.mime,"image/png");
        wcscpy(meta.display_name,L"photo.png");
        int shown=chat_append(context_chat,CHAT_ROLE_USER,L"what's this?");
        CHECK(chat_message_add_image(&context_chat->conversations[0].
            messages[shown],&meta,0));
        int ask=chat_append(context_chat,CHAT_ROLE_USER,L"and this?");
        CHECK(chat_context_build(context_chat,&context_chat->conversations[0],
            ask,CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
        /* The multimodal turn is the entry before the trigger. */
        int mm=context.count-2;
        CHECK(context.messages[mm].part_count==2);      /* TEXT + IMAGE */
        CHECK(context.messages[mm].parts!=NULL);
        CHECK(!wcscmp(context.messages[mm].text,L"what's this?"));
        supply_bytes(&context,blob,sizeof blob);
        JsonBuf pinned;
        CHECK(chat_completion_request_build(&pinned,CHAT_BACKEND_OPENROUTER,
            context_chat->model,context.messages,context.count,NULL,true));
        CHECK(json_validate(pinned.data));
        CHECK(pinned.length==context.bytes);
        CHECK(context.text_bytes+context.attachment_bytes==context.bytes);
        /* The multimodal turn is a content array in run order; a text-only
           message in the same request keeps its plain string content. */
        CHECK(strstr(pinned.data,
            "\"content\":[{\"type\":\"text\",\"text\":\"what's this?\"},"
            "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,")
            !=NULL);
        CHECK(strstr(pinned.data,"\"content\":\"and this?\"")!=NULL);
        CHECK(strstr(pinned.data,
            "\"content\":\"Answer in one short sentence.\"")!=NULL);
        /* Per-message agreement: the split is exactly the encoded span. */
        ChatMessageCost cost=chat_completion_message_costs(
            CHAT_BACKEND_OPENROUTER,CHAT_ROLE_USER,&context.messages[mm]);
        CHECK(cost.text_bytes>0 && cost.attachment_bytes>0);
        JsonBuf one;
        CHECK(chat_completion_request_build(&one,CHAT_BACKEND_OPENROUTER,
            context_chat->model,&context.messages[mm],1,NULL,true));
        CHECK(one.length==chat_completion_envelope_bytes(
            CHAT_BACKEND_OPENROUTER,context_chat->model,NULL,true)
            +cost.text_bytes+cost.attachment_bytes);
        json_buf_free(&one);
        json_buf_free(&pinned);
        /* Ollama: the same view, the bare-string image_url form. */
        context_chat->backend=CHAT_BACKEND_OLLAMA;
        wcscpy(context_chat->ollama_model,L"local/model:latest");
        CHECK(chat_context_build(context_chat,&context_chat->conversations[0],
            ask,CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
        supply_bytes(&context,blob,sizeof blob);
        CHECK(chat_completion_request_build(&pinned,CHAT_BACKEND_OLLAMA,
            context_chat->ollama_model,context.messages,context.count,NULL,true));
        CHECK(json_validate(pinned.data));
        CHECK(pinned.length==context.bytes);
        CHECK(context.text_bytes+context.attachment_bytes==context.bytes);
        CHECK(strstr(pinned.data,
            "\"content\":[{\"type\":\"text\",\"text\":\"what's this?\"},"
            "{\"type\":\"image_url\",\"image_url\":\"data:image/png;base64,")
            !=NULL);
        CHECK(strstr(pinned.data,"\"image_url\":{\"url\":")==NULL);
        cost=chat_completion_message_costs(CHAT_BACKEND_OLLAMA,
            CHAT_ROLE_USER,&context.messages[mm]);
        CHECK(chat_completion_request_build(&one,CHAT_BACKEND_OLLAMA,
            context_chat->ollama_model,&context.messages[mm],1,NULL,true));
        CHECK(one.length==chat_completion_envelope_bytes(
            CHAT_BACKEND_OLLAMA,context_chat->ollama_model,NULL,true)
            +cost.text_bytes+cost.attachment_bytes);
        json_buf_free(&one);
        json_buf_free(&pinned);
        context_chat->backend=CHAT_BACKEND_OPENROUTER;

        /* One-byte image golden: the literals carry the URL's quotes, so the
           payload between them is exactly 4 bytes of base64 -- a double-count
           of the quotes or the payload size fails here. */
        CHECK(chat_completion_image_part_bytes(CHAT_BACKEND_OPENROUTER,
            "image/png",1)==69);
        CHECK(chat_completion_image_part_bytes(CHAT_BACKEND_OPENROUTER,
            "image/png",1)==strlen(
            "{\"type\":\"image_url\",\"image_url\":{\"url\":"
            "\"data:image/png;base64,QQ==\"}}"));
        ChatAttachmentMeta rec1={0};
        rec1.id=2;
        for (int i=0;i<64;i++) rec1.digest[i]='b';
        rec1.digest[64]=0;
        strcpy(rec1.mime,"image/png");
        rec1.bytes=1;
        rec1.created_at=1;
        wcscpy(rec1.display_name,L"one.png");
        CHECK(chat_attachment_add(context_chat,&rec1));
        ChatRequestMessage alone_view={0};
        ChatRequestPart alone_part={0};
        alone_view.role=CHAT_ROLE_USER;
        alone_view.text=L"";
        alone_part.kind=CHAT_PART_IMAGE;
        alone_part.u.image.rec=chat_attachment(context_chat,2);
        alone_part.u.image.bytes=&one_byte;
        alone_part.u.image.byte_length=1;
        alone_view.parts=&alone_part;
        alone_view.part_count=1;
        JsonBuf golden;
        CHECK(chat_completion_request_build(&golden,CHAT_BACKEND_OPENROUTER,
            context_chat->model,&alone_view,1,NULL,true));
        CHECK(strstr(golden.data,"\"content\":[{\"type\":\"image_url\","
            "\"image_url\":{\"url\":\"data:image/png;base64,QQ==\"}}]}")!=NULL);
        CHECK(strstr(golden.data,"\"content\":[{\"type\":\"text\"")==NULL);
        json_buf_free(&golden);

        /* Two images in one run: multi-image body equality and run order on
           both backends (the context tests cover the costing side). */
        ChatRequestMessage two_view={0};
        ChatRequestPart two_parts[3];
        memset(two_parts,0,sizeof two_parts);
        two_view.role=CHAT_ROLE_USER;
        two_view.text=L"two images";
        two_parts[0].kind=CHAT_PART_TEXT;
        two_parts[0].u.text=L"two images";
        two_parts[1].kind=CHAT_PART_IMAGE;
        two_parts[1].u.image.rec=chat_attachment(context_chat,1);
        two_parts[1].u.image.bytes=blob;
        two_parts[1].u.image.byte_length=rec.bytes;
        two_parts[2].kind=CHAT_PART_IMAGE;
        two_parts[2].u.image.rec=chat_attachment(context_chat,2);
        two_parts[2].u.image.bytes=&one_byte;
        two_parts[2].u.image.byte_length=1;
        two_view.parts=two_parts;
        two_view.part_count=3;
        for (int b=0;b<2;b++) {
            ChatBackend backend=b ? CHAT_BACKEND_OLLAMA : CHAT_BACKEND_OPENROUTER;
            const wchar_t *two_model=b ? context_chat->ollama_model
                : context_chat->model;
            ChatMessageCost two_cost=chat_completion_message_costs(
                backend,CHAT_ROLE_USER,&two_view);
            CHECK(two_cost.attachment_bytes>
                chat_completion_image_part_bytes(backend,"image/png",rec.bytes));
            JsonBuf two;
            CHECK(chat_completion_request_build(&two,backend,two_model,
                &two_view,1,NULL,true));
            CHECK(json_validate(two.data));
            CHECK(two.length==chat_completion_envelope_bytes(backend,
                two_model,NULL,true)+two_cost.text_bytes+
                two_cost.attachment_bytes);
            /* Run order: the text term leads, both image terms follow. */
            const char *text_part=strstr(two.data,"\"type\":\"text\"");
            const char *first_image=strstr(two.data,"\"type\":\"image_url\"");
            CHECK(text_part && first_image && text_part<first_image);
            int images=0;
            for (const char *p=two.data;
                (p=strstr(p,"\"type\":\"image_url\""))!=NULL;p+=8) ++images;
            CHECK(images==2);
            /* The last term is the one-byte image; its payload closes the
               part, the array and the message. */
            CHECK(strstr(two.data,b ? "data:image/png;base64,QQ==\"}]}" :
                "data:image/png;base64,QQ==\"}}]}")!=NULL);
            json_buf_free(&two);
        }

        /* Fail closed on untrustworthy image terms: no bytes to write, or no
           record to name the MIME. */
        {
            ChatRequestMessage bad_message={0};
            ChatRequestPart bad_part={0};
            bad_message.role=CHAT_ROLE_USER;
            bad_message.text=L"";
            bad_part.kind=CHAT_PART_IMAGE;
            bad_part.u.image.rec=&rec;
            bad_part.u.image.byte_length=1000;
            bad_message.parts=&bad_part;
            bad_message.part_count=1;
            JsonBuf bad;
            CHECK(!chat_completion_request_build(&bad,CHAT_BACKEND_OPENROUTER,
                context_chat->model,&bad_message,1,NULL,true));
            json_buf_free(&bad);
            bad_part.u.image.bytes=blob;
            bad_part.u.image.rec=NULL;
            CHECK(!chat_completion_request_build(&bad,CHAT_BACKEND_OPENROUTER,
                context_chat->model,&bad_message,1,NULL,true));
            json_buf_free(&bad);
        }
    }
    chat_dispose(context_chat); free(context_chat);
    /* ---- Commit 7: the worker carries owned parts --------------------- */
    {
        /* The host's commit-7 job (point each kept run's image slot at
           loaded bytes) is simulated with fixture bytes; this block proves
           the worker's owned carry: copy_work_messages deep-copies the
           view, build_request re-wraps it, and the body is byte-identical
           to the pure encoder's over the borrowed view. */
        ChatAttachmentMeta rec={0};
        rec.id=1;
        for (int i=0;i<64;i++) rec.digest[i]='a';
        rec.digest[64]=0;
        strcpy(rec.mime,"image/png");
        rec.bytes=1237;
        rec.created_at=1;
        wcscpy(rec.display_name,L"photo.png");
        static unsigned char blob[1237];
        for (size_t i=0;i<sizeof blob;i++) blob[i]=(unsigned char)(i&0xff);
        wchar_t text_a[32]; wcscpy(text_a,L"look at this");
        wchar_t text_b[32]; wcscpy(text_b,L"and this?");
        ChatRequestPart parts[3]; memset(parts,0,sizeof parts);
        parts[0].kind=CHAT_PART_TEXT;
        parts[0].u.text=text_a;
        parts[1].kind=CHAT_PART_IMAGE;
        parts[1].u.image.rec=&rec;
        parts[1].u.image.bytes=blob;
        parts[1].u.image.byte_length=sizeof blob;
        parts[2].kind=CHAT_PART_IMAGE;
        parts[2].u.image.rec=&rec;
        parts[2].u.image.bytes=blob;
        parts[2].u.image.byte_length=sizeof blob;
        ChatRequestMessage view[3]; memset(view,0,sizeof view);
        view[0].role=CHAT_ROLE_USER;
        view[0].text=L"first question";
        view[1].role=CHAT_ROLE_USER;
        view[1].text=text_a;
        view[1].parts=parts;
        view[1].part_count=3;
        view[2].role=CHAT_ROLE_USER;
        view[2].text=text_b;

        CompletionWork *work=(CompletionWork *)calloc(1,sizeof *work);
        CHECK(work);
        work->notify=window; work->message=CHAT_WM_COMPLETION_EVENT;
        work->client=&client; work->generation=7;
        work->backend=CHAT_BACKEND_OPENROUTER;
        work->reasoning=true;
        work->model=copy_wide(L"test/model");
        CHECK(work->model);
        CHECK(copy_work_messages(work,view,3));
        CHECK(work->part_counts && work->part_counts[0]==0 &&
              work->part_counts[1]==3 && work->part_counts[2]==0);
        CHECK(work->texts[0] && !work->texts[1] && work->texts[2]);

        JsonBuf from_view, from_work;
        CHECK(chat_completion_request_build(&from_view,CHAT_BACKEND_OPENROUTER,
            L"test/model",view,3,NULL,true));
        CHECK(encode(work,&from_work));
        CHECK(json_validate(from_work.data));
        CHECK(from_work.length==from_view.length);
        CHECK(memcmp(from_work.data,from_view.data,from_view.length)==0);
        /* The same carry through Ollama's bare-string image shape. */
        work->backend=CHAT_BACKEND_OLLAMA;
        JsonBuf ollama_view, ollama_work;
        CHECK(chat_completion_request_build(&ollama_view,CHAT_BACKEND_OLLAMA,
            L"test/model",view,3,NULL,true));
        CHECK(encode(work,&ollama_work));
        CHECK(ollama_work.length==ollama_view.length);
        CHECK(memcmp(ollama_work.data,ollama_view.data,ollama_view.length)==0);
        CHECK(strstr(ollama_work.data,
            "\"image_url\":\"data:image/png;base64,")!=NULL);
        CHECK(strstr(ollama_work.data,"\"image_url\":{\"url\":")==NULL);
        work->backend=CHAT_BACKEND_OPENROUTER;

        /* Owned carry: scribble every borrowed source and re-encode. The
           worker body cannot change because nothing it sends borrows them. */
        wcscpy(text_a,L"tampered");
        wcscpy(text_b,L"tampered");
        memset(blob,0x5a,sizeof blob);
        memset(&rec,0,sizeof rec);
        JsonBuf again;
        CHECK(encode(work,&again));
        CHECK(again.length==from_view.length);
        CHECK(memcmp(again.data,from_view.data,from_view.length)==0);

        /* Scrub: free_work zeroes the image bytes before releasing them. */
        free_watch_ptr=work->parts[1][1].u.image.bytes;
        free_watch_n=work->parts[1][1].u.image.n;
        free_watch_seen=false; free_watch_zeroed=false;
        CHECK(free_watch_ptr && free_watch_n==sizeof blob);
        free_work(work);
        CHECK(free_watch_seen && free_watch_zeroed);

        /* Fail closed on malformed views, before anything is copied:
           an IMAGE with no record (the copy would dereference it, while the
           encoder rejects it cleanly), an image with no bytes, a zero-length
           image (never an empty image), an unknown kind, and out-of-range
           counts. */
        {
            ChatRequestMessage bad={0};
            ChatRequestPart bad_part={0};
            bad.role=CHAT_ROLE_USER;
            bad.text=L"";
            bad.parts=&bad_part;
            bad.part_count=1;
            bad_part.kind=CHAT_PART_IMAGE;
            bad_part.u.image.byte_length=8;
            bad_part.u.image.bytes=blob;
            for (int variant=0;variant<7;variant++) {
                CompletionWork *w=(CompletionWork *)calloc(1,sizeof *w);
                CHECK(w);
                bad.part_count=1;
                bad_part.kind=CHAT_PART_IMAGE;
                bad_part.u.image.rec=NULL;
                bad_part.u.image.byte_length=8;
                bad_part.u.image.bytes=blob;
                if (variant==0) {           /* IMAGE with no record */
                    bad_part.u.image.rec=NULL;
                } else if (variant==1) {    /* image with no bytes */
                    bad_part.u.image.rec=&rec;
                    bad_part.u.image.bytes=NULL;
                } else if (variant==2) {    /* unknown part kind */
                    bad_part.u.image.rec=&rec;
                    bad_part.u.image.bytes=blob;
                    bad_part.kind=99;
                } else if (variant==3) {    /* run without its array */
                    bad.parts=NULL;
                } else if (variant==4) {    /* count above CHAT_MAX_PARTS */
                    bad.parts=&bad_part;
                    bad.part_count=CHAT_MAX_PARTS+1;
                } else if (variant==5) {    /* zero-length image term */
                    bad.parts=&bad_part;
                    bad_part.u.image.rec=&rec;
                    bad_part.u.image.byte_length=0;
                    bad_part.u.image.bytes=blob;
                } else {                    /* negative message count */
                    bad.parts=&bad_part;
                    bad.part_count=1;
                    CHECK(!copy_work_messages(w,&bad,-1));
                    free_work(w);
                    break;
                }
                CHECK(!copy_work_messages(w,&bad,1));
                free_work(w);
            }
            /* The pure encoder rejects the same term: a zero-length image
               is never encoded as an empty image. */
            bad.parts=&bad_part;
            bad.part_count=1;
            bad_part.kind=CHAT_PART_IMAGE;
            bad_part.u.image.rec=&rec;
            bad_part.u.image.byte_length=0;
            bad_part.u.image.bytes=blob;
            JsonBuf never;
            CHECK(!chat_completion_request_build(&never,CHAT_BACKEND_OPENROUTER,
                L"test/model",&bad,1,NULL,true));
            json_buf_free(&never);
        }
        json_buf_free(&from_view); json_buf_free(&from_work);
        json_buf_free(&ollama_view); json_buf_free(&ollama_work);
        json_buf_free(&again);
    }
    puts("The bounded request context measures exactly what the encoder writes");
    puts("Actual request encoder and SSE metadata/error decoding passed for both backends");
    if (argc>1 && !strcmp(argv[1],"--live")) {
        char key[8192]={0};
        DWORD size=GetEnvironmentVariableA("OPENROUTER_API_KEY",key,sizeof key);
        CompletionMessage message={0};
        message.role=CHAT_ROLE_USER;
        message.text=L"Reply with exactly the word OK.";
        /* Commit 7 live probes: one tiny image over the real worker path.
           The image message is the same borrowed view the host builds. */
        static const unsigned char tiny_png[]={
            0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
            0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
            0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,
            0x01,0x73,0x52,0x47,0x42,0x00,0xae,0xce,0x1c,0xe9,0x00,0x00,
            0x00,0x04,0x67,0x41,0x4d,0x41,0x00,0x00,0xb1,0x8f,0x0b,0xfc,
            0x61,0x05,0x00,0x00,0x00,0x09,0x70,0x48,0x59,0x73,0x00,0x00,
            0x0e,0xc3,0x00,0x00,0x0e,0xc3,0x01,0xc7,0x6f,0xa8,0x64,0x00,
            0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x18,0x57,0x63,0xf8,0xcf,
            0xc0,0xf0,0x1f,0x00,0x05,0x00,0x01,0xff,0xa6,0x5c,0x9b,0x5d,
            0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
        };
        ChatAttachmentMeta img_rec={0};
        img_rec.id=1;
        for (int i=0;i<64;i++) img_rec.digest[i]='a';
        img_rec.digest[64]=0;
        strcpy(img_rec.mime,"image/png");
        img_rec.bytes=sizeof tiny_png;
        img_rec.created_at=1;
        wcscpy(img_rec.display_name,L"pixel.png");
        ChatRequestPart img_parts[2]; memset(img_parts,0,sizeof img_parts);
        img_parts[0].kind=CHAT_PART_TEXT;
        img_parts[0].u.text=L"What color is this image? Answer in one word.";
        img_parts[1].kind=CHAT_PART_IMAGE;
        img_parts[1].u.image.rec=&img_rec;
        img_parts[1].u.image.bytes=tiny_png;
        img_parts[1].u.image.byte_length=sizeof tiny_png;
        CompletionMessage image_message={0};
        image_message.role=CHAT_ROLE_USER;
        image_message.text=L"What color is this image? Answer in one word.";
        image_message.parts=img_parts;
        image_message.part_count=2;
        if (size && size<sizeof key) {
            terminal=0; deltas=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-4o-mini",&message,1,NULL,true);
            CHECK(generation>0);
            CHECK(await_terminal(90000)); completion_complete(&client,generation);
            printf("OpenRouter live outcome=%d, text chunks=%d, usage=%s, cost=%s, model=%s\n",
                outcome,deltas,metadata.total_tokens>=0 ? "present":"absent",metadata.cost>=0 ? "present":"absent",
                metadata.actual_model[0] ? "present":"absent");
            CHECK(outcome==COMPLETION_DONE && deltas>0 && metadata.total_tokens>0 && metadata.actual_model[0]);
            terminal=0; deltas=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-4o-mini",&message,1,NULL,true);
            CHECK(generation>0 && completion_cancel(&client,generation));
            CHECK(await_terminal(10000)); completion_complete(&client,generation);
            CHECK(outcome==COMPLETION_CANCELLED && terminal==1);
            /* A vision model must accept the image and answer. */
            terminal=0; deltas=0; terminal_text[0]=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-4o-mini",&image_message,1,NULL,true);
            CHECK(generation>0);
            CHECK(await_terminal(90000)); completion_complete(&client,generation);
            printf("OpenRouter vision image outcome=%d, text chunks=%d\n",
                outcome,deltas);
            if (terminal_text[0])
                printf("OpenRouter vision image reply: %ls\n",terminal_text);
            CHECK(outcome==COMPLETION_DONE && deltas>0);
            /* Non-vision probe: first prove the chosen model is alive and
               answers a text-only request, so the recorded image error
               demonstrates a vision limitation rather than model
               unavailability. */
            terminal=0; deltas=0; terminal_text[0]=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-3.5-turbo",&message,1,NULL,true);
            CHECK(generation>0);
            CHECK(await_terminal(90000)); completion_complete(&client,generation);
            printf("OpenRouter non-vision text control outcome=%d, chunks=%d\n",
                outcome,deltas);
            CHECK(outcome==COMPLETION_DONE && deltas>0);
            /* The image send to that same model must fail: that failure is
               what closes the ledger's unverified row. Assert the error
               outcome and that text was captured, and record the actual
               wording -- the message itself is never assumed. */
            terminal=0; deltas=0; terminal_text[0]=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-3.5-turbo",&image_message,1,NULL,true);
            CHECK(generation>0);
            CHECK(await_terminal(90000)); completion_complete(&client,generation);
            printf("OpenRouter non-vision image outcome=%d\n",outcome);
            printf("OpenRouter non-vision image reply: %ls\n",
                terminal_text[0] ? terminal_text : L"(no message)");
            CHECK(outcome==COMPLETION_ERROR);
            CHECK(terminal_text[0]!=0);
        } else {
            puts("Live OpenRouter test skipped: API key unavailable");
        }
        /* Ollama is optional: run it only when a local server answers and the
           caller names a model, so the normal suite never depends on it. */
        char ollama_model[256]={0};
        DWORD model_size=GetEnvironmentVariableA("DARKCHAT_OLLAMA_MODEL",
            ollama_model,sizeof ollama_model);
        if (model_size && model_size<sizeof ollama_model) {
            wchar_t wide_model[256];
            MultiByteToWideChar(CP_UTF8,0,ollama_model,-1,wide_model,256);
            terminal=0; deltas=0;
            generation=completion_request(&client,CHAT_BACKEND_OLLAMA,NULL,
                wide_model,&message,1,NULL,true);
            if (generation>0 && await_terminal(60000)) {
                completion_complete(&client,generation);
                printf("Ollama live outcome=%d, text chunks=%d\n",outcome,deltas);
                CHECK(outcome==COMPLETION_DONE || outcome==COMPLETION_ERROR);
            } else {
                puts("Live Ollama test skipped or unavailable");
            }
            /* The same tiny image to the same model. DARKCHAT_OLLAMA_MODEL
               configures the VISION model for this probe: it must complete
               with content, so the probe can never pass by printing an
               error, timing out, or being "skipped" while configured. */
            terminal=0; deltas=0; terminal_text[0]=0;
            generation=completion_request(&client,CHAT_BACKEND_OLLAMA,NULL,
                wide_model,&image_message,1,NULL,true);
            CHECK(generation>0);
            CHECK(await_terminal(60000));
            completion_complete(&client,generation);
            printf("Ollama image outcome=%d, text chunks=%d\n",outcome,deltas);
            if (terminal_text[0])
                printf("Ollama image reply: %ls\n",terminal_text);
            CHECK(outcome==COMPLETION_DONE && deltas>0);
        } else {
            puts("Live Ollama test skipped: set DARKCHAT_OLLAMA_MODEL to run it");
        }
        SecureZeroMemory(key,sizeof key);
    }
    completion_shutdown(&client); pump(); DestroyWindow(window);
    return 0;
}
