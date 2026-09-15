/* Hidden HWND integration of the real host, lifecycle and storage. */
#include "../chat/chat_host_win32.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
/* Client seam: chat.bat test links this suite with -Wl,--wrap=openrouter_request,
   so every request the host starts passes through __wrap_openrouter_request.
   That shows exactly which context (if any) would reach the network client
   without touching the network: the wrapper records the messages the host was
   about to send, and can fake a started generation so a real successful send
   with omitted history is driven end to end. */
static int openrouter_request_calls;
static int openrouter_request_last_count;
static ChatRole openrouter_request_last_roles[CHAT_CONTEXT_MAX_ENTRIES];
static const wchar_t *openrouter_request_last_texts[CHAT_CONTEXT_MAX_ENTRIES];
static int openrouter_request_fake_generation;   /* 0: delegate to the real client */
int __real_openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count);
int __wrap_openrouter_request(OpenRouterClient *client, const char *api_key_utf8,
    const wchar_t *model, const OpenRouterMessage *messages, int count) {
    ++openrouter_request_calls;
    openrouter_request_last_count=count;
    for (int i=0;i<count && i<CHAT_CONTEXT_MAX_ENTRIES;i++) {
        openrouter_request_last_roles[i]=messages[i].role;
        openrouter_request_last_texts[i]=messages[i].text;
    }
    if (openrouter_request_fake_generation) return openrouter_request_fake_generation;
    return __real_openrouter_request(client,api_key_utf8,model,messages,count);
}
/* Writer seam (linked with -Wl,--wrap=storage_save): counts every
   storage_save call and can pause the writer while it holds an in-flight
   snapshot, which makes coalescing, isolation and shutdown-drain
   deterministic. Arming pauses exactly one call; a paused save falls through
   when the test releases it or when teardown begins (so saver_shutdown can
   always join). */
static volatile LONG pause_next_save, storage_save_calls;
static HANDLE save_paused_event, save_resume_event, save_teardown_event;
bool __real_storage_save(ChatStorage *store, const Chat *chat);
bool __wrap_storage_save(ChatStorage *store, const Chat *chat) {
    InterlockedIncrement(&storage_save_calls);
    if (InterlockedExchange(&pause_next_save,0)) {
        SetEvent(save_paused_event);
        for (;;) {
            if (WaitForSingleObject(save_resume_event,50)==WAIT_OBJECT_0) break;
            if (WaitForSingleObject(save_teardown_event,0)==WAIT_OBJECT_0) break;
        }
    }
    return __real_storage_save(store,chat);
}
/* Arms exactly one paused storage_save on the writer. */
static void arm_save_pause(void) {
    ResetEvent(save_resume_event);
    pause_next_save=1;
}
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
    h->transcript.body_render_tick=0;
    cancel_body_flush(h);
    /* Matches start_response: reused turn slots drop stale identity, pending
       updates and selections before the replacement is rendered. */
    transcript_invalidate_from(&h->transcript,h->request_message);
    h->started_tick=GetTickCount64(); render_transcript(h);
}
static void begin_fixture(ChatHost *h) { begin_mode(h,CHAT_RETRY); }
static void begin_regenerate(ChatHost *h) { begin_mode(h,CHAT_REGENERATE); }
/* Reads a turn's rendered controls back to prove per-turn ownership. */
static void head_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->transcript.turns[i].head.window) rich_text_get_text(&h->transcript.turns[i].head,out,cap);
}
static void reasoning_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->transcript.turns[i].reasoning.window)
        rich_text_get_text(&h->transcript.turns[i].reasoning,out,cap);
}
static void body_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->transcript.turns[i].body.window) rich_text_get_text(&h->transcript.turns[i].body,out,cap);
}
static void meta_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    if (h->transcript.turns[i].meta.window) rich_text_get_text(&h->transcript.turns[i].meta,out,cap);
}
static bool row_present(ChatHost *h,int i) {
    wchar_t text[96]; head_text(h,i,text,96);
    return wcsstr(text,L"Thinking")!=NULL || wcsstr(text,L"Thought for")!=NULL;
}
/* Drives the real whole-row click path for one turn. */
static void click_row(ChatHost *h,int i) {
    if (h->transcript.turns[i].head.window) turn_row_click(h,&h->transcript.turns[i].head,1,false);
}
/* Appends a completed user/assistant turn with optional reasoning. */
static int add_turn(Chat *chat,const wchar_t *prompt,const wchar_t *answer,
    const wchar_t *reasoning,double reasoning_ms) {
    chat_append(chat,CHAT_ROLE_USER,prompt);
    int index=chat_append(chat,CHAT_ROLE_ASSISTANT,answer);
    ChatMessage *m=&chat->conversations[chat->active].messages[index];
    m->generation.state=CHAT_GENERATION_COMPLETE;
    if (reasoning) chat_message_set_reasoning(m,reasoning);
    m->generation.reasoning_ms=reasoning?reasoning_ms:-1;
    chat_message_touch(m);
    return index;
}
/* Render timing uses the performance counter, not the host's tick clock. */
static double now_ms(void) {
    LARGE_INTEGER frequency,counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart*1000.0/(double)frequency.QuadPart;
}
/* Pumps real messages, letting the host's scheduled flush timer fire the way
   it does in the running app. */
static void pump_messages(DWORD ms) {
    ULONGLONG deadline=GetTickCount64()+ms;
    do {
        MSG message;
        if (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjects(0,NULL,FALSE,10,QS_ALLINPUT);
        }
    } while (GetTickCount64()<deadline);
}
/* Pumps until the host has interpreted the completion of `attempt`. */
static void wait_attempt_handled(ChatHost *h,uint64_t attempt) {
    ULONGLONG deadline=GetTickCount64()+5000;
    while (h->handled_attempt<attempt && GetTickCount64()<deadline)
        pump_messages(5);
}
/* Pumps until a turn's body contains needle; false on timeout. */
static bool pump_until_body(ChatHost *h,int index,const wchar_t *needle,
    DWORD timeout_ms) {
    ULONGLONG deadline=GetTickCount64()+timeout_ms;
    for (;;) {
        wchar_t body[2048]; body_text(h,index,body,2048);
        if (wcsstr(body,needle)) return true;
        if (GetTickCount64()>=deadline) return false;
        pump_messages(5);
    }
}
/* Pumps until posted saver completions report every handed-off change
   durable (dirty cleared, failure latch clear); false on timeout. */
static bool wait_save_settled(ChatHost *h,DWORD timeout_ms) {
    ULONGLONG deadline=GetTickCount64()+timeout_ms;
    for (;;) {
        pump_messages(5);
        if (!h->dirty && !h->save_failed) return true;
        if (GetTickCount64()>=deadline) return false;
    }
}
/* Pumps until a save completion reports failure; false on timeout. */
static bool wait_save_failed(ChatHost *h,DWORD timeout_ms) {
    ULONGLONG deadline=GetTickCount64()+timeout_ms;
    for (;;) {
        pump_messages(5);
        if (h->save_failed) return true;
        if (GetTickCount64()>=deadline) return false;
    }
}
/* Transcript child controls the container currently owns (realized or shown).
   WS_VISIBLE is read directly: the host's own top-level window is never shown
   in this headless test, so IsWindowVisible would report every child hidden. */
static int child_controls(HWND parent,bool visible_only) {
    int count=0;
    for (HWND child=GetWindow(parent,GW_CHILD); child;
         child=GetWindow(child,GW_HWNDNEXT))
        if (!visible_only ||
            (GetWindowLongPtrW(child,GWL_STYLE) & WS_VISIBLE)) count++;
    return count;
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
    /* The real background writer runs for the whole suite, exactly as in the
       app: every save below is a handoff, and flush-and-wait sites (the
       pre-request gate, close, and the explicit save_sync calls) block until
       their own attempt is durable. */
    save_paused_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    save_resume_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    save_teardown_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(save_paused_event && save_resume_event && save_teardown_event);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    rich_text_set_text(&h->composer,L"Question"); perform_send(h);
    CHECK(chat->conversations[0].message_count==2);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED); /* no key */
    CHECK(!h->generating);
    /* The seam is live: the missing-key send above did reach the client (which
       refused it), so "the client was never called" below means something. */
    CHECK(openrouter_request_calls==1);
    /* An oversized indispensable context must fail explicitly instead of being
       truncated: here the system prompt and the message each fit the budget
       alone, but not together. The pending turn stays retryable, neither the
       system prompt nor the message is shortened, and the client is never
       asked to send the unusable context. */
    {
        wchar_t *wide=(wchar_t *)malloc(16384*sizeof *wide);
        CHECK(wide);
        int calls=openrouter_request_calls;
        for (int i=0;i<16383;i++) wide[i]=0x2014;   /* three encoded bytes each */
        wide[16383]=0;
        wcscpy(chat->system_prompt,wide);
        rich_text_set_text(&h->composer,wide);
        perform_send(h);
        ChatConversation *c=&chat->conversations[0];
        CHECK(c->message_count==4);
        CHECK(c->messages[3].generation.state==CHAT_GENERATION_FAILED);
        CHECK(!h->generating && h->request_generation==0 && h->context_dropped==0);
        CHECK(openrouter_request_calls==calls);           /* nothing was sent */
        CHECK(wcsstr(c->messages[3].generation.error,L"too large together")!=NULL);
        CHECK(wcsstr(c->messages[3].generation.error,L"65536")!=NULL);
        CHECK(wcslen(chat_message_text(&c->messages[2]))==16383);  /* never truncated */
        CHECK(wcslen(chat->system_prompt)==16383);
        CHECK(wcsstr(chat->status,L"Request failed")!=NULL);
        { wchar_t meta[512]; meta_text(h,3,meta,512);
          CHECK(wcsstr(meta,L"too large together")!=NULL); }
        /* Retry goes through the real send path, fails identically, and still
           never reaches the client. */
        start_response(h,CHAT_RETRY,NULL);
        c=&chat->conversations[0];
        CHECK(c->message_count==4);
        CHECK(c->messages[3].generation.state==CHAT_GENERATION_FAILED);
        CHECK(wcsstr(c->messages[3].generation.error,L"too large together")!=NULL);
        CHECK(!h->generating && h->request_generation==0);
        CHECK(openrouter_request_calls==calls);
        /* Restore the state the following checks expect. */
        for (size_t i=2;i<c->message_count;i++) chat_message_dispose(&c->messages[i]);
        c->message_count=2;
        chat->system_prompt[0]=0; c->draft[0]=0;
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=0;
        h->generating=false; h->context_dropped=0;
        render_transcript(h);
        free(wide);
    }
    /* A real successful send that omitted history: the seam fakes the started
       generation, so the whole send path runs and the client sees exactly the
       bounded context, with the omission count carried as request-scoped host
       state through the one-second status sweep. */
    {
        wchar_t *huge=(wchar_t *)malloc(200001*sizeof *huge);
        CHECK(huge);
        for (int i=0;i<200000;i++) huge[i]=L'x';
        huge[200000]=0;
        ChatConversation *c=&chat->conversations[0];
        wcscpy(chat->system_prompt,L"Be brief.");
        chat_append(chat,CHAT_ROLE_USER,L"old question");                  /* 2 */
        int old=chat_append(chat,CHAT_ROLE_ASSISTANT,huge);                /* 3 dropped */
        c->messages[old].generation.state=CHAT_GENERATION_COMPLETE;
        chat_append(chat,CHAT_ROLE_USER,L"middle question");               /* 4 kept */
        int middle=chat_append(chat,CHAT_ROLE_ASSISTANT,L"middle answer"); /* 5 kept */
        c->messages[middle].generation.state=CHAT_GENERATION_COMPLETE;
        int calls=openrouter_request_calls;
        rich_text_set_text(&h->composer,L"final question");
        openrouter_request_fake_generation=4242;
        perform_send(h);
        openrouter_request_fake_generation=0;
        CHECK(openrouter_request_calls==calls+1);          /* exactly one send */
        CHECK(h->generating && h->request_generation==4242);
        /* What the client was handed: the system prompt, the newest eligible
           history that fits, and the trigger -- never the dropped text. */
        CHECK(openrouter_request_last_count==4);
        CHECK(openrouter_request_last_roles[0]==CHAT_ROLE_SYSTEM &&
              !wcscmp(openrouter_request_last_texts[0],L"Be brief."));
        CHECK(openrouter_request_last_roles[1]==CHAT_ROLE_USER &&
              !wcscmp(openrouter_request_last_texts[1],L"middle question"));
        CHECK(openrouter_request_last_roles[2]==CHAT_ROLE_ASSISTANT &&
              !wcscmp(openrouter_request_last_texts[2],L"middle answer"));
        CHECK(openrouter_request_last_roles[3]==CHAT_ROLE_USER &&
              !wcscmp(openrouter_request_last_texts[3],L"final question"));
        int sent_huge=0;
        for (int i=0;i<openrouter_request_last_count;i++)
            if (openrouter_request_last_texts[i]==chat_message_text(&c->messages[old]))
                sent_huge=1;
        CHECK(!sent_huge);                                 /* the huge message stayed home */
        CHECK(h->context_dropped==3);                      /* "Question", "old question", huge */
        CHECK(wcsstr(chat->status,L"3 older messages omitted")!=NULL);
        /* The status sweep keeps showing it for the life of the request. */
        SendMessageW(window,WM_TIMER,2,0);
        CHECK(wcsstr(chat->status,L"elapsed")!=NULL &&
              wcsstr(chat->status,L"3 older messages omitted")!=NULL);
        /* Finish the faked request through the real event path. */
        handle_event(h,fixture(h,OPENROUTER_DELTA,L"streamed answer"));
        handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
        c=&chat->conversations[0];
        CHECK(!h->generating && !h->context_dropped);
        CHECK(c->message_count==8 && c->messages[6].role==CHAT_ROLE_USER);
        CHECK(c->messages[7].generation.state==CHAT_GENERATION_COMPLETE);
        CHECK(!wcscmp(chat_message_text(&c->messages[7]),L"streamed answer"));
        /* Restore the state the following checks expect. */
        for (size_t i=2;i<c->message_count;i++) chat_message_dispose(&c->messages[i]);
        c->message_count=2;
        chat->system_prompt[0]=0; c->draft[0]=0;
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=0;
        h->generating=false; h->context_dropped=0; h->request_generation=0;
        render_transcript(h);
        free(huge);
    }
    begin_fixture(h); CHECK(h->request_message==1);
    OpenRouterEvent *e=fixture(h,OPENROUTER_DELTA,L"Partial answer");
    e->metadata.ttft_ms=12; e->metadata.first_token_at=chat_now();
    handle_event(h,e); CHECK(!wcscmp(pending(h)->text,L"Partial answer"));
    mark_dirty(h); CHECK(save_sync(h));   /* durable before the stale event lands */
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
    CHECK(h->transcript.turn_count==4);
    CHECK(row_present(h,first) && row_present(h,second));
    CHECK(!h->transcript.turns[first].reason_live && !h->transcript.turns[second].reason_live);
    head_text(h,first,text,128);
    CHECK(wcsstr(text,L"1.5s")!=NULL);
    /* Expanding one turn never touches another. */
    click_row(h,first);
    CHECK(h->transcript.turns[first].reason_live && !h->transcript.turns[second].reason_live);
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    /* Measuring text must not include its position in the transcript. */
    { TranscriptTurn *turn=&h->transcript.turns[first];
      int height=turn->head_h;
      CHECK(height<px(h,50));
      CHECK(turn->reason_y-turn->head_y-height==px(h,8));
      transcript_layout_from(&h->transcript,0,false);
      CHECK(turn->head_h==height);
      transcript_layout_from(&h->transcript,0,false);
      CHECK(turn->head_h==height); }
    click_row(h,second);
    CHECK(h->transcript.turns[first].reason_live && h->transcript.turns[second].reason_live);
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"Beta reasoning"));
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    click_row(h,first);
    CHECK(!h->transcript.turns[first].reason_live && h->transcript.turns[second].reason_live);
    /* Collapsed by default while waiting; explicit expansion streams live into
       that turn only and is not collapsed when the answer begins. */
    begin_regenerate(h);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_RUNNING);
    head_text(h,second,text,128);
    CHECK(wcsstr(text,L"Thinking")!=NULL);
    CHECK(!h->transcript.turns[second].reason_live);
    click_row(h,second);
    CHECK(h->transcript.turns[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"stream rea"));
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"soning"));
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"stream reasoning"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Streamed answer"));
    CHECK(h->transcript.turns[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    CHECK(pending(h)->generation.reasoning_ms>=0);
    /* The running answer is never mixed with metadata, and no footer exists
       until the turn is terminal. */
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"Streamed answer")); }
    CHECK(!h->transcript.turns[second].meta_live);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(h->transcript.turns[second].meta_live);
    { wchar_t meta[256]; meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"Complete")!=NULL); }
    /* Reasoning and duration persist per message. */
    mark_dirty(h); CHECK(save_sync(h));
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
    chat_message_touch(&chat->conversations[cv].messages[second]);
    render_transcript(h);
    CHECK(h->transcript.turns[second].reason_live);
    begin_regenerate(h);
    CHECK(chat->conversations[cv].messages[second].reasoning[0]==0);
    CHECK(!chat->conversations[cv].messages[second].reasoning_open);
    CHECK(!h->transcript.turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    /* Cancellation/error keeps that turn coherent: reasoning retained, row and
       expansion intact. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"kept"));
    click_row(h,second);
    CHECK(h->transcript.turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_ERROR,L"boom"));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    CHECK(!wcscmp(chat->conversations[cv].messages[second].reasoning,L"kept"));
    CHECK(row_present(h,second) &&
        chat->conversations[cv].messages[second].reasoning_open);
    /* Switching conversations restores each message's own reasoning. */
    wcscpy(chat->conversations[0].messages[1].reasoning,L"Conv zero");
    chat->conversations[0].messages[1].reasoning_open=false;
    chat_message_touch(&chat->conversations[0].messages[1]);
    command(h,CHAT_COMMAND_SELECT,0);
    CHECK(h->transcript.turn_count==2);
    CHECK(row_present(h,1));
    click_row(h,1);
    { wchar_t text[128]; reasoning_text(h,1,text,128);
      CHECK(!wcscmp(text,L"Conv zero")); }
    command(h,CHAT_COMMAND_SELECT,cv);
    CHECK(h->transcript.turn_count==4);
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
    /* Native composer text uses CRLF. The transcript must treat that pair as
       one break instead of rendering CR and LF as separate paragraphs. */
    { RichTextControl probe;
      CHECK(rich_text_create_block(&probe,window,899,&h->rich_theme,96));
      SetWindowPos(probe.window,NULL,0,0,600,200,
          SWP_NOZORDER|SWP_NOACTIVATE);
      rich_text_set_body(&probe,CHAT_ROLE_USER,L"one\r\n\r\ntwo");
      CHECK(SendMessageW(probe.window,EM_GETLINECOUNT,0,0)==3);
      DestroyWindow(probe.window); }
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
    /* Composer text comes back as CRLF; rendering must not count CR and LF as
       separate paragraph breaks. */
    { RichTextControl probe;
      CHECK(rich_text_create_block(&probe,window,899,&h->rich_theme,96));
      SetWindowPos(probe.window,NULL,0,0,500,200,SWP_NOZORDER|SWP_NOACTIVATE);
      rich_text_set_body(&probe,CHAT_ROLE_USER,L"one\r\n\r\ntwo");
      CHECK(SendMessageW(probe.window,EM_GETLINECOUNT,0,0)==3);
      DestroyWindow(probe.window); }
    /* Reasoning-to-answer streaming keeps geometry stable across repeated
       layouts, follows only at the bottom, and never scrolls inside a body. */
    begin_regenerate(h);
    click_row(h,second);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"Working through it"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"Answer"));
    { TranscriptTurn *turn=&h->transcript.turns[second];
      int head_height=turn->head_h;
      int reason_y=turn->reason_y;
      position_turns(h,true);
      for (int i=0;i<80;i++) {
          int height=turn->body_h, scroll=h->transcript.view_scroll;
          handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nAnother line of the streamed answer."));
          CHECK(turn->head_h==head_height && turn->reason_y==reason_y);
          CHECK(turn->body_h>=height && turn->body_h-height<px(h,40));
          CHECK(h->transcript.view_scroll>=scroll && transcript_pinned(&h->transcript));
          POINT origin={0,0};
          SendMessageW(turn->body.window,EM_GETSCROLLPOS,0,(LPARAM)&origin);
          CHECK(origin.y==0);
      }
      /* The burst stayed within the throttle window, so nothing above was
         reparsed per token; flushing renders the accumulated burst in one
         step and the transcript still follows at the bottom. */
      int before_flush=turn->body_h;
      h->transcript.body_render_tick=0;
      handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nFlushed rebuild."));
      CHECK(turn->body_h>before_flush && transcript_pinned(&h->transcript));
      h->transcript.view_scroll=px(h,30); position_turns(h,false);
      CHECK(!transcript_pinned(&h->transcript));
      int scroll=h->transcript.view_scroll;
      for (int i=0;i<8;i++) {
          handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nMore text while reading above."));
          CHECK(h->transcript.view_scroll==scroll);
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
    /* ---- Progressive Markdown streaming: the first delta renders
       immediately, rapid deltas stay within the throttle window (no per-token
       reparse), an elapsed window renders the accumulated text so markers
       fragmented across deltas reassemble, incomplete syntax stays literal,
       and the terminal event flushes the final render. Stored message text
       keeps the raw markers throughout. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    add_turn(chat,L"seed",L"seed answer",NULL,-1);
    begin_regenerate(h);
    int stream_turn=h->request_message;
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"# Tit"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Tit")); }
    h->transcript.body_render_tick=GetTickCount64();
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"le **bo"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"ld** an"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"d `co"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"de` an"));
    CHECK(GetTickCount64()-h->transcript.body_render_tick<CHAT_BODY_RENDER_MS);
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Tit")); }
    Sleep(CHAT_BODY_RENDER_MS+20);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"d\nnext **ope"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext **ope"));
      CHARFORMAT2W f;
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* "bold" */
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_SETSEL,6,7);
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK((f.dwEffects & CFE_BOLD) && !(f.dwEffects & CFE_ITALIC));
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* "code" */
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_SETSEL,15,16);
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!wcscmp(f.szFaceName,L"Consolas") &&
          (f.dwMask & CFM_BACKCOLOR));
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* plain tail */
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_SETSEL,26,27);
      SendMessageW(h->transcript.turns[stream_turn].body.window,EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!(f.dwEffects & CFE_BOLD) && !wcscmp(f.szFaceName,L"Segoe UI")); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **ope"));
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext **ope")); }
    command(h,CHAT_COMMAND_SELECT,cv);
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
      chat_message_touch(m);
      render_transcript(h);
      CHECK(h->transcript.turns[second].meta_live);
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
      chat_message_touch(m);
      render_transcript(h);
      meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"deepseek/deepseek-v4.1-flash \u2192 deepseek/other")!=NULL);
      wcscpy(m->generation.actual_model,m->generation.requested_model);
      wcscpy(m->generation.finish_reason,L"length");
      chat_message_touch(m);
      render_transcript(h);
      meta_text(h,second,meta,256);
      CHECK(wcsstr(meta,L"finish: length")!=NULL);
      CHECK(wcsstr(meta,L"stop")==NULL);
      wcscpy(m->generation.finish_reason,L"stop");
      chat_message_touch(m);
    }
    /* ---- Revision-tracked updates and selection preservation ---- */
    /* An active selection in an unchanged historical turn survives transcript
       rebuilds and a sibling turn's replacement untouched. */
    { HWND body=h->transcript.turns[first].body.window;
      CHECK(body);
      SendMessageW(body,EM_SETSEL,2,6);
      render_transcript(h);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==6);
      begin_regenerate(h);
      handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
      CHECK(!h->generating);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==6);
      wchar_t kept[128]; body_text(h,first,kept,128);
      CHECK(wcsstr(kept,L"First answer")!=NULL); }
    /* Narrow updates: a metadata-only bump (completion writes the footer, not
       the answer) leaves the body current, so the body write is skipped, not
       deferred, and a selection placed in it stays put. */
    { ChatMessage *m=&chat->conversations[cv].messages[first];
      HWND body=h->transcript.turns[first].body.window;
      TranscriptTurn *turn=&h->transcript.turns[first];
      CHECK(body && turn->body_live);
      SendMessageW(body,EM_SETSEL,0,0);        /* apply any prior deferral */
      render_transcript(h);
      CHECK(!turn->body_pending);
      SendMessageW(body,EM_SETSEL,1,4);
      int body_before=turn->body_h;
      m->generation.prompt_tokens=11;          /* metadata only; text unchanged */
      chat_message_touch(m);
      render_transcript(h);
      CHECK(!turn->body_pending);              /* skipped, not deferred */
      CHECK(turn->body_h==body_before);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==1 && sel.cpMax==4); }
    /* Starting another response resets the host-global content_started, which
       only shapes the row of the turn that is actually streaming: a completed
       historical turn stays current, so head and body are not rewritten. */
    { HWND body=h->transcript.turns[first].body.window;
      TranscriptTurn *turn=&h->transcript.turns[first];
      SendMessageW(body,EM_SETSEL,0,0);
      render_transcript(h);
      CHECK(!turn->body_pending && !turn->head_pending);
      SendMessageW(body,EM_SETSEL,1,4);
      begin_regenerate(h);
      CHECK(!h->content_started);
      CHECK(!turn->body_pending && !turn->head_pending);
      wchar_t kept[128]; body_text(h,first,kept,128);
      CHECK(wcsstr(kept,L"First answer")!=NULL);
      handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
      SendMessageW(body,EM_SETSEL,0,0); }
    /* Streaming: a selection inside the live answer defers the throttled
       Markdown rebuild; clearing the selection applies the deferred render and
       relayouts from the affected turn, so geometry, the scrollbar range and
       bottom-following stay correct even with no further stream event. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"**raw *stream"));
    { HWND body=h->transcript.turns[second].body.window;
      TranscriptTurn *turn=&h->transcript.turns[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      h->transcript.body_render_tick=0;   /* force the throttled rebuild */
      handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nmore"));
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==3 && sel.cpMax==7);   /* selection survived the flush */
      wchar_t shown[256]; body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"**raw *stream")!=NULL);   /* not rebuilt */
      CHECK(wcsstr(shown,L"more")==NULL);            /* rebuild deferred */
      /* Clearing the selection applies the deferred Markdown rebuild and
         remeasures: the turn grows, the following surface and the scrollbar
         range move, and a pinned transcript still follows the bottom. */
      h->transcript.view_scroll=0x7fffffff;
      transcript_position(&h->transcript,false);   /* pin to the bottom */
      int body_before=turn->body_h, height_before=turn->height;
      int content_before=h->transcript.view_content;
      SendMessageW(body,EM_SETSEL,0,0);
      CHECK(turn->body_h>body_before);
      CHECK(turn->height>height_before);
      CHECK(h->transcript.view_content>content_before);
      CHECK(transcript_pinned(&h->transcript));
      body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"more")!=NULL); }
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* The same deferral persists through generation completion: the terminal
       Markdown render waits for the selection, and clearing afterwards still
       relayouts so the following metadata footer and scrollbar follow. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"**raw *stream"));
    { HWND body=h->transcript.turns[second].body.window;
      TranscriptTurn *turn=&h->transcript.turns[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      h->transcript.body_render_tick=0;
      handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nmore"));
      handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
      CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
      CHECK(h->transcript.turns[second].meta_live);
      wchar_t shown[256]; body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"more")==NULL);   /* terminal render still deferred */
      h->transcript.view_scroll=0x7fffffff;
      transcript_position(&h->transcript,false);
      int body_before=turn->body_h, meta_before=turn->meta_y;
      int content_before=h->transcript.view_content;
      SendMessageW(body,EM_SETSEL,0,0);
      CHECK(turn->body_h>body_before);
      CHECK(turn->meta_y>meta_before);     /* following footer moved down */
      CHECK(h->transcript.view_content>content_before);
      CHECK(transcript_pinned(&h->transcript));
      body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"more")!=NULL); }
    /* Streaming reasoning appends preserve an in-progress selection. */
    begin_regenerate(h);
    click_row(h,second);
    CHECK(h->transcript.turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"alpha beta"));
    { HWND vp=h->transcript.turns[second].reasoning.window;
      CHECK(vp);
      SendMessageW(vp,EM_SETSEL,2,5);
      handle_event(h,fixture(h,OPENROUTER_REASONING,L" gamma"));
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(vp,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==5);
      wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"alpha beta gamma")); }
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* ---- Scheduled streaming flush ---- */
    /* A burst inside the throttle window marks the body dirty and arms a
       one-shot flush; with no further delta, the accumulated text still
       reaches the body once the timer fires. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"first token"));
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"first token")); }
    handle_event(h,fixture(h,OPENROUTER_DELTA,L" second"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L" part"));
    CHECK(h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")==NULL); }        /* not rebuilt yet */
    CHECK(pump_until_body(h,second,L"first token second part",1000));
    CHECK(!h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"first token second part")); }
    CHECK(transcript_pinned(&h->transcript));
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* Scheduling cannot strand dirty text: when no flush can be armed
       (SetTimer failure) the body renders at once and the pending flag clears
       so later deltas can arm again. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"first token"));
    handle_event(h,fixture(h,OPENROUTER_DELTA,L" second"));
    CHECK(h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")==NULL); }
    { HWND window=h->window; h->window=(HWND)1;   /* invalid: SetTimer fails */
      schedule_body_flush(h);
      h->window=window; }
    CHECK(!h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")!=NULL); }      /* rendered immediately */
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* A scheduled flush must not destroy a selection in the live body: the
       rebuild is deferred, then applied and relaid out when the range clears. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"streamed so far"));
    { HWND body=h->transcript.turns[second].body.window;
      TranscriptTurn *turn=&h->transcript.turns[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      int body_before=turn->body_h;
      handle_event(h,fixture(h,OPENROUTER_DELTA,L"\nplus more"));
      CHECK(h->body_flush_pending);
      pump_messages(500);                    /* let the scheduled flush fire */
      CHECK(!h->body_flush_pending);
      CHECK(turn->body_pending);             /* deferred, not rewritten */
      /* The deferred write must not claim text it has not shown yet. */
      CHECK(turn->body_revision!=
          chat->conversations[cv].messages[second].body_revision);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==3 && sel.cpMax==7);
      wchar_t shown[256]; body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"plus more")==NULL);
      h->transcript.view_scroll=0x7fffffff;
      transcript_position(&h->transcript,false);   /* pin to the bottom */
      int content_before=h->transcript.view_content;
      SendMessageW(body,EM_SETSEL,0,0);
      CHECK(turn->body_h>body_before);
      CHECK(h->transcript.view_content>content_before);
      CHECK(transcript_pinned(&h->transcript));
      body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"plus more")!=NULL);
      /* The deferred apply must leave the recorded revision describing the
         text now in the control. */
      CHECK(turn->body_revision==
          chat->conversations[cv].messages[second].body_revision); }
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* A scheduled flush writes the body outside prepare_turn(), so the
       recorded revision must advance with it. Completion then refreshes only
       the footer, leaving a selection in the live body untouched. */
    begin_regenerate(h);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"first token"));
    { TranscriptTurn *turn=&h->transcript.turns[second];
      ChatMessage *m=&chat->conversations[cv].messages[second];
      HWND body=turn->body.window; CHECK(body);
      CHECK(turn->body_revision==m->body_revision);
      handle_event(h,fixture(h,OPENROUTER_DELTA,L" second"));
      handle_event(h,fixture(h,OPENROUTER_DELTA,L" part"));
      CHECK(h->body_flush_pending);
      CHECK(pump_until_body(h,second,L"first token second part",1000));
      CHECK(!h->body_flush_pending);
      CHECK(turn->body_revision==m->body_revision);
      SendMessageW(body,EM_SETSEL,1,4);
      handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
      CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
      CHECK(!turn->body_pending);                  /* footer only */
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==1 && sel.cpMax==4);
      SendMessageW(body,EM_SETSEL,0,0); }
    /* Live reasoning survives collapse and reopen mid-stream: the collapsed
       viewport stops appending, reopening loads the accumulation, and the
       stream then resumes appending into that turn's own viewport. */
    begin_regenerate(h);
    click_row(h,second);
    CHECK(h->transcript.turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"first"));
    handle_event(h,fixture(h,OPENROUTER_REASONING,L" second"));
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second")); }
    click_row(h,second);                     /* collapse mid-stream */
    CHECK(!h->transcript.turns[second].reason_live);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L" hidden"));
    click_row(h,second);                     /* reopen */
    CHECK(h->transcript.turns[second].reason_live);
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second hidden")); }
    handle_event(h,fixture(h,OPENROUTER_REASONING,L" live"));
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second hidden live")); }
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"answer after reasoning"));
    CHECK(h->transcript.turns[second].reason_live);  /* answer start keeps it */
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* ---- Long transcript: bounded renders and reader state ---- */
    /* A maximum-length transcript must not realize additional controls when it
       is rendered again or when one turn streams, and streaming that turn must
       not destructively rewrite an unchanged historical turn. Render cost is
       recorded, not asserted as a wall-clock threshold. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    { wchar_t answer[1024];
      for (int t=0;t<32;t++) {
          size_t n=0;
          for (int line=0;line<12;line++)
              n+=swprintf(answer+n,1024-n,L"Line %d of answer %d.\n",line,t);
          add_turn(chat,L"question",answer,NULL,-1);
      }
    }
    CHECK(chat->conversations[chat->active].message_count==CHAT_MAX_MESSAGES);
    double render_started=now_ms();
    render_transcript(h);
    double render_ms=now_ms()-render_started;
    int realized=child_controls(h->view,false);
    int visible=child_controls(h->view,true);
    CHECK(realized>0 && visible>0 && visible<realized);
    CHECK(realized<=CHAT_MAX_MESSAGES*4);
    render_transcript(h);
    CHECK(child_controls(h->view,false)==realized);  /* rerender adds none */
    int older=1;                                     /* unchanged historical turn */
    { HWND body=h->transcript.turns[older].body.window;
      CHECK(body);
      SendMessageW(body,EM_SETSEL,1,5); }
    begin_regenerate(h);
    int long_turn=h->request_message;
    CHECK(child_controls(h->view,false)==realized);  /* streaming adds none */
    handle_event(h,fixture(h,OPENROUTER_DELTA,L"long"));
    for (int i=0;i<40;i++)
        handle_event(h,fixture(h,OPENROUTER_DELTA,L" burst line"));
    CHECK(h->body_flush_pending);
    CHECK(h->transcript.view_content>h->transcript.view_page);
    h->transcript.view_scroll=0;
    transcript_position(&h->transcript,false);       /* reading an older turn */
    CHECK(!transcript_pinned(&h->transcript));
    int scroll=h->transcript.view_scroll;
    double flush_started=now_ms();
    CHECK(pump_until_body(h,long_turn,L"burst line burst line",1000));
    double flush_ms=now_ms()-flush_started;
    CHECK(!h->body_flush_pending);
    CHECK(h->transcript.view_scroll==scroll);        /* flush did not follow */
    CHECK(child_controls(h->view,false)==realized);
    handle_event(h,fixture(h,OPENROUTER_DELTA,L" tail"));
    CHECK(h->transcript.view_scroll==scroll);
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(h->transcript.view_scroll==scroll);        /* completion did not follow */
    /* The historical turn was neither rewritten nor deselected by the stream,
       the flush or the terminal render. */
    { HWND body=h->transcript.turns[older].body.window;
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==1 && sel.cpMax==5);
      wchar_t kept[512]; body_text(h,older,kept,512);
      CHECK(wcsstr(kept,L"Line 0 of answer 0")!=NULL); }
    printf("long transcript: %d controls (%d visible), full render %.1f ms, "
        "scheduled flush to visible %.1f ms\n",
        realized,visible,render_ms,flush_ms);
    /* ---- Stable conversation search and jump ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int search_a=chat->active;
    for (int i=0;i<8;i++) add_turn(chat,L"search filler",L"filler answer",NULL,-1);
    int search_body=add_turn(chat,L"search target question",
        L"First host-search-token body",NULL,-1);
    uint64_t search_a_id=chat->conversations[search_a].id;
    uint64_t search_body_id=chat->conversations[search_a].messages[search_body].id;
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int search_b=chat->active;
    for (int i=0;i<6;i++) add_turn(chat,L"other filler",L"other answer",NULL,-1);
    int search_reason=add_turn(chat,L"reason target question",L"plain answer",
        L"Second host-search-token reasoning",900);
    uint64_t search_b_id=chat->conversations[search_b].id;
    uint64_t search_reason_id=chat->conversations[search_b].messages[search_reason].id;
    render_transcript(h);
    h->transcript.view_scroll=0;
    transcript_position(&h->transcript,false);
    CHECK(h->search.window &&
        (GetWindowLongPtrW(h->search.window,GWL_STYLE)&WS_VISIBLE));
    rich_text_set_text(&h->search,L"HOST-search-token");
    CHECK(search_submit(h));
    CHECK(h->search_results.count==2 && h->search_has_selection &&
        h->search_selected==0);
    CHECK(chat->conversations[chat->active].id==search_a_id &&
        h->transcript.turns[search_body].message==search_body_id);
    { TranscriptTurn *turn=&h->transcript.turns[search_body];
      int expected=turn->height>h->transcript.view_page ? turn->y :
          turn->y+turn->height-h->transcript.view_page;
      if (expected<0) expected=0;
      CHECK(h->transcript.view_scroll==expected);
      CHECK(turn->y+turn->height>h->transcript.view_scroll &&
          turn->y<h->transcript.view_scroll+h->transcript.view_page);
      int revealed=h->transcript.view_scroll;
      CHECK(transcript_reveal_turn(&h->transcript,search_body) &&
          h->transcript.view_scroll==revealed); }
    /* Jumping again to an active body only reveals it. A deliberately stale
       unrelated rendered identity proves no full transcript pass occurred. */
    { bool valid=h->transcript.turns[0].rendered_valid;
      h->transcript.turns[0].rendered_valid=false;
      CHECK(jump_search_result(h,0));
      CHECK(!h->transcript.turns[0].rendered_valid);
      h->transcript.turns[0].rendered_valid=valid; }
    CHECK(wcsstr(ui_node(ui,h->chat_ui.search_status)->text,
        L"Assistant message")!=NULL);
    /* F3 resolves the next result by stable ids, switches conversations, opens
       a matched reasoning viewport, and minimally reveals the target turn. */
    CHECK(surface_key(h,VK_F3,false,false,true));
    CHECK(h->search_selected==1 &&
        chat->conversations[chat->active].id==search_b_id &&
        h->transcript.turns[search_reason].message==search_reason_id &&
        chat->conversations[chat->active].messages[search_reason].reasoning_open &&
        h->transcript.turns[search_reason].reason_live);
    { TranscriptTurn *turn=&h->transcript.turns[search_reason];
      CHECK(turn->y+turn->height>h->transcript.view_scroll &&
          turn->y<h->transcript.view_scroll+h->transcript.view_page); }
    /* Opening reasoning in the active conversation refreshes only its turn. */
    chat->conversations[search_b].messages[search_reason].reasoning_open=false;
    refresh_turn(h,search_reason);
    { bool valid=h->transcript.turns[0].rendered_valid;
      h->transcript.turns[0].rendered_valid=false;
      CHECK(jump_search_result(h,1));
      CHECK(h->transcript.turns[search_reason].reason_live &&
          !h->transcript.turns[0].rendered_valid);
      h->transcript.turns[0].rendered_valid=valid; }
    CHECK(surface_key(h,VK_F3,true,false,true));
    CHECK(h->search_selected==0 &&
        chat->conversations[chat->active].id==search_a_id);
    CHECK(surface_key(h,VK_F3,false,false,true));
    CHECK(h->search_selected==1 &&
        chat->conversations[chat->active].id==search_b_id);
    /* A changed query honors Shift+F3's direction on its fresh result set. */
    rich_text_set_text(&h->search,L"host-search-token");
    CHECK(surface_key(h,VK_F3,true,false,true));
    CHECK(h->search_selected==1 &&
        chat->conversations[chat->active].id==search_b_id);
    /* An edited matched field invalidates the retained result instead of
       letting the old offset or reused turn resolve to new content. */
    CHECK(chat_message_set_reasoning(
        &chat->conversations[chat->active].messages[search_reason],
        L"reasoning changed after search"));
    CHECK(!jump_search_result(h,1));
    CHECK(surface_key(h,VK_F3,false,false,true));
    CHECK(h->search_selected==0 &&
        chat->conversations[chat->active].id==search_a_id);
    SetFocus(h->composer.window);
    CHECK(surface_key(h,L'F',false,true,true) && GetFocus()==h->search.window);
    int before_empty=chat->active;
    rich_text_set_text(&h->search,L"");
    CHECK(search_submit(h) && !h->search_results.count &&
        chat->active==before_empty);
    /* ---- Reasoning never carries across conversations ---- */
    /* Switching A -> B -> A while both corresponding reasoning viewports are
       expanded, with A still streaming while hidden, must reload A's own
       reasoning: the reused control must not keep showing B's content or
       accumulate A's deltas on top of it. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int conv_a=chat->active;
    add_turn(chat,L"a question",L"A answer",L"A reasoning",1000);
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int conv_b=chat->active;
    add_turn(chat,L"b question",L"B answer",L"B reasoning",1000);
    chat->conversations[conv_a].messages[1].reasoning_open=true;
    chat->conversations[conv_b].messages[1].reasoning_open=true;
    chat_message_touch(&chat->conversations[conv_a].messages[1]);
    chat_message_touch(&chat->conversations[conv_b].messages[1]);
    command(h,CHAT_COMMAND_SELECT,conv_a);
    begin_regenerate(h);
    click_row(h,1);
    CHECK(h->transcript.turns[1].reason_live);
    handle_event(h,fixture(h,OPENROUTER_REASONING,L"A live"));
    command(h,CHAT_COMMAND_SELECT,conv_b);
    CHECK(chat->active==conv_b);
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"B reasoning")); }
    /* A keeps streaming while hidden; the reply stays with its origin. */
    handle_event(h,fixture(h,OPENROUTER_REASONING,L" more"));
    CHECK(!wcscmp(chat_message_reasoning(
        &chat->conversations[conv_a].messages[1]),L"A live more"));
    command(h,CHAT_COMMAND_SELECT,conv_a);
    CHECK(h->generating && h->request_conversation==conv_a);
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"A live more")); }
    handle_event(h,fixture(h,OPENROUTER_REASONING,L" end"));
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"A live more end")); }
    handle_event(h,fixture(h,OPENROUTER_DONE,NULL));
    /* A selection must never carry across conversations: switching while text
       is selected forces immediate replacement with the other conversation's
       own content. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int cvb=chat->active;
    add_turn(chat,L"prompt",L"Distinct answer",NULL,-1);
    render_transcript(h);
    CHECK(h->transcript.turn_count==2);
    SendMessageW(h->transcript.turns[1].body.window,EM_SETSEL,0,4);
    command(h,CHAT_COMMAND_SELECT,0);
    CHECK(h->transcript.turn_count==2);
    { CHARRANGE sel; memset(&sel,0,sizeof sel);
      HWND body=h->transcript.turns[1].body.window;
      CHECK(body);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==0 && sel.cpMax==0);   /* nothing carried over */
      wchar_t shown[256]; body_text(h,1,shown,256);
      CHECK(wcsstr(shown,L"Distinct answer")==NULL); }
    command(h,CHAT_COMMAND_SELECT,cvb);
    { wchar_t shown[256]; body_text(h,1,shown,256);
      CHECK(wcsstr(shown,L"Distinct answer")!=NULL); }
    command(h,CHAT_COMMAND_SELECT,0);
    rich_text_set_text(&h->composer,L"Unsent draft");
    action(h,ACTION_EDIT);
    CHECK(h->editing && !wcscmp(chat->conversations[0].draft,L"Unsent draft"));
    rich_text_set_text(&h->composer,L"Edited question"); perform_send(h);
    CHECK(!h->editing && !wcscmp(chat->conversations[0].messages[0].text,L"Edited question"));
    CHECK(chat->conversations[0].message_count==2);
    wchar_t composer[CHAT_COMPOSER_TEXT]; rich_text_get_text(&h->composer,composer,CHAT_COMPOSER_TEXT);
    CHECK(!wcscmp(composer,L"Unsent draft"));
    /* ---- Background snapshot writer ---- */
    /* The saver hands off immutable deep snapshots; the live Chat may keep
       mutating while a write is in flight without affecting what the writer
       serializes; latest-wins coalescing displaces unstarted snapshots and
       bounds the writer's passes; and every posted result carries its own
       attempt id plus the mutation counter captured at that handoff, so
       stale completions can neither mark newer mutations durable nor
       re-latch a failure that a newer attempt already resolved. */
    {
        Chat *loaded=calloc(1,sizeof *loaded); CHECK(loaded);
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int saver_conv=chat->active;
        add_turn(chat,L"saver question",L"saver answer",NULL,-1);
        /* Snapshot isolation across a paused in-flight write: the writer
           holds the pre-mutation snapshot while the live Chat moves on, and
           the file lands in the pre-mutation shape. The store is only read
           after the writer is idle again, so the single-thread confinement
           of ChatStorage is preserved. */
        uint64_t attempt_a=h->saver.next_attempt+1;
        arm_save_pause();
        mark_dirty(h);
        CHECK(save(h));
        CHECK(WaitForSingleObject(save_paused_event,5000)==WAIT_OBJECT_0);
        chat_append(chat,CHAT_ROLE_USER,L"mutation after handoff");
        mark_dirty(h);
        SetEvent(save_resume_event);
        wait_attempt_handled(h,attempt_a);
        CHECK(h->handled_attempt>=attempt_a);
        CHECK(storage_load(&h->storage,loaded)==1);
        CHECK(loaded->conversation_count==saver_conv+1);
        CHECK(loaded->conversations[saver_conv].message_count==2 &&
            !wcscmp(loaded->conversations[saver_conv].messages[0].text,
                L"saver question"));
        /* The mutation is newer state: flushing it makes it durable. The
           first attempt's completion cannot clear dirty, because a newer
           mutation already arrived after its own captured counter. */
        mark_dirty(h); save(h);
        CHECK(wait_save_settled(h,5000));
        CHECK(storage_load(&h->storage,loaded)==1);
        CHECK(loaded->conversations[saver_conv].message_count==3 &&
            !wcscmp(loaded->conversations[saver_conv].messages[2].text,
                L"mutation after handoff"));
        /* Result ordering, per attempt: an older attempt's success never
           marks newer mutations durable, a stale result never re-interprets,
           and a failure older than the newest submitted attempt is
           superseded — it must not latch failure, because that newer attempt
           is guaranteed to complete and report authoritatively. The
           synthetic attempt ids stay above the real ones while the sequence
           plays out and both real counters are restored afterwards. */
        uint64_t real_handled=h->handled_attempt;
        uint64_t real_submitted=h->last_submitted_attempt;
        mark_dirty(h);
        uint64_t counter_a=h->mutations;
        uint64_t attempt_u=real_submitted+10;
        h->last_submitted_attempt=attempt_u;   /* attempt_u is the newest submission */
        saver_completed(h,true,attempt_u,counter_a);
        CHECK(!h->dirty);                    /* nothing newer: durable */
        mark_dirty(h);                       /* a newer mutation */
        CHECK(h->dirty);
        saver_completed(h,true,attempt_u,counter_a);   /* same attempt again */
        CHECK(h->dirty);                     /* must not claim durability */
        saver_completed(h,false,attempt_u-1,h->mutations); /* late stale failure */
        CHECK(!h->save_failed);              /* must not re-latch failure */
        CHECK(h->dirty);
        /* Superseded failure: a newer attempt is already submitted, so this
           failure reports nothing. */
        h->last_submitted_attempt=attempt_u+2;
        saver_completed(h,false,attempt_u+1,counter_a);
        CHECK(!h->save_failed);
        /* Once that failure is the newest submission it is authoritative. */
        h->last_submitted_attempt=attempt_u+1;
        saver_completed(h,false,attempt_u+1,counter_a);
        CHECK(h->save_failed && h->dirty);
        /* The next handoff's success resolves everything. */
        h->last_submitted_attempt=attempt_u+2;
        saver_completed(h,true,attempt_u+2,h->mutations);
        CHECK(!h->dirty && !h->save_failed);
        h->handled_attempt=real_handled;     /* real writer sequence resumes */
        h->last_submitted_attempt=real_submitted;
        /* Failure propagation through the writer: a blocked temporary file
           fails the save, latches the failure and explicitly keeps dirty set
           for the next autosave retry, then recovers. */
        HANDLE block=CreateFileW(h->storage.temporary,GENERIC_WRITE,0,NULL,
            OPEN_ALWAYS,0,NULL);
        CHECK(block!=INVALID_HANDLE_VALUE);
        mark_dirty(h); save(h);
        CHECK(wait_save_failed(h,5000));
        CHECK(wcsstr(chat->status,L"Save failed")!=NULL);
        CHECK(h->dirty);
        CloseHandle(block);
        save(h);
        CHECK(wait_save_settled(h,5000));
        /* The pre-request durability gate holds through the background
           writer: a failed flush still refuses to send the request. */
        block=CreateFileW(h->storage.temporary,GENERIC_WRITE,0,NULL,
            OPEN_ALWAYS,0,NULL);
        CHECK(block!=INVALID_HANDLE_VALUE);
        int calls=openrouter_request_calls;
        rich_text_set_text(&h->composer,L"gate question");
        perform_send(h);
        CHECK(openrouter_request_calls==calls);          /* request not sent */
        CHECK(!h->generating);
        { ChatConversation *c=&chat->conversations[chat->active];
          CHECK(c->message_count>=2);
          size_t last=c->message_count-1;
          CHECK(c->messages[last].generation.state==CHAT_GENERATION_FAILED);
          CHECK(wcsstr(c->messages[last].generation.error,
              L"Could not save pending response")!=NULL); }
        CloseHandle(block);
        save(h);                             /* retry now that the file is writable */
        CHECK(wait_save_settled(h,5000));
        /* Latest-wins coalescing, deterministically: while the writer is
           paused inside the first save, two more handoffs are submitted; the
           middle snapshot is displaced before it is ever serialized, so the
           writer passes exactly twice and only the newest state persists. */
        storage_save_calls=0;
        arm_save_pause();
        wcscpy(chat->conversations[chat->active].draft,L"coalesce one");
        mark_dirty(h); CHECK(save(h));
        CHECK(WaitForSingleObject(save_paused_event,5000)==WAIT_OBJECT_0);
        wcscpy(chat->conversations[chat->active].draft,L"coalesce two");
        mark_dirty(h); save(h);              /* pending only */
        wcscpy(chat->conversations[chat->active].draft,L"coalesce three");
        mark_dirty(h); save(h);              /* displaces "coalesce two" */
        SetEvent(save_resume_event);
        CHECK(wait_save_settled(h,5000));
        CHECK(storage_save_calls==2);        /* the middle snapshot never saved */
        CHECK(storage_load(&h->storage,loaded)==1);
        CHECK(!wcscmp(loaded->conversations[saver_conv].draft,L"coalesce three"));
        /* Shutdown with a guaranteed pending job: a second saver is paused
           inside its in-flight save, so the second handoff is certainly
           still pending when saver_shutdown is called; teardown must release
           the pause, drain the pending job and join before returning. */
        {
            ChatSaver drain_saver;
            arm_save_pause();
            wcscpy(chat->conversations[chat->active].draft,L"drain in flight");
            mark_dirty(h);
            Chat *in_flight=chat_snapshot(chat); CHECK(in_flight);
            CHECK(saver_init(&drain_saver,window,CHAT_WM_SAVER_RESULT,
                &h->storage));                   /* the main writer is parked */
            saver_submit(&drain_saver,in_flight,h->mutations);
            CHECK(WaitForSingleObject(save_paused_event,5000)==WAIT_OBJECT_0);
            wcscpy(chat->conversations[chat->active].draft,L"drain pending");
            mark_dirty(h);
            Chat *pending_snap=chat_snapshot(chat); CHECK(pending_snap);
            saver_submit(&drain_saver,pending_snap,h->mutations);
            SetEvent(save_teardown_event);
            saver_shutdown(&drain_saver);
            CHECK(storage_load(&h->storage,loaded)==1);
            CHECK(!wcscmp(loaded->conversations[saver_conv].draft,
                L"drain pending"));
        }
        /* Return to the state the close/reopen test below expects. */
        pump_messages(50);                    /* drain queued completions */
        h->save_failed=false; h->dirty=false;
        command(h,CHAT_COMMAND_SELECT,0);
        rich_text_set_text(&h->composer,L"Unsent draft");
        chat_dispose(loaded); free(loaded);
    }
    /* Stage 6: the windowed sidebar through the real flush pipeline. */
    {
        while (chat->conversation_count < 110) chat_new_conversation(chat);
        command(h, CHAT_COMMAND_SELECT, 109);   /* selects and requests reveal */
        pump_messages(50);
        CHECK(h->chat_ui.pool_count > 1 && h->chat_ui.pool_count < 40);
        CHECK(h->chat_ui.window_offset > 0);    /* the reveal scrolled down */
        bool found = false;
        for (int j = 0; j < h->chat_ui.pool_count; j++)
            if ((uint64_t)ui_node(ui, h->chat_ui.rows[j])->tag ==
                chat->conversations[109].id) found = true;
        CHECK(found);
        /* A settled, no-op flush changes nothing: layout stays gated and no
           new paint is requested. */
        pump_messages(30);
        bool paint_before = ui->paint_dirty;
        CHECK(!ui_layout_pending(ui));
        flush(h);
        CHECK(!ui_layout_pending(ui));
        CHECK(ui->paint_dirty == paint_before);
        /* Invoking a row deep in the window selects the conversation it
           displays, by id. */
        int last = h->chat_ui.pool_count - 1;
        uint64_t displayed = (uint64_t)ui_node(ui, h->chat_ui.rows[last])->tag;
        CHECK(ui_invoke(ui, h->chat_ui.rows[last]));
        CHECK(chat->active == chat_index_of_id(chat, displayed));
        /* Wheel-style scrolling does not snap back to the active row. */
        UiRect row_rect = ui_node(ui, h->chat_ui.rows[0])->rect;
        ui_scroll(ui, row_rect.x + 10, row_rect.y + 5, -400);
        pump_messages(30);
        CHECK(!chat_ui_apply_reveal(&h->chat_ui));
        CHECK(chat->active == chat_index_of_id(chat, displayed));
        /* Search navigation to a conversation above the window selects and
           reveals it by stable id. */
        CHECK(chat_append_at(chat, 1, CHAT_ROLE_USER, L"sidebar needle zzz") >= 0);
        rich_text_set_text(&h->search, L"sidebar needle");
        CHECK(search_refresh(h, false));
        CHECK(chat->active == 1);
        pump_messages(30);
        bool in_window = false;
        for (int j = 0; j < h->chat_ui.pool_count; j++)
            if ((uint64_t)ui_node(ui, h->chat_ui.rows[j])->tag ==
                chat->conversations[chat->active].id) in_window = true;
        CHECK(in_window);
        /* Restore the exact state the close/reopen test below expects. */
        command(h, CHAT_COMMAND_SELECT, 0);
        pump_messages(30);
        rich_text_set_text(&h->composer, L"Unsent draft");
    }
    /* Reveal geometry and report merging through the real flush: renaming a
       conversation bound to the partly visible overscan row requests a
       reveal; the reveal scrolls without changing the offset, so the
       second remap is a no-op and must not discard the first remap's
       NamePropertyChanged finding. */
    {
        int probe_j = h->chat_ui.pool_count - 1;   /* the overscan row */
        UiId probe_row = h->chat_ui.rows[probe_j];
        int probe_index = h->chat_ui.window_offset + probe_j;
        wchar_t probe_old[CHAT_TITLE_TEXT];
        wcscpy(probe_old, ui_node(ui, probe_row)->text);
        float viewport = ui_scroll_viewport_h(ui, h->chat_ui.list);
        float pitch = ui->theme.control_height +
            ui_node(ui, h->chat_ui.list)->style.gap;
        float row_bottom = probe_index * pitch + ui->theme.control_height;
        int offset_before = h->chat_ui.window_offset;
        float scroll = ui_scroll_offset(ui, h->chat_ui.list);
        CHECK(probe_index * pitch >= scroll &&
            row_bottom > scroll + viewport);   /* partly visible only */
        wcscpy(chat->conversations[probe_index].title, L"Merge probe");
        chat_ui_request_reveal(&h->chat_ui,
            chat->conversations[probe_index].id);
        flush(h);
        pump_messages(30);
        CHECK(fabsf(ui_scroll_offset(ui, h->chat_ui.list) -
            (row_bottom - viewport)) < .05f);
        CHECK(h->chat_ui.window_offset == offset_before);
        bool merged = false;
        for (int i = 0; i < h->remap.name_changed_count; i++)
            if (h->remap.name_changed[i].id == probe_row &&
                !wcscmp(h->remap.name_changed[i].old_title, probe_old))
                merged = true;
        CHECK(merged);
        CHECK(!wcscmp(ui_node(ui, probe_row)->text, L"Merge probe"));
    }
    /* Search navigation within the already-active conversation must also
       reveal the sidebar row. */
    {
        command(h, CHAT_COMMAND_SELECT, 100);
        pump_messages(30);
        UiRect list_rect = ui_node(ui, h->chat_ui.list)->rect;
        ui_scroll(ui, list_rect.x + 20, list_rect.y + list_rect.h / 2, -1000);
        flush(h);                                /* remap the scrolled window */
        pump_messages(30);
        bool out_of_window = true;
        for (int j = 0; j < h->chat_ui.pool_count; j++)
            if ((uint64_t)ui_node(ui, h->chat_ui.rows[j])->tag ==
                chat->conversations[chat->active].id) out_of_window = false;
        CHECK(out_of_window);   /* the active row is scrolled away */
        CHECK(chat_append(chat, CHAT_ROLE_USER, L"active needle qqq") >= 0);
        render_transcript(h);   /* the host renders after every mutation */
        rich_text_set_text(&h->search, L"active needle");
        CHECK(search_refresh(h, false));
        CHECK(chat->active == 100);
        pump_messages(30);
        bool in_window = false;
        for (int j = 0; j < h->chat_ui.pool_count; j++)
            if ((uint64_t)ui_node(ui, h->chat_ui.rows[j])->tag ==
                chat->conversations[chat->active].id) in_window = true;
        CHECK(in_window);
        /* Restore the exact state the close/reopen test below expects. */
        command(h, CHAT_COMMAND_SELECT, 0);
        pump_messages(30);
        rich_text_set_text(&h->composer, L"Unsent draft");
    }
    begin_fixture(h); handle_event(h,fixture(h,OPENROUTER_DELTA,L"Closing partial"));
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window) && !h->generating);
    Chat *loaded=calloc(1,sizeof *loaded); CHECK(loaded);
    CHECK(storage_load(&h->storage,loaded)==1);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].text,L"Closing partial"));
    CHECK(!wcscmp(loaded->conversations[0].draft,L"Unsent draft"));
    /* Join the writer before the storage lock is released, as the app does. */
    saver_shutdown(&h->saver);
    CloseHandle(save_paused_event); CloseHandle(save_resume_event);
    CloseHandle(save_teardown_event);
    storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background); rich_text_library_close();
    chat_dispose(loaded); chat_dispose(chat);
    free(loaded); free(chat); free(ui); free(h); CoUninitialize();
    puts("Hidden host: failures, oversized request-context failure that never invokes the client, a successful omitted-history send through the client seam with a request-scoped omission status, stale events, switch, cancel/DONE race, empty reply, per-turn reasoning ownership, metadata footer, revision-tracked updates with preserved selections, deferred markdown under a streaming selection, scheduled flush on burst-then-pause, flush fallback when arming fails, selection across a scheduled flush, live reasoning collapse/reopen, stable-id conversation search with body/reasoning jumps and stale-result rejection, reasoning isolation across A/B/A switching while hidden, cross-conversation selection isolation, completion while reading an older turn with bounded long-transcript controls, edit/draft and close/reopen, background snapshot writer (snapshot isolation across an in-flight write, per-handoff completion accounting, failure latch and retry, pre-request flush gate refusing to send, latest-wins coalescing, shutdown drain) passed");
    return 0;
}
