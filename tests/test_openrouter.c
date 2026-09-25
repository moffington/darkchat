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
                else { ++terminal; outcome=e->type; }
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
    /* Commit-5 boundary pin: a view that carries a part run still encodes its
       plain-text projection as a plain JSON string content. The content-array
       encoding lands with the encoder commit; at this boundary not one
       request byte may depend on the parts. */
    {
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
        JsonBuf pinned;
        CHECK(chat_completion_request_build(&pinned,CHAT_BACKEND_OPENROUTER,
            context_chat->model,context.messages,context.count,NULL,true));
        CHECK(json_validate(pinned.data));
        CHECK(pinned.length==context.bytes);
        CHECK(strstr(pinned.data,"\"content\":[")==NULL);
        CHECK(strstr(pinned.data,"\"content\":\"what's this?\"")!=NULL);
        CHECK(strstr(pinned.data,"\"content\":\"and this?\"")!=NULL);
        json_buf_free(&pinned);
    }
    chat_dispose(context_chat); free(context_chat);
    puts("The bounded request context measures exactly what the encoder writes");
    puts("Actual request encoder and SSE metadata/error decoding passed for both backends");
    if (argc>1 && !strcmp(argv[1],"--live")) {
        char key[8192]={0};
        DWORD size=GetEnvironmentVariableA("OPENROUTER_API_KEY",key,sizeof key);
        CompletionMessage message={0};
        message.role=CHAT_ROLE_USER;
        message.text=L"Reply with exactly the word OK.";
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
        } else {
            puts("Live Ollama test skipped: set DARKCHAT_OLLAMA_MODEL to run it");
        }
        SecureZeroMemory(key,sizeof key);
    }
    completion_shutdown(&client); pump(); DestroyWindow(window);
    return 0;
}
