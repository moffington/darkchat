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
static void begin_mode(ChatHost *h,ChatSendMode mode) {
    h->request_message=chat_begin_response(h->config.chat,mode,NULL);
    h->request_conversation=h->config.chat->active;
    ++h->request_generation; h->generating=true; h->accepting=true; h->stopping=false;
    h->reasoning_streaming=false; h->content_started=false;
    h->started_tick=GetTickCount64(); render_transcript(h);
}
static void begin_fixture(ChatHost *h) { begin_mode(h,CHAT_RETRY); }
static void begin_regenerate(ChatHost *h) { begin_mode(h,CHAT_REGENERATE); }
/* Reads a turn's rendered controls back to prove per-turn ownership. */
static void head_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->turns[i].head.window) rich_text_get_text(&h->turns[i].head,out,cap);
}
static void reasoning_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->turns[i].reasoning.window)
        rich_text_get_text(&h->turns[i].reasoning,out,cap);
}
static void body_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->turns[i].body.window) rich_text_get_text(&h->turns[i].body,out,cap);
}
static void meta_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->turns[i].meta.window) rich_text_get_text(&h->turns[i].meta,out,cap);
}
static bool row_present(ChatHost *h,int i) {
    wchar_t text[96]; head_text(h,i,text,96);
    return wcsstr(text,L"Thinking")!=NULL || wcsstr(text,L"Thought for")!=NULL;
}
/* Drives the real whole-row click path for one turn. */
static void click_row(ChatHost *h,int i) {
    if (h->turns[i].head.window) turn_row_click(h,&h->turns[i].head,1,false);
}
/* Appends a completed user/assistant turn with optional reasoning. */
static int add_turn(Chat *chat,const wchar_t *prompt,const wchar_t *answer,
    const wchar_t *reasoning,double reasoning_ms) {
    chat_append(chat,CHAT_ROLE_USER,prompt);
    int index=chat_append(chat,CHAT_ROLE_ASSISTANT,answer);
    ChatMessage *m=&chat->conversations[chat->active].messages[index];
    m->generation.state=CHAT_GENERATION_COMPLETE;
    if (reasoning) wcscpy(m->reasoning,reasoning);
    m->generation.reasoning_ms=reasoning?reasoning_ms:-1;
    return index;
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
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(RegisterClassW(&view_cls));
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
    /* ---- Per-turn reasoning ownership ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int cv=chat->active;
    int first=add_turn(chat,L"first",L"First answer",L"Alpha reasoning",1500);
    int second=add_turn(chat,L"second",L"Second answer",L"Beta reasoning",2500);
    wchar_t text[128];
    render_transcript(h);
    CHECK(h->turn_count==4);
    CHECK(row_present(h,first) && row_present(h,second));
    CHECK(!h->turns[first].reason_live && !h->turns[second].reason_live);
    head_text(h,first,text,128);
    CHECK(wcsstr(text,L"1.5s")!=NULL);
    /* Expanding one turn never touches another. */
    click_row(h,first);
    CHECK(h->turns[first].reason_live && !h->turns[second].reason_live);
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    /* Measuring text must not include its position in the transcript. */
    { TurnView *turn=&h->turns[first];
      int height=turn->head_h;
      CHECK(height<px(h,50));
      CHECK(turn->reason_y-turn->head_y-height==px(h,8));
      layout_from(h,0,false);
      CHECK(turn->head_h==height);
      layout_from(h,0,false);
      CHECK(turn->head_h==height); }
    click_row(h,second);
    CHECK(h->turns[first].reason_live && h->turns[second].reason_live);
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"Beta reasoning"));
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    click_row(h,first);
    CHECK(!h->turns[first].reason_live && h->turns[second].reason_live);
    /* Collapsed by default while waiting; explicit expansion streams live into
       that turn only and is not collapsed when the answer begins. */
    begin_regenerate(h);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_RUNNING);
    head_text(h,second,text,128);
    CHECK(wcsstr(text,L"Thinking")!=NULL);
    CHECK(!h->turns[second].reason_live);
    click_row(h,second);
    CHECK(h->turns[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"stream rea"));
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"soning"));
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"stream reasoning"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Streamed answer"));
    CHECK(h->turns[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    CHECK(pending(h)->generation.reasoning_ms>=0);
    /* The running answer is never mixed with metadata, and no footer exists
       until the turn is terminal. */
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"Streamed answer")); }
    CHECK(!h->turns[second].meta_live);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(h->turns[second].meta_live);
    { wchar_t meta[256]; meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"Complete")!=NULL); }
    /* Reasoning and duration persist per message. */
    h->dirty=true; CHECK(save(h));
    Chat *reloaded=calloc(1,sizeof *reloaded); CHECK(reloaded);
    CHECK(storage_load(&h->storage,reloaded)==1);
    CHECK(!wcscmp(reloaded->conversations[cv].messages[first].reasoning,L"Alpha reasoning"));
    CHECK(!wcscmp(reloaded->conversations[cv].messages[second].reasoning,L"stream reasoning"));
    CHECK(reloaded->conversations[cv].messages[second].generation.reasoning_ms>=0);
    free(reloaded);
    /* With no reasoning supplied, the pending row is removed once the answer
       begins rather than left empty. */
    begin_regenerate(h);
    CHECK(row_present(h,second));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Only answer"));
    CHECK(!row_present(h,second));
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    /* Retry/regenerate replaces the turn and cannot leak reasoning or its
       expansion state. */
    wcscpy(chat->conversations[cv].messages[second].reasoning,L"leak?");
    chat->conversations[cv].messages[second].reasoning_open=true;
    render_transcript(h);
    CHECK(h->turns[second].reason_live);
    begin_regenerate(h);
    CHECK(chat->conversations[cv].messages[second].reasoning[0]==0);
    CHECK(!chat->conversations[cv].messages[second].reasoning_open);
    CHECK(!h->turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    /* Cancellation/error keeps that turn coherent: reasoning retained, row and
       expansion intact. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"kept"));
    click_row(h,second);
    CHECK(h->turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_ERROR,L"boom"));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    CHECK(!wcscmp(chat->conversations[cv].messages[second].reasoning,L"kept"));
    CHECK(row_present(h,second) &&
        chat->conversations[cv].messages[second].reasoning_open);
    /* Switching conversations restores each message's own reasoning. */
    wcscpy(chat->conversations[0].messages[1].reasoning,L"Conv zero");
    chat->conversations[0].messages[1].reasoning_open=false;
    command(h,CHAT_COMMAND_SELECT,0);
    CHECK(h->turn_count==2);
    CHECK(row_present(h,1));
    click_row(h,1);
    { wchar_t text[128]; reasoning_text(h,1,text,128);
      CHECK(!wcscmp(text,L"Conv zero")); }
    command(h,CHAT_COMMAND_SELECT,cv);
    CHECK(h->turn_count==4);
    { wchar_t text[128]; reasoning_text(h,second,text,128);
      CHECK(!wcscmp(text,L"kept")); }
    /* An answer past the old fixed-size limit streams to completion. */
    begin_regenerate(h);
    wchar_t *long_text=(wchar_t *)malloc(sizeof(wchar_t)*40001);
    CHECK(long_text);
    for (int i=0;i<40000;i++) long_text[i]=L'z';
    long_text[40000]=0;
    handle_event(h,fixture(h,OPENROUTER_DELTA,long_text));
    free(long_text);
    CHECK(h->accepting && pending(h)->generation.state==CHAT_GENERATION_RUNNING);
    CHECK(wcslen(chat_message_text(pending(h)))==40000);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    /* Reasoning may legitimately be much longer than the final answer. It no
       longer disappears at the answer-sized 16K boundary. */
    begin_regenerate(h);
    wchar_t *long_reasoning=(wchar_t *)malloc(sizeof(wchar_t)*70001);
    CHECK(long_reasoning);
    for (int i=0;i<70000;i++) long_reasoning[i]=L'r';
    long_reasoning[70000]=0;
    handle_event(h,fixture(h,OPENROUTER_REASONING,long_reasoning));
    free(long_reasoning);
    CHECK(h->accepting &&
        wcslen(chat_message_reasoning(pending(h)))==70000);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Answer after long reasoning"));
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    /* Reasoning-to-answer streaming keeps geometry stable across repeated
       layouts, follows only at the bottom, and never scrolls inside a body. */
    begin_regenerate(h);
    click_row(h,second);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"Working through it"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Answer"));
    { TurnView *turn=&h->turns[second];
      int head_height=turn->head_h;
      int reason_y=turn->reason_y;
      position_turns(h,true);
      for (int i=0;i<80;i++) {
          int height=turn->body_h, scroll=h->view_scroll;
          handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nAnother line of the streamed answer."));
          CHECK(turn->head_h==head_height && turn->reason_y==reason_y);
          CHECK(turn->body_h>=height && turn->body_h-height<px(h,40));
          CHECK(h->view_scroll>=scroll && view_pinned(h));
          POINT origin={0,0};
          SendMessageW(turn->body.window,EM_GETSCROLLPOS,0,(LPARAM)&origin);
          CHECK(origin.y==0);
      }
      h->view_scroll=px(h,30); position_turns(h,false);
      CHECK(!view_pinned(h));
      int scroll=h->view_scroll;
      for (int i=0;i<8;i++) {
          handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nMore text while reading above."));
          CHECK(h->view_scroll==scroll);
      }
    }
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* A viewport the reader scrolled up is not force-followed on append. */
    { RichTextControl probe;
      CHECK(rich_text_create_viewport(&probe,window,900,&h->rich_theme,96));
      SetWindowPos(probe.window,NULL,0,0,220,40,SWP_NOZORDER|SWP_NOACTIVATE);
      size_t count=200*20;
      wchar_t *big=(wchar_t *)malloc(count*sizeof(wchar_t));
      CHECK(big);
      for (size_t i=0;i<count;i++) big[i]=(wchar_t)(L'a'+(i%26));
      for (int line=0;line<19;line++) big[line*200+199]=L'\n';
      big[count-1]=0;
      rich_text_set_reasoning(&probe,big);
      SendMessageW(probe.window,WM_VSCROLL,SB_TOP,0);
      CHECK(!rich_text_pinned(&probe));
      SCROLLINFO info; memset(&info,0,sizeof info);
      info.cbSize=sizeof info; info.fMask=SIF_ALL;
      GetScrollInfo(probe.window,SB_VERT,&info);
      int before=info.nPos;
      rich_text_append_reasoning(&probe,L"tail");
      memset(&info,0,sizeof info); info.cbSize=sizeof info; info.fMask=SIF_ALL;
      GetScrollInfo(probe.window,SB_VERT,&info);
      int maximum=info.nMax-(int)info.nPage+1;
      if (maximum<0) maximum=0;
      CHECK(info.nPos<=before+4 && info.nPos<maximum);
      free(big); DestroyWindow(probe.window); }
    /* Compact metadata footer: deduplicated model, grouped tokens, no "stop",
       and unusual finish reasons surfaced. */
    { ChatMessage *m=&chat->conversations[cv].messages[second];
      wchar_t meta[256];
      m->generation.state=CHAT_GENERATION_COMPLETE;
      m->generation.ttft_ms=17969; m->generation.latency_ms=19422;
      m->generation.prompt_tokens=44; m->generation.completion_tokens=1365;
      m->generation.total_tokens=1409; m->generation.cost=0.00165120;
      wcscpy(m->generation.finish_reason,L"stop");
      wcscpy(m->generation.requested_model,L"deepseek/deepseek-v4.1-flash");
      wcscpy(m->generation.actual_model,L"deepseek/deepseek-v4.1-flash");
      render_transcript(h);
      CHECK(h->turns[second].meta_live);
      meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"Complete")!=NULL);
      CHECK(wcsstr(meta,L"TTFT 18.0s")!=NULL);
      CHECK(wcsstr(meta,L"19.4s")!=NULL);
      CHECK(wcsstr(meta,L"44 in / 1,365 out")!=NULL);
      CHECK(wcsstr(meta,L"$0.00165")!=NULL);
      CHECK(wcsstr(meta,L"stop")==NULL);
      CHECK(wcsstr(meta,L"deepseek/deepseek-v4.1-flash")!=NULL);
      CHECK(wcsstr(meta,L"\u2192")==NULL);   /* requested == actual: shown once */
      wcscpy(m->generation.actual_model,L"deepseek/other");
      render_transcript(h);
      meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"deepseek/deepseek-v4.1-flash \u2192 deepseek/other")!=NULL);
      wcscpy(m->generation.actual_model,m->generation.requested_model);
      wcscpy(m->generation.finish_reason,L"length");
      render_transcript(h);
      meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"finish: length")!=NULL);
      CHECK(wcsstr(meta,L"stop")==NULL);
      wcscpy(m->generation.finish_reason,L"stop");
    }
    command(h,CHAT_COMMAND_SELECT,0);
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
    chat_dispose(loaded); chat_dispose(chat);
    free(loaded); free(chat); free(ui); free(h); CoUninitialize();
    puts("Hidden host: failures, stale events, switch, cancel/DONE race, empty reply, per-turn reasoning ownership, metadata footer, edit/draft and close/reopen passed");
    return 0;
}
