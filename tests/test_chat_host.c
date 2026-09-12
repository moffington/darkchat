/* Hidden HWND integration of the real host, lifecycle and storage. */
#include "../chat/chat_host_win32.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static OpenRouterEvent *fixture(ChatHost *h,OpenRouterEventType type,const wchar_t *text) {
    OpenRouterEvent *e=calloc(1,sizeof *e);
    e->generation=h->request_generation; e->type=type;
    e->metadata=pending(h)->generation;
    if (text) { size_t size=(wcslen(text)+1)*sizeof(wchar_t); e->text=malloc(size); memcpy(e->text,text,size); }
    return e;
}
static void begin_fixture(ChatHost *h) {
    h->request_message=chat_begin_response(h->config.chat,CHAT_RETRY,NULL);
    h->request_conversation=h->config.chat->active;
    ++h->request_generation; h->generating=true; h->accepting=true; h->stopping=false;
    h->started_tick=GetTickCount64(); render_transcript(h);
}
int main(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Host test",1100,720,720,480,NULL};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-test-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Host integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    rich_text_set_text(&h->composer,L"Question"); perform_send(h);
    CHECK(chat->conversations[0].message_count==2);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED); /* no key */
    CHECK(!h->generating);
    begin_fixture(h); CHECK(h->request_message==1);
    OpenRouterEvent *e=fixture(h,OPENROUTER_DELTA,L"Partial answer");
    e->metadata.ttft_ms=12; e->metadata.first_token_at=chat_now();
    handle_event(h,e); CHECK(!wcscmp(pending(h)->text,L"Partial answer"));
    h->dirty=true; CHECK(save(h));
    e=fixture(h,OPENROUTER_DELTA,L"STALE"); --e->generation; handle_event(h,e);
    CHECK(!wcscmp(pending(h)->text,L"Partial answer"));
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    CHECK(chat->active==1 && chat->conversations[1].message_count==0);
    handle_event(h,fixture(h,OPENROUTER_ERROR,L"Provider failed"));
    CHECK(chat->conversations[0].messages[1].generation.state==CHAT_GENERATION_FAILED);
    CHECK(!wcscmp(chat->conversations[0].messages[1].text,L"Partial answer"));
    CHECK(chat->conversations[1].message_count==0);
    command(h,CHAT_COMMAND_SELECT,0);
    begin_fixture(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"New response"));
    h->stopping=true; h->accepting=false; h->stop_state=CHAT_GENERATION_CANCELLED;
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"late chunk"));
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_CANCELLED);
    CHECK(!wcscmp(pending(h)->text,L"New response"));
    CHECK(chat->conversations[0].message_count==2);
    begin_fixture(h); handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED); /* empty completion */
    rich_text_set_text(&h->composer,L"Unsent draft");
    action(h,ACTION_EDIT);
    CHECK(h->editing && !wcscmp(chat->conversations[0].draft,L"Unsent draft"));
    rich_text_set_text(&h->composer,L"Edited question"); perform_send(h);
    CHECK(!h->editing && !wcscmp(chat->conversations[0].messages[0].text,L"Edited question"));
    CHECK(chat->conversations[0].message_count==2);
    wchar_t composer[CHAT_MESSAGE_TEXT]; rich_text_get_text(&h->composer,composer,CHAT_MESSAGE_TEXT);
    CHECK(!wcscmp(composer,L"Unsent draft"));
    begin_fixture(h); handle_event(h,fixture(h,OPENROUTER_DELTA,L"Closing partial"));
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window) && !h->generating);
    Chat *loaded=calloc(1,sizeof *loaded); CHECK(loaded);
    CHECK(storage_load(&h->storage,loaded)==1);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].text,L"Closing partial"));
    CHECK(!wcscmp(loaded->conversations[0].draft,L"Unsent draft"));
    storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background); rich_text_library_close();
    free(loaded); free(chat); free(ui); free(h); CoUninitialize();
    puts("Hidden host: partial failures, stale events, switch, cancel/DONE race, empty reply, edit/draft and close/reopen passed");
    return 0;
}
