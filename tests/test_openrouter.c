/* Includes the client to test its actual request encoder and SSE event decoder
   for both backends. */
#include "../chat/completion_winhttp.c"
#include "../chat/completion_request.h"
#include "../chat/context.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int deltas, reasons, terminal;
static wchar_t reasoning_text[256];
static CompletionEventType outcome;
static ChatGeneration metadata;
static int generation;
static LRESULT CALLBACK test_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
    if (msg==CHAT_WM_COMPLETION_EVENT) {
        CompletionEvent *e=(CompletionEvent *)l;
        if (e->generation==generation) {
            if (e->type==COMPLETION_DELTA) { if (e->text && e->text[0]) ++deltas; }
            else if (e->type==COMPLETION_REASONING) {
                if (e->text && e->text[0]) {
                    ++reasons;
                    wcsncat(reasoning_text,e->text,
                        255-wcslen(reasoning_text));
                }
            }
            else { ++terminal; outcome=e->type; }
            metadata=e->metadata;
        }
        completion_event_free(e); return 0;
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
/* Builds a request body through the real work adapter. */
static bool encode(CompletionWork *work, JsonBuf *body) {
    return build_request(work, body);
}
int main(int argc,char **argv) {
    WNDCLASSW cls={0}; cls.lpfnWndProc=test_proc; cls.lpszClassName=L"DarkChat.NetworkTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,NULL,NULL); CHECK(window);
    CompletionClient client; completion_init(&client,window,CHAT_WM_COMPLETION_EVENT);
    CompletionWork work={0}; work.notify=window; work.message=CHAT_WM_COMPLETION_EVENT;
    work.client=&client; work.generation=7; generation=7;
    work.backend=CHAT_BACKEND_OPENROUTER;
    work.started_tick=GetTickCount64(); chat_generation_init(&work.metadata);
    work.metadata.backend=CHAT_BACKEND_OPENROUTER;
    Stream stream={&work,false,false,NULL};
    const char *chunk="{\"model\":\"actual/model\",\"choices\":[{\"delta\":{\"content\":\"Hi\\uD83D\\uDE80\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(deltas==1);
    CHECK(work.metadata.ttft_ms>=0 && work.metadata.first_token_at>0);
    chunk="{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5,\"cost\":0.00002}}";
    CHECK(stream_event(&stream,chunk,strlen(chunk)));
    CHECK(work.metadata.prompt_tokens==3 && work.metadata.completion_tokens==2 && work.metadata.total_tokens==5);
    CHECK(work.metadata.cost==0.00002 && !wcscmp(work.metadata.actual_model,L"actual/model"));
    CHECK(stream_event(&stream,"[DONE]",6) && stream.done);
    chunk="{\"choices\":[{\"delta\":{\"content\":\"partial\"}}],\"error\":{\"message\":\"provider failed\"}}";
    CHECK(!stream_event(&stream,chunk,strlen(chunk)) && stream.failed && stream.error);
    free(stream.error); stream.error=NULL; stream.failed=false;
    CHECK(!stream_event(&stream,"{bad}",5)); free(stream.error); pump();
    /* Reasoning is parsed from structured details and compatibility fallbacks;
       an entry with no displayable text never fabricates one. */
    stream.error=NULL; stream.failed=false; reasons=0; deltas=0;
    reasoning_text[0]=0;
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"step one \"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(reasons==1);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.summary\",\"summary\":\"summary\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(reasons==2);
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"plain text\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(reasons==3);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(reasons==3);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"multi one \"},{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"},{\"type\":\"reasoning.summary\",\"summary\":\"multi two\"}]}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump();
    CHECK(reasons==5 && wcsstr(reasoning_text,L"multi one multi two"));
    chunk="{\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}],\"reasoning_content\":\"alias text\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump();
    CHECK(reasons==6 && wcsstr(reasoning_text,L"alias text"));
    chunk="{\"choices\":[{\"delta\":{\"content\":\"answer only\"}}]}";
    CHECK(stream_event(&stream,chunk,strlen(chunk))); pump(); CHECK(reasons==6 && deltas==1);
    /* Ollama's OpenAI-compatible reasoning output arrives through the same
       parser fallbacks (reasoning, then reasoning_content). */
    stream.error=NULL; stream.failed=false; reasons=0; deltas=0; reasoning_text[0]=0;
    CompletionWork ollama=work; ollama.backend=CHAT_BACKEND_OLLAMA;
    ollama.metadata.backend=CHAT_BACKEND_OLLAMA;
    Stream ollama_stream={&ollama,false,false,NULL};
    chunk="{\"choices\":[{\"delta\":{\"reasoning\":\"local step \"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); pump(); CHECK(reasons==1);
    chunk="{\"choices\":[{\"delta\":{\"reasoning_content\":\"more\"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); pump();
    CHECK(reasons==2 && wcsstr(reasoning_text,L"local step more"));
    chunk="{\"model\":\"llama3\",\"choices\":[{\"delta\":{\"content\":\"local answer\"}}]}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk))); pump(); CHECK(deltas==1);
    chunk="{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":4,\"total_tokens\":11}}";
    CHECK(stream_event(&ollama_stream,chunk,strlen(chunk)));
    CHECK(ollama.metadata.prompt_tokens==7 && ollama.metadata.completion_tokens==4 &&
        ollama.metadata.total_tokens==11);
    CHECK(!wcscmp(ollama.metadata.actual_model,L"llama3"));
    CHECK(!stream_event(&ollama_stream,"{oops",5) && ollama_stream.failed);
    free(ollama_stream.error); ollama_stream.error=NULL; ollama_stream.failed=false;
    CHECK(stream_event(&ollama_stream,"[DONE]",6) && ollama_stream.done);
    /* Backend identity travels with the generation metadata. */
    CHECK(ollama.metadata.backend==CHAT_BACKEND_OLLAMA &&
        work.metadata.backend==CHAT_BACKEND_OPENROUTER);

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
    CHECK(chat_completion_envelope_bytes(CHAT_BACKEND_OLLAMA,work.model,NULL)==
        78 + json_encoded_string_size(work.model));
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
    ChatRequestContext context;
    CHECK(chat_context_build(context_chat,&context_chat->conversations[0],trigger,
        CHAT_CONTEXT_BUDGET_BYTES,&context)==CHAT_CONTEXT_OK);
    CHECK(context.count==8 && context.dropped_messages==0);
    CompletionWork sized={0};
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
    chat_dispose(context_chat); free(context_chat);
    puts("The bounded request context measures exactly what the encoder writes");
    puts("Actual request encoder and SSE metadata/error decoding passed for both backends");
    if (argc>1 && !strcmp(argv[1],"--live")) {
        char key[8192]={0};
        DWORD size=GetEnvironmentVariableA("OPENROUTER_API_KEY",key,sizeof key);
        CompletionMessage message={CHAT_ROLE_USER,L"Reply with exactly the word OK."};
        if (size && size<sizeof key) {
            terminal=0; deltas=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-4o-mini",&message,1,NULL);
            CHECK(generation>0);
            CHECK(await_terminal(90000)); completion_complete(&client,generation);
            printf("OpenRouter live outcome=%d, text chunks=%d, usage=%s, cost=%s, model=%s\n",
                outcome,deltas,metadata.total_tokens>=0 ? "present":"absent",metadata.cost>=0 ? "present":"absent",
                metadata.actual_model[0] ? "present":"absent");
            CHECK(outcome==COMPLETION_DONE && deltas>0 && metadata.total_tokens>0 && metadata.actual_model[0]);
            terminal=0; deltas=0;
            generation=completion_request(&client,CHAT_BACKEND_OPENROUTER,key,
                L"openai/gpt-4o-mini",&message,1,NULL);
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
                wide_model,&message,1,NULL);
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
