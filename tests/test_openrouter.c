/* Includes the client to test its actual request encoder and SSE event decoder. */
#include "../chat/openrouter_winhttp.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int deltas, reasons, terminal;
static wchar_t reasoning_text[256];
static OpenRouterEventType outcome;
static ChatGeneration metadata;
static int generation;
static LRESULT CALLBACK test_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
    if (msg==CHAT_WM_OPENROUTER_EVENT) {
        OpenRouterEvent *e=(OpenRouterEvent *)l;
        if (e->generation==generation) {
            if (e->type==OPENROUTER_DELTA) { if (e->text && e->text[0]) ++deltas; }
            else if (e->type==OPENROUTER_REASONING) {
                if (e->text && e->text[0]) {
                    ++reasons;
                    wcsncat(reasoning_text,e->text,
                        255-wcslen(reasoning_text));
                }
            }
            else { ++terminal; outcome=e->type; }
            metadata=e->metadata;
        }
        openrouter_event_free(e); return 0;
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
int main(int argc,char **argv) {
    WNDCLASSW cls={0}; cls.lpfnWndProc=test_proc; cls.lpszClassName=L"DarkChat.NetworkTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,NULL,NULL); CHECK(window);
    OpenRouterClient client; openrouter_init(&client,window,CHAT_WM_OPENROUTER_EVENT);
    OpenRouterWork work={0}; work.notify=window; work.message=CHAT_WM_OPENROUTER_EVENT;
    work.client=&client; work.generation=7; generation=7;
    work.started_tick=GetTickCount64(); chat_generation_init(&work.metadata);
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
    work.model=L"test/model";
    ChatRole roles[]={CHAT_ROLE_SYSTEM,CHAT_ROLE_USER,CHAT_ROLE_ERROR};
    wchar_t *texts[]={L"system",L"question \"quoted\"",L"local error"};
    work.roles=roles; work.texts=texts; work.count=3;
    JsonBuf body; CHECK(build_request(&work,&body)); CHECK(json_validate(body.data));
    CHECK(strstr(body.data,"\"reasoning\":{\"enabled\":true}")!=NULL);
    char value[128]; CHECK(json_query_string(body.data,"messages[1].content",value,sizeof value));
    CHECK(!json_query_string(body.data,"messages[2].content",value,sizeof value));
    json_buf_free(&body);
    puts("Actual request encoder and SSE metadata/error decoding passed");
    if (argc>1 && !strcmp(argv[1],"--live")) {
        char key[8192]={0};
        DWORD size=GetEnvironmentVariableA("OPENROUTER_API_KEY",key,sizeof key);
        if (!size || size>=sizeof key) { puts("Live test skipped: API key unavailable"); return 77; }
        OpenRouterMessage message={CHAT_ROLE_USER,L"Reply with exactly the word OK."};
        terminal=0; deltas=0;
        generation=openrouter_request(&client,key,L"openai/gpt-4o-mini",&message,1);
        CHECK(generation>0);
        CHECK(await_terminal(90000)); openrouter_complete(&client,generation);
        printf("Live outcome=%d, text chunks=%d, usage=%s, cost=%s, model=%s, TTFT=%.0f ms, latency=%.0f ms\n",
            outcome,deltas,metadata.total_tokens>=0 ? "present":"absent",metadata.cost>=0 ? "present":"absent",
            metadata.actual_model[0] ? "present":"absent",metadata.ttft_ms,metadata.latency_ms);
        CHECK(outcome==OPENROUTER_DONE && deltas>0 && metadata.total_tokens>0 && metadata.actual_model[0]);
        terminal=0; deltas=0;
        generation=openrouter_request(&client,key,L"openai/gpt-4o-mini",&message,1);
        CHECK(generation>0 && openrouter_cancel(&client,generation));
        CHECK(await_terminal(10000)); openrouter_complete(&client,generation);
        CHECK(outcome==OPENROUTER_CANCELLED && terminal==1);
        puts("Live immediate cancellation passed");
        terminal=0;
        generation=openrouter_request(&client,key,L"darkchat-invalid/model-does-not-exist",&message,1);
        CHECK(generation>0 && await_terminal(30000)); openrouter_complete(&client,generation);
        CHECK(outcome==OPENROUTER_ERROR);
        puts("Live invalid-model API error passed");
        SecureZeroMemory(key,sizeof key);
    }
    openrouter_shutdown(&client); pump(); DestroyWindow(window);
    return 0;
}
