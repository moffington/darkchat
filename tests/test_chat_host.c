/* Hidden HWND integration of the real host, lifecycle and storage. */
#include "../chat/chat_host_win32.c"
#include "../chat/provider_routing.h"
#include <process.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
/* Client seam: chat.bat test links this suite with -Wl,--wrap=completion_request,
   so every request the host starts passes through __wrap_completion_request.
   That shows exactly which context (if any) would reach the network client
   without touching the network: the wrapper records the messages the host was
   about to send, and can fake a started generation so a real successful send
   with omitted history is driven end to end. */
static int completion_request_calls;
static int completion_request_last_count;
static ChatRole completion_request_last_roles[CHAT_CONTEXT_MAX_ENTRIES];
static const wchar_t *completion_request_last_texts[CHAT_CONTEXT_MAX_ENTRIES];
static int completion_request_fake_generation;   /* 0: delegate to the real client */
static ChatProviderRouting completion_request_last_routing;
static ChatBackend completion_request_last_backend;
static bool completion_request_last_had_routing;
int __real_completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing);
int __wrap_completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing) {
    ++completion_request_calls;
    completion_request_last_count=count;
    completion_request_last_backend=backend;
    for (int i=0;i<count && i<CHAT_CONTEXT_MAX_ENTRIES;i++) {
        completion_request_last_roles[i]=messages[i].role;
        completion_request_last_texts[i]=messages[i].text;
    }
    completion_request_last_had_routing=routing!=NULL;
    if (routing) completion_request_last_routing=*routing;
    else chat_provider_routing_init(&completion_request_last_routing);
    if (completion_request_fake_generation) return completion_request_fake_generation;
    return __real_completion_request(client,backend,api_key_utf8,model,messages,
        count,routing);
}
/* Catalog seams (linked with -Wl,--wrap=model_catalog_request and
   -Wl,--wrap=model_picker_pump): the fetch is counted but never starts a
   worker, and the modal pump returns at once so the suite can drive accept,
   cancel and a mid-open source refresh itself. With lost_worker set the wrapper
   leaves an already-finished thread handle on the client, exactly as a real
   worker whose completion could not be posted does. */
static int catalog_request_calls, catalog_request_generation;
static bool catalog_request_lost_worker;
static unsigned __stdcall catalog_fake_worker(void *parameter) {
    (void)parameter;
    return 0;
}
int __real_model_catalog_request(ModelCatalogClient *client, ChatBackend backend,
    const char *api_key_utf8);
int __wrap_model_catalog_request(ModelCatalogClient *client, ChatBackend backend,
    const char *api_key_utf8) {
    if (backend == CHAT_BACKEND_OPENROUTER &&
        (!api_key_utf8 || !api_key_utf8[0])) return 0;
    ++catalog_request_calls;
    int generation=++catalog_request_generation;
    if (catalog_request_lost_worker) {
        uintptr_t thread=_beginthreadex(NULL,0,catalog_fake_worker,NULL,0,NULL);
        if (!thread) return 0;
        WaitForSingleObject((HANDLE)thread,INFINITE);
        client->thread=(HANDLE)thread;
    }
    /* Mirror the real request's generation bookkeeping so a completion can
       join the (possibly parked) worker. */
    client->generation=generation;
    return generation;
}
/* Focused allocation-failure seams for the picker's transactional refresh;
   disarmed (-1) they forward to the CRT. */
static long alloc_fail_malloc=-1, alloc_fail_realloc=-1;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_malloc>=0) {
        if (alloc_fail_malloc==0) return NULL;
        --alloc_fail_malloc;
    }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (alloc_fail_realloc>=0) {
        if (alloc_fail_realloc==0) return NULL;
        --alloc_fail_realloc;
    }
    return __real_realloc(pointer,size);
}
static int picker_pump_calls;
void __real_model_picker_pump(ModelPicker *picker);
void __wrap_model_picker_pump(ModelPicker *picker) {
    (void)picker;
    ++picker_pump_calls;
}
static ModelCatalogEvent *catalog_fixture(ChatHost *h, ModelCatalogResult result,
    const char *json, const wchar_t *error) {
    ModelCatalogEvent *event=calloc(1,sizeof *event);
    event->generation=h->catalog_generation;
    event->result=result;
    if (json) { size_t size=strlen(json)+1; event->json=malloc(size); memcpy(event->json,json,size); }
    if (error) {
        size_t size=(wcslen(error)+1)*sizeof(wchar_t);
        event->error=malloc(size); memcpy(event->error,error,size);
    }
    return event;
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
static CompletionEvent *fixture(ChatHost *h,CompletionEventType type,const wchar_t *text) {
    CompletionEvent *e=calloc(1,sizeof *e);
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
/* A parked worker lets the catalog hand-off test model a fetch that is still
   in flight without driving the real transport. */
static HANDLE parked_release;
static unsigned __stdcall parked_worker(void *parameter) {
    WaitForSingleObject((HANDLE)parameter, INFINITE);
    return 0;
}
/* Reads a turn's rendered controls back to prove per-turn ownership. */
static void head_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    RichTextControl *control=transcript_surface(&h->transcript,i,TRANSCRIPT_HEAD);
    if (control) rich_text_get_text(control,out,cap);
}
static void reasoning_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    RichTextControl *control=transcript_surface(&h->transcript,i,TRANSCRIPT_REASON);
    if (control) rich_text_get_text(control,out,cap);
}
static void body_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    RichTextControl *control=transcript_surface(&h->transcript,i,TRANSCRIPT_BODY);
    if (control) rich_text_get_text(control,out,cap);
}
static void meta_text(ChatHost *h,int i,wchar_t *out,size_t cap) {
    out[0]=0;
    RichTextControl *control=transcript_surface(&h->transcript,i,TRANSCRIPT_META);
    if (control) rich_text_get_text(control,out,cap);
}
static bool row_present(ChatHost *h,int i) {
    wchar_t text[96]; head_text(h,i,text,96);
    return wcsstr(text,L"Thinking")!=NULL || wcsstr(text,L"Thought for")!=NULL;
}
/* Drives the real whole-row click path for one turn. */
static void click_row(ChatHost *h,int i) {
    RichTextControl *head=transcript_surface(&h->transcript,i,TRANSCRIPT_HEAD);
    if (head) turn_row_click(h,head,1,false);
}
/* Bound body window of one record, or NULL. */
static HWND body_window(ChatHost *h,int i) {
    RichTextControl *control=transcript_surface(&h->transcript,i,TRANSCRIPT_BODY);
    return control ? control->window : NULL;
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
/* Feed seam for direct transcript_position/reveal calls in the tests. */
static TranscriptFeed *feed_arg(ChatHost *h) {
    static TranscriptFeed feed;
    feed = transcript_feed(h);
    return &feed;
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
/* Bounded-realization failure injector: fail exactly the surface creation
    whose control id matches (0 disarms). Deterministic per surface: turn ids
    are 100 + slot*4 + surface, and the measurer is id TRANSCRIPT_MEASURE_ID. */
static int block_fail_id, viewport_fail_id;
bool __real_rich_text_create_block(RichTextControl *control, HWND parent,
    int id, const RichTextTheme *theme, float dpi);
bool __wrap_rich_text_create_block(RichTextControl *control, HWND parent,
    int id, const RichTextTheme *theme, float dpi) {
    if (block_fail_id && id == block_fail_id) return false;
    return __real_rich_text_create_block(control, parent, id, theme, dpi);
}
bool __real_rich_text_create_viewport(RichTextControl *control, HWND parent,
    int id, const RichTextTheme *theme, float dpi);
bool __wrap_rich_text_create_viewport(RichTextControl *control, HWND parent,
    int id, const RichTextTheme *theme, float dpi) {
    if (viewport_fail_id && id == viewport_fail_id) return false;
    return __real_rich_text_create_viewport(control, parent, id, theme, dpi);
}
static int child_controls(HWND parent,bool visible_only) {
    int count=0;
    for (HWND child=GetWindow(parent,GW_CHILD); child;
         child=GetWindow(child,GW_HWNDNEXT))
        if (!visible_only ||
            (GetWindowLongPtrW(child,GWL_STYLE) & WS_VISIBLE)) count++;
    return count;
}
/* Every record that strictly intersects the viewport is bound and shows its
    own body — the qualified I-GAP probe used across the bounded suite. */
static bool visible_realized(ChatHost *h) {
    Transcript *tr=&h->transcript;
    int count=tr->record_count;
    if (count<0) count=0;
    if (count>CHAT_MAX_MESSAGES) count=CHAT_MAX_MESSAGES;
    for (int i=0;i<count;i++) {
        TranscriptRecord *rec=&tr->records[i];
        int top=rec->y-tr->view_scroll;
        if (top>=tr->view_page || top+rec->height<=0) continue;
        if (!transcript_surface(tr,i,TRANSCRIPT_BODY)) return false;
        if (!rec->body_live) return false;
    }
    return true;
}
static int default_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Host test",1100,720,720,480,NULL,
        false};
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
    CHECK(completion_request_calls==1);
    /* An oversized indispensable context must fail explicitly instead of being
       truncated: here the system prompt and the message each fit the budget
       alone, but not together. The pending turn stays retryable, neither the
       system prompt nor the message is shortened, and the client is never
       asked to send the unusable context. */
    {
        wchar_t *wide=(wchar_t *)malloc(16384*sizeof *wide);
        CHECK(wide);
        int calls=completion_request_calls;
        for (int i=0;i<16383;i++) wide[i]=0x2014;   /* three encoded bytes each */
        wide[16383]=0;
        wcscpy(chat->system_prompt,wide);
        rich_text_set_text(&h->composer,wide);
        perform_send(h);
        ChatConversation *c=&chat->conversations[0];
        CHECK(c->message_count==4);
        CHECK(c->messages[3].generation.state==CHAT_GENERATION_FAILED);
        CHECK(!h->generating && h->request_generation==0 && h->context_dropped==0);
        CHECK(completion_request_calls==calls);           /* nothing was sent */
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
        CHECK(completion_request_calls==calls);
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
        int calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"final question");
        completion_request_fake_generation=4242;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);          /* exactly one send */
        CHECK(h->generating && h->request_generation==4242);
        /* What the client was handed: the system prompt, the newest eligible
           history that fits, and the trigger -- never the dropped text. */
        CHECK(completion_request_last_count==4);
        CHECK(completion_request_last_roles[0]==CHAT_ROLE_SYSTEM &&
              !wcscmp(completion_request_last_texts[0],L"Be brief."));
        CHECK(completion_request_last_roles[1]==CHAT_ROLE_USER &&
              !wcscmp(completion_request_last_texts[1],L"middle question"));
        CHECK(completion_request_last_roles[2]==CHAT_ROLE_ASSISTANT &&
              !wcscmp(completion_request_last_texts[2],L"middle answer"));
        CHECK(completion_request_last_roles[3]==CHAT_ROLE_USER &&
              !wcscmp(completion_request_last_texts[3],L"final question"));
        int sent_huge=0;
        for (int i=0;i<completion_request_last_count;i++)
            if (completion_request_last_texts[i]==chat_message_text(&c->messages[old]))
                sent_huge=1;
        CHECK(!sent_huge);                                 /* the huge message stayed home */
        CHECK(h->context_dropped==3);                      /* "Question", "old question", huge */
        CHECK(wcsstr(chat->status,L"3 older messages omitted")!=NULL);
        /* The status sweep keeps showing it for the life of the request. */
        SendMessageW(window,WM_TIMER,2,0);
        CHECK(wcsstr(chat->status,L"elapsed")!=NULL &&
              wcsstr(chat->status,L"3 older messages omitted")!=NULL);
        /* Finish the faked request through the real event path. */
        handle_event(h,fixture(h,COMPLETION_DELTA,L"streamed answer"));
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
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
    /* ---- Provider routing: menu action -> persisted setting -> request ---- */
    {
        int calls=completion_request_calls;
        action(h,ACTION_ROUTING_SORT_LATENCY);
        action(h,ACTION_ROUTING_ALLOW_FALLBACKS);   /* toggles fallbacks off */
        action(h,ACTION_ROUTING_DATA_COLLECTION);   /* toggles data collection to deny */
        action(h,ACTION_ROUTING_ZDR);               /* adds request-level ZDR */
        CHECK(chat->provider_routing.sort==CHAT_PROVIDER_SORT_LATENCY);
        CHECK(chat->provider_routing.disallow_fallbacks);
        CHECK(chat->provider_routing.data_collection==CHAT_DATA_COLLECTION_DENY);
        CHECK(chat->provider_routing.zdr);
        /* The next request carries exactly this routing, captured at the seam. */
        rich_text_set_text(&h->composer,L"routed question");
        completion_request_fake_generation=7373;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);
        CHECK(h->generating && h->request_generation==7373);
        CHECK(completion_request_last_routing.sort==CHAT_PROVIDER_SORT_LATENCY &&
            completion_request_last_routing.disallow_fallbacks &&
            completion_request_last_routing.data_collection==CHAT_DATA_COLLECTION_DENY &&
            completion_request_last_routing.zdr);
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        /* Restore OpenRouter defaults so the rest of the suite is unaffected. */
        action(h,ACTION_ROUTING_SORT_DEFAULT);
        action(h,ACTION_ROUTING_ALLOW_FALLBACKS);
        action(h,ACTION_ROUTING_DATA_COLLECTION);
        action(h,ACTION_ROUTING_ZDR);
        CHECK(chat->provider_routing.sort==CHAT_PROVIDER_SORT_DEFAULT &&
            !chat->provider_routing.disallow_fallbacks &&
            chat->provider_routing.data_collection==CHAT_DATA_COLLECTION_ALLOW &&
            !chat->provider_routing.zdr);
        ChatConversation *route_conv=&chat->conversations[0];
        for (size_t i=2;i<route_conv->message_count;i++)
            chat_message_dispose(&route_conv->messages[i]);
        route_conv->message_count=2;
        chat->system_prompt[0]=0; route_conv->draft[0]=0;
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=0;
        h->generating=false; h->context_dropped=0; h->request_generation=0;
        render_transcript(h);
    }
    begin_fixture(h); CHECK(h->request_message==1);
    CompletionEvent *e=fixture(h,COMPLETION_DELTA,L"Partial answer");
    e->metadata.ttft_ms=12; e->metadata.first_token_at=chat_now();
    handle_event(h,e); CHECK(!wcscmp(pending(h)->text,L"Partial answer"));
    mark_dirty(h); CHECK(save_sync(h));   /* durable before the stale event lands */
    e=fixture(h,COMPLETION_DELTA,L"STALE"); --e->generation; handle_event(h,e);
    CHECK(!wcscmp(pending(h)->text,L"Partial answer"));
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    CHECK(chat->active==1 && chat->conversations[1].message_count==0);
    handle_event(h,fixture(h,COMPLETION_ERROR,L"Provider failed"));
    CHECK(chat->conversations[0].messages[1].generation.state==CHAT_GENERATION_FAILED);
    CHECK(!wcscmp(chat->conversations[0].messages[1].text,L"Partial answer"));
    CHECK(chat->conversations[1].message_count==0);
    command(h,CHAT_COMMAND_SELECT,0);
    begin_fixture(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"New response"));
    h->stopping=true; h->accepting=false; h->stop_state=CHAT_GENERATION_CANCELLED;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"late chunk"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_CANCELLED);
    CHECK(!wcscmp(pending(h)->text,L"New response"));
    CHECK(chat->conversations[0].message_count==2);
    begin_fixture(h); handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED); /* empty completion */
    /* ---- Per-turn reasoning ownership ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int cv=chat->active;
    int first=add_turn(chat,L"first",L"First answer",L"Alpha reasoning",1500);
    int second=add_turn(chat,L"second",L"Second answer",L"Beta reasoning",2500);
    wchar_t text[128];
    render_transcript(h);
    CHECK(h->transcript.record_count==4);
    CHECK(row_present(h,first) && row_present(h,second));
    CHECK(!h->transcript.records[first].reason_live && !h->transcript.records[second].reason_live);
    head_text(h,first,text,128);
    CHECK(wcsstr(text,L"1.5s")!=NULL);
    /* Expanding one turn never touches another. */
    click_row(h,first);
    CHECK(h->transcript.records[first].reason_live && !h->transcript.records[second].reason_live);
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    /* Measuring text must not include its position in the transcript. */
    { TranscriptRecord *turn=&h->transcript.records[first];
      int height=turn->head_h;
      CHECK(height<px(h,50));
      CHECK(turn->reason_y-turn->head_y-height==px(h,8));
      transcript_layout_from(&h->transcript,0,false);
      CHECK(turn->head_h==height);
      transcript_layout_from(&h->transcript,0,false);
      CHECK(turn->head_h==height); }
    click_row(h,second);
    CHECK(h->transcript.records[first].reason_live && h->transcript.records[second].reason_live);
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"Beta reasoning"));
    reasoning_text(h,first,text,128);
    CHECK(!wcscmp(text,L"Alpha reasoning"));
    click_row(h,first);
    CHECK(!h->transcript.records[first].reason_live && h->transcript.records[second].reason_live);
    /* Collapsed by default while waiting; explicit expansion streams live into
       that turn only and is not collapsed when the answer begins. */
    begin_regenerate(h);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_RUNNING);
    head_text(h,second,text,128);
    CHECK(wcsstr(text,L"Thinking")!=NULL);
    CHECK(!h->transcript.records[second].reason_live);
    click_row(h,second);
    CHECK(h->transcript.records[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"stream rea"));
    handle_event(h,fixture(h,COMPLETION_REASONING,L"soning"));
    reasoning_text(h,second,text,128);
    CHECK(!wcscmp(text,L"stream reasoning"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L"Streamed answer"));
    CHECK(h->transcript.records[second].reason_live &&
        chat->conversations[cv].messages[second].reasoning_open);
    CHECK(pending(h)->generation.reasoning_ms>=0);
    /* The running answer is never mixed with metadata, and no footer exists
       until the turn is terminal. */
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"Streamed answer")); }
    CHECK(!h->transcript.records[second].meta_live);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(h->transcript.records[second].meta_live);
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
    handle_event(h,fixture(h,COMPLETION_DELTA,L"Only answer"));
    CHECK(!row_present(h,second));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    /* Retry/regenerate replaces the turn and cannot leak reasoning or its
       expansion state. */
    wcscpy(chat->conversations[cv].messages[second].reasoning,L"leak?");
    chat->conversations[cv].messages[second].reasoning_open=true;
    chat_message_touch(&chat->conversations[cv].messages[second]);
    render_transcript(h);
    CHECK(h->transcript.records[second].reason_live);
    begin_regenerate(h);
    CHECK(chat->conversations[cv].messages[second].reasoning[0]==0);
    CHECK(!chat->conversations[cv].messages[second].reasoning_open);
    CHECK(!h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    /* Cancellation/error keeps that turn coherent: reasoning retained, row and
       expansion intact. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"kept"));
    click_row(h,second);
    CHECK(h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_ERROR,L"boom"));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    CHECK(!wcscmp(chat->conversations[cv].messages[second].reasoning,L"kept"));
    CHECK(row_present(h,second) &&
        chat->conversations[cv].messages[second].reasoning_open);
    /* Switching conversations restores each message's own reasoning. */
    wcscpy(chat->conversations[0].messages[1].reasoning,L"Conv zero");
    chat->conversations[0].messages[1].reasoning_open=false;
    chat_message_touch(&chat->conversations[0].messages[1]);
    command(h,CHAT_COMMAND_SELECT,0);
    CHECK(h->transcript.record_count==2);
    CHECK(row_present(h,1));
    click_row(h,1);
    { wchar_t text[128]; reasoning_text(h,1,text,128);
      CHECK(!wcscmp(text,L"Conv zero")); }
    command(h,CHAT_COMMAND_SELECT,cv);
    CHECK(h->transcript.record_count==4);
    { wchar_t text[128]; reasoning_text(h,second,text,128);
      CHECK(!wcscmp(text,L"kept")); }
    /* An answer past the old fixed-size limit streams to completion. */
    begin_regenerate(h);
    wchar_t *long_text=(wchar_t *)malloc(sizeof(wchar_t)*40001);
    CHECK(long_text);
    for (int i=0;i<40000;i++) long_text[i]=L'z';
    long_text[40000]=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,long_text));
    free(long_text);
    CHECK(h->accepting && pending(h)->generation.state==CHAT_GENERATION_RUNNING);
    CHECK(wcslen(chat_message_text(pending(h)))==40000);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
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
    handle_event(h,fixture(h,COMPLETION_REASONING,long_reasoning));
    free(long_reasoning);
    CHECK(h->accepting &&
        wcslen(chat_message_reasoning(pending(h)))==70000);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"Answer after long reasoning"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
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
    handle_event(h,fixture(h,COMPLETION_REASONING,L"Working through it"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L"Answer"));
    { TranscriptRecord *turn=&h->transcript.records[second];
      int head_height=turn->head_h;
      int reason_y=turn->reason_y;
      position_turns(h,true);
      for (int i=0;i<80;i++) {
          int height=turn->body_h, scroll=h->transcript.view_scroll;
          handle_event(h,fixture(h,COMPLETION_DELTA,L"\nAnother line of the streamed answer."));
          CHECK(turn->head_h==head_height && turn->reason_y==reason_y);
          CHECK(turn->body_h>=height && turn->body_h-height<px(h,40));
          CHECK(h->transcript.view_scroll>=scroll && transcript_pinned(&h->transcript));
          POINT origin={0,0};
          SendMessageW(body_window(h,second),EM_GETSCROLLPOS,0,(LPARAM)&origin);
          CHECK(origin.y==0);
      }
      /* The burst stayed within the throttle window, so nothing above was
         reparsed per token; flushing renders the accumulated burst in one
         step and the transcript still follows at the bottom. */
      int before_flush=turn->body_h;
      h->transcript.body_render_tick=0;
      handle_event(h,fixture(h,COMPLETION_DELTA,L"\nFlushed rebuild."));
      CHECK(turn->body_h>before_flush && transcript_pinned(&h->transcript));
       transcript_note_user_scroll(&h->transcript,px(h,30)); position_turns(h,false);
       CHECK(!transcript_pinned(&h->transcript));
      int scroll=h->transcript.view_scroll;
      for (int i=0;i<8;i++) {
          handle_event(h,fixture(h,COMPLETION_DELTA,L"\nMore text while reading above."));
          CHECK(h->transcript.view_scroll==scroll);
      }
    }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
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
    handle_event(h,fixture(h,COMPLETION_DELTA,L"# Tit"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Tit")); }
    h->transcript.body_render_tick=GetTickCount64();
    handle_event(h,fixture(h,COMPLETION_DELTA,L"le **bo"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L"ld** an"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L"d `co"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L"de` an"));
    CHECK(GetTickCount64()-h->transcript.body_render_tick<CHAT_BODY_RENDER_MS);
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Tit")); }
    Sleep(CHAT_BODY_RENDER_MS+20);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"d\nnext **ope"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext **ope"));
      CHARFORMAT2W f;
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* "bold" */
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,6,7);
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK((f.dwEffects & CFE_BOLD) && !(f.dwEffects & CFE_ITALIC));
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* "code" */
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,15,16);
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!wcscmp(f.szFaceName,L"Consolas") &&
          (f.dwMask & CFM_BACKCOLOR));
      memset(&f,0,sizeof f); f.cbSize=sizeof f;             /* plain tail */
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,26,27);
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!(f.dwEffects & CFE_BOLD) && !wcscmp(f.szFaceName,L"Segoe UI")); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **ope"));
    SendMessageW(body_window(h,stream_turn),EM_SETSEL,(WPARAM)-1,(LPARAM)-1);
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"n** ~~old and ``variable"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext open ~~old and ``variable")); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **open** ~~old and ``variable"));
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,
        L"``~~\n~~~~\n~~fenced~~\n- [x] task\n~~~"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext open old and variable\r\n~~fenced~~\r\n- [x] task\r\n~~~"));
      CHARFORMAT2W f;
      memset(&f,0,sizeof f); f.cbSize=sizeof f;
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,
          (WPARAM)wcslen(L"Title bold and code and\r\nnext open old and variable\r\n"),
          (LPARAM)(wcslen(L"Title bold and code and\r\nnext open old and variable\r\n")+1));
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!(f.dwEffects&CFE_STRIKEOUT) && !wcscmp(f.szFaceName,L"Consolas") &&
          (f.dwMask&CFM_BACKCOLOR)); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **open** ~~old and ``variable``~~\n~~~~\n~~fenced~~\n- [x] task\n~~~"));
    SendMessageW(body_window(h,stream_turn),EM_SETSEL,(WPARAM)-1,(LPARAM)-1);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"\n~~~~\nafter"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title bold and code and\r\nnext open old and variable\r\n~~fenced~~\r\n- [x] task\r\n~~~\r\nafter"));
      CHARFORMAT2W f;
      memset(&f,0,sizeof f); f.cbSize=sizeof f;
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,
          (WPARAM)wcslen(L"Title bold and code and\r\nnext open old and "),
          (LPARAM)(wcslen(L"Title bold and code and\r\nnext open old and ")+1));
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK((f.dwEffects&CFE_STRIKEOUT) && !wcscmp(f.szFaceName,L"Consolas") &&
          (f.dwMask&CFM_BACKCOLOR));
      memset(&f,0,sizeof f); f.cbSize=sizeof f;
      SendMessageW(body_window(h,stream_turn),EM_SETSEL,
          (WPARAM)wcslen(L"Title bold and code and\r\nnext open old and variable\r\n~~fenced~~\r\n- [x] task\r\n~~~\r\n"),
          (LPARAM)(wcslen(L"Title bold and code and\r\nnext open old and variable\r\n~~fenced~~\r\n- [x] task\r\n~~~\r\n")+1));
      SendMessageW(body_window(h,stream_turn),EM_GETCHARFORMAT,
          SCF_SELECTION,(LPARAM)&f);
      CHECK(!(f.dwEffects&(CFE_BOLD|CFE_ITALIC|CFE_STRIKEOUT)) &&
          !wcscmp(f.szFaceName,L"Segoe UI")); }
    /* ---- Nested Markdown streams the same way: an incomplete prefix stays
       literal, the accumulated message is reparsed at the throttle interval,
       and the terminal render carries the paragraph layout. ---- */
    add_turn(chat,L"seed",L"seed answer",NULL,-1);
    begin_regenerate(h);
    int nested_turn=h->request_message;
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"> -"));
    { wchar_t body[256]; body_text(h,nested_turn,body,256);
      CHECK(!wcscmp(body,L"\u258C -")); }        /* marker without content */
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L" item"));
    { wchar_t body[256]; body_text(h,nested_turn,body,256);
      CHECK(!wcscmp(body,L"\u258C \u2022 item")); }
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"\n>   - nested"));
    { wchar_t body[256]; body_text(h,nested_turn,body,256);
      CHECK(!wcscmp(body,L"\u258C \u2022 item\r\n\u258C \u2022 nested")); }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    { wchar_t body[256]; body_text(h,nested_turn,body,256);
      CHECK(!wcscmp(body,L"\u258C \u2022 item\r\n\u258C \u2022 nested"));
      HWND window=body_window(h,nested_turn);
      size_t second=wcslen(L"\u258C \u2022 item\r\n");
      PARAFORMAT2 pf;
      memset(&pf,0,sizeof pf); pf.cbSize=sizeof pf;
      SendMessageW(window,EM_SETSEL,(WPARAM)second,(LPARAM)(second+1));
      SendMessageW(window,EM_GETPARAFORMAT,0,(LPARAM)&pf);
      CHECK(pf.dxStartIndent>0);             /* the nested item is indented */
      memset(&pf,0,sizeof pf); pf.cbSize=sizeof pf;
      SendMessageW(window,EM_SETSEL,0,1);
      SendMessageW(window,EM_GETPARAFORMAT,0,(LPARAM)&pf);
      CHECK(pf.dxStartIndent==0 && pf.dxOffset>0); }   /* bars hang the text */
    /* ---- Link markdown streams through the same throttle: the fragmented
       destination reassembles at the render, the live body shows the label
       only, and the surface owns the link effect and destination. ---- */
    add_turn(chat,L"seed",L"seed answer",NULL,-1);
    begin_regenerate(h);
    int link_turn=h->request_message;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"[site](https://exa"));
    { wchar_t body[128]; body_text(h,link_turn,body,128);
      CHECK(!wcscmp(body,L"[site](https://exa")); }  /* incomplete: literal */
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"mple.com) tail"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    { wchar_t body[128]; body_text(h,link_turn,body,128);
      CHECK(!wcscmp(body,L"site tail")); }
    { RichTextControl *body=transcript_surface(&h->transcript,link_turn,
          TRANSCRIPT_BODY);
      CHECK(body && body->link_count==1);
      CHARFORMAT2W f; memset(&f,0,sizeof f); f.cbSize=sizeof f;
      SendMessageW(body->window,EM_SETSEL,0,1);
      SendMessageW(body->window,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&f);
      CHECK(f.dwEffects & CFE_LINK); }
    /* ---- Table markdown streams through the same throttle: the header alone
       stays literal until its delimiter row arrives, then the table flattens
       to tab-separated physical lines, and the terminal render completes it
       with the paragraph's tab stops. ---- */
    add_turn(chat,L"seed",L"seed answer",NULL,-1);
    begin_regenerate(h);
    int table_stream_turn=h->request_message;
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"| a | b |\n"));
    { wchar_t body[256]; body_text(h,table_stream_turn,body,256);
      CHECK(wcsstr(body,L"| a | b |")!=NULL); }   /* header alone: literal */
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"| --- | --- |\n"));
    { wchar_t body[256]; body_text(h,table_stream_turn,body,256);
      CHECK(wcsstr(body,L"|")==NULL && wcsstr(body,L"\t")!=NULL); }
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"| c | d |"));
    { wchar_t body[256]; body_text(h,table_stream_turn,body,256);
      CHECK(!wcscmp(body,L"a\tb\r\nc\td")); }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    { wchar_t body[256]; body_text(h,table_stream_turn,body,256);
      CHECK(!wcscmp(body,L"a\tb\r\nc\td"));
      HWND window=body_window(h,table_stream_turn);
      CHECK(window);
      PARAFORMAT2 pf; memset(&pf,0,sizeof pf); pf.cbSize=sizeof pf;
      SendMessageW(window,EM_SETSEL,0,1);
      SendMessageW(window,EM_GETPARAFORMAT,0,(LPARAM)&pf);
      CHECK(pf.cTabCount==2);
      CHECK(((pf.rgxTabs[1] >> 24) & 0xF)==MD_ALIGN_LEFT); }
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
      CHECK(h->transcript.records[second].meta_live);
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
    { HWND body=body_window(h,first);
      CHECK(body);
      SendMessageW(body,EM_SETSEL,2,6);
      render_transcript(h);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==6);
      begin_regenerate(h);
      handle_event(h,fixture(h,COMPLETION_DONE,NULL));
      CHECK(!h->generating);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==6);
      wchar_t kept[128]; body_text(h,first,kept,128);
      CHECK(wcsstr(kept,L"First answer")!=NULL); }
    /* Narrow updates: a metadata-only bump (completion writes the footer, not
       the answer) leaves the body current, so the body write is skipped, not
       deferred, and a selection placed in it stays put. */
    { ChatMessage *m=&chat->conversations[cv].messages[first];
      HWND body=body_window(h,first);
      TranscriptRecord *turn=&h->transcript.records[first];
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
    { HWND body=body_window(h,first);
      TranscriptRecord *turn=&h->transcript.records[first];
      SendMessageW(body,EM_SETSEL,0,0);
      render_transcript(h);
      CHECK(!turn->body_pending && !turn->head_pending);
      SendMessageW(body,EM_SETSEL,1,4);
      begin_regenerate(h);
      CHECK(!h->content_started);
      CHECK(!turn->body_pending && !turn->head_pending);
      wchar_t kept[128]; body_text(h,first,kept,128);
      CHECK(wcsstr(kept,L"First answer")!=NULL);
      handle_event(h,fixture(h,COMPLETION_DONE,NULL));
      SendMessageW(body,EM_SETSEL,0,0); }
    /* Streaming: a selection inside the live answer defers the throttled
       Markdown rebuild; clearing the selection applies the deferred render and
       relayouts from the affected turn, so geometry, the scrollbar range and
       bottom-following stay correct even with no further stream event. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"**raw *stream"));
    { HWND body=body_window(h,second);
      TranscriptRecord *turn=&h->transcript.records[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      h->transcript.body_render_tick=0;   /* force the throttled rebuild */
      handle_event(h,fixture(h,COMPLETION_DELTA,L"\nmore"));
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==3 && sel.cpMax==7);   /* selection survived the flush */
      wchar_t shown[256]; body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"**raw *stream")!=NULL);   /* not rebuilt */
      CHECK(wcsstr(shown,L"more")==NULL);            /* rebuild deferred */
      /* Clearing the selection applies the deferred Markdown rebuild and
         remeasures: the turn grows, the following surface and the scrollbar
         range move, and a pinned transcript still follows the bottom. */
       transcript_note_user_scroll(&h->transcript,0x7fffffff);
       transcript_position(&h->transcript,feed_arg(h),false);   /* pin to the bottom */
      int body_before=turn->body_h, height_before=turn->height;
      int content_before=h->transcript.view_content;
      SendMessageW(body,EM_SETSEL,0,0);
      CHECK(turn->body_h>body_before);
      CHECK(turn->height>height_before);
      CHECK(h->transcript.view_content>content_before);
      CHECK(transcript_pinned(&h->transcript));
      body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"more")!=NULL); }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* The same deferral persists through generation completion: the terminal
       Markdown render waits for the selection, and clearing afterwards still
       relayouts so the following metadata footer and scrollbar follow. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"**raw *stream"));
    { HWND body=body_window(h,second);
      TranscriptRecord *turn=&h->transcript.records[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      h->transcript.body_render_tick=0;
      handle_event(h,fixture(h,COMPLETION_DELTA,L"\nmore"));
      handle_event(h,fixture(h,COMPLETION_DONE,NULL));
      CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
      CHECK(h->transcript.records[second].meta_live);
      wchar_t shown[256]; body_text(h,second,shown,256);
      CHECK(wcsstr(shown,L"more")==NULL);   /* terminal render still deferred */
       transcript_note_user_scroll(&h->transcript,0x7fffffff);
       transcript_position(&h->transcript,feed_arg(h),false);
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
    CHECK(h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"alpha beta"));
    { HWND vp=transcript_surface(&h->transcript,second,TRANSCRIPT_REASON)->window;
      CHECK(vp);
      SendMessageW(vp,EM_SETSEL,2,5);
      handle_event(h,fixture(h,COMPLETION_REASONING,L" gamma"));
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(vp,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==2 && sel.cpMax==5);
      wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"alpha beta gamma")); }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* ---- Scheduled streaming flush ---- */
    /* A burst inside the throttle window marks the body dirty and arms a
       one-shot flush; with no further delta, the accumulated text still
       reaches the body once the timer fires. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"first token"));
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"first token")); }
    handle_event(h,fixture(h,COMPLETION_DELTA,L" second"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L" part"));
    CHECK(h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")==NULL); }        /* not rebuilt yet */
    CHECK(pump_until_body(h,second,L"first token second part",1000));
    CHECK(!h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(!wcscmp(body,L"first token second part")); }
    CHECK(transcript_pinned(&h->transcript));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* Scheduling cannot strand dirty text: when no flush can be armed
       (SetTimer failure) the body renders at once and the pending flag clears
       so later deltas can arm again. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"first token"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L" second"));
    CHECK(h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")==NULL); }
    { HWND window=h->window; h->window=(HWND)1;   /* invalid: SetTimer fails */
      schedule_body_flush(h);
      h->window=window; }
    CHECK(!h->body_flush_pending);
    { wchar_t body[256]; body_text(h,second,body,256);
      CHECK(wcsstr(body,L"second")!=NULL); }      /* rendered immediately */
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* A scheduled flush must not destroy a selection in the live body: the
       rebuild is deferred, then applied and relaid out when the range clears. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"streamed so far"));
    { HWND body=body_window(h,second);
      TranscriptRecord *turn=&h->transcript.records[second];
      CHECK(body);
      SendMessageW(body,EM_SETSEL,3,7);
      int body_before=turn->body_h;
      handle_event(h,fixture(h,COMPLETION_DELTA,L"\nplus more"));
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
       transcript_note_user_scroll(&h->transcript,0x7fffffff);
       transcript_position(&h->transcript,feed_arg(h),false);   /* pin to the bottom */
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
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* A scheduled flush writes the body outside prepare_turn(), so the
       recorded revision must advance with it. Completion then refreshes only
       the footer, leaving a selection in the live body untouched. */
    begin_regenerate(h);
    handle_event(h,fixture(h,COMPLETION_DELTA,L"first token"));
    { TranscriptRecord *turn=&h->transcript.records[second];
      ChatMessage *m=&chat->conversations[cv].messages[second];
      HWND body=body_window(h,second); CHECK(body);
      CHECK(turn->body_revision==m->body_revision);
      handle_event(h,fixture(h,COMPLETION_DELTA,L" second"));
      handle_event(h,fixture(h,COMPLETION_DELTA,L" part"));
      CHECK(h->body_flush_pending);
      CHECK(pump_until_body(h,second,L"first token second part",1000));
      CHECK(!h->body_flush_pending);
      CHECK(turn->body_revision==m->body_revision);
      SendMessageW(body,EM_SETSEL,1,4);
      handle_event(h,fixture(h,COMPLETION_DONE,NULL));
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
    CHECK(h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"first"));
    handle_event(h,fixture(h,COMPLETION_REASONING,L" second"));
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second")); }
    click_row(h,second);                     /* collapse mid-stream */
    CHECK(!h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_REASONING,L" hidden"));
    click_row(h,second);                     /* reopen */
    CHECK(h->transcript.records[second].reason_live);
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second hidden")); }
    handle_event(h,fixture(h,COMPLETION_REASONING,L" live"));
    { wchar_t shown[128]; reasoning_text(h,second,shown,128);
      CHECK(!wcscmp(shown,L"first second hidden live")); }
    handle_event(h,fixture(h,COMPLETION_DELTA,L"answer after reasoning"));
    CHECK(h->transcript.records[second].reason_live);  /* answer start keeps it */
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* ---- Long transcript: bounded renders and reader state ---- */
    /* A maximum-length transcript must not realize additional controls when it
       is rendered again or when one turn streams, and streaming that turn must
       not destructively rewrite an unchanged historical turn. Render cost is
       recorded, not asserted as a wall-clock threshold. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    { wchar_t answer[1024];
      for (int t=0;t<256;t++) {
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
    { HWND body=body_window(h,older);
      CHECK(body);
      SendMessageW(body,EM_SETSEL,1,5); }
    begin_regenerate(h);
    int long_turn=h->request_message;
    CHECK(child_controls(h->view,false)==realized);  /* streaming adds none */
    handle_event(h,fixture(h,COMPLETION_DELTA,L"long"));
    for (int i=0;i<40;i++)
        handle_event(h,fixture(h,COMPLETION_DELTA,L" burst line"));
    CHECK(h->body_flush_pending);
    CHECK(h->transcript.view_content>h->transcript.view_page);
     transcript_note_user_scroll(&h->transcript,0);
     transcript_position(&h->transcript,feed_arg(h),false);       /* reading an older turn */
    CHECK(!transcript_pinned(&h->transcript));
    int scroll=h->transcript.view_scroll;
    double flush_started=now_ms();
    CHECK(pump_until_body(h,long_turn,L"burst line burst line",1000));
    double flush_ms=now_ms()-flush_started;
    CHECK(!h->body_flush_pending);
    CHECK(h->transcript.view_scroll==scroll);        /* flush did not follow */
    CHECK(child_controls(h->view,false)==realized);
    handle_event(h,fixture(h,COMPLETION_DELTA,L" tail"));
    CHECK(h->transcript.view_scroll==scroll);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(h->transcript.view_scroll==scroll);        /* completion did not follow */
    /* The historical turn was neither rewritten nor deselected by the stream,
       the flush or the terminal render. */
    { HWND body=body_window(h,older);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==1 && sel.cpMax==5);
      wchar_t kept[512]; body_text(h,older,kept,512);
      CHECK(wcsstr(kept,L"Line 0 of answer 0")!=NULL); }
    printf("long transcript: %d controls (%d visible), full render %.1f ms, "
        "scheduled flush to visible %.1f ms\n",
        realized,visible,render_ms,flush_ms);
    /* ---- Send while FREE: no force-follow through the whole send path ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<12;t++) add_turn(chat,L"q",L"a",NULL,-1);
        render_transcript(h);
        /* Reading an older turn through the real user-scroll path: only a
            user-driven scroll may leave bottom-follow. */
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(!transcript_following(&h->transcript));
        TranscriptRecord *held=&h->transcript.records[0];
        uint64_t held_id=chat->conversations[chat->active].messages[0].id;
        int held_y=held->y, scroll=h->transcript.view_scroll;
        rich_text_set_text(&h->composer,L"send while reading");
        perform_send(h);
        CHECK(!h->generating);   /* no key: the request failed closed */
        CHECK(chat->conversations[chat->active].message_count==26);
        CHECK(!transcript_following(&h->transcript));
        /* Anchored content stationary: the turn above never moved and the
            reader's scroll was restored, not followed (view_scroll may
            change to keep anchored content still; here nothing above the
            anchor changed, so it must be exactly held). */
        CHECK(held->y==held_y && held->message==held_id);
        CHECK(h->transcript.view_scroll==scroll);
        CHECK(h->transcript.stat.anchor_rejected==0);
    }
    /* ---- Records, slots: rebind, debt survival, protection, P-CAP ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int ra=add_turn(chat,L"rebind question a",L"Alpha answer",NULL,-1);
        int rb=add_turn(chat,L"rebind question b",L"Beta answer",NULL,-1);
        render_transcript(h);
        Transcript *tr=&h->transcript;
        CHECK(tr->slot_capacity==CHAT_MAX_MESSAGES);      /* retain-all */
        CHECK(tr->records[ra].slot>=0 && tr->records[rb].slot>=0 &&
            tr->records[ra].slot!=tr->records[rb].slot);
        /* Retain-all P-CAP checkpoint: the required capacity is computed
           every render, recorded, and can never exceed the pool. */
        CHECK(tr->policy_needed>0 && tr->policy_needed<=tr->slot_capacity);
        /* An unfocused live selection is decision-time debt: protected. */
        CHECK(!transcript_record_debt(tr,ra));
        SendMessageW(body_window(h,ra),EM_SETSEL,0,4);
        CHECK(transcript_record_debt(tr,ra));
        CHECK(tr->policy_needed<=tr->slot_capacity);
        SendMessageW(body_window(h,ra),EM_SETSEL,0,0);
        CHECK(!transcript_record_debt(tr,ra));

        /* Debt survives an unrealized interval AND a fresh binding
           incarnation: defer a body rewrite behind a live selection, unbind
           the record, and the pending write must remain; rebinding assigns
           a new binding generation, so the acquired surfaces' selection is
           cleared and the preserved debt applies in the same pass. */
        SendMessageW(body_window(h,rb),EM_SETSEL,0,4);
        CHECK(chat_message_set_text(
            &chat->conversations[chat->active].messages[rb],
            L"Beta answer edited"));
        render_transcript(h);
        CHECK(tr->records[rb].body_pending);
        CHECK(tr->records[rb].body_revision!=
            chat->conversations[chat->active].messages[rb].body_revision);
        { int old=tr->records[rb].slot;
          tr->slots[old].record=-1;          /* dormant unrealized state */
          tr->records[rb].slot=-1; }
        CHECK(transcript_surface(tr,rb,TRANSCRIPT_BODY)==NULL);
        { TranscriptFeed feed=transcript_feed(h);
          transcript_apply_pending(tr,&feed); }   /* must not clear debt */
        CHECK(tr->records[rb].body_pending);
        render_transcript(h);   /* rebind: fresh generation, same slot number */
        CHECK(tr->records[rb].slot>=0);
        CHECK(tr->records[rb].rendered_generation==
            tr->slots[tr->records[rb].slot].generation);
        CHECK(!tr->records[rb].body_pending);  /* debt applied to the new
                                                  incarnation */
        CHECK(tr->records[rb].body_revision==
            chat->conversations[chat->active].messages[rb].body_revision);
        { wchar_t shown[256]; body_text(h,rb,shown,256);
          CHECK(wcsstr(shown,L"Beta answer edited")!=NULL); }
        CHECK(tr->policy_needed<=tr->slot_capacity);

        /* A debt-bearing record rebinding to a DIFFERENT slot preserves and
           applies its debt on the newly bound surfaces. The pool here is
           fully bound (the long transcript's retain-all bindings persist
           across conversation switches), so the rebind target is another
           live record's slot, exchanged so the bijection survives; each
           acquired association is fresh, as ensure_slot would make it. */
        {
            int rc=add_turn(chat,L"debt question",L"Delta answer",NULL,-1);
            render_transcript(h);
            int old=tr->records[rc].slot;
            int neighbor=rc-1, other=tr->records[neighbor].slot;
            CHECK(old>=0 && other>=0 && other!=old);
            SendMessageW(body_window(h,rc),EM_SETSEL,0,4);
            CHECK(chat_message_set_text(
                &chat->conversations[chat->active].messages[rc],
                L"Delta answer edited"));
            render_transcript(h);
            CHECK(tr->records[rc].body_pending);
            tr->slots[other].record=rc; tr->records[rc].slot=other;
            tr->slots[other].generation=++tr->clock;
            tr->slots[old].record=neighbor; tr->records[neighbor].slot=old;
            tr->slots[old].generation=++tr->clock;
            render_transcript(h);
            CHECK(tr->records[rc].slot==other);
            CHECK(!tr->records[rc].body_pending);
            CHECK(tr->records[rc].body_revision==
                chat->conversations[chat->active].messages[rc].body_revision);
            { wchar_t shown[256]; body_text(h,rc,shown,256);
              CHECK(wcsstr(shown,L"Delta answer edited")!=NULL); }
        }

        /* A record rebound onto surfaces that rendered another record must
           never accept its cached identity as fresh: the slot's stale
           selection is cleared first and every applicable surface is
           completely rewritten. */
        { int sa=tr->records[ra].slot, sb=tr->records[rb].slot;
          CHECK(sa>=0 && sb>=0 && sa!=sb);
          SendMessageW(body_window(h,rb),EM_SETSEL,1,5);
          /* Exchange the bindings, preserving the bijection; each acquired
             association is fresh, as ensure_slot would make it. Record ra
             now points at surfaces still holding Beta's content. */
          tr->records[ra].slot=sb; tr->slots[sb].record=ra;
          tr->slots[sb].generation=++tr->clock;
          tr->records[rb].slot=sa; tr->slots[sa].record=rb;
          tr->slots[sa].generation=++tr->clock;
          wchar_t stale[256]; body_text(h,ra,stale,256);
          CHECK(wcsstr(stale,L"Beta answer")!=NULL);   /* the hazard */
          render_transcript(h);
          wchar_t shown[256]; body_text(h,ra,shown,256);
          CHECK(wcsstr(shown,L"Alpha answer")!=NULL);
          CHECK(wcsstr(shown,L"Beta answer")==NULL);
          CHARRANGE sel; memset(&sel,0,sizeof sel);
          SendMessageW(body_window(h,ra),EM_EXGETSEL,0,(LPARAM)&sel);
          /* The stale selection is gone: the range is collapsed (the
             rewrite leaves the caret at the text end, never a live range). */
          CHECK(sel.cpMin==sel.cpMax);
          CHECK(!rich_text_has_selection(
              transcript_surface(tr,ra,TRANSCRIPT_BODY)));
          body_text(h,rb,shown,256);
          CHECK(wcsstr(shown,L"Beta answer edited")!=NULL); }

        /* catch_up refuses stale conversation/message identity: debt that
           outlives its message is dropped, the selection is cleared, the
           cached identity invalidated, and the old content is not
           rewritten; the next render performs the replacement. */
        SendMessageW(body_window(h,rb),EM_SETSEL,0,4);
        CHECK(chat_message_set_text(
            &chat->conversations[chat->active].messages[rb],
            L"Beta rewritten"));
        render_transcript(h);
        CHECK(tr->records[rb].body_pending);
        tr->records[rb].message+=1;          /* simulated identity drift */
        { TranscriptFeed feed=transcript_feed(h);
          transcript_apply_pending(tr,&feed); }
        CHECK(!tr->records[rb].body_pending);
        CHECK(!tr->records[rb].rendered_valid);
        { CHARRANGE sel; memset(&sel,0,sizeof sel);
          SendMessageW(body_window(h,rb),EM_EXGETSEL,0,(LPARAM)&sel);
          CHECK(sel.cpMin==0 && sel.cpMax==0); }
        { wchar_t shown[256]; body_text(h,rb,shown,256);
          CHECK(wcsstr(shown,L"Beta answer")!=NULL);
          CHECK(wcsstr(shown,L"Beta rewritten")==NULL); }
        render_transcript(h);
        CHECK(tr->records[rb].rendered_valid);
        CHECK(tr->records[rb].message==
            chat->conversations[chat->active].messages[rb].id);
        { wchar_t shown[256]; body_text(h,rb,shown,256);
          CHECK(wcsstr(shown,L"Beta rewritten")!=NULL); }
        CHECK(tr->records[rb].body_revision==
            chat->conversations[chat->active].messages[rb].body_revision);
        CHECK(tr->policy_needed<=tr->slot_capacity);
    }
    /* ---- Binding generations: ABA slot-number reuse ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int ra=add_turn(chat,L"aba question a",L"Alpha answer",NULL,-1);
        int rb=add_turn(chat,L"aba question b",L"Beta answer",NULL,-1);
        render_transcript(h);
        Transcript *tr=&h->transcript;
        int sa=tr->records[ra].slot, sb=tr->records[rb].slot;
        CHECK(sa>=0 && sb>=0 && sa!=sb);
        CHECK(tr->records[ra].rendered_slot==sa);
        CHECK(tr->records[ra].rendered_generation==
            tr->slots[sa].generation);
        /* A carries a deferred body write, then unbinds; its cached
           certification still names slot sa with its old generation. */
        SendMessageW(body_window(h,ra),EM_SETSEL,0,4);
        CHECK(chat_message_set_text(
            &chat->conversations[chat->active].messages[ra],
            L"Alpha answer edited"));
        render_transcript(h);
        CHECK(tr->records[ra].body_pending);
        tr->slots[sa].record=-1; tr->records[ra].slot=-1;
        CHECK(tr->records[ra].rendered_slot==sa);   /* cache untouched */
        /* B takes the SAME numerical slot as a new binding association (a
           fresh generation, exactly what ensure_slot assigns) and rewrites
           the surfaces. Only B prepares, so A's cache survives. */
        tr->slots[sb].record=-1; tr->records[rb].slot=-1;
        tr->slots[sa].record=rb; tr->records[rb].slot=sa;
        tr->slots[sa].generation=++tr->clock;
        CHECK(tr->slots[sa].generation!=
            tr->records[ra].rendered_generation);
        refresh_turn(h,rb);
        { RichTextControl *body=transcript_surface(tr,rb,TRANSCRIPT_BODY);
          CHECK(body);
          wchar_t shown[256]; rich_text_get_text(body,shown,256);
          CHECK(wcsstr(shown,L"Beta answer")!=NULL); }   /* B rewrote it */
        /* A returns to the SAME numerical slot while still unprepared: the
           slot number agrees with A's rendered_slot and only the generation
           differs -- a slot-number-only check would wrongly certify. */
        tr->slots[sa].record=-1; tr->records[rb].slot=-1;
        tr->slots[sa].record=ra; tr->records[ra].slot=sa;
        CHECK(tr->records[ra].rendered_slot==sa);
        CHECK(tr->records[ra].rendered_generation!=
            tr->slots[sa].generation);
        /* catch_up: matching message identity, stale binding generation --
           it must touch no surface and preserve the debt for prepare. */
        { TranscriptFeed feed=transcript_feed(h);
          transcript_apply_pending(tr,&feed); }
        CHECK(tr->records[ra].body_pending);
        { RichTextControl *body=transcript_surface(tr,ra,TRANSCRIPT_BODY);
          CHECK(body);
          wchar_t shown[256]; rich_text_get_text(body,shown,256);
          CHECK(wcsstr(shown,L"Beta answer")!=NULL);      /* untouched */
          CHECK(wcsstr(shown,L"Alpha answer edited")==NULL); }
        /* The render rebinds the incarnation: complete rewrite of B's
           surfaces, and the preserved debt applies. */
        render_transcript(h);
        { wchar_t shown[256]; body_text(h,ra,shown,256);
          CHECK(wcsstr(shown,L"Alpha answer edited")!=NULL);
          CHECK(wcsstr(shown,L"Beta answer")==NULL); }
        CHECK(!tr->records[ra].body_pending);
        CHECK(tr->records[ra].body_revision==
            chat->conversations[chat->active].messages[ra].body_revision);
        CHECK(tr->records[ra].rendered_generation==
            tr->slots[sa].generation);
        CHECK(tr->policy_needed<=tr->slot_capacity);
    }
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
    transcript_note_user_scroll(&h->transcript,0);
    transcript_position(&h->transcript,feed_arg(h),false);
    CHECK(h->search.window &&
        (GetWindowLongPtrW(h->search.window,GWL_STYLE)&WS_VISIBLE));
    rich_text_set_text(&h->search,L"HOST-search-token");
    CHECK(search_submit(h));
    CHECK(h->search_results.count==2 && h->search_has_selection &&
        h->search_selected==0);
    CHECK(chat->conversations[chat->active].id==search_a_id &&
        h->transcript.records[search_body].message==search_body_id);
    { TranscriptRecord *turn=&h->transcript.records[search_body];
      int before=h->transcript.view_scroll;   /* the switch's BOTTOM landing */
      int page=h->transcript.view_page;
      /* The reveal scrolls only enough to make the target visible. From
         the BOTTOM landing (no saved anchor for a fresh conversation) a
         short conversation's target is already visible, so the position
         is held; otherwise it is bottom-aligned at the viewport bottom
         (or top-aligned when taller than the viewport). */
      bool already_visible=turn->y+turn->height>before && turn->y<before+page;
      int expected=turn->height>page ? turn->y :
          turn->y+turn->height-page;
      if (expected<0) expected=0;
      CHECK(h->transcript.view_scroll==(already_visible?before:expected));
      CHECK(turn->y+turn->height>h->transcript.view_scroll &&
          turn->y<h->transcript.view_scroll+page);
      int revealed=h->transcript.view_scroll;
      CHECK(transcript_reveal_turn(&h->transcript,feed_arg(h),search_body) &&
          h->transcript.view_scroll==revealed); }
    /* Jumping again to an active body only reveals it. A deliberately stale
       unrelated rendered identity proves no full transcript pass occurred. */
    { bool valid=h->transcript.records[0].rendered_valid;
      h->transcript.records[0].rendered_valid=false;
      CHECK(jump_search_result(h,0));
      CHECK(!h->transcript.records[0].rendered_valid);
      h->transcript.records[0].rendered_valid=valid; }
    CHECK(wcsstr(ui_node(ui,h->chat_ui.search_status)->text,
        L"Assistant message")!=NULL);
    /* F3 resolves the next result by stable ids, switches conversations, opens
       a matched reasoning viewport, and minimally reveals the target turn. */
    CHECK(surface_key(h,VK_F3,false,false,true));
    CHECK(h->search_selected==1 &&
        chat->conversations[chat->active].id==search_b_id &&
        h->transcript.records[search_reason].message==search_reason_id &&
        chat->conversations[chat->active].messages[search_reason].reasoning_open &&
        h->transcript.records[search_reason].reason_live);
    { TranscriptRecord *turn=&h->transcript.records[search_reason];
      CHECK(turn->y+turn->height>h->transcript.view_scroll &&
          turn->y<h->transcript.view_scroll+h->transcript.view_page); }
    /* Opening reasoning in the active conversation refreshes only its turn. */
    chat->conversations[search_b].messages[search_reason].reasoning_open=false;
    refresh_turn(h,search_reason);
    { bool valid=h->transcript.records[0].rendered_valid;
      h->transcript.records[0].rendered_valid=false;
      CHECK(jump_search_result(h,1));
      CHECK(h->transcript.records[search_reason].reason_live &&
          !h->transcript.records[0].rendered_valid);
      h->transcript.records[0].rendered_valid=valid; }
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
    CHECK(h->transcript.records[1].reason_live);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"A live"));
    command(h,CHAT_COMMAND_SELECT,conv_b);
    CHECK(chat->active==conv_b);
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"B reasoning")); }
    /* A keeps streaming while hidden; the reply stays with its origin. */
    handle_event(h,fixture(h,COMPLETION_REASONING,L" more"));
    CHECK(!wcscmp(chat_message_reasoning(
        &chat->conversations[conv_a].messages[1]),L"A live more"));
    command(h,CHAT_COMMAND_SELECT,conv_a);
    CHECK(h->generating && h->request_conversation==conv_a);
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"A live more")); }
    handle_event(h,fixture(h,COMPLETION_REASONING,L" end"));
    { wchar_t shown[128]; reasoning_text(h,1,shown,128);
      CHECK(!wcscmp(shown,L"A live more end")); }
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    /* A selection must never carry across conversations: switching while text
       is selected forces immediate replacement with the other conversation's
       own content. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int cvb=chat->active;
    add_turn(chat,L"prompt",L"Distinct answer",NULL,-1);
    render_transcript(h);
    CHECK(h->transcript.record_count==2);
    SendMessageW(body_window(h,1),EM_SETSEL,0,4);
    command(h,CHAT_COMMAND_SELECT,0);
    CHECK(h->transcript.record_count==2);
    { CHARRANGE sel; memset(&sel,0,sizeof sel);
      HWND body=body_window(h,1);
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
        int calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"gate question");
        perform_send(h);
        CHECK(completion_request_calls==calls);          /* request not sent */
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
        float scroll = ui_scroll_offset(ui, h->chat_ui.list);
        CHECK(probe_index * pitch >= scroll &&
            row_bottom > scroll + viewport);   /* partly visible only */
        wcscpy(chat->conversations[probe_index].title, L"Merge probe");
        chat_ui_request_reveal(&h->chat_ui,
            chat->conversations[probe_index].id);
        flush(h);
        pump_messages(30);
        float revealed = ui_scroll_offset(ui, h->chat_ui.list);
        CHECK(fabsf(revealed - (row_bottom - viewport)) < .05f);
        /* Bottom-aligned: the probe row is now fully visible. The reveal may
           move the window by a whole row, which is exactly the state the
           second remap of the flush must fold into the accumulated report. */
        CHECK(revealed <= probe_index * pitch + .05f &&
            row_bottom <= revealed + viewport + .05f);
        bool merged = false;
        for (int i = 0; i < h->remap.name_changed_count; i++)
            if (h->remap.name_changed[i].id == probe_row &&
                !wcscmp(h->remap.name_changed[i].old_title, probe_old))
                merged = true;
        CHECK(merged);
        /* Identity-based binding: whichever pool row now carries the probe
           conversation shows the renamed title. */
        bool shows_name = false;
        for (int j = 0; j < h->chat_ui.pool_count; j++)
            if ((uint64_t)ui_node(ui, h->chat_ui.rows[j])->tag ==
                    chat->conversations[probe_index].id &&
                !wcscmp(ui_node(ui, h->chat_ui.rows[j])->text,
                    L"Merge probe"))
                shows_name = true;
        CHECK(shows_name);
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
    /* The native Tab cycle excludes a collapsed sidebar's hidden search edit,
       and a search command reveals the sidebar (restoring the persisted
       preference) before the edit is placed and focused. */
    {
        if (h->chat_ui.sidebar_open) {
            CHECK(chat_ui_toggle_sidebar(&h->chat_ui));
            flush(h);
        }
        CHECK(!h->chat_ui.sidebar_open && chat->sidebar_collapsed == 1);
        HWND order[3];
        int count = native_focus_order(h, order, 3);
        CHECK(count == 2 && order[0] == h->field.window &&
            order[1] == h->composer.window);
        action(h, ACTION_SEARCH);
        pump_messages(10);
        CHECK(h->chat_ui.sidebar_open && chat->sidebar_collapsed == 0);
        count = native_focus_order(h, order, 3);
        CHECK(count == 3 && order[1] == h->search.window);
    }
    /* F1: retain-all never enters bounded realization or creates its measurer. */
    CHECK(!h->transcript.bounded && !h->transcript.measurer_valid);
    CHECK(h->transcript.render_epoch==0 && h->transcript.stat.fallback_rounds==0);
    begin_fixture(h); handle_event(h,fixture(h,COMPLETION_DELTA,L"Closing partial"));
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
    DeleteObject(h->background);
    /* Slot-pool lifetime order: the container and EVERY child surface are
       already destroyed (WM_CLOSE completed full teardown above), so each
       surface's GWLP_USERDATA is unreachable before the pool is freed. The
       dispose call lives in this ownership layer, never in the window
       procedure. */
    for (int s = 0; s < h->transcript.slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            CHECK(!IsWindow(h->transcript.slots[s].surface[k].window));
    CHECK(!IsWindow(window) && !IsWindow(h->view));
    transcript_dispose(&h->transcript);
    CHECK(!h->transcript.slots && !h->transcript.slot_capacity);
    rich_text_library_close();
    chat_dispose(loaded); chat_dispose(chat);
    free(loaded); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Bounded-mode suite (separate clean fixture) -------------------------- */

/* Registers a window class that may already exist (the default suite above
    registered the same names). */
static bool register_class_once(const WNDCLASSW *cls) {
    if (RegisterClassW(cls)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/* F2 is intentionally independent from the pristine bounded fixture below:
   activating bounded mode after retain-all must not contaminate first-arena
   measurements used by the R-suite. */
static int seam_toggle_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Seam host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-seam-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Seam integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    for (int t=0;t<20;t++) add_turn(chat,L"seam question",
        L"seam answer\nline two\nline three\nline four\nline five\n"
        L"line six\nline seven\nline eight",NULL,-1);
    render_transcript(h);
    CHECK(!h->transcript.bounded && !h->transcript.measurer_valid);
    CHECK(h->transcript.render_epoch==0 && transcript_bound_slots(&h->transcript)==40);
    { wchar_t shown[128]; body_text(h,1,shown,128); CHECK(wcsstr(shown,L"seam answer")); }
    SetWindowPos(h->view,NULL,0,0,1100,120,
        SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
    transcript_set_bounded(&h->transcript,true);
    render_transcript(h);
    CHECK(transcript_bound_slots(&h->transcript)<40);
    CHECK(visible_realized(h));
    { wchar_t shown[128]; body_text(h,39,shown,128); CHECK(wcsstr(shown,L"seam answer")); }
    int activated_bound=transcript_bound_slots(&h->transcript);
    int activated_binds=h->transcript.stat.binds;
    int activated_evictions=h->transcript.stat.evictions;
    render_transcript(h);
    CHECK(transcript_bound_slots(&h->transcript)==activated_bound);
    CHECK(h->transcript.stat.binds==activated_binds &&
        h->transcript.stat.evictions==activated_evictions);
    transcript_set_bounded(&h->transcript,false);
    render_transcript(h);
    CHECK(transcript_bound_slots(&h->transcript)==40 && visible_realized(h));
    { wchar_t shown[128]; body_text(h,1,shown,128); CHECK(wcsstr(shown,L"seam answer")); }
    SendMessageW(window,WM_CLOSE,0,0); CHECK(!IsWindow(window));
    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript); rich_text_library_close();
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Model catalog / picker suite (separate clean fixture) ---------------- */

static const char *catalog_first_json =
    "{\"data\":["
    "{\"id\":\"openai/gpt-4\",\"name\":\"GPT-4\"},"
    "{\"id\":\"anthropic/claude-3\",\"name\":\"Claude 3\"},"
    "{\"id\":\"meta-llama/llama-3\",\"name\":\"Llama 3\"}]}";
static const char *catalog_second_json =
    "{\"data\":["
    "{\"id\":\"openai/gpt-4\",\"name\":\"GPT-4\"},"
    "{\"id\":\"google/gemini-2\",\"name\":\"Gemini 2\"},"
    "{\"id\":\"anthropic/claude-3\",\"name\":\"Claude 3\"}]}";
static const char *catalog_five_json =
    "{\"data\":["
    "{\"id\":\"openai/gpt-4\",\"name\":\"GPT-4\"},"
    "{\"id\":\"anthropic/claude-3\",\"name\":\"Claude 3\"},"
    "{\"id\":\"meta-llama/llama-3\",\"name\":\"Llama 3\"},"
    "{\"id\":\"google/gemini-2\",\"name\":\"Gemini 2\"},"
    "{\"id\":\"mistral/mistral-large\",\"name\":\"Mistral Large\"}]}";
/* Ollama's OpenAI-compatible /v1/models shape: an object list whose data
   entries carry an id and no display name. */
static const char *catalog_ollama_json =
    "{\"object\":\"list\",\"data\":["
    "{\"id\":\"llama3.2:latest\",\"object\":\"model\",\"owned_by\":\"library\"},"
    "{\"id\":\"qwen2.5:7b\",\"object\":\"model\",\"owned_by\":\"library\"}]}";

static int catalog_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Catalog host",1100,720,720,480,"test-key",false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-catalog-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Catalog integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    catalog_request_calls=0; catalog_request_generation=0; picker_pump_calls=0;

    /* apply_model: rejects empty/over-capacity ids, updates Chat and the field
       together, and dirties only on a real change. It never touches history. */
    h->dirty=false;
    CHECK(!apply_model(h,chat->model));
    CHECK(!h->dirty);
    CHECK(apply_model(h,L"custom/model"));
    CHECK(h->dirty && !wcscmp(chat->model,L"custom/model"));
    { wchar_t shown[CHAT_MODEL_TEXT]; rich_text_get_text(&h->field,shown,CHAT_MODEL_TEXT);
      CHECK(!wcscmp(shown,L"custom/model")); }
    CHECK(!apply_model(h,L""));
    { wchar_t long_id[200]; for (int i=0;i<150;i++) long_id[i]=L'a'; long_id[150]=0;
      CHECK(!apply_model(h,long_id)); }
    CHECK(apply_model(h,L"openai/gpt-4o-mini"));
    CHECK(!chat->model_history_count);

    /* One Ctrl+Space open starts exactly one fetch; a browse applies nothing. */
    catalog_request_calls=0; picker_pump_calls=0;
    action(h,ACTION_MODELS);
    CHECK(picker_pump_calls==1 && catalog_request_calls==1);
    CHECK(h->catalog_loading && h->catalog_generation>0 && !h->open_picker);
    CHECK(!wcscmp(chat->model,L"openai/gpt-4o-mini"));
    CHECK(!h->model_applied);

    /* Success replaces catalog state and clears loading. */
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
    CHECK(!h->catalog_loading && h->catalog_loaded[CHAT_BACKEND_OPENROUTER] && h->catalog[CHAT_BACKEND_OPENROUTER].count==3);
    CHECK(h->catalog_generation==0);

    /* A live picker gets the merged list; a refresh while open preserves the
       filter text and the selected id. The current model is one of the catalog
       entries, so it deduplicates instead of adding a fourth row. */
    CHECK(apply_model(h,L"openai/gpt-4"));
    begin_model_picker(h);
    CHECK(h->open_picker && catalog_request_calls==1);
    CHECK(model_picker_match_count(h->open_picker)==3);
    model_picker_set_filter(h->open_picker,L"gpt");
    CHECK(model_picker_match_count(h->open_picker)==1);
    CHECK(!wcscmp(model_picker_match_id(h->open_picker,0),L"openai/gpt-4"));
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_second_json,NULL));
    CHECK(h->open_picker);
    CHECK(!wcscmp(model_picker_filter(h->open_picker),L"gpt"));
    CHECK(model_picker_match_count(h->open_picker)==1);
    model_picker_set_filter(h->open_picker,L"");
    model_picker_set_selected(h->open_picker,L"openai/gpt-4");
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
    CHECK(!wcscmp(model_picker_selected_id(h->open_picker),L"openai/gpt-4"));

    /* A source refresh is transactional across the snapshot and the filter
       array: an allocation failure at either step keeps the old source and a
       fully usable old list, with no out-of-capacity pointer. */
    {
        ChatModelCatalog big;
        chat_model_catalog_init(&big);
        ChatModelParseStats stats;
        CHECK(chat_model_catalog_parse(&big,catalog_five_json,&stats));
        size_t before=model_picker_match_count(h->open_picker);
        CHECK(before==3);
        alloc_fail_realloc=0;
        model_picker_source_updated(h->open_picker,&big,L"oom");
        alloc_fail_realloc=-1;
        CHECK(model_picker_match_count(h->open_picker)==before);
        CHECK(model_picker_match_id(h->open_picker,before-1)!=NULL);
        CHECK(model_picker_match_id(h->open_picker,99)==NULL);
        alloc_fail_malloc=0;
        model_picker_source_updated(h->open_picker,&big,L"oom");
        alloc_fail_malloc=-1;
        CHECK(model_picker_match_count(h->open_picker)==before);
        CHECK(model_picker_match_id(h->open_picker,before-1)!=NULL);
        chat_model_catalog_dispose(&big);
    }

    /* Accepting applies the id to both values and marks dirty once. */
    model_picker_set_selected(h->open_picker,L"anthropic/claude-3");
    h->dirty=false;
    model_picker_accept(h->open_picker);
    end_model_picker(h);
    CHECK(!h->open_picker && h->model_applied && h->dirty);
    CHECK(!wcscmp(chat->model,L"anthropic/claude-3"));
    { wchar_t shown[CHAT_MODEL_TEXT]; rich_text_get_text(&h->field,shown,CHAT_MODEL_TEXT);
      CHECK(!wcscmp(shown,L"anthropic/claude-3")); }

    /* Cancelling applies nothing. */
    begin_model_picker(h);
    CHECK(h->open_picker && catalog_request_calls==1);
    model_picker_cancel(h->open_picker);
    h->dirty=false;
    end_model_picker(h);
    CHECK(!h->model_applied && !h->dirty);
    CHECK(!wcscmp(chat->model,L"anthropic/claude-3"));

    /* A stale generation is ignored and cannot replace the catalog. */
    {
        size_t before=h->catalog[CHAT_BACKEND_OPENROUTER].count;
        ModelCatalogEvent *stale=catalog_fixture(h,MODEL_CATALOG_OK,
            catalog_second_json,NULL);
        stale->generation=h->catalog_generation+77;
        catalog_event(h,stale);
        CHECK(h->catalog[CHAT_BACKEND_OPENROUTER].count==before);
    }

    /* Without a catalog, a failed refresh keeps the history fallback, and a
       later open retries the fetch. */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]);
    h->catalog_loaded[CHAT_BACKEND_OPENROUTER]=false; h->catalog_failed[CHAT_BACKEND_OPENROUTER]=false;
    h->catalog_loading=false; h->catalog_generation=0;
    wcscpy(chat->model_history[0],L"history/model"); chat->model_history_count=1;
    CHECK(apply_model(h,L"typed/model"));
    catalog_request_calls=0;
    begin_model_picker(h);
    CHECK(catalog_request_calls==1 && h->open_picker);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_NETWORK_ERROR,NULL,L"offline"));
    CHECK(h->catalog_failed[CHAT_BACKEND_OPENROUTER] && !h->catalog_loading && h->catalog[CHAT_BACKEND_OPENROUTER].count==0);
    CHECK(h->open_picker && model_picker_match_count(h->open_picker)>=2);
    CHECK(!wcscmp(model_picker_match_id(h->open_picker,0),L"typed/model"));
    model_picker_cancel(h->open_picker);
    end_model_picker(h);
    catalog_request_calls=0;
    begin_model_picker(h);
    CHECK(catalog_request_calls==1 && h->open_picker);
    model_picker_cancel(h->open_picker);
    end_model_picker(h);

    /* A worker whose completion was lost (the event could not be allocated or
       posted) must be reaped so the next open starts a fresh request, without
       any manual model_catalog_complete. The wrapper leaves an already-finished
       thread handle, exactly as that worker does. */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]);
    h->catalog_loaded[CHAT_BACKEND_OPENROUTER]=false; h->catalog_failed[CHAT_BACKEND_OPENROUTER]=false;
    h->catalog_loading=false; h->catalog_generation=0;
    catalog_request_lost_worker=true;
    catalog_request_calls=0;
    begin_model_picker(h);
    CHECK(catalog_request_calls==1 && h->catalog_loading && h->open_picker);
    CHECK(h->catalog_client.thread!=NULL);   /* lost completion still held */
    model_picker_cancel(h->open_picker);
    end_model_picker(h);
    begin_model_picker(h);
    CHECK(catalog_request_calls==2 && h->catalog_loading && h->open_picker);
    CHECK(h->catalog_client.thread!=NULL);   /* request #2's worker is unjoined */
    model_picker_cancel(h->open_picker);
    end_model_picker(h);
    catalog_request_lost_worker=false;

    /* A close arriving while the picker is live is deferred until the picker
       has unwound, so the parent is never destroyed under the modal loop. */
    begin_model_picker(h);
    CHECK(h->open_picker);
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(IsWindow(window) && h->close_pending && !h->generating);
    end_model_picker(h);
    pump_messages(30);
    CHECK(!IsWindow(window));
    CHECK(!h->open_picker && !h->close_pending);

    saver_shutdown(&h->saver); storage_close(&h->storage);
    model_catalog_shutdown(&h->catalog_client);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript); rich_text_library_close();
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]); chat_model_catalog_dispose(&h->picker_source);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

static int bounded_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Bounded host",1100,720,720,480,NULL,
        true};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-bounded-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Bounded integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    /* The seam: bounded realization active for this whole fixture through
        the init-time config field, exactly as production activates it. */
    CHECK(h->transcript.bounded && !h->transcript.measurer_valid);
    CHECK(transcript_created_windows(&h->transcript) == 0);

    /* ---- F3: pristine bounded first-render counters ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int f3_conv=chat->active;
    for (int t=0;t<40;t++) add_turn(chat,L"question",L"answer text",NULL,-1);
    render_transcript(h);
    CHECK(chat->conversations[f3_conv].message_count==80);
    int f3_bound=transcript_bound_slots(&h->transcript);
    int f3_created=transcript_created_windows(&h->transcript);
    CHECK(f3_bound>0 && f3_bound<80);            /* window-shaped, not all */
    CHECK(f3_created<=4*f3_bound+1);             /* arena within bound slots */
    /* The governed limit is positive, within the arena, and was raised. */
    CHECK(h->transcript.slot_limit>0 && h->transcript.slot_limit<=512);
    CHECK(h->transcript.stat.capacity_raises>0);
    CHECK(f3_created<=4*h->transcript.slot_limit+1);
    CHECK(transcript_created_windows(&h->transcript)==child_controls(h->view,false));
    CHECK(visible_realized(h));
    CHECK(h->transcript.measurer_valid);         /* lazy creation succeeded */
    CHECK(h->transcript.stat.rounds<=2*80+4);
    /* Every stamp EXACT after the first-layout sweep; off-screen heights
       equal the live measure (equality asserted where both are EXACT). */
    for (int i=0;i<80;i++)
        CHECK(h->transcript.records[i].measured_valid &&
            !h->transcript.records[i].measured_estimated);
    int exact_after_first=h->transcript.stat.exact_measures;
    render_transcript(h);
    CHECK(h->transcript.stat.exact_measures==exact_after_first);

    /* ---- R1: A-short -> B-long replacement on already-bound slots ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int conv_a=chat->active;
    add_turn(chat,L"a one",L"A answer one",NULL,-1);
    add_turn(chat,L"a two",L"A answer two",NULL,-1);
    render_transcript(h);
    int created_before_r1=transcript_created_windows(&h->transcript);
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int conv_b=chat->active;
    for (int t=0;t<256;t++) {
        wchar_t answer[128];
        swprintf(answer,128,L"B answer %d unique-token-%d",t,t);
        add_turn(chat,L"B question",answer,NULL,-1);
    }
    CHECK(chat->conversations[conv_b].message_count==512);
    render_transcript(h);
    CHECK(visible_realized(h));
    { wchar_t shown[256]; body_text(h,511,shown,256);
      CHECK(wcsstr(shown,L"B answer 255")!=NULL); }   /* replacement, not A */
    for (int i=0;i<h->transcript.record_count;i++) {
        if (!transcript_surface(&h->transcript,i,TRANSCRIPT_BODY)) continue;
        wchar_t shown[256]; body_text(h,i,shown,256);
        CHECK(wcsstr(shown,L"A answer")==NULL);      /* never foreign */
    }
    /* Pure replacement: B's window rebinds onto A's departed slots with
       matching shapes — zero pristine consumptions, zero new HWNDs. */
    CHECK(transcript_created_windows(&h->transcript)==created_before_r1);
    CHECK(transcript_created_windows(&h->transcript)<=4*h->transcript.slot_limit+1);
    /* A returns intact: switching back re-renders A's own content on the
       recycled slots (A/B/A through real recycling, never foreign). */
    command(h,CHAT_COMMAND_SELECT,conv_a);
    render_transcript(h);
    CHECK(visible_realized(h));
    { wchar_t shown[256]; body_text(h,3,shown,256);
      CHECK(wcsstr(shown,L"A answer two")!=NULL);
      CHECK(wcsstr(shown,L"B answer")==NULL); }

    /* ---- R3: nonlocal jumps, cumulative HWND peaks, zero-creation
            revisits ---- */
    command(h,CHAT_COMMAND_SELECT,conv_b);
    render_transcript(h);
    transcript_note_user_scroll(&h->transcript,0);
    transcript_position(&h->transcript,feed_arg(h),false);   /* top */
    CHECK(visible_realized(h));
    { wchar_t shown[256]; body_text(h,1,shown,256);
      CHECK(wcsstr(shown,L"B answer 0")!=NULL); }
    transcript_note_user_scroll(&h->transcript,h->transcript.view_content/2);
    transcript_position(&h->transcript,feed_arg(h),false);   /* middle */
    CHECK(visible_realized(h));
    /* Content assertion: every realized visible body renders its own B
       turn -- never foreign A content, and the assistant turn nearest the
       middle shows its expected answer text. */
    { bool any=false;
      for (int i=0;i<h->transcript.record_count;i++) {
        TranscriptRecord *rec=&h->transcript.records[i];
        int top=rec->y-h->transcript.view_scroll;
        if (top>=h->transcript.view_page || top+rec->height<=0) continue;
        if (!transcript_surface(&h->transcript,i,TRANSCRIPT_BODY)) continue;
        wchar_t shown[256]; body_text(h,i,shown,256);
        CHECK(wcsstr(shown,L"A answer")==NULL);
        any=true;
      }
      CHECK(any); }
    /* Continuous traversal catches cross-turn bleed within conversation B,
       not merely stale content from the earlier A conversation. */
    for (int scroll=0;scroll<h->transcript.view_content;
         scroll+=h->transcript.view_page/2) {
        transcript_note_user_scroll(&h->transcript,scroll);
        transcript_position(&h->transcript,feed_arg(h),false);
        for (int i=0;i<h->transcript.record_count;i++) {
            TranscriptRecord *rec=&h->transcript.records[i];
            int top=rec->y-h->transcript.view_scroll;
            if (top>=h->transcript.view_page || top+rec->height<=0) continue;
            wchar_t shown[256], expected[64]; body_text(h,i,shown,256);
            if (i&1) swprintf(expected,64,L"B answer %d",i/2);
            else wcscpy(expected,L"B question");
            CHECK(wcsstr(shown,expected)!=NULL);
        }
    }
    transcript_note_user_scroll(&h->transcript,0x7fffffff);
    transcript_position(&h->transcript,feed_arg(h),false);   /* bottom */
    CHECK(visible_realized(h));
    { wchar_t shown[256]; body_text(h,511,shown,256);
      CHECK(wcsstr(shown,L"B answer 255")!=NULL); }   /* bottom content */
    /* Revisits consume no pristine slots: departed bindings were
        pre-evicted and their HWNDs reused. */
    int created_before_revisit=transcript_created_windows(&h->transcript);
    transcript_note_user_scroll(&h->transcript,0);
    transcript_position(&h->transcript,feed_arg(h),false);
    CHECK(visible_realized(h));
    transcript_note_user_scroll(&h->transcript,0x7fffffff);
    transcript_position(&h->transcript,feed_arg(h),false);
    CHECK(visible_realized(h));
    CHECK(transcript_created_windows(&h->transcript)==created_before_revisit);
    CHECK(transcript_created_windows(&h->transcript)<=4*h->transcript.slot_limit+1);
    CHECK(transcript_bound_slots(&h->transcript)<=h->transcript.slot_limit);
    /* A record revisited in a region measured once keeps its exact stamp:
       heights are consumed, not re-measured (round count stays small). */
    CHECK(h->transcript.stat.rounds<=2*512+4);

    /* ---- R2: deferred selected content with changed height, including
            the newly-created-debt preflight ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int r2_conv=chat->active;
    for (int t=0;t<6;t++) add_turn(chat,L"r2 question",L"r2 answer line",NULL,-1);
    render_transcript(h);
    int r2=0;
    /* Reveal record 0 first: the funnel realizes it, so the selection below
       lands on a real surface. */
    CHECK(transcript_reveal_turn(&h->transcript,feed_arg(h),r2));
    CHECK(body_window(h,r2));
    SendMessageW(body_window(h,r2),EM_SETSEL,0,4);
    int body_before=h->transcript.records[r2].body_h;
    CHECK(chat_message_set_text(&chat->conversations[r2_conv].messages[r2],
        L"r2 edited\nsecond much longer line of edited text"));
    render_transcript(h);
    CHECK(h->transcript.records[r2].body_pending);
    CHECK(h->transcript.records[r2].blocked_debt);
    /* Displayed-true geometry: the old text still shows and the height
       describes it; the new revision is not claimed. */
    CHECK(h->transcript.records[r2].body_h==body_before);
    { CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body_window(h,r2),EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==0 && sel.cpMax==4); }
    { wchar_t shown[256]; body_text(h,r2,shown,256);
      CHECK(wcsstr(shown,L"r2 edited")==NULL); }
    /* The selection clears: the external EN_SELCHANGE event applies the
       debt (one attempt this render; progress rides this event). */
    SendMessageW(body_window(h,r2),EM_SETSEL,0,0);
    CHECK(!h->transcript.records[r2].body_pending);
    CHECK(h->transcript.records[r2].body_revision==
        chat->conversations[r2_conv].messages[r2].body_revision);
    CHECK(h->transcript.records[r2].body_h>body_before);
    { wchar_t shown[256]; body_text(h,r2,shown,256);
      CHECK(wcsstr(shown,L"r2 edited")!=NULL); }

    /* ---- R4: per-surface creation failure -> bounded retries -> recovery
            during streaming ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    int r4_conv=chat->active;
    add_turn(chat,L"r4 filler q",L"r4 filler a",NULL,-1);
    begin_regenerate(h);
    int r4=h->request_message;
    render_transcript(h);
    { RichTextControl *body=transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY);
      CHECK(body);
      block_fail_id=100+h->transcript.records[r4].slot*4+(int)TRANSCRIPT_BODY;
      DestroyWindow(body->window);
      CHECK(!body->window);   /* WM_NCDESTROY cleared the destroyed handle */ }
    handle_event(h,fixture(h,COMPLETION_DELTA,L"first"));
    handle_event(h,fixture(h,COMPLETION_DELTA,L" second"));
    CHECK(h->body_flush_pending);                /* armed by the missing body */
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)==NULL);
    CHECK(h->transcript.records[r4].blocked_resource);
    CHECK(!wcscmp(chat_message_text(pending(h)),L"first second"));
    pump_messages(1300);                         /* 1 Hz sweep retries (fails) */
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)==NULL);
    CHECK(h->body_flush_pending);
    block_fail_id=0;
    pump_messages(1300);                         /* retry succeeds */
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)!=NULL);
    CHECK(pump_until_body(h,r4,L"first second",3000));
    CHECK(h->transcript.records[r4].body_revision==
        pending(h)->body_revision);
    /* Head surface fails independently; body stays intact. */
    { RichTextControl *head=transcript_surface(&h->transcript,r4,TRANSCRIPT_HEAD);
      CHECK(head);
      block_fail_id=100+h->transcript.records[r4].slot*4+(int)TRANSCRIPT_HEAD;
      DestroyWindow(head->window);
      CHECK(!head->window);   /* WM_NCDESTROY cleared the destroyed handle */ }
    render_transcript(h);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_HEAD)==NULL);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)!=NULL);
    { wchar_t shown[256]; body_text(h,r4,shown,256);
      CHECK(wcsstr(shown,L"first second")!=NULL); }   /* no foreign content */
    block_fail_id=0;
    render_transcript(h);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_HEAD)!=NULL);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.state==CHAT_GENERATION_COMPLETE);
    /* Metadata footer fails independently; body untouched. */
    { RichTextControl *meta=transcript_surface(&h->transcript,r4,TRANSCRIPT_META);
      CHECK(meta);
      block_fail_id=100+h->transcript.records[r4].slot*4+(int)TRANSCRIPT_META;
      DestroyWindow(meta->window);
      CHECK(!meta->window);   /* WM_NCDESTROY cleared the destroyed handle */ }
    render_transcript(h);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_META)==NULL);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)!=NULL);
    block_fail_id=0;
    render_transcript(h);
    { wchar_t meta[512]; meta_text(h,r4,meta,512);
      CHECK(wcsstr(meta,L"Complete")!=NULL); }
    /* Reasoning viewport (viewport injector): failed expansion stays
       absent without disturbing the other surfaces; retry recovers. */
    CHECK(chat_message_set_reasoning(&chat->conversations[r4_conv].messages[r4],
        L"R4 reasoning"));
    chat->conversations[r4_conv].messages[r4].reasoning_open=false;
    refresh_turn(h,r4);
    CHECK(!h->transcript.records[r4].reason_live);
    viewport_fail_id=100+h->transcript.records[r4].slot*4+(int)TRANSCRIPT_REASON;
    chat->conversations[r4_conv].messages[r4].reasoning_open=true;
    refresh_turn(h,r4);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_REASON)==NULL);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_BODY)!=NULL);
    viewport_fail_id=0;
    refresh_turn(h,r4);
    CHECK(transcript_surface(&h->transcript,r4,TRANSCRIPT_REASON)!=NULL);
    { wchar_t shown[128]; reasoning_text(h,r4,shown,128);
      CHECK(!wcscmp(shown,L"R4 reasoning")); }

    /* ---- R5: exact-vs-estimated measurement status, measurer loss and
            recovery ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    for (int t=0;t<20;t++) add_turn(chat,L"r5 question",L"r5 answer",NULL,-1);
    render_transcript(h);
    for (int i=0;i<40;i++)
        CHECK(h->transcript.records[i].measured_valid &&
            !h->transcript.records[i].measured_estimated);
    CHECK(h->transcript.measurer_valid);
    DestroyWindow(h->transcript.measurer.window);
    CHECK(!h->transcript.measurer.window);  /* WM_NCDESTROY cleared the handle */
    block_fail_id=TRANSCRIPT_MEASURE_ID;
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    /* 40 turns: whatever position the reader holds, records far outside the
        realization window stay unbound, so a dead measurer must degrade
        their geometry to flagged estimates rather than live measuring. */
    for (int t=0;t<40;t++) add_turn(chat,L"r5b question",L"r5b answer",NULL,-1);
    render_transcript(h);                        /* measurer cannot be created */
    CHECK(!h->transcript.measurer_valid);
    bool any_estimated=false;
    for (int i=0;i<h->transcript.record_count;i++)
        if (h->transcript.records[i].measured_estimated) any_estimated=true;
    CHECK(any_estimated);                        /* flagged, not silently wrong */
    CHECK(visible_realized(h));                  /* I-GAP under estimates */
    block_fail_id=0;
    render_transcript(h);                        /* recovery: recreated */
    CHECK(h->transcript.measurer_valid);
    for (int i=0;i<h->transcript.record_count;i++)
        CHECK(h->transcript.records[i].measured_valid &&
            !h->transcript.records[i].measured_estimated);
    CHECK(h->transcript.stat.measure_retries>0);
    /* I-EQUAL spot check on EXACT results: a realized record's cached body
       height equals its live measurement exactly. */
    for (int i=0;i<h->transcript.record_count;i++) {
        RichTextControl *body=transcript_surface(&h->transcript,i,TRANSCRIPT_BODY);
        if (!body) continue;
        CHECK(h->transcript.records[i].body_h==transcript_measure_live(&h->transcript,body,NULL));
    }

    /* A live measurer that drops its resize notification yields retryable
       estimates, then recovers to exact results without HWND loss. */
    h->transcript.diagnostic_drop_measure_notify=true;
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    add_turn(chat,L"notify question",L"notify answer",NULL,-1);
    render_transcript(h);
    CHECK(h->transcript.records[0].measured_estimated);
    h->transcript.diagnostic_drop_measure_notify=false;
    render_transcript(h);
    CHECK(h->transcript.records[0].measured_valid &&
        !h->transcript.records[0].measured_estimated);
    /* Theme mutation advances a real stamp input and reforms the surface. */
    RichTextTheme changed=h->transcript.theme;
    uint32_t theme_before=h->transcript.theme_epoch;
    changed.ui_size+=1.0f;
    transcript_set_theme(&h->transcript,&changed);
    render_transcript(h);
    CHECK(h->transcript.theme_epoch!=theme_before);
    CHECK(h->transcript.records[0].measured_theme==h->transcript.theme_epoch &&
        h->transcript.records[0].measured_valid);
    /* The defensive cap still prepares strict-visible records and recomputes
       their geometry, but records no false stable claim. */
    int fallback_before=h->transcript.stat.fallback_rounds;
    h->transcript.diagnostic_round_cap=0;
    CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[1],
        L"cap fallback answer with enough new content to change geometry and "
        L"require strict-visible preparation"));
    render_transcript(h);
    CHECK(h->transcript.stat.fallback_rounds==fallback_before+1);
    CHECK(visible_realized(h));
    { wchar_t shown[256]; body_text(h,1,shown,256);
      CHECK(wcsstr(shown,L"cap fallback answer")!=NULL); }
    h->transcript.diagnostic_round_cap=-1;
    render_transcript(h);

    /* ---- Focused equality: the shared measurement surface must answer
            exactly what an equivalent live surface answers for identical
            content, width, DPI, theme and formatting. The same production
            setters feed both surfaces; both results must be EXACT and
            equal, and the record's cached (measurer-produced) heights must
            equal the live measurement for every family it owns. ---- */
    {
        Transcript *tr=&h->transcript;
        int eq=1;    /* a realized assistant turn: head, body and meta live */
        CHECK(transcript_surface(tr,eq,TRANSCRIPT_HEAD)!=NULL);
        CHECK(transcript_surface(tr,eq,TRANSCRIPT_BODY)!=NULL);
        CHECK(transcript_surface(tr,eq,TRANSCRIPT_META)!=NULL);
        RichTextControl *meas=&tr->measurer;
        TranscriptMeasureQuality qm=TRANSCRIPT_MEASURE_ESTIMATE,
            ql=TRANSCRIPT_MEASURE_ESTIMATE;
        const ChatConversation *cv=&chat->conversations[chat->active];
        /* verbatim body: record 0 is a user turn whose live body holds
           exactly set_block(role, text), so feeding the same setter with the
           same inputs to the measurer must measure identically. The funnel
           realizes it first: at this point in the suite record 0 may be
           outside the realization window. */
        CHECK(transcript_reveal_turn(&h->transcript,feed_arg(h),0));
        const ChatMessage *u=&cv->messages[0];
        CHECK(transcript_surface(tr,0,TRANSCRIPT_BODY)!=NULL);
        rich_text_set_block(meas,u->role,chat_message_text(u));
        int hm=transcript_measure_live(tr,meas,&qm);
        int hl=transcript_measure_live(tr,
            transcript_surface(tr,0,TRANSCRIPT_BODY),&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        /* markdown body with real wrapping: headings, prose, an unbroken
           long token, inline code and a list */
        const ChatMessage *m=&cv->messages[eq];
        CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[eq],
            L"## Wrapped heading\n\nplain prose paragraph that is long enough to "
            L"wrap several times at this width plus an unbroken token "
            L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA and `inline code`.\n\n"
            L"- list one\n- list two"));
        refresh_turn(h,eq);
        m=&chat->conversations[chat->active].messages[eq];
        rich_text_set_markdown(meas,m->role,chat_message_text(m));
        hm=transcript_measure_live(tr,meas,&qm);
        RichTextControl *body=transcript_surface(tr,eq,TRANSCRIPT_BODY);
        hl=transcript_measure_live(tr,body,&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        CHECK(tr->records[eq].body_h==hl);       /* cached == live */
        /* nested markdown body: quoted lists and continuations indent their
           paragraphs, so both surfaces must apply the same layout */
        CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[eq],
            L"> - quoted item\n>   - nested item\n>     continuation of the "
            L"nested item that is long enough to wrap at this width"));
        refresh_turn(h,eq);
        m=&chat->conversations[chat->active].messages[eq];
        rich_text_set_markdown(meas,m->role,chat_message_text(m));
        hm=transcript_measure_live(tr,meas,&qm);
        hl=transcript_measure_live(tr,
            transcript_surface(tr,eq,TRANSCRIPT_BODY),&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        CHECK(tr->records[eq].body_h==hl);       /* cached == live */
        /* markdown link body: the label replaces the URL on both surfaces,
           both own the destination, and both still measure identically */
        CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[eq],
            L"see [the link](https://example.com/a) for details"));
        refresh_turn(h,eq);
        m=&chat->conversations[chat->active].messages[eq];
        rich_text_set_markdown(meas,m->role,chat_message_text(m));
        hm=transcript_measure_live(tr,meas,&qm);
        RichTextControl *link_body=transcript_surface(tr,eq,TRANSCRIPT_BODY);
        hl=transcript_measure_live(tr,link_body,&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        CHECK(tr->records[eq].body_h==hl);       /* cached == live */
        CHECK(meas->link_count==1 && link_body->link_count==1);
        { wchar_t shown[128]; body_text(h,eq,shown,128);
          CHECK(!wcscmp(shown,L"see the link for details")); }
        /* head label */
        rich_text_set_head(meas,m->role,NULL);
        hm=transcript_measure_live(tr,meas,&qm);
        hl=transcript_measure_live(tr,
            transcript_surface(tr,eq,TRANSCRIPT_HEAD),&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        CHECK(tr->records[eq].head_h==hl);       /* cached == live */
        /* metadata footer: the cached height came from the measurer's exact
           production text (format_stats), so cached == live here too */
        hl=transcript_measure_live(tr,
            transcript_surface(tr,eq,TRANSCRIPT_META),&ql);
        CHECK(ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(tr->records[eq].meta_h==hl);
        /* table body: the hidden measurer and the live surface must flatten
           to code-unit-identical text at identical layout inputs, and both
           must measure EXACT with the cached height equal to the live one */
        CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[eq],
            L"| left | right |\n| :--- | ---: |\n"
            L"| alpha alpha alpha alpha | beta beta beta beta |\n"
            L"| gamma gamma | delta delta delta |"));
        refresh_turn(h,eq);
        m=&chat->conversations[chat->active].messages[eq];
        CHECK(rich_text_set_markdown_width(meas,m->role,
            chat_message_text(m),tr->view_width));
        hm=transcript_measure_live(tr,meas,&qm);
        RichTextControl *table_body=transcript_surface(tr,eq,TRANSCRIPT_BODY);
        hl=transcript_measure_live(tr,table_body,&ql);
        CHECK(qm==TRANSCRIPT_MEASURE_EXACT && ql==TRANSCRIPT_MEASURE_EXACT);
        CHECK(hm==hl);
        CHECK(tr->records[eq].body_h==hl);       /* cached == live */
        {
            wchar_t meas_text[512], live_text[512];
            rich_text_get_text(meas,meas_text,512);
            rich_text_get_text(table_body,live_text,512);
            CHECK(!wcscmp(meas_text,live_text));  /* code-unit-identical */
            CHECK(wcsstr(live_text,L"|")==NULL && wcsstr(live_text,L"\t")!=NULL);
            CHECK(meas->link_count==table_body->link_count);
        }
        /* restore the turn's own content and certify the restoration */
        CHECK(chat_message_set_text(&chat->conversations[chat->active].messages[eq],
            L"r5 answer"));
        refresh_turn(h,eq);
        { wchar_t shown[64]; body_text(h,eq,shown,64);
          CHECK(!wcscmp(shown,L"r5 answer")); }
    }

    /* ---- R6: resize storm and settle, then DPI ---- */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    for (int t=0;t<12;t++) add_turn(chat,L"r6 question",L"r6 answer",NULL,-1);
    render_transcript(h);
    SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
    CHECK(h->transcript.resizing);
    /* A real resize: the view's WM_SIZE drives render_transcript, which
       re-derives view_width and runs the realize loop under the storm. A
       bare top-level WM_SIZE changes no geometry and would never reach the
       transcript, so the storm is simulated by resizing the view itself. */
    SetWindowPos(h->view,NULL,0,0,700,430,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
    CHECK(visible_realized(h));                  /* I-GAP holds each step */
    SetWindowPos(h->view,NULL,0,0,860,510,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
    CHECK(visible_realized(h));
    bool stale_offscreen=false;
    for (int i=0;i<h->transcript.record_count;i++) {
        TranscriptRecord *rec=&h->transcript.records[i];
        int top=rec->y-h->transcript.view_scroll;
        if (top>=h->transcript.view_page || top+rec->height<=0) {
            if (rec->measured_width!=h->transcript.view_width ||
                !rec->measured_valid) stale_offscreen=true;
        }
    }
    CHECK(stale_offscreen);                      /* off-screen exactness relaxed */
    SendMessageW(window,WM_EXITSIZEMOVE,0,0);
    CHECK(!h->transcript.resizing);
    for (int i=0;i<h->transcript.record_count;i++)
        CHECK(h->transcript.records[i].measured_width==h->transcript.view_width &&
            h->transcript.records[i].measured_valid &&
            !h->transcript.records[i].measured_estimated);
    CHECK(visible_realized(h));
    /* Settled geometry is exact: a reveal to a far turn lands on it. */
    { TranscriptFeed feed=transcript_feed(h);
      CHECK(transcript_reveal_turn(&h->transcript,&feed,4));
      TranscriptRecord *rec=&h->transcript.records[4];
      CHECK(rec->y+rec->height>h->transcript.view_scroll &&
          rec->y<h->transcript.view_scroll+h->transcript.view_page); }
    transcript_set_dpi(&h->transcript,120.0f);
    render_transcript(h);
    for (int i=0;i<h->transcript.record_count;i++)
        CHECK(h->transcript.records[i].measured_dpi==120.0f &&
            h->transcript.records[i].measured_valid);
    CHECK(visible_realized(h));
    transcript_set_dpi(&h->transcript,96.0f);
    render_transcript(h);
    for (int i=0;i<h->transcript.record_count;i++)
        CHECK(h->transcript.records[i].measured_dpi==96.0f &&
            h->transcript.records[i].measured_valid);

    /* ---- Reader anchoring: stationary content through every mutation ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<24;t++)
            add_turn(chat,L"anchor question",L"anchor answer body",
                L"anchor reasoning text",42.0);
        render_transcript(h);
        /* Anchor inside an older assistant turn's body through the real
            user-scroll path: only reader input leaves bottom-follow. */
        TranscriptRecord *target=&h->transcript.records[21];
        transcript_note_user_scroll(&h->transcript,target->body_y+px(h,8));
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid && !h->transcript.anchor.top &&
            h->transcript.anchor.surface==TRANSCRIPT_BODY &&
            h->transcript.anchor.message==
                chat->conversations[chat->active].messages[21].id);
        /* The restore invariant: the anchored surface's top sits exactly
            `offset` pixels above the viewport top after every mutation
            pass; view_scroll itself may change to keep it there. */
        #define ANCHOR_HELD(hh) \
            ((hh)->transcript.view_scroll - \
             (hh)->transcript.records[21].body_y == \
             (hh)->transcript.anchor.offset)
        int restores_before=h->transcript.stat.anchor_restores;
        /* Streaming below the anchor: deltas, the throttle and completion
            never force-follow a free reader. */
        begin_regenerate(h);
        int stream_turn=h->request_message;
        CHECK(stream_turn>21);
        handle_event(h,fixture(h,COMPLETION_DELTA,L"streamed"));
        for (int i=0;i<8;i++)
            handle_event(h,fixture(h,COMPLETION_DELTA,L" tail line"));
        CHECK(!transcript_following(&h->transcript) && ANCHOR_HELD(h));
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        CHECK(!transcript_following(&h->transcript) && ANCHOR_HELD(h));
        /* Reasoning toggle on the anchor turn itself: the viewport inserts
            above the anchored body and the restore tracks the body down, so
            the anchored content stays on screen. */
        int body_y_before=target->body_y;
        click_row(h,21);
        CHECK(chat->conversations[chat->active].messages[21].reasoning_open);
        CHECK(target->body_y>body_y_before);   /* viewport inserted above */
        CHECK(!transcript_following(&h->transcript) && ANCHOR_HELD(h));
        click_row(h,21);                       /* collapse again */
        CHECK(!transcript_following(&h->transcript) && ANCHOR_HELD(h));
        /* Resize storm and settle: each step restores against fresh
            visible geometry. */
        SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
        SetWindowPos(h->view,NULL,0,0,700,430,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        SetWindowPos(h->view,NULL,0,0,860,510,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        SendMessageW(window,WM_EXITSIZEMOVE,0,0);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        /* DPI reflow in both directions. */
        transcript_set_dpi(&h->transcript,120.0f);
        render_transcript(h);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        transcript_set_dpi(&h->transcript,96.0f);
        render_transcript(h);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        /* Estimate -> exact height correction above the anchor: an edited
            turn re-measures under the dropped-notification seam, the
            flagged estimate moves geometry, and the restore tracks it; the
            exact correction is restored the same way. */
        h->transcript.diagnostic_drop_measure_notify=true;
        chat_message_set_text(&chat->conversations[chat->active].messages[2],
            L"anchor answer body edited longer with a second line");
        chat_message_touch(&chat->conversations[chat->active].messages[2]);
        render_transcript(h);
        bool any_estimated=false;
        for (int i=0;i<21;i++)
            if (h->transcript.records[i].measured_estimated) any_estimated=true;
        CHECK(any_estimated);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        h->transcript.diagnostic_drop_measure_notify=false;
        render_transcript(h);
        CHECK(visible_realized(h) && ANCHOR_HELD(h));
        CHECK(h->transcript.stat.anchor_restores>restores_before);
        CHECK(h->transcript.stat.anchor_rejected==0);
        /* Explicit resumption: only a user scroll landing at the bottom
            re-enters follow, and the next delta then follows. */
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(transcript_following(&h->transcript));
        begin_regenerate(h);
        handle_event(h,fixture(h,COMPLETION_DELTA,L"resume"));
        CHECK(transcript_following(&h->transcript) &&
            transcript_pinned(&h->transcript));
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        CHECK(transcript_following(&h->transcript));
        /* Boundary: a position inside the top margin (above the first
            turn) is not the top anchor -- it is named with a negative
            offset and restores exactly, including across re-renders. */
        transcript_note_user_scroll(&h->transcript,px(h,5));
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(h->transcript.anchor.valid && !h->transcript.anchor.top);
        CHECK(h->transcript.anchor.offset<0);
        CHECK(h->transcript.view_scroll==px(h,5));
        render_transcript(h);
        CHECK(h->transcript.view_scroll==px(h,5));
        /* Boundary: an offset at a surface's last pixel stays inside the
            surface when it shrinks -- the clamp target is height-1, never
            height (which would name the next surface's top). */
        { TranscriptRecord *held=&h->transcript.records[21];
          CHECK(chat_message_set_text(
              &chat->conversations[chat->active].messages[21],
              L"tall body line one\nline two\nline three"));
          chat_message_touch(&chat->conversations[chat->active].messages[21]);
          /* Realize the edited turn first so the geometry the anchor is
              taken against is settled (a far record's heights come from
              the measurer until its prepare re-measures live). */
          { TranscriptFeed feed=transcript_feed(h);
            CHECK(transcript_reveal_turn(&h->transcript,&feed,21)); }
          transcript_note_user_scroll(&h->transcript,
              held->body_y+held->body_h-1);
          transcript_position(&h->transcript,feed_arg(h),false);
          CHECK(h->transcript.anchor.valid && !h->transcript.anchor.top);
          CHECK(h->transcript.anchor.surface==TRANSCRIPT_BODY);
          CHECK(h->transcript.anchor.offset==held->body_h-1);
          int old_h=held->body_h;
          CHECK(chat_message_set_text(
              &chat->conversations[chat->active].messages[21],L"tiny"));
          chat_message_touch(&chat->conversations[chat->active].messages[21]);
          render_transcript(h);
          CHECK(held->body_h<old_h);          /* the surface shrank */
          CHECK(h->transcript.view_scroll==
              held->body_y+held->body_h-1);   /* last pixel, not past it */ }
        #undef ANCHOR_HELD
    }

    /* ---- Per-conversation anchors: the linear stable-ID table ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int table_a=chat->active;
        for (int t=0;t<16;t++) add_turn(chat,L"tableA question",
            L"tableA answer body",NULL,-1);
        render_transcript(h);
        TranscriptRecord *held=&h->transcript.records[9];
        uint64_t held_id=chat->conversations[table_a].messages[9].id;
        transcript_note_user_scroll(&h->transcript,held->body_y+px(h,8));
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid &&
            h->transcript.anchor.message==held_id);
        int anchor_offset=h->transcript.anchor.offset;
        /* A fresh conversation has no entry: the switch sets BOTTOM and
            clears the anchor -- a fresh conversation is never blessed with
            FREE and nothing to restore. A's stays stored under A's stable
            id. */
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int table_b=chat->active;
        for (int t=0;t<6;t++) add_turn(chat,L"tableB question",
            L"tableB answer body",NULL,-1);
        render_transcript(h);
        CHECK(h->transcript.active_conversation==
            chat->conversations[table_b].id);
        CHECK(!h->transcript.anchor.valid);
        CHECK(transcript_following(&h->transcript));
        /* Switching back restores the saved position: a valid saved anchor
            re-enters FREE and the anchored content sits exactly where the
            reader left it. */
        command(h,CHAT_COMMAND_SELECT,table_a);
        render_transcript(h);
        CHECK(h->transcript.active_conversation==
            chat->conversations[table_a].id);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid &&
            h->transcript.anchor.message==held_id);
        CHECK(h->transcript.records[9].message==held_id);
        CHECK(h->transcript.view_scroll-h->transcript.records[9].body_y==
            anchor_offset);
        CHECK(visible_realized(h));
        CHECK(h->transcript.stat.conv_anchor_restores>0);
        /* Leaving A again keeps its entry in the table for the next return
            (B is BOTTOM, so leaving B stores nothing). */
        command(h,CHAT_COMMAND_SELECT,table_b);
        render_transcript(h);
        command(h,CHAT_COMMAND_SELECT,table_a);
        render_transcript(h);
        CHECK(h->transcript.view_scroll-h->transcript.records[9].body_y==
            anchor_offset);
        /* Returning to A and scrolling to the bottom leaves no position:
            the departure must CLEAR A's stored entry, so a later return
            lands BOTTOM instead of restoring the stale FREE position. */
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(transcript_following(&h->transcript));
        command(h,CHAT_COMMAND_SELECT,table_b);
        render_transcript(h);
        command(h,CHAT_COMMAND_SELECT,table_a);
        render_transcript(h);
        CHECK(transcript_following(&h->transcript));   /* BOTTOM, not stale */
        CHECK(!h->transcript.anchor.valid);
    }

    /* ---- A saved anchor whose message no longer exists is stale ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int stale_conv=chat->active;
        for (int t=0;t<7;t++)
            add_turn(chat,L"stale question",L"stale filler answer",NULL,-1);
        wchar_t tall[2048];
        wcscpy(tall,L"tall answer");
        for (int line=0;line<32;line++)
            wcscat(tall,L"\nfiller answer line text");
        add_turn(chat,L"stale last question",tall,NULL,-1);
        int stale_turn=chat->conversations[stale_conv].message_count-1;
        render_transcript(h);
        /* Revealing the taller-than-viewport last turn aligns its top, so
            the pinned anchor names that turn's own message. */
        { TranscriptFeed feed=transcript_feed(h);
          CHECK(transcript_reveal_turn(&h->transcript,&feed,stale_turn)); }
        CHECK(h->transcript.anchor.valid);
        uint64_t stale_id=
            chat->conversations[stale_conv].messages[stale_turn].id;
        CHECK(h->transcript.anchor.message==stale_id);
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);   /* saves the entry */
        render_transcript(h);
        command(h,CHAT_COMMAND_SELECT,stale_conv);     /* FREE restored */
        render_transcript(h);
        CHECK(!transcript_following(&h->transcript) &&
            h->transcript.anchor.message==stale_id);
        /* Regenerate replaces the anchored response with a fresh identity:
            the saved anchor names a message that no longer exists. The
            in-conversation resolve falls back to the nearest earlier turn
            (the documented retry behavior), but the SWITCH back after
            leaving must not bless the dead position. */
        begin_regenerate(h);
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        CHECK(chat->conversations[stale_conv].messages[stale_turn].id!=
            stale_id);
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);   /* saves dead anchor */
        render_transcript(h);
        command(h,CHAT_COMMAND_SELECT,stale_conv);
        render_transcript(h);
        CHECK(transcript_following(&h->transcript));   /* BOTTOM, not FREE */
        CHECK(!h->transcript.anchor.valid);
    }

    /* ---- Anchor-table lifecycle: entries for deleted conversations are
            pruned, so a session can mint far more than 128 distinct ids ----
    */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int life_conv=chat->active;
        for (int t=0;t<8;t++) add_turn(chat,L"life question",
            L"life answer body",NULL,-1);
        render_transcript(h);
        transcript_note_user_scroll(&h->transcript,
            h->transcript.records[5].body_y+px(h,8));
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(h->transcript.anchor.valid &&
            h->transcript.anchor.conversation==
                chat->conversations[life_conv].id);
        /* Churn well past the table's 128 slots: create, anchor, delete.
            Each deleted conversation's entry must be pruned (or reused),
            so the table never wedges full of dead ids. The chat-level
            delete plus the production bookkeeping (invalidate, render)
            reproduces the host's delete sequence without the dialog. A
            one-turn conversation is shorter than the viewport, so the
            anchor comes from the reveal path, which pins FREE with a valid
            top anchor regardless of conversation height. */
        for (int k=0;k<136;k++) {
            command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
            add_turn(chat,L"churn question",L"churn answer",NULL,-1);
            render_transcript(h);
            { TranscriptFeed feed=transcript_feed(h);
              CHECK(transcript_reveal_turn(&h->transcript,&feed,1)); }
            CHECK(h->transcript.anchor.valid);
            chat_delete(chat);
            transcript_invalidate(&h->transcript);   /* saves the dead id */
            render_transcript(h);
        }
        CHECK(chat->conversation_count>=1);
        /* A real conversation after the churn still saves and restores its
            anchor: the table never wedged full of dead entries. */
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int life_g=chat->active;
        for (int t=0;t<8;t++) add_turn(chat,L"final question",
            L"final answer body",NULL,-1);
        render_transcript(h);
        transcript_note_user_scroll(&h->transcript,
            h->transcript.records[5].body_y+px(h,8));
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(h->transcript.anchor.valid);
        int life_offset=h->transcript.anchor.offset;
        uint64_t life_id=chat->conversations[life_g].messages[5].id;
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        render_transcript(h);
        CHECK(transcript_following(&h->transcript) &&
            !h->transcript.anchor.valid);      /* fresh: BOTTOM, no FREE */
        command(h,CHAT_COMMAND_SELECT,life_g);
        render_transcript(h);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid &&
            h->transcript.anchor.message==life_id);
        CHECK(h->transcript.records[5].message==life_id);
        CHECK(h->transcript.view_scroll-h->transcript.records[5].body_y==
            life_offset);
        CHECK(visible_realized(h));
    }

    /* ---- Send while FREE (bounded): no force-follow through the send path ----
    */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<12;t++) add_turn(chat,L"send question",
            L"send answer body",NULL,-1);
        render_transcript(h);
        /* Reading an older turn through the real user-scroll path: only a
            user-driven scroll may leave bottom-follow. */
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(!transcript_following(&h->transcript));
        TranscriptRecord *held=&h->transcript.records[0];
        uint64_t held_id=chat->conversations[chat->active].messages[0].id;
        int held_y=held->y, scroll=h->transcript.view_scroll;
        int rejected_before=h->transcript.stat.anchor_rejected;
        /* A focused transcript surface must survive the send's turn-slot
            reset: start_response transfers focus to the composer through
            the callback before the invalidation. */
        { HWND focused=body_window(h,2);
          CHECK(focused);
          SetFocus(focused);
          CHECK(h->transcript.focus_window==focused); }
        rich_text_set_text(&h->composer,L"send while reading");
        perform_send(h);
        CHECK(!h->generating);   /* no key: the request failed closed */
        CHECK(chat->conversations[chat->active].message_count==26);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.focus_window==NULL);
        CHECK(GetFocus()==h->composer.window);
        /* Anchored content stationary: the reader's scroll was restored,
            not followed (the anchor is the top-of-transcript marker here:
            nothing above it changed, so the scroll is exactly held). */
        CHECK(held->y==held_y && held->message==held_id);
        CHECK(h->transcript.view_scroll==scroll);
        CHECK(h->transcript.stat.anchor_rejected==rejected_before);
        CHECK(visible_realized(h));
    }

    /* ---- A cancelled thumb drag finishes through the end-of-drag path ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<12;t++) add_turn(chat,L"drag question",
            L"drag answer body",NULL,-1);
        render_transcript(h);
        /* Begin a real thumb drag on the container's scrollbar: the drag
            owns the position and suspends the qualification while held. */
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_THUMBTRACK,0),0);
        CHECK(h->transcript.thumb_drag);
        CHECK(transcript_following(&h->transcript));   /* unchanged mid-drag */
        /* The cancellation can arrive at the transcript child itself: it
            must finish the drag (qualification plus fresh-anchor capture),
            not merely clear the flag. */
        SendMessageW(h->view,WM_CANCELMODE,0,0);
        CHECK(!h->transcript.thumb_drag);
        CHECK(!h->transcript.user_scroll_pending);     /* capture completed */
        { SCROLLINFO si; memset(&si,0,sizeof si); si.cbSize=sizeof si;
          si.fMask=SIF_ALL; GetScrollInfo(h->view,SB_VERT,&si);
          int maximum=h->transcript.view_content-h->transcript.view_page;
          if (maximum<0) maximum=0;
          bool at_bottom=si.nPos>=maximum-1;
          /* The mode matches the drag's end position. */
          CHECK(transcript_following(&h->transcript)==at_bottom); }
        /* The same completion through the top-level's cancellation: the
            reader has left the bottom first, so the drag ends FREE. */
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_LINEUP,0),0);
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_LINEUP,0),0);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid);
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_THUMBTRACK,0),0);
        CHECK(h->transcript.thumb_drag);
        SendMessageW(window,WM_CANCELMODE,0,0);
        CHECK(!h->transcript.thumb_drag);
        CHECK(!transcript_following(&h->transcript));
        CHECK(h->transcript.anchor.valid);
        /* A thumb release (SB_THUMBPOSITION) ends the drag through the
            same qualification: the flag clears, the pending capture
            completes, and the mode matches the release position. */
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_THUMBTRACK,0),0);
        CHECK(h->transcript.thumb_drag);
        SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_THUMBPOSITION,0),0);
        CHECK(!h->transcript.thumb_drag);
        CHECK(!h->transcript.user_scroll_pending);
        { SCROLLINFO si; memset(&si,0,sizeof si); si.cbSize=sizeof si;
          si.fMask=SIF_ALL; GetScrollInfo(h->view,SB_VERT,&si);
          int maximum=h->transcript.view_content-h->transcript.view_page;
          if (maximum<0) maximum=0;
          bool at_bottom=si.nPos>=maximum-1;
          CHECK(transcript_following(&h->transcript)==at_bottom);
          if (at_bottom) CHECK(transcript_pinned(&h->transcript)); }
    }

    /* ---- Reader focus tracking through WM_COMMAND ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<12;t++) add_turn(chat,L"focus question",
            L"focus answer body",NULL,-1);
        render_transcript(h);
        /* The switch landed BOTTOM, so an early record is outside the
            realization window until the reveal realizes it. */
        { TranscriptFeed feed=transcript_feed(h);
          CHECK(transcript_reveal_turn(&h->transcript,&feed,2)); }
        HWND body=body_window(h,2);
        CHECK(body);
        SetFocus(body);                          /* real EN_SETFOCUS route */
        CHECK(h->transcript.focus_window==body);
        /* The focused record is Tier-A protected at decision time. */
        CHECK(h->transcript.policy_needed>0);
        /* Scrolling the focused surface out of the window transfers focus
            to the host composer through the focus_release callback before
            the surface is hidden. */
        int transfers_before=h->transcript.stat.focus_transfers;
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(GetFocus()==h->composer.window);
        CHECK(h->transcript.focus_window==NULL);
        CHECK(h->transcript.stat.focus_transfers==transfers_before+1);
        /* Switching conversations also transfers focus, never destroying a
            focused child. */
        { TranscriptFeed feed=transcript_feed(h);
          CHECK(transcript_reveal_turn(&h->transcript,&feed,5)); }
        HWND body5=body_window(h,5);
        CHECK(body5);
        SetFocus(body5);
        CHECK(GetFocus()==body5);
        CHECK(h->transcript.focus_window==body5);
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        CHECK(h->transcript.focus_window==NULL);
        { HWND f=GetFocus();
          CHECK(f==h->composer.window); }
        CHECK(h->transcript.stat.focus_transfers==transfers_before+2);
        render_transcript(h);
        CHECK(visible_realized(h));
    }

    /* ---- Forced eviction: the governed limit exhausts, state survives ----
    */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int evict_conv=chat->active;
        wchar_t reason[512];
        /* Record 1 (the first assistant turn) carries a table body, so the
           forced-eviction and rebinding cycle below can prove that a
           rebinding reproduces the same flattened table. */
        const wchar_t *table_answer=
            L"| name | qty |\n| :--- | ---: |\n| alpha | 1 |\n| beta | 22 |";
        for (int t=0;t<256;t++) {
            swprintf(reason,512,L"evict reasoning line 1\nline 2\nline 3\n"
                L"line 4\nline 5\nline 6\nline 7\nline 8\nline 9\nline 10 "
                L"tail %d",t);
            add_turn(chat,L"evict question",
                t==0 ? table_answer : L"evict answer body",reason,1000);
        }
        CHECK(chat->conversations[evict_conv].message_count==512);
        render_transcript(h);
        /* The governed limit is geometric, positive, within the 512-slot
            arena, and was raised (never shrunk). */
        int limit=h->transcript.slot_limit;
        CHECK(limit>0 && limit<=512);
        CHECK(h->transcript.stat.capacity_raises>0);
        CHECK(transcript_created_windows(&h->transcript)<=4*limit+1);
        /* The reader moves away from the top through the real user-scroll
            path, so the early assistant records sit outside the overscan
            window and their expanded viewports are evictable Tier-B. */
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(transcript_following(&h->transcript));
        /* Expand the first 8 assistant reasoning viewports through the
            real refresh path: every clicked record is off-window and
            becomes Tier-B (expanded), beyond the 24-record allowance only
            once more arrive. Exceeding the allowance alone must NOT evict
            anything: forced eviction fires only when selection inside
            [0, slot_limit) fails. */
        int evictions_before=h->transcript.stat.forced_evictions;
        for (int i=1;i<16;i+=2) {
            chat->conversations[evict_conv].messages[i].reasoning_open=true;
            chat_message_touch(&chat->conversations[evict_conv].messages[i]);
            refresh_turn(h,i);
            CHECK(h->transcript.records[i].reason_live);
        }
        CHECK(h->transcript.stat.forced_evictions==evictions_before);
        /* Reader state to preserve across the coming eviction: an inner
            scroll in record 1's viewport and a selection in record 3's. */
        POINT saved_pos;
        { RichTextControl *control=transcript_surface(&h->transcript,1,
              TRANSCRIPT_REASON);
          CHECK(control);
          POINT point={0,px(h,40)};
          SendMessageW(control->window,EM_SETSCROLLPOS,0,(LPARAM)&point);
          SendMessageW(control->window,EM_GETSCROLLPOS,0,(LPARAM)&saved_pos);
          CHECK(saved_pos.y>0); }
        CHARRANGE saved_sel;
        { RichTextControl *control=transcript_surface(&h->transcript,3,
              TRANSCRIPT_REASON);
          CHECK(control);
          SendMessageW(control->window,EM_SETSEL,2,7);
          SendMessageW(control->window,EM_EXGETSEL,0,(LPARAM)&saved_sel);
          CHECK(saved_sel.cpMax>saved_sel.cpMin); }
        /* Exhaust the governed limit: every assistant expansion keeps
            binding a Tier-B record inside [0, slot_limit) -- bindings
            accumulate, nothing is trimmed -- until the limit is genuinely
            exhausted and selection returns -1; only then is the oldest LRU
            Tier-B binding force-evicted. The 512-message conversation
            holds 256 assistant records, far beyond any page's limit. */
        for (int i=17;i<512;i+=2) {
            chat->conversations[evict_conv].messages[i].reasoning_open=true;
            chat_message_touch(&chat->conversations[evict_conv].messages[i]);
            refresh_turn(h,i);
        }
        render_transcript(h);                    /* settle */
        CHECK(h->transcript.stat.forced_evictions>evictions_before);
        /* The saturation was observed and answered, and nothing was
            silently refused: the diagnostics make acceptance verifiable. */
        CHECK(h->transcript.stat.limit_saturated>0);
        CHECK(h->transcript.stat.exhaustion_refusals==0);
        int bound_after=transcript_bound_slots(&h->transcript);
        CHECK(bound_after<=limit);               /* bounded by the limit */
        CHECK(bound_after>0);
        /* Expansion survives eviction: every clicked turn is still open in
            state even though most lost their slots. */
        for (int i=1;i<512;i+=2)
            CHECK(chat->conversations[evict_conv].messages[i].reasoning_open);
        CHECK(h->transcript.records[1].slot<0);  /* the LRU was evicted */
        CHECK(h->transcript.records[3].slot<0);
        /* The evicted turn re-reveals with its own reasoning, and the
            viewport's inner scroll and reader selection are restored. */
        { TranscriptFeed feed=transcript_feed(h);
          CHECK(transcript_reveal_turn(&h->transcript,&feed,1)); }
        CHECK(h->transcript.records[1].slot>=0 &&
            h->transcript.records[1].reason_live);
        { wchar_t shown[512]; reasoning_text(h,1,shown,512);
          CHECK(wcsstr(shown,L"tail 0")!=NULL); }
        /* Rebinding equivalence: the re-realized table body is the same
           flattened text, and the hidden measurer at the same width produces
           code-unit-identical text. */
        { RichTextControl *tb=transcript_surface(&h->transcript,1,
              TRANSCRIPT_BODY);
          CHECK(tb);
          wchar_t bound[512]; rich_text_get_text(tb,bound,512);
          CHECK(!wcscmp(bound,L"name\tqty\r\nalpha\t1\r\nbeta\t22"));
          const ChatMessage *tm=&chat->conversations[evict_conv].messages[1];
          CHECK(rich_text_set_markdown_width(&h->transcript.measurer,tm->role,
              chat_message_text(tm),h->transcript.view_width));
          wchar_t mt[512]; rich_text_get_text(&h->transcript.measurer,mt,512);
          CHECK(!wcscmp(mt,bound)); }
        POINT restored_pos;
        { RichTextControl *control=transcript_surface(&h->transcript,1,
              TRANSCRIPT_REASON);
          SendMessageW(control->window,EM_GETSCROLLPOS,0,(LPARAM)&restored_pos); }
        CHECK(restored_pos.y==saved_pos.y);
        CHECK(h->transcript.stat.eviction_restores>0);
        CHECK(h->transcript.stat.reason_scroll_restores>0);
        { TranscriptFeed feed=transcript_feed(h);
          CHECK(transcript_reveal_turn(&h->transcript,&feed,3)); }
        CHECK(h->transcript.records[3].reason_live);
        CHARRANGE restored_sel;
        { RichTextControl *control=transcript_surface(&h->transcript,3,
              TRANSCRIPT_REASON);
          SendMessageW(control->window,EM_EXGETSEL,0,(LPARAM)&restored_sel); }
        CHECK(restored_sel.cpMin==saved_sel.cpMin &&
            restored_sel.cpMax==saved_sel.cpMax);
        CHECK(h->transcript.stat.selection_restores>0);
        CHECK(visible_realized(h));
    }

    /* ---- Dynamic overscan capacity: computed, warmed shapes ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        for (int t=0;t<24;t++) add_turn(chat,L"capacity question",
            L"capacity answer body text",NULL,-1);
        render_transcript(h);
        /* Warm every shape at this viewport: top, middle, bottom. The
            original view geometry is restored exactly afterwards, since an
            earlier block may have left the view resized. */
        RECT capacity_original;
        GetWindowRect(h->view,&capacity_original);
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        transcript_note_user_scroll(&h->transcript,
            h->transcript.view_content/2);
        transcript_position(&h->transcript,feed_arg(h),false);
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        int created_warm=transcript_created_windows(&h->transcript);
        int bound_warm=transcript_bound_slots(&h->transcript);
        CHECK(bound_warm>0 && bound_warm<=h->transcript.slot_limit);
        CHECK(h->transcript.slot_limit>0 && h->transcript.slot_limit<=512);
        int needed_short=h->transcript.policy_needed;
        CHECK(needed_short>0);
        /* Revisiting the warmed shapes consumes no new HWNDs and never
            grows the bound set past its warmed high-water. */
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        transcript_note_user_scroll(&h->transcript,0x7fffffff);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(transcript_created_windows(&h->transcript)==created_warm);
        CHECK(transcript_bound_slots(&h->transcript)<=bound_warm);
        /* The computed capacity follows the pixel window: a taller viewport
            requires more window slots than a shorter one. */
        SetWindowPos(h->view,NULL,0,0,1100,900,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        render_transcript(h);
        int needed_tall=h->transcript.policy_needed;
        CHECK(needed_tall>needed_short);
        CHECK(visible_realized(h));
        SetWindowPos(h->view,NULL,0,0,
            capacity_original.right-capacity_original.left,
            capacity_original.bottom-capacity_original.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        render_transcript(h);
        CHECK(h->transcript.policy_needed==needed_short);
        CHECK(visible_realized(h));
    }

    /* ---- R7: GFM table flattening and assistant layout currency ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        int r7_conv=chat->active;
        wchar_t r7_table[512];
        {
            wchar_t cell[96], cell2[96];
            for (int i=0;i<80;i++) cell[i]=(wchar_t)(L'a'+(i%20));
            cell[80]=0;
            for (int i=0;i<80;i++) cell2[i]=(wchar_t)(L'A'+(i%20));
            cell2[80]=0;
            swprintf(r7_table,512,
                L"| %ls | %ls |\n| ---: | :--- |\n| %ls | %ls |",
                cell,cell2,cell2,cell);
        }
        int r7=add_turn(chat,L"r7 table question",r7_table,NULL,-1);
        render_transcript(h);
        CHECK(transcript_reveal_turn(&h->transcript,feed_arg(h),r7));
        CHECK(transcript_surface(&h->transcript,r7,TRANSCRIPT_BODY)!=NULL);
        TranscriptRecord *r7rec=&h->transcript.records[r7];
        { wchar_t shown[1024]; body_text(h,r7,shown,1024);
          CHECK(wcsstr(shown,L"|")==NULL && wcsstr(shown,L"\t")!=NULL); }
        int r7_wide_h=r7rec->body_h;
        int r7_wide_width=h->transcript.view_width;
        CHECK(r7_wide_width>0 && r7_wide_h>0);
        CHECK(r7rec->body_layout_width==r7_wide_width);
        CHECK(r7rec->body_layout_dpi==h->transcript.dpi);
        CHECK(r7rec->body_layout_theme==h->transcript.theme_epoch);
        uint64_t r7_rev=r7rec->body_revision;
        RichTextTheme r7_saved_theme=h->transcript.theme;
        RECT r7_original; GetWindowRect(h->view,&r7_original);

        /* (1) A narrower control width re-flattens the table to a taller
               line structure and restamps the assistant currency. */
        SetWindowPos(h->view,NULL,0,0,420,
            r7_original.bottom-r7_original.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        render_transcript(h);
        CHECK(h->transcript.view_width<r7_wide_width);
        CHECK(r7rec->body_layout_width==h->transcript.view_width);
        CHECK(r7rec->body_h>r7_wide_h);
        { wchar_t shown[1024]; body_text(h,r7,shown,1024);
          CHECK(wcsstr(shown,L"|")==NULL && wcsstr(shown,L"\t")!=NULL); }

        /* (2) A selection defers the width-driven re-flatten: the displayed
               text and its currency stay until the selection clears, then the
               current-width layout is applied. */
        int r7_displayed_width=r7rec->body_layout_width;
        SendMessageW(body_window(h,r7),EM_SETSEL,0,3);
        SetWindowPos(h->view,NULL,0,0,300,
            r7_original.bottom-r7_original.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        render_transcript(h);
        CHECK(r7rec->body_pending && r7rec->blocked_debt);
        CHECK(r7rec->body_layout_width==r7_displayed_width);
        CHECK(r7rec->body_layout_width!=h->transcript.view_width);
        CHECK(r7rec->body_revision==r7_rev);
        { wchar_t shown[1024]; body_text(h,r7,shown,1024);
          CHECK(wcsstr(shown,L"|")==NULL && wcsstr(shown,L"\t")!=NULL); }
        SendMessageW(body_window(h,r7),EM_SETSEL,0,0);
        CHECK(!r7rec->body_pending);
        CHECK(r7rec->body_layout_width==h->transcript.view_width);

        /* (3) A DPI change with an unchanged message revision re-flattens and
               restamps; the theme epoch participates in the same currency. */
        SetWindowPos(h->view,NULL,0,0,
            r7_original.right-r7_original.left,
            r7_original.bottom-r7_original.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        render_transcript(h);
        CHECK(r7rec->body_revision==r7_rev);
        transcript_set_dpi(&h->transcript,120.0f);
        render_transcript(h);
        CHECK(r7rec->body_layout_dpi==120.0f);
        CHECK(r7rec->body_layout_width==h->transcript.view_width);
        CHECK(r7rec->body_revision==r7_rev);
        transcript_set_dpi(&h->transcript,96.0f);
        render_transcript(h);
        CHECK(r7rec->body_layout_dpi==96.0f);
        uint32_t r7_epoch=h->transcript.theme_epoch;
        RichTextTheme r7_changed=h->transcript.theme;
        r7_changed.ui_size+=1.0f;
        transcript_set_theme(&h->transcript,&r7_changed);
        render_transcript(h);
        CHECK(h->transcript.theme_epoch!=r7_epoch);
        CHECK(r7rec->body_layout_theme==h->transcript.theme_epoch);

        /* (3b) A DPI change while the reader holds a selection is deferred
               exactly like a width change: the displayed layout and its
               currency stay, then the new DPI layout applies once the
               selection clears. */
        int r7_dpi_before=(int)r7rec->body_layout_dpi;
        uint64_t r7_dpi_rev=r7rec->body_revision;
        SendMessageW(body_window(h,r7),EM_SETSEL,0,3);
        transcript_set_dpi(&h->transcript,120.0f);
        render_transcript(h);
        CHECK(r7rec->body_pending && r7rec->blocked_debt);
        CHECK((int)r7rec->body_layout_dpi==r7_dpi_before);
        CHECK(r7rec->body_revision==r7_dpi_rev);
        SendMessageW(body_window(h,r7),EM_SETSEL,0,0);
        CHECK(!r7rec->body_pending);
        CHECK((int)r7rec->body_layout_dpi==120);
        transcript_set_dpi(&h->transcript,96.0f);
        render_transcript(h);
        CHECK((int)r7rec->body_layout_dpi==96);

        /* (3c) A theme change under selection is likewise deferred and only
               restamps after the selection clears. */
        uint32_t r7_theme_before=r7rec->body_layout_theme;
        RichTextTheme r7_theme2=h->transcript.theme;
        r7_theme2.ui_size+=1.0f;
        SendMessageW(body_window(h,r7),EM_SETSEL,0,3);
        transcript_set_theme(&h->transcript,&r7_theme2);
        render_transcript(h);
        CHECK(r7rec->body_pending && r7rec->blocked_debt);
        CHECK(r7rec->body_layout_theme==r7_theme_before);
        CHECK(r7rec->body_layout_theme!=h->transcript.theme_epoch);
        SendMessageW(body_window(h,r7),EM_SETSEL,0,0);
        CHECK(!r7rec->body_pending);
        CHECK(r7rec->body_layout_theme==h->transcript.theme_epoch);

        /* (4) An unchanged non-assistant body is never perpetually stale:
               currency does not apply, so a selection there is never deferred
               and no destructive rewrite is attempted. */
        int r7_user=r7-1;
        TranscriptRecord *r7u=&h->transcript.records[r7_user];
        CHECK(r7u->role==CHAT_ROLE_USER);
        CHECK(r7u->body_layout_width==0);
        CHECK(transcript_surface(&h->transcript,r7_user,TRANSCRIPT_BODY)!=NULL);
        SendMessageW(body_window(h,r7_user),EM_SETSEL,0,2);
        for (int rep=0;rep<3;rep++) render_transcript(h);
        CHECK(!r7u->body_pending && !r7u->blocked_debt);
        SendMessageW(body_window(h,r7_user),EM_SETSEL,0,0);
        render_transcript(h);

        /* Restore the shared theme for the remaining checks. */
        transcript_set_theme(&h->transcript,&r7_saved_theme);
        render_transcript(h);

        /* (5) Conversation switching: leaving and returning to the table's
               conversation leaves the flattened body current at the same
               width, with no outstanding debt. */
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        add_turn(chat,L"switch away",L"switch away answer",NULL,-1);
        render_transcript(h);
        command(h,CHAT_COMMAND_SELECT,r7_conv);
        render_transcript(h);
        CHECK(r7rec->body_layout_width==h->transcript.view_width);
        CHECK(!r7rec->body_pending);
        { wchar_t shown[2048]; body_text(h,r7,shown,2048);
          CHECK(wcsstr(shown,L"|")==NULL && wcsstr(shown,L"\t")!=NULL); }
    }

    /* ---- R8: a mixed body whose over-wide table falls back to literal keeps
            the surrounding Markdown and a later semantic link (regression 5).
            The default view is wide enough that only the 24-column maximum
            cannot satisfy its minimum layout. ---- */
    {
        command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
        wchar_t mixed[2048];
        wcscpy(mixed,L"Lead **bold**\n\n|");
        for (int c=0;c<24;c++) wcscat(mixed,L" h |");
        wcscat(mixed,L"\n|");
        for (int c=0;c<24;c++) wcscat(mixed,L" --- |");
        wcscat(mixed,L"\n|");
        for (int c=0;c<24;c++) wcscat(mixed,L" v |");
        wcscat(mixed,L"\n\nTail [x](https://example.com/host)");
        int r8=add_turn(chat,L"r8 question",mixed,NULL,-1);
        render_transcript(h);
        CHECK(transcript_reveal_turn(&h->transcript,feed_arg(h),r8));
        RichTextControl *body=transcript_surface(&h->transcript,r8,TRANSCRIPT_BODY);
        CHECK(body);
        wchar_t shown[4096]; body_text(h,r8,shown,4096);
        CHECK(wcsstr(shown,L"| h | h | h | h |")!=NULL);  /* table literal */
        CHECK(wcsstr(shown,L"Lead")!=NULL && wcsstr(shown,L"Tail x")!=NULL);
        CHECK(body->link_count==1);               /* later semantic link kept */
        FINDTEXTW find;
        memset(&find,0,sizeof find);
        find.chrg.cpMin=0; find.chrg.cpMax=-1;
        find.lpstrText=L"| h | h |";
        LONG table_cp=(LONG)SendMessageW(body->window,EM_FINDTEXTW,1,
            (LPARAM)&find);
        CHECK(table_cp>=0);
        PARAFORMAT2 pf; memset(&pf,0,sizeof pf); pf.cbSize=sizeof pf;
        SendMessageW(body->window,EM_SETSEL,table_cp,table_cp+1);
        SendMessageW(body->window,EM_GETPARAFORMAT,0,(LPARAM)&pf);
        CHECK(pf.cTabCount==0);                   /* literal: ordinary paragraph */
        memset(&find,0,sizeof find);
        find.chrg.cpMin=0; find.chrg.cpMax=-1;
        find.lpstrText=L"Tail x";
        LONG link_cp=(LONG)SendMessageW(body->window,EM_FINDTEXTW,1,
            (LPARAM)&find);
        CHECK(link_cp>=0);
        link_cp+=5;                               /* "x" */
        CHARFORMAT2W cf; memset(&cf,0,sizeof cf); cf.cbSize=sizeof cf;
        SendMessageW(body->window,EM_SETSEL,link_cp,link_cp+1);
        SendMessageW(body->window,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
        CHECK(cf.dwEffects & CFE_LINK);
        memset(&cf,0,sizeof cf); cf.cbSize=sizeof cf;
        SendMessageW(body->window,EM_SETSEL,5,6);  /* "bold" */
        SendMessageW(body->window,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&cf);
        CHECK(cf.dwEffects & CFE_BOLD);
    }

    /* Convergence bookkeeping: no cap fallback, no degraded settle. */
    CHECK(h->transcript.stat.fallback_rounds==fallback_before+1);
    CHECK(h->transcript.stat.degraded_rounds==0);

    /* ---- Teardown (same ownership order as the default fixture) ----
        A focused transcript surface first: WM_CLOSE transfers focus to the
        top-level window immediately before DestroyWindow, so no focused
        transcript child is ever destroyed, and the kill-focus reaches the
        tracked state before the hierarchy collapses. */
    { HWND body=body_window(h,1); CHECK(body); SetFocus(body);
      CHECK(h->transcript.focus_window==body); }
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window) && !IsWindow(h->view));
    CHECK(h->transcript.focus_window==NULL);
    saver_shutdown(&h->saver);
    storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background);
    for (int s = 0; s < h->transcript.slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            CHECK(!IsWindow(h->transcript.slots[s].surface[k].window));
    transcript_dispose(&h->transcript);
    CHECK(!h->transcript.slots && !h->transcript.slot_capacity);
    rich_text_library_close();
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Backend selection / Ollama suite ------------------------------------ */

static int backend_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Backend host",1100,720,720,480,"test-key",false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-backend-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Backend integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    catalog_request_calls=0; catalog_request_generation=0; picker_pump_calls=0;

    /* Defaults: OpenRouter with the historical model as the active model. */
    CHECK(chat->backend==CHAT_BACKEND_OPENROUTER);
    CHECK(!wcscmp(chat_active_model(chat),chat->model));
    CHECK(!wcscmp(chat_backend_name(CHAT_BACKEND_OPENROUTER),L"OpenRouter") &&
        !wcscmp(chat_backend_name(CHAT_BACKEND_OLLAMA),L"Ollama"));

    /* apply_model targets the active backend's remembered slot. During a
       pending switch it writes the target slot without touching the active
       one, and leaves the field alone until the switch commits. */
    h->backend_target=CHAT_BACKEND_OPENROUTER;
    CHECK(apply_model(h,L"openrouter/model"));
    CHECK(!wcscmp(chat->model,L"openrouter/model") && !chat->ollama_model[0]);
    h->backend_target=CHAT_BACKEND_OLLAMA; h->backend_switch_pending=true;
    CHECK(apply_model(h,L"local/model:latest"));
    CHECK(!wcscmp(chat->ollama_model,L"local/model:latest") &&
        !wcscmp(chat->model,L"openrouter/model"));
    h->backend_switch_pending=false; h->backend_target=chat->backend;

    /* Generation metadata records the backend and the active model. */
    add_turn(chat,L"hello",L"answer",NULL,-1);
    render_transcript(h);
    begin_regenerate(h);
    CHECK(pending(h)->generation.backend==CHAT_BACKEND_OPENROUTER);
    CHECK(!wcscmp(pending(h)->generation.requested_model,L"openrouter/model"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));

    /* A remembered Ollama model switches directly, and the next request is
       dispatched to Ollama with no provider-routing object. */
    select_backend(h,CHAT_BACKEND_OLLAMA);
    CHECK(chat->backend==CHAT_BACKEND_OLLAMA);
    CHECK(!wcscmp(chat_active_model(chat),L"local/model:latest"));
    completion_request_fake_generation=5151; completion_request_calls=0;
    start_response(h,CHAT_REGENERATE,NULL);
    completion_request_fake_generation=0;
    CHECK(completion_request_calls==1);
    CHECK(completion_request_last_backend==CHAT_BACKEND_OLLAMA);
    CHECK(!completion_request_last_had_routing);
    CHECK(h->generating && h->request_generation==5151);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(pending(h)->generation.backend==CHAT_BACKEND_OLLAMA);

    /* Missing OPENROUTER_API_KEY never blocks Ollama, but still blocks
       OpenRouter before any client call. */
    h->config.api_key_utf8=NULL;
    completion_request_fake_generation=6161; completion_request_calls=0;
    start_response(h,CHAT_REGENERATE,NULL);
    completion_request_fake_generation=0;
    CHECK(completion_request_calls==1);
    CHECK(completion_request_last_backend==CHAT_BACKEND_OLLAMA);
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    select_backend(h,CHAT_BACKEND_OPENROUTER);
    completion_request_calls=0;
    start_response(h,CHAT_REGENERATE,NULL);
    /* The client is consulted but refuses to start without a key. */
    CHECK(completion_request_calls==1);
    CHECK(!h->generating);
    CHECK(pending(h)->generation.state==CHAT_GENERATION_FAILED);
    CHECK(wcsstr(pending(h)->generation.error,L"OPENROUTER_API_KEY")!=NULL);
    h->config.api_key_utf8="test-key";

    /* Provider routing is clearly ignored while Ollama is active, and the
       routing menu items are grayed. */
    chat->backend=CHAT_BACKEND_OLLAMA;
    chat->provider_routing.zdr=false;
    action(h,ACTION_ROUTING_ZDR);
    CHECK(!chat->provider_routing.zdr);
    CHECK(wcsstr(chat->status,L"OpenRouter only")!=NULL);
    {
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,ACTION_BACKEND_OPENROUTER,L"OpenRouter");
        AppendMenuW(menu,MF_STRING,ACTION_BACKEND_OLLAMA,L"Ollama");
        AppendMenuW(menu,MF_STRING,ACTION_ROUTING_ZDR,L"zdr");
        chat_actions_sync_routing(menu,chat);
        CHECK((GetMenuState(menu,ACTION_BACKEND_OLLAMA,MF_BYCOMMAND)&MF_CHECKED)!=0);
        CHECK((GetMenuState(menu,ACTION_BACKEND_OPENROUTER,MF_BYCOMMAND)&MF_CHECKED)==0);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)!=0);
        chat->backend=CHAT_BACKEND_OPENROUTER;
        chat_actions_sync_routing(menu,chat);
        CHECK((GetMenuState(menu,ACTION_BACKEND_OPENROUTER,MF_BYCOMMAND)&MF_CHECKED)!=0);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)==0);
        DestroyMenu(menu);
    }

    /* Switching to Ollama with no remembered model opens the picker and does
       not commit the switch; accepting commits and stores the selection. */
    select_backend(h,CHAT_BACKEND_OPENROUTER);
    chat->ollama_model[0]=0;
    h->backend_target=chat->backend;
    select_backend(h,CHAT_BACKEND_OLLAMA);
    CHECK(chat->backend==CHAT_BACKEND_OPENROUTER);   /* pump did not accept */
    CHECK(!h->backend_switch_pending);
    /* Complete the catalogue fetch the cancelled switch started. */
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));
    h->backend_target=CHAT_BACKEND_OLLAMA; h->backend_switch_pending=true;
    begin_model_picker(h);
    CHECK(h->open_picker);
    CHECK(!wcscmp(current_model(chat,CHAT_BACKEND_OLLAMA),L""));
    model_picker_set_selected(h->open_picker,L"llama3.2:latest");
    model_picker_accept(h->open_picker);
    end_model_picker(h);
    CHECK(chat->backend==CHAT_BACKEND_OLLAMA);
    CHECK(!wcscmp(chat->ollama_model,L"llama3.2:latest"));
    { wchar_t shown[CHAT_MODEL_TEXT]; rich_text_get_text(&h->field,shown,CHAT_MODEL_TEXT);
      CHECK(!wcscmp(shown,L"llama3.2:latest")); }
    /* Complete the accept's fetch so the client is not left busy. */
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));

    /* Separate last-good catalogs and status per backend. */
    chat->backend=CHAT_BACKEND_OPENROUTER; h->backend_target=CHAT_BACKEND_OPENROUTER;
    catalog_request_calls=0;
    begin_model_picker(h);
    CHECK(catalog_request_calls==1);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
    CHECK(h->catalog[CHAT_BACKEND_OPENROUTER].count==3 &&
        h->catalog_loaded[CHAT_BACKEND_OPENROUTER]);
    CHECK(h->open_picker &&
        !wcscmp(model_picker_match_id(h->open_picker,0),L"openrouter/model"));
    model_picker_cancel(h->open_picker);
    end_model_picker(h);
    /* A fresh Ollama open fetches Ollama's own catalogue and leaves the
       OpenRouter one untouched. */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OLLAMA]);
    h->catalog_loaded[CHAT_BACKEND_OLLAMA]=false;
    h->catalog_failed[CHAT_BACKEND_OLLAMA]=false;
    chat->backend=CHAT_BACKEND_OLLAMA; h->backend_target=CHAT_BACKEND_OLLAMA;
    catalog_request_calls=0;
    begin_model_picker(h);
    CHECK(catalog_request_calls==1);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));
    CHECK(h->catalog[CHAT_BACKEND_OLLAMA].count==2 &&
        h->catalog[CHAT_BACKEND_OPENROUTER].count==3);
    CHECK(!wcscmp(h->catalog[CHAT_BACKEND_OLLAMA].items[0].id,L"llama3.2:latest"));
    CHECK(!wcscmp(h->catalog[CHAT_BACKEND_OPENROUTER].items[0].id,L"openai/gpt-4"));
    CHECK(h->open_picker &&
        !wcscmp(model_picker_match_id(h->open_picker,0),L"llama3.2:latest"));
    model_picker_cancel(h->open_picker);
    end_model_picker(h);

    /* Cross-backend history isolation: a backend's picker never shows the
       other backend's recent models, and chat_remember_model tags the active
       backend while preserving each backend's MRU subsequence. */
    {
        wcsncpy(chat->model_history[0],L"openrouter/only",CHAT_MODEL_TEXT-1);
        chat->model_history_backend[0]=CHAT_BACKEND_OPENROUTER;
        wcsncpy(chat->model_history[1],L"local/only",CHAT_MODEL_TEXT-1);
        chat->model_history_backend[1]=CHAT_BACKEND_OLLAMA;
        chat->model_history_count=2;
        build_picker_source(h,CHAT_BACKEND_OPENROUTER);
        CHECK(chat_model_catalog_contains(&h->picker_source,L"openrouter/only"));
        CHECK(!chat_model_catalog_contains(&h->picker_source,L"local/only"));
        build_picker_source(h,CHAT_BACKEND_OLLAMA);
        CHECK(chat_model_catalog_contains(&h->picker_source,L"local/only"));
        CHECK(!chat_model_catalog_contains(&h->picker_source,L"openrouter/only"));
        chat->model_history_count=0;
        chat->backend=CHAT_BACKEND_OPENROUTER;
        wcscpy(chat->model,L"or/one"); chat_remember_model(chat);
        wcscpy(chat->model,L"or/two"); chat_remember_model(chat);
        chat->backend=CHAT_BACKEND_OLLAMA;
        wcscpy(chat->ollama_model,L"local/one"); chat_remember_model(chat);
        CHECK(chat->model_history_count==3);
        CHECK(chat->model_history_backend[0]==CHAT_BACKEND_OLLAMA &&
            !wcscmp(chat->model_history[0],L"local/one"));
        CHECK(chat->model_history_backend[1]==CHAT_BACKEND_OPENROUTER &&
            !wcscmp(chat->model_history[1],L"or/two"));
        CHECK(chat->model_history_backend[2]==CHAT_BACKEND_OPENROUTER &&
            !wcscmp(chat->model_history[2],L"or/one"));
        chat->model_history_count=0;
        chat->backend=CHAT_BACKEND_OPENROUTER;
    }

    /* Catalog hand-off: an OpenRouter fetch that settles while the Ollama
       picker is open queues Ollama's own fetch without a reopen. */
    {
        chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]);
        h->catalog_loaded[CHAT_BACKEND_OPENROUTER]=false;
        h->catalog_failed[CHAT_BACKEND_OPENROUTER]=false;
        chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OLLAMA]);
        h->catalog_loaded[CHAT_BACKEND_OLLAMA]=false;
        h->catalog_failed[CHAT_BACKEND_OLLAMA]=false;
        chat->backend=CHAT_BACKEND_OPENROUTER; h->backend_target=CHAT_BACKEND_OPENROUTER;
        catalog_request_calls=0;
        begin_model_picker(h);
        CHECK(catalog_request_calls==1 &&
            h->catalog_loading && h->catalog_backend==CHAT_BACKEND_OPENROUTER);
        /* The OR worker is still running while the user opens Ollama. */
        parked_release=CreateEventW(NULL,TRUE,FALSE,NULL);
        CHECK(parked_release);
        { uintptr_t thread=_beginthreadex(NULL,0,parked_worker,parked_release,0,NULL);
          CHECK(thread);
          h->catalog_client.thread=(HANDLE)thread; }
        model_picker_cancel(h->open_picker);
        end_model_picker(h);
        h->backend_target=CHAT_BACKEND_OLLAMA;
        begin_model_picker(h);
        CHECK(h->open_picker && catalog_request_calls==1);
        /* The OpenRouter completion settles; Ollama's fetch is now queued. */
        SetEvent(parked_release); CloseHandle(parked_release); parked_release=NULL;
        catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
        CHECK(h->catalog[CHAT_BACKEND_OPENROUTER].count==3);
        CHECK(catalog_request_calls==2 &&
            h->catalog_loading && h->catalog_backend==CHAT_BACKEND_OLLAMA);
        catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));
        CHECK(h->catalog[CHAT_BACKEND_OLLAMA].count==2);
        CHECK(h->open_picker &&
            chat_model_catalog_contains(&h->picker_source,L"llama3.2:latest") &&
            chat_model_catalog_contains(&h->picker_source,L"qwen2.5:7b"));
        model_picker_cancel(h->open_picker);
        end_model_picker(h);
    }

    /* Metadata names the backend and marks local generations. */
    {
        int index=add_turn(chat,L"meta q",L"meta answer",NULL,-1);
        ChatMessage *m=&chat->conversations[chat->active].messages[index];
        wchar_t meta[512];
        m->generation.backend=CHAT_BACKEND_OLLAMA;
        m->generation.cost=0.0;
        wcscpy(m->generation.requested_model,L"local/model");
        wcscpy(m->generation.actual_model,L"local/model");
        chat_message_touch(m);
        render_transcript(h);
        meta_text(h,index,meta,512);
        CHECK(wcsstr(meta,L"Ollama \u00b7 local/model")!=NULL);
        CHECK(wcsstr(meta,L"local")!=NULL);
        CHECK(wcsstr(meta,L"$0.00000")==NULL);
        m->generation.backend=CHAT_BACKEND_OPENROUTER;
        chat_message_touch(m);
        render_transcript(h);
        meta_text(h,index,meta,512);
        CHECK(wcsstr(meta,L"OpenRouter \u00b7 local/model")!=NULL);
        CHECK(wcsstr(meta,L"$0.00000")!=NULL);
    }

    saver_shutdown(&h->saver);
    model_catalog_shutdown(&h->catalog_client);
    storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript); rich_text_library_close();
    for (int i=0;i<CHAT_BACKEND_COUNT;i++) chat_model_catalog_dispose(&h->catalog[i]);
    chat_model_catalog_dispose(&h->picker_source);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

int main(void) {
    int failed=default_suite();
    if (failed) return failed;
    failed=seam_toggle_suite();
    if (failed) return failed;
    failed=bounded_suite();
    if (failed) return failed;
    failed=catalog_suite();
    if (failed) return failed;
    failed=backend_suite();
    if (failed) return failed;
    puts("Hidden host (default + bounded + catalog + backend fixtures) passed");
    return failed;
}
