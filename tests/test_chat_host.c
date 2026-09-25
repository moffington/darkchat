/* Hidden HWND integration of the real host, lifecycle and storage. */
/* COBJMACROS must precede the first UIA header (pulled in through the host),
    so the palette suite can drive the popup's provider with C macros. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
/* Confirmation seam: MessageBoxW is dllimport, so --wrap cannot intercept it.
    The macro redirects every host call site (the host's own translation unit
    is included below) to this deterministic stub instead of opening a real
    dialog on the desktop. */
static int message_box_result = IDYES;
static int host_MessageBoxW_stub(HWND window, LPCWSTR text, LPCWSTR caption,
    UINT type) {
    (void)window; (void)text; (void)caption; (void)type;
    return message_box_result;
}
#define MessageBoxW host_MessageBoxW_stub
#include "chat/shell/chat_host_win32.c"
#include <uiautomationclient.h>
#include <uiautomationcoreapi.h>
#include "chat/generation/provider_routing.h"
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
/* Scalar summaries of the ordered-part view the client was handed: the
   borrowed ChatRequestPart runs die with the per-send request context, so the
   seam records what it saw instead of aliasing it. */
static int completion_request_last_part_counts[CHAT_CONTEXT_MAX_ENTRIES];
static size_t completion_request_last_image_bytes[CHAT_CONTEXT_MAX_ENTRIES];
static int completion_request_fake_generation;   /* 0: delegate to the real client */
static ChatProviderRouting completion_request_last_routing;
static ChatBackend completion_request_last_backend;
static bool completion_request_last_had_routing;
static bool completion_request_last_reasoning;
static wchar_t completion_request_last_model[CHAT_MODEL_TEXT];
int __real_completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning);
int __wrap_completion_request(CompletionClient *client, ChatBackend backend,
    const char *api_key_utf8, const wchar_t *model,
    const CompletionMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning) {
    ++completion_request_calls;
    completion_request_last_count=count;
    completion_request_last_backend=backend;
    completion_request_last_reasoning=reasoning;
    if (model) {
        wcsncpy(completion_request_last_model, model, CHAT_MODEL_TEXT - 1);
        completion_request_last_model[CHAT_MODEL_TEXT - 1] = 0;
    } else completion_request_last_model[0] = 0;
    for (int i=0;i<count && i<CHAT_CONTEXT_MAX_ENTRIES;i++) {
        completion_request_last_roles[i]=messages[i].role;
        completion_request_last_texts[i]=messages[i].text;
        completion_request_last_part_counts[i]=messages[i].part_count;
        completion_request_last_image_bytes[i]=0;
        for (int p=0;p<messages[i].part_count;p++)
            if (messages[i].parts[p].kind==CHAT_PART_IMAGE)
                completion_request_last_image_bytes[i]+=
                    messages[i].parts[p].u.image.byte_length;
    }
    completion_request_last_had_routing=routing!=NULL;
    if (routing) completion_request_last_routing=*routing;
    else chat_provider_routing_init(&completion_request_last_routing);
    if (completion_request_fake_generation) return completion_request_fake_generation;
    return __real_completion_request(client,backend,api_key_utf8,model,messages,
        count,routing,reasoning);
}
/* Catalog seams (linked with -Wl,--wrap=model_catalog_request and
   -Wl,--wrap=palette_popup_pump): the fetch is counted but never starts a
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
   disarmed (-1) they forward to the CRT. `alloc_fail_malloc_size` narrows the
   malloc seam to one allocation size (0 = any), so a test can fail exactly the
   per-send request context without arming every earlier malloc on the path. */
static long alloc_fail_malloc=-1, alloc_fail_realloc=-1;
static size_t alloc_fail_malloc_size=0;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size) {
    if (alloc_fail_malloc>=0 &&
        (alloc_fail_malloc_size==0 || size==alloc_fail_malloc_size)) {
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
/* Palette seam (linked with -Wl,--wrap=palette_popup_pump): the modal pump
   returns at once so the suite can drive open/filter/accept/cancel and
   inspect the popup's own state between steps without blocking. Both the
   command and the model palette share this seam. */
static int palette_pump_calls;
void __real_palette_popup_pump(PalettePopup *popup);
void __wrap_palette_popup_pump(PalettePopup *popup) {
    (void)popup;
    ++palette_pump_calls;
}
/* Code-copy seam (linked with -Wl,--wrap=chat_copy_text): records the text the
   host hands to the clipboard helper without touching the real clipboard. */
static wchar_t copied_text[8192];
static int copy_calls;
static bool copy_result = true;
bool __real_chat_copy_text(HWND owner, const wchar_t *text);
bool __wrap_chat_copy_text(HWND owner, const wchar_t *text) {
    (void)owner;
    ++copy_calls;
    wcsncpy(copied_text, text, 8191);
    copied_text[8191] = 0;
    return copy_result;
}
/* Tray/notification seams (linked with -Wl,--wrap=chat_shell_notify and
   -Wl,--wrap=chat_foreground_window). The real tray helpers build the
   NOTIFYICONDATAW, so the wrapper records the operation and the whole
   structure: flags, id, callback message, icon and version are asserted, not
   just the call count. The foreground query is injected so the trigger is
   deterministic in a headless window. */
#define NOTIFY_CALL_CAPACITY 64
static struct {
    DWORD operation;
    NOTIFYICONDATAW data;
} notify_calls[NOTIFY_CALL_CAPACITY];
static int notify_count;
static bool notify_result = true;
static int notify_fail_index = -1;   /* 0-based call to fail; -1 never */
bool __real_chat_shell_notify(NOTIFYICONDATAW *data, DWORD operation);
bool __wrap_chat_shell_notify(NOTIFYICONDATAW *data, DWORD operation) {
    int index = notify_count;
    if (notify_count < NOTIFY_CALL_CAPACITY) {
        notify_calls[notify_count].operation = operation;
        notify_calls[notify_count].data = *data;
    }
    ++notify_count;
    if (index == notify_fail_index) return false;
    return notify_result;
}
static void notify_reset(void) {
    notify_count = 0;
    notify_result = true;
    notify_fail_index = -1;
}
static int notify_op_count(DWORD operation) {
    int count = 0;
    for (int i = 0; i < notify_count && i < NOTIFY_CALL_CAPACITY; i++)
        if (notify_calls[i].operation == operation) count++;
    return count;
}
static NOTIFYICONDATAW *last_balloon(void) {
    int top = notify_count < NOTIFY_CALL_CAPACITY ? notify_count
                                                  : NOTIFY_CALL_CAPACITY;
    for (int i = top - 1; i >= 0; i--)
        if (notify_calls[i].operation == NIM_MODIFY) return &notify_calls[i].data;
    return NULL;
}
static HWND foreground_window_override;
static bool foreground_window_forced;
HWND __real_chat_foreground_window(void);
HWND __wrap_chat_foreground_window(void) {
    return foreground_window_forced ? foreground_window_override
                                    : __real_chat_foreground_window();
}
static int set_foreground_calls;
bool __real_chat_set_foreground(HWND window);
bool __wrap_chat_set_foreground(HWND window) {
    (void)window;
    ++set_foreground_calls;
    return true;
}
/* IME seam (linked with -Wl,--wrap=chat_ime_active): the thread's real input
   locale is machine-dependent, so the Ctrl+Space guard is driven through a
   deterministic override instead. Default is "no IME", which keeps the
   existing Ctrl+Space palette expectations valid everywhere. */
static bool ime_forced, ime_value;
bool __real_chat_ime_active(void);
bool __wrap_chat_ime_active(void) {
    return ime_forced ? ime_value : false;
}
/* Edit-dialog seam (linked with -Wl,--wrap=chat_edit_dialog): dialog-opening
    actions (per-conversation prompts, profile save/edit) are driven
    deterministically. Each call consumes the next queued answer; with the
    queue exhausted the dialog is declined, which is the cancel path. */
static wchar_t edit_dialog_answers[8][CHAT_COMPOSER_TEXT];
static int edit_dialog_answer_count, edit_dialog_answer_head;
static wchar_t edit_dialog_last_title[128];
static int edit_dialog_calls;
bool __real_chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text,
    size_t capacity, bool multiline);
bool __wrap_chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text,
    size_t capacity, bool multiline) {
    (void)owner; (void)multiline;
    ++edit_dialog_calls;
    wcsncpy(edit_dialog_last_title, title, 127);
    edit_dialog_last_title[127] = 0;
    if (edit_dialog_answer_head >= edit_dialog_answer_count) return false;
    wcsncpy(text, edit_dialog_answers[edit_dialog_answer_head++],
        capacity - 1);
    text[capacity - 1] = 0;
    return true;
}
static void edit_queue_clear(void) {
    edit_dialog_answer_count = edit_dialog_answer_head = 0;
    edit_dialog_calls = 0;
}
static void edit_queue_push(const wchar_t *text) {
    if (edit_dialog_answer_count < 8)
        wcscpy(edit_dialog_answers[edit_dialog_answer_count++], text);
}
/* Save-dialog seam (linked with -Wl,--wrap=chat_save_dialog): each call
    consumes the next queued path/result. With the queue exhausted the dialog
    is declined, which is the cancel path. */
static wchar_t save_dialog_paths[8][512];
static ChatFileDialogResult save_dialog_results[8];
static int save_dialog_count, save_dialog_head, save_dialog_calls;
static wchar_t save_dialog_last_default[512];
ChatFileDialogResult __real_chat_save_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, const wchar_t *default_ext,
    const wchar_t *default_name, wchar_t *path, size_t capacity);
ChatFileDialogResult __wrap_chat_save_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, const wchar_t *default_ext,
    const wchar_t *default_name, wchar_t *path, size_t capacity) {
    (void)owner; (void)title; (void)filter; (void)default_ext;
    ++save_dialog_calls;
    wcsncpy(save_dialog_last_default, default_name ? default_name : L"", 511);
    save_dialog_last_default[511] = 0;
    if (save_dialog_head >= save_dialog_count) return CHAT_FILE_DIALOG_CANCELLED;
    ChatFileDialogResult result = save_dialog_results[save_dialog_head];
    const wchar_t *queued = save_dialog_paths[save_dialog_head++];
    if (result == CHAT_FILE_DIALOG_ACCEPTED) {
        wcsncpy(path, queued, capacity - 1);
        path[capacity - 1] = 0;
    }
    return result;
}
static void save_queue_clear(void) {
    save_dialog_count = save_dialog_head = save_dialog_calls = 0;
}
static void save_queue_push(const wchar_t *path, ChatFileDialogResult result) {
    if (save_dialog_count >= 8) return;
    wcsncpy(save_dialog_paths[save_dialog_count], path, 511);
    save_dialog_paths[save_dialog_count][511] = 0;
    save_dialog_results[save_dialog_count++] = result;
}
/* Export-writer seam (linked with -Wl,--wrap=chat_write_file_utf8): records
    the host's payload and, when armed, fails without touching the disk. The
    writer's own atomic guarantees are tested directly against
    __real_chat_write_file_utf8 in export_suite(). */
static int write_file_calls;
static bool write_file_fail;
static wchar_t write_file_last_path[512];
static size_t write_file_last_length;
static char write_file_capture[8192];
bool __real_chat_write_file_utf8(const wchar_t *path, const char *data,
    size_t length);
bool __wrap_chat_write_file_utf8(const wchar_t *path, const char *data,
    size_t length) {
    ++write_file_calls;
    wcsncpy(write_file_last_path, path, 511); write_file_last_path[511] = 0;
    write_file_last_length = length;
    size_t copy = data ? (length < 8191 ? length : 8191) : 0;
    if (copy) memcpy(write_file_capture, data, copy);
    write_file_capture[copy] = 0;
    if (write_file_fail) return false;
    return __real_chat_write_file_utf8(path, data, length);
}
/* Open-dialog seam (linked with -Wl,--wrap=chat_open_dialog): each call
   consumes the next queued path/result; an exhausted queue is a cancel. */
static wchar_t open_dialog_paths[8][512];
static ChatFileDialogResult open_dialog_results[8];
static int open_dialog_count, open_dialog_head, open_dialog_calls;
ChatFileDialogResult __real_chat_open_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, wchar_t *path, size_t capacity);
ChatFileDialogResult __wrap_chat_open_dialog(HWND owner, const wchar_t *title,
    const wchar_t *filter, wchar_t *path, size_t capacity) {
    (void)owner; (void)title; (void)filter;
    ++open_dialog_calls;
    if (open_dialog_head >= open_dialog_count)
        return CHAT_FILE_DIALOG_CANCELLED;
    ChatFileDialogResult result = open_dialog_results[open_dialog_head];
    const wchar_t *queued = open_dialog_paths[open_dialog_head++];
    if (result == CHAT_FILE_DIALOG_ACCEPTED) {
        wcsncpy(path, queued, capacity - 1);
        path[capacity - 1] = 0;
    }
    return result;
}
static void open_queue_clear(void) {
    open_dialog_count = open_dialog_head = open_dialog_calls = 0;
}
static void open_queue_push(const wchar_t *path, ChatFileDialogResult result) {
    if (open_dialog_count >= 8) return;
    wcsncpy(open_dialog_paths[open_dialog_count], path, 511);
    open_dialog_paths[open_dialog_count][511] = 0;
    open_dialog_results[open_dialog_count++] = result;
}
/* File-reader seam (linked with -Wl,--wrap=chat_read_file_utf8): serves a
   fixed in-memory payload or a queued failure. The reader's own boundary
   behavior is tested directly against __real_chat_read_file_utf8_limited. */
static char read_payload[65536];
static size_t read_payload_length;
static ChatFileReadResult read_result = CHAT_FILE_READ_OK;
static int read_calls;
ChatFileReadResult __real_chat_read_file_utf8(const wchar_t *path, char **data,
    size_t *length);
ChatFileReadResult __wrap_chat_read_file_utf8(const wchar_t *path, char **data,
    size_t *length) {
    (void)path;
    ++read_calls;
    *data = NULL;
    *length = 0;
    if (read_result != CHAT_FILE_READ_OK) return read_result;
    char *copy = (char *)malloc(read_payload_length + 1);
    if (!copy) return CHAT_FILE_READ_OOM;
    memcpy(copy, read_payload, read_payload_length);
    copy[read_payload_length] = 0;
    *data = copy;
    *length = read_payload_length;
    return CHAT_FILE_READ_OK;
}
static void read_set(const char *bytes, size_t length,
    ChatFileReadResult result) {
    if (length > sizeof read_payload) length = sizeof read_payload;
    if (bytes) memcpy(read_payload, bytes, length);
    read_payload_length = length;
    read_result = result;
    read_calls = 0;
}
/* Export-timestamp seam (linked with -Wl,--wrap=chat_export_timestamp): a
    fixed value keeps host export tests byte-deterministic. */
static int64_t export_timestamp_value = 1700000000000LL;
int64_t __real_chat_export_timestamp(void);
int64_t __wrap_chat_export_timestamp(void) { return export_timestamp_value; }
/* Confirmation seam lives above the host include (MessageBoxW macro
    redirection must precede the host's call sites). */
/* Identity helpers over the popup's visible rows: the controller labels are
   display text, so id assertions resolve the stable row key instead. */
static const wchar_t *palette_row_id(PalettePopup *popup, size_t index) {
    static wchar_t buffer[CHAT_MODEL_TEXT];
    PaletteRowKey key;
    if (!popup || !palette_popup_row_key(popup,index,&key)) return NULL;
    wcsncpy(buffer,key.id,CHAT_MODEL_TEXT-1); buffer[CHAT_MODEL_TEXT-1]=0;
    return buffer;
}
static const wchar_t *palette_highlighted_id(PalettePopup *popup) {
    static wchar_t buffer[CHAT_MODEL_TEXT];
    if (!palette_popup_highlighted_model(popup,buffer)) buffer[0]=0;
    return buffer;
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
    /* ---- Content parts ride the borrowed request view ------------------- */
    {
        /* A user turn carrying one managed image: the send must still start,
           and the client must see the projection text plus a two-part run
           (TEXT + IMAGE) whose image budget size comes from the attachment
           record with no blob loaded. */
        Chat *chat=h->config.chat;
        ChatAttachmentMeta rec={0};
        rec.id=1;
        for (int i=0;i<64;i++) rec.digest[i]='a';
        rec.digest[64]=0;
        strcpy(rec.mime,"image/png");
        rec.bytes=1234;
        rec.created_at=1;
        wcscpy(rec.display_name,L"photo.png");
        CHECK(chat_attachment_add(chat,&rec));
        ChatImagePart img={0};
        img.attachment_id=1; img.pixel_width=2; img.pixel_height=3;
        strcpy(img.mime,"image/png");
        wcscpy(img.display_name,L"photo.png");
        ChatConversation *c=&chat->conversations[0];
        int shown=chat_append(chat,CHAT_ROLE_USER,L"look at this");
        CHECK(chat_message_add_image(&c->messages[shown],&img,0));
        int seen=chat_append(chat,CHAT_ROLE_ASSISTANT,L"seen it");
        c->messages[seen].generation.state=CHAT_GENERATION_COMPLETE;
        int calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"and now?");
        completion_request_fake_generation=5151;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);          /* exactly one send */
        CHECK(h->generating && h->request_generation==5151);
        CHECK(completion_request_last_count==4);           /* Q0, shown, seen, trigger */
        CHECK(completion_request_last_roles[1]==CHAT_ROLE_USER &&
              !wcscmp(completion_request_last_texts[1],L"look at this"));
        CHECK(completion_request_last_part_counts[1]==2);   /* TEXT + IMAGE */
        CHECK(completion_request_last_image_bytes[1]==1234);
        CHECK(completion_request_last_part_counts[0]==0 &&
              completion_request_last_part_counts[2]==0 &&
              completion_request_last_part_counts[3]==0);
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        CHECK(!h->generating);
        /* Restore the state the following checks expect. */
        c=&chat->conversations[0];
        for (size_t i=2;i<c->message_count;i++) chat_message_dispose(&c->messages[i]);
        c->message_count=2;
        chat->system_prompt[0]=0; c->draft[0]=0;
        chat_attachment_prune(chat,NULL,0);
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=0;
        h->generating=false; h->context_dropped=0; h->request_generation=0;
        render_transcript(h);
    }
    /* ---- Per-send context allocation failure and diagnostic precedence --- */
    {
        /* The request context is heap-allocated per send: failing exactly
           that allocation fails the turn cleanly with a named error, sends
           nothing, and leaves the turn retryable. */
        Chat *chat=h->config.chat;
        int calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"oom question");
        alloc_fail_malloc=0;
        alloc_fail_malloc_size=sizeof(ChatRequestContext);
        perform_send(h);
        alloc_fail_malloc=-1; alloc_fail_malloc_size=0;
        CHECK(completion_request_calls==calls);            /* nothing sent */
        CHECK(!h->generating && h->request_generation==0 && h->context_dropped==0);
        { ChatConversation *c=&chat->conversations[chat->active];
          size_t last=c->message_count-1;
          CHECK(c->messages[last].generation.state==CHAT_GENERATION_FAILED);
          CHECK(wcsstr(c->messages[last].generation.error,
              L"Out of memory building the request context")!=NULL);
          CHECK(wcsstr(c->messages[last].generation.error,
              L"request was not sent")!=NULL); }
        CHECK(wcsstr(chat->status,L"Request failed")!=NULL);
        /* A save failure outranks the allocation diagnostic: the same send
           with the durability gate broken reports the save, not the OOM. */
        HANDLE block=CreateFileW(h->storage.temporary,GENERIC_WRITE,0,NULL,
            OPEN_ALWAYS,0,NULL);
        CHECK(block!=INVALID_HANDLE_VALUE);
        calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"gate and oom question");
        alloc_fail_malloc=0;
        alloc_fail_malloc_size=sizeof(ChatRequestContext);
        perform_send(h);
        alloc_fail_malloc=-1; alloc_fail_malloc_size=0;
        CHECK(completion_request_calls==calls);            /* nothing sent */
        { ChatConversation *c=&chat->conversations[chat->active];
          size_t last=c->message_count-1;
          CHECK(c->messages[last].generation.state==CHAT_GENERATION_FAILED);
          CHECK(wcsstr(c->messages[last].generation.error,
              L"Could not save pending response")!=NULL); }
        CloseHandle(block);
        save(h);
        CHECK(wait_save_settled(h,5000));
        /* Restore the state the following checks expect. */
        ChatConversation *c=&chat->conversations[chat->active];
        for (size_t i=2;i<c->message_count;i++) chat_message_dispose(&c->messages[i]);
        c->message_count=2;
        chat->system_prompt[0]=0; c->draft[0]=0;
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=0;
        h->generating=false; h->context_dropped=0; h->request_generation=0;
        render_transcript(h);
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
    /* ---- Conversation overrides and prompt profiles ---- */
    {
        /* The model override reaches the request: the client sees the
            conversation's model and the audit metadata records the same
            resolution, and the field/palette path targets the override
            while it is active. */
        CHECK(chat_conversation_set_model(chat,chat->active,
            CHAT_BACKEND_OPENROUTER,L"override/model"));
        sync_model_field(h);   /* what the UI actions do on override change */
        int calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"override question");
        completion_request_fake_generation=8484;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);
        CHECK(!wcscmp(completion_request_last_model,L"override/model"));
        CHECK(!wcscmp(pending(h)->generation.requested_model,
            L"override/model"));
        CHECK(h->generating && h->request_generation==8484);
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        CHECK(!h->generating);
        /* The chip marks the override by presence, not value. */
        chat_ui_sync(&h->chat_ui);
        { UiNode *chip=ui_node(h->config.ui,h->chat_ui.model);
          CHECK(chip && wcsstr(chip->help_text,L"\u00b7 this chat")!=NULL); }
        /* The field edits the override while it is active; the global slot
            is untouched. */
        rich_text_set_text(&h->field,L"typed/model");
        sync_model(h);
        CHECK(!wcscmp(chat->conversations[chat->active].model,
            L"typed/model"));
        CHECK(!wcscmp(chat->model,L"openai/gpt-4o-mini"));
        /* The palette targets the override too, and highlights the
            effective model when opened. */
        h->dirty=false;
        CHECK(apply_model(h,L"palette/model") &&
            !wcscmp(chat->conversations[chat->active].model,
                L"palette/model"));
        CHECK(!wcscmp(chat->model,L"openai/gpt-4o-mini"));
        CHECK(h->dirty);
        wcscpy(chat->model_history[0],L"palette/model");
        chat->model_history_backend[0]=CHAT_BACKEND_OPENROUTER;
        chat->model_history_count=1;
        begin_model_palette(h);
        CHECK(h->open_palette);
        { wchar_t highlighted[CHAT_MODEL_TEXT];
          CHECK(palette_popup_highlighted_model(h->open_palette,highlighted));
          CHECK(!wcscmp(highlighted,L"palette/model")); }
        palette_popup_cancel(h->open_palette);
        end_palette(h);
        /* Clearing the override hands the field back to the global slot. */
        CHECK(chat_conversation_set_model(chat,chat->active,
            CHAT_BACKEND_OPENROUTER,L""));
        sync_model_field(h);
        chat_ui_sync(&h->chat_ui);
        { UiNode *chip=ui_node(h->config.ui,h->chat_ui.model);
          CHECK(chip && wcsstr(chip->help_text,L"\u00b7 this chat")==NULL); }
        rich_text_set_text(&h->field,L"global/typed");
        sync_model(h);
        CHECK(!wcscmp(chat->model,L"global/typed"));
        CHECK(chat->conversations[chat->active].model[0]==0);
        CHECK(apply_model(h,L"openai/gpt-4o-mini") &&
            !wcscmp(chat->model,L"openai/gpt-4o-mini"));
        /* The prompt override reaches the request in all three states. */
        wcscpy(chat->system_prompt,L"Global persona.");
        CHECK(chat_conversation_apply_system_prompt(chat,chat->active,
            L"Override persona."));
        calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"persona question");
        completion_request_fake_generation=8485;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);
        CHECK(completion_request_last_count>=1);
        CHECK(completion_request_last_roles[0]==CHAT_ROLE_SYSTEM &&
            !wcscmp(completion_request_last_texts[0],L"Override persona."));
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        /* The deliberately-empty override sends no system message. */
        CHECK(chat_conversation_apply_system_prompt(chat,chat->active,L""));
        calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"empty persona question");
        completion_request_fake_generation=8486;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);
        CHECK(completion_request_last_roles[0]!=CHAT_ROLE_SYSTEM);
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        /* Clearing inherits the global prompt again. */
        CHECK(chat_conversation_set_system_prompt(chat,chat->active,L""));
        calls=completion_request_calls;
        rich_text_set_text(&h->composer,L"inherited persona question");
        completion_request_fake_generation=8487;
        perform_send(h);
        completion_request_fake_generation=0;
        CHECK(completion_request_calls==calls+1);
        CHECK(completion_request_last_roles[0]==CHAT_ROLE_SYSTEM &&
            !wcscmp(completion_request_last_texts[0],L"Global persona."));
        handle_event(h,fixture(h,COMPLETION_DONE,NULL));
        /* Trim back to the fixture shape the following checks expect. The
            global prompt stays set: the profile tests below capture it. */
        ChatConversation *oc=&chat->conversations[chat->active];
        for (size_t i=2;i<oc->message_count;i++)
            chat_message_dispose(&oc->messages[i]);
        oc->message_count=2;
        oc->draft[0]=0;
        rich_text_set_text(&h->composer,L"");
        h->request_message=1; h->request_conversation=chat->active;
        h->generating=false; h->context_dropped=0; h->request_generation=0;
        render_transcript(h);

        /* A conversation override must not swallow the just-picked model
            during a pending switch: the switch seeds the global slot and
            aligns the existing override, so the committed backend uses the
            model the user selected. Driven through the real end_model_palette
            path below, together with the backend suite's first-Ollama-switch
            fixture. */

        /* Profile actions, driven through action() with the dialog and
            confirmation seams. Save captures the effective prompt. */
        edit_queue_clear();
        message_box_result=IDYES;
        edit_queue_push(L"My Profile");
        action(h,ACTION_PROFILE_SAVE);
        CHECK(chat->profile_count==1 &&
            !wcscmp(chat->profiles[0].name,L"My Profile"));
        CHECK(!wcscmp(chat_text_value(&chat->profiles[0].prompt),
            L"Global persona."));
        /* A duplicate name is rejected with a status and no second profile. */
        edit_queue_push(L"My Profile");
        action(h,ACTION_PROFILE_SAVE);
        CHECK(chat->profile_count==1);
        CHECK(wcsstr(chat->status,L"Could not save the profile")!=NULL);
        /* Apply to the global prompt, verbatim. */
        action(h,CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE);
        CHECK(!wcscmp(chat->system_prompt,L"Global persona."));
        /* Apply to this conversation: the text override lands. */
        action(h,CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE);
        CHECK(chat->conversations[chat->active].system_prompt.data!=NULL &&
            !wcscmp(chat_text_value(
                &chat->conversations[chat->active].system_prompt),
                L"Global persona."));
        /* An empty profile applied here is a real empty override. */
        CHECK(chat_profile_add(chat,L"Silent",L"")==1);
        action(h,CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE+1);
        CHECK(chat->conversations[chat->active].system_prompt.data==NULL &&
            chat->conversations[chat->active].system_prompt_present);
        /* A stale dynamic id (beyond the live count) does nothing. */
        action(h,CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE+23);
        CHECK(chat->conversations[chat->active].system_prompt_present);
        /* Edit renames and re-prompts through two dialogs; cancel after the
            first dialog changes nothing. */
        edit_queue_clear();
        action(h,CHAT_ACTION_DYNAMIC_EDIT_BASE);
        CHECK(edit_dialog_calls==1 &&
            !wcscmp(chat->profiles[0].name,L"My Profile"));
        edit_queue_push(L"Renamed");
        action(h,CHAT_ACTION_DYNAMIC_EDIT_BASE);
        /* Two dialogs ran (the declined prompt counts too); the accepted
            name alone changes nothing. */
        CHECK(edit_dialog_calls==3 && edit_dialog_answer_head==1 &&
            !wcscmp(chat->profiles[0].name,L"My Profile"));
        edit_queue_push(L"Renamed");
        edit_queue_push(L"Better prompt");
        action(h,CHAT_ACTION_DYNAMIC_EDIT_BASE);
        CHECK(!wcscmp(chat->profiles[0].name,L"Renamed") &&
            !wcscmp(chat_text_value(&chat->profiles[0].prompt),
                L"Better prompt"));
        /* A rename colliding with the other profile fails cleanly. */
        edit_queue_push(L"Silent");
        edit_queue_push(L"x");
        action(h,CHAT_ACTION_DYNAMIC_EDIT_BASE);
        CHECK(!wcscmp(chat->profiles[0].name,L"Renamed"));
        /* Delete confirms through the box; declining keeps the profile. */
        message_box_result=IDNO;
        action(h,CHAT_ACTION_DYNAMIC_DELETE_BASE);
        CHECK(chat->profile_count==2);
        message_box_result=IDYES;
        action(h,CHAT_ACTION_DYNAMIC_DELETE_BASE);
        CHECK(chat->profile_count==1 &&
            !wcscmp(chat->profiles[0].name,L"Silent"));
        /* Canceled dialogs return before the save tail: the override and
            the status stay untouched. */
        edit_queue_clear();
        { wchar_t before[CHAT_STATUS_TEXT];
          wcscpy(before,chat->status);
          action(h,ACTION_SYSTEM_HERE);
          CHECK(!wcscmp(chat->status,before) &&
              chat->conversations[chat->active].system_prompt.data==NULL &&
              chat->conversations[chat->active].system_prompt_present); }
        /* The per-conversation prompt dialog applies an explicit empty. */
        edit_queue_push(L"");
        action(h,ACTION_SYSTEM_HERE);
        CHECK(chat->conversations[chat->active].system_prompt_present);
        edit_queue_push(L"This chat only");
        action(h,ACTION_SYSTEM_HERE);
        CHECK(!wcscmp(chat_text_value(
            &chat->conversations[chat->active].system_prompt),
            L"This chat only"));
        /* Use global clears the override; a stale no-op dispatch on a
            submenu header changes nothing. */
        action(h,ACTION_SYSTEM_CLEAR_HERE);
        CHECK(chat->conversations[chat->active].system_prompt.data==NULL &&
            !chat->conversations[chat->active].system_prompt_present);
        action(h,ACTION_PROFILE_APPLY);
        action(h,ACTION_PROFILE_EDIT);
        action(h,ACTION_PROFILE_DELETE);
        CHECK(chat->profile_count==1);
        /* "Use model for this chat" copies the global model into the
            override; clearing restores inherit. */
        action(h,ACTION_MODEL_USE_HERE);
        CHECK(!wcscmp(chat->conversations[chat->active].model,
            chat->model));
        action(h,ACTION_MODEL_CLEAR_HERE);
        CHECK(chat->conversations[chat->active].model[0]==0);
        /* Cleanup: drop the library, restore the global prompt and the
            fixture shape the following checks expect. */
        chat_profile_remove(chat,0);
        CHECK(chat->profile_count==0);
        chat->system_prompt[0]=0;
        chat_conversation_set_system_prompt(chat,chat->active,L"");
        chat->model_history_count=0;
        oc->messages[1].generation.state=CHAT_GENERATION_FAILED;
        mark_dirty(h);
    }
    /* ---- Overflow menu structure: dynamic profile submenus ---- */
    /* The dispatch tests above drive dynamic ids directly; this block
        exercises chat_actions_menu() itself: the nested submenu structure,
        the item ids, the escaped labels and the header enablement that the
        implementation depends on through MIIM_ID assignment. */
    {
        CHECK(chat_profile_add(chat,L"R&D",L"be terse")==0);
        CHECK(chat_profile_add(chat,L"Plain",L"")==1);
        HMENU bar=chat_actions_menu(chat);
        CHECK(bar);
        /* Conversation, Response, Settings, Customization, Data. */
        CHECK(GetMenuItemCount(bar)==5);
        MENUITEMINFOW top; memset(&top,0,sizeof top);
        top.cbSize=sizeof top; top.fMask=MIIM_SUBMENU;
        CHECK(GetMenuItemInfoW(bar,3,TRUE,&top));
        HMENU custom=top.hSubMenu;
        CHECK(custom);
        /* Eight registry entries plus three flagged separators. */
        CHECK(GetMenuItemCount(custom)==11);
        static const struct { int id; bool submenu; bool separator; }
            expected[] = {
            { ACTION_MODEL_USE_HERE, false, false },
            { ACTION_MODEL_CLEAR_HERE, false, false },
            { 0, false, true },
            { ACTION_SYSTEM_HERE, false, false },
            { ACTION_SYSTEM_CLEAR_HERE, false, false },
            { 0, false, true },
            { ACTION_PROFILE_APPLY, true, false },
            { ACTION_PROFILE_SAVE, false, false },
            { 0, false, true },
            { ACTION_PROFILE_EDIT, true, false },
            { ACTION_PROFILE_DELETE, true, false },
        };
        for (UINT i=0;
            i<(UINT)GetMenuItemCount(custom) &&
            i<sizeof expected/sizeof expected[0];i++) {
            MENUITEMINFOW info; memset(&info,0,sizeof info);
            info.cbSize=sizeof info;
            info.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_FTYPE;
            CHECK(GetMenuItemInfoW(custom,i,TRUE,&info));
            CHECK(((info.fType & MFT_SEPARATOR)!=0) ==
                (int)expected[i].separator);
            if (expected[i].separator) { CHECK(info.wID==0); continue; }
            CHECK(info.wID==(UINT)expected[i].id);
            CHECK((info.hSubMenu!=NULL)==expected[i].submenu);
        }
        /* The Data submenu is the last top-level group: Markdown, JSON,
            separator, Export all, separator, Import JSON, Import Markdown.
            Availability leaves them enabled at idle and grays them while
            generating. */
        {
            CHECK(GetMenuItemInfoW(bar,4,TRUE,&top));
            HMENU data=top.hSubMenu;
            CHECK(data && GetMenuItemCount(data)==7);
            static const struct { int id; bool separator; } data_expected[] = {
                { ACTION_EXPORT_MARKDOWN, false },
                { ACTION_EXPORT_JSON, false },
                { 0, true },
                { ACTION_EXPORT_ALL, false },
                { 0, true },
                { ACTION_IMPORT_JSON, false },
                { ACTION_IMPORT_MARKDOWN, false },
            };
            for (UINT i=0;i<(UINT)GetMenuItemCount(data) &&
                i<sizeof data_expected/sizeof data_expected[0];i++) {
                MENUITEMINFOW info; memset(&info,0,sizeof info);
                info.cbSize=sizeof info; info.fMask=MIIM_ID|MIIM_FTYPE;
                CHECK(GetMenuItemInfoW(data,i,TRUE,&info));
                CHECK(((info.fType & MFT_SEPARATOR)!=0)==
                    (int)data_expected[i].separator);
                if (data_expected[i].separator) { CHECK(info.wID==0); continue; }
                CHECK(info.wID==(UINT)data_expected[i].id);
            }
            ChatActionContext data_ctx;
            chat_action_context_init(&data_ctx,chat);
            chat_actions_sync(data,&data_ctx);
            CHECK(!(GetMenuState(data,ACTION_EXPORT_MARKDOWN,MF_BYCOMMAND)&MF_GRAYED));
            CHECK(!(GetMenuState(data,ACTION_EXPORT_JSON,MF_BYCOMMAND)&MF_GRAYED));
            CHECK(!(GetMenuState(data,ACTION_EXPORT_ALL,MF_BYCOMMAND)&MF_GRAYED));
            CHECK(!(GetMenuState(data,ACTION_IMPORT_JSON,MF_BYCOMMAND)&MF_GRAYED));
            CHECK(!(GetMenuState(data,ACTION_IMPORT_MARKDOWN,MF_BYCOMMAND)&MF_GRAYED));
            data_ctx.generating=true;
            chat_actions_sync(data,&data_ctx);
            CHECK(GetMenuState(data,ACTION_EXPORT_MARKDOWN,MF_BYCOMMAND)&MF_GRAYED);
            CHECK(GetMenuState(data,ACTION_EXPORT_JSON,MF_BYCOMMAND)&MF_GRAYED);
            CHECK(GetMenuState(data,ACTION_EXPORT_ALL,MF_BYCOMMAND)&MF_GRAYED);
            CHECK(GetMenuState(data,ACTION_IMPORT_JSON,MF_BYCOMMAND)&MF_GRAYED);
            CHECK(GetMenuState(data,ACTION_IMPORT_MARKDOWN,MF_BYCOMMAND)&MF_GRAYED);
        }
        /* Registry-bound leaves compose native Label<Tab>Shortcut text so
            the menu right-aligns the binding; unbound leaves carry no tab. */
        {
            wchar_t label[CHAT_ACTION_LABEL_TEXT];
            MENUITEMINFOW info; memset(&info,0,sizeof info);
            info.cbSize=sizeof info; info.fMask=MIIM_STRING;
            info.dwTypeData=label; info.cch=CHAT_ACTION_LABEL_TEXT;
            CHECK(GetMenuItemInfoW(bar,ACTION_RENAME,FALSE,&info));
            wchar_t *tab=wcschr(label,L'\t');
            CHECK(tab && !wcscmp(tab,L"\tF2"));
            CHECK((size_t)(tab-label)==wcslen(L"&Rename..."));
            info.cch=CHAT_ACTION_LABEL_TEXT;   /* the call overwrites cch */
            CHECK(GetMenuItemInfoW(bar,ACTION_NEW,FALSE,&info));
            CHECK(wcschr(label,L'\t')==NULL);
        }
        /* The Apply submenu carries two nested popups, each with one item
            per live profile; names are mnemonic-escaped. */
        MENUITEMINFOW apply; memset(&apply,0,sizeof apply);
        apply.cbSize=sizeof apply;
        apply.fMask=MIIM_SUBMENU;
        CHECK(GetMenuItemInfoW(custom,ACTION_PROFILE_APPLY,FALSE,&apply));
        CHECK(apply.hSubMenu && GetMenuItemCount(apply.hSubMenu)==2);
        MENUITEMINFOW nested; memset(&nested,0,sizeof nested);
        nested.cbSize=sizeof nested; nested.fMask=MIIM_SUBMENU;
        CHECK(GetMenuItemInfoW(apply.hSubMenu,0,TRUE,&nested));
        HMENU global=nested.hSubMenu;
        CHECK(global && GetMenuItemCount(global)==2);
        CHECK(GetMenuItemInfoW(apply.hSubMenu,1,TRUE,&nested));
        HMENU here=nested.hSubMenu;
        CHECK(here && GetMenuItemCount(here)==2);
        for (int i=0;i<2;i++) {
            wchar_t label[128];
            MENUITEMINFOW leaf; memset(&leaf,0,sizeof leaf);
            leaf.cbSize=sizeof leaf;
            leaf.fMask=MIIM_ID|MIIM_STRING;
            leaf.dwTypeData=label; leaf.cch=128;
            CHECK(GetMenuItemInfoW(global,(UINT)i,TRUE,&leaf));
            CHECK(leaf.wID==(UINT)(CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE+i));
            CHECK(GetMenuItemInfoW(here,(UINT)i,TRUE,&leaf));
            CHECK(leaf.wID==(UINT)(CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE+i));
        }
        {
            wchar_t label[128];
            MENUITEMINFOW leaf; memset(&leaf,0,sizeof leaf);
            leaf.cbSize=sizeof leaf;
            leaf.fMask=MIIM_ID|MIIM_STRING;
            leaf.dwTypeData=label; leaf.cch=128;
            CHECK(GetMenuItemInfoW(global,0,TRUE,&leaf));
            CHECK(!wcscmp(label,L"R&&D"));
            leaf.cch=128;   /* the call overwrites cch with the copied count */
            CHECK(GetMenuItemInfoW(global,1,TRUE,&leaf));
            CHECK(!wcscmp(label,L"Plain"));
        }
        /* Edit and Delete submenus carry one item per profile. */
        MENUITEMINFOW edit; memset(&edit,0,sizeof edit);
        edit.cbSize=sizeof edit; edit.fMask=MIIM_SUBMENU;
        CHECK(GetMenuItemInfoW(custom,ACTION_PROFILE_EDIT,FALSE,&edit));
        CHECK(edit.hSubMenu && GetMenuItemCount(edit.hSubMenu)==2);
        { MENUITEMINFOW leaf; memset(&leaf,0,sizeof leaf);
          leaf.cbSize=sizeof leaf; leaf.fMask=MIIM_ID;
          CHECK(GetMenuItemInfoW(edit.hSubMenu,1,TRUE,&leaf));
          CHECK(leaf.wID==(UINT)(CHAT_ACTION_DYNAMIC_EDIT_BASE+1)); }
        MENUITEMINFOW del; memset(&del,0,sizeof del);
        del.cbSize=sizeof del; del.fMask=MIIM_SUBMENU;
        CHECK(GetMenuItemInfoW(custom,ACTION_PROFILE_DELETE,FALSE,&del));
        CHECK(del.hSubMenu && GetMenuItemCount(del.hSubMenu)==2);
        { MENUITEMINFOW leaf; memset(&leaf,0,sizeof leaf);
          leaf.cbSize=sizeof leaf; leaf.fMask=MIIM_ID;
          CHECK(GetMenuItemInfoW(del.hSubMenu,0,TRUE,&leaf));
          CHECK(leaf.wID==(UINT)(CHAT_ACTION_DYNAMIC_DELETE_BASE+0)); }
        /* Sync enables live profile leaves and headers, and grays stale
            indexes; with no profiles the headers gray. */
        ChatActionContext ctx;
        chat_action_context_init(&ctx,chat);
        chat_actions_sync(custom,&ctx);
        CHECK(!(GetMenuState(custom,ACTION_PROFILE_APPLY,MF_BYCOMMAND)&MF_GRAYED));
        CHECK(!(GetMenuState(custom,ACTION_PROFILE_EDIT,MF_BYCOMMAND)&MF_GRAYED));
        CHECK(!(GetMenuState(custom,ACTION_PROFILE_DELETE,MF_BYCOMMAND)&MF_GRAYED));
        CHECK(!(GetMenuState(custom,CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE,
            MF_BYCOMMAND)&MF_GRAYED));
        CHECK(GetMenuState(custom,CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE+2,
            MF_BYCOMMAND)&MF_GRAYED);
        CHECK(GetMenuState(custom,CHAT_ACTION_DYNAMIC_DELETE_BASE+7,
            MF_BYCOMMAND)&MF_GRAYED);
        CHECK(chat_profile_remove(chat,0) && chat_profile_remove(chat,0));
        CHECK(chat->profile_count==0);
        HMENU empty=chat_actions_menu(chat);
        CHECK(empty);
        CHECK(GetMenuItemInfoW(empty,3,TRUE,&top));
        HMENU empty_custom=top.hSubMenu;
        chat_action_context_init(&ctx,chat);
        chat_actions_sync(empty_custom,&ctx);
        CHECK(GetMenuState(empty_custom,ACTION_PROFILE_APPLY,MF_BYCOMMAND)&MF_GRAYED);
        CHECK(GetMenuState(empty_custom,ACTION_PROFILE_DELETE,MF_BYCOMMAND)&MF_GRAYED);
        CHECK(!(GetMenuState(empty_custom,ACTION_PROFILE_SAVE,MF_BYCOMMAND)&MF_GRAYED));
        DestroyMenu(empty);
        DestroyMenu(bar);
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
    CHECK(text[0]==0); /* paint waits for the shared stream-flush timer */
    flush_stream_body(h);
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
       immediately, rapid deltas stay within the throttle window, and later
       flushes append only the raw tail instead of reparsing accumulated
       Markdown. The terminal event performs one full render, reassembling
       markers fragmented across deltas. Stored message text keeps the raw
       markers throughout. */
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
      CHECK(!wcscmp(body,L"Title **bold** and `code` and\r\nnext **ope")); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **ope"));
    SendMessageW(body_window(h,stream_turn),EM_SETSEL,(WPARAM)-1,(LPARAM)-1);
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"n** ~~old and ``variable"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title **bold** and `code` and\r\nnext **open** ~~old and ``variable")); }
    CHECK(!wcscmp(pending(h)->text,
        L"# Title **bold** and `code` and\nnext **open** ~~old and ``variable"));
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,
        L"``~~\n~~~~\n~~fenced~~\n- [x] task\n~~~"));
    { wchar_t body[256]; body_text(h,stream_turn,body,256);
      CHECK(!wcscmp(body,L"Title **bold** and `code` and\r\nnext **open** ~~old and ``variable``~~\r\n~~~~\r\n~~fenced~~\r\n- [x] task\r\n~~~")); }
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
       literal, later fragments append plainly, and the terminal render
       carries the completed paragraph layout. ---- */
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
      CHECK(!wcscmp(body,L"\u258C - item")); }
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"\n>   - nested"));
    { wchar_t body[256]; body_text(h,nested_turn,body,256);
      CHECK(!wcscmp(body,L"\u258C - item\r\n>   - nested")); }
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
    /* ---- Table markdown streams through the same throttle: the header and
       subsequent rows remain literal while streaming, and the terminal
       render flattens the completed table and installs its tab stops. ---- */
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
      CHECK(wcsstr(body,L"| --- | --- |")!=NULL && wcsstr(body,L"\t")==NULL); }
    h->transcript.body_render_tick=0;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"| c | d |"));
    { wchar_t body[256]; body_text(h,table_stream_turn,body,256);
      CHECK(wcsstr(body,L"| c | d |")!=NULL && wcsstr(body,L"\t")==NULL); }
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
    flush_stream_body(h);
    { HWND vp=transcript_surface(&h->transcript,second,TRANSCRIPT_REASON)->window;
      CHECK(vp);
      SendMessageW(vp,EM_SETSEL,2,5);
      handle_event(h,fixture(h,COMPLETION_REASONING,L" gamma"));
      flush_stream_body(h);
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
       recorded revision must advance with it. Completion still owes the one
       terminal Markdown render; an active selection defers that render
       without disturbing the selection. */
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
      CHECK(turn->body_pending);
      CHARRANGE sel; memset(&sel,0,sizeof sel);
      SendMessageW(body,EM_EXGETSEL,0,(LPARAM)&sel);
      CHECK(sel.cpMin==1 && sel.cpMax==4);
      SendMessageW(body,EM_SETSEL,0,0);
      CHECK(!turn->body_pending); }
    /* Live reasoning survives collapse and reopen mid-stream: the collapsed
       viewport stops appending, reopening loads the accumulation, and the
       stream then resumes appending into that turn's own viewport. */
    begin_regenerate(h);
    click_row(h,second);
    CHECK(h->transcript.records[second].reason_live);
    handle_event(h,fixture(h,COMPLETION_REASONING,L"first"));
    handle_event(h,fixture(h,COMPLETION_REASONING,L" second"));
    flush_stream_body(h);
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
    flush_stream_body(h);
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
    flush_stream_body(h);
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
    /* Msftedit stays loaded for the process (see main). */
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
    transcript_dispose(&h->transcript); /* Msftedit stays loaded for the process (see main). */
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
    catalog_request_calls=0; catalog_request_generation=0; palette_pump_calls=0;

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
    catalog_request_calls=0; palette_pump_calls=0;
    action(h,ACTION_MODELS);
    CHECK(palette_pump_calls==1 && catalog_request_calls==1);
    CHECK(h->catalog_loading && h->catalog_generation>0 && !h->open_palette);
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
    begin_model_palette(h);
    CHECK(h->open_palette && catalog_request_calls==1);
    CHECK(palette_popup_row_count(h->open_palette)==3);
    palette_popup_set_query(h->open_palette,L"gpt");
    CHECK(palette_popup_row_count(h->open_palette)==1);
    CHECK(!wcscmp(palette_row_id(h->open_palette,0),L"openai/gpt-4"));
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_second_json,NULL));
    CHECK(h->open_palette);
    CHECK(!wcscmp(palette_popup_query(h->open_palette),L"gpt"));
    CHECK(palette_popup_row_count(h->open_palette)==1);
    palette_popup_set_query(h->open_palette,L"");
    palette_popup_set_selected_model(h->open_palette,L"openai/gpt-4");
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
    CHECK(!wcscmp(palette_highlighted_id(h->open_palette),L"openai/gpt-4"));

    /* A source refresh is transactional: an allocation failure inside the
       rebuild keeps the old source, the query, the selection and the display
       fully usable (the popup reports the failure instead of half-swapping). */
    {
        ChatModelCatalog big;
        chat_model_catalog_init(&big);
        ChatModelParseStats stats;
        CHECK(chat_model_catalog_parse(&big,catalog_five_json,&stats));
        size_t before=palette_popup_row_count(h->open_palette);
        CHECK(before==3);
        palette_popup_set_selected_model(h->open_palette,L"openai/gpt-4");
        CHECK(!wcscmp(palette_highlighted_id(h->open_palette),L"openai/gpt-4"));
        alloc_fail_malloc=0;
        CHECK(!palette_popup_set_models(h->open_palette,&big,NULL,NULL,0,
            L"oom"));
        alloc_fail_malloc=-1;
        CHECK(palette_popup_row_count(h->open_palette)==before);
        CHECK(palette_row_id(h->open_palette,before-1)!=NULL);
        CHECK(palette_row_id(h->open_palette,99)==NULL);
        CHECK(!wcscmp(palette_highlighted_id(h->open_palette),L"openai/gpt-4"));
        /* A later successful refresh works normally. */
        CHECK(palette_popup_set_models(h->open_palette,&big,NULL,NULL,0,
            L"reloaded"));
        CHECK(palette_popup_row_count(h->open_palette)==5);
        chat_model_catalog_dispose(&big);
    }

    /* Accepting applies the id to both values and marks dirty once. */
    palette_popup_set_selected_model(h->open_palette,L"anthropic/claude-3");
    h->dirty=false;
    palette_popup_accept(h->open_palette);
    end_palette(h);
    CHECK(!h->open_palette && h->model_applied && h->dirty);
    CHECK(!wcscmp(chat->model,L"anthropic/claude-3"));
    { wchar_t shown[CHAT_MODEL_TEXT]; rich_text_get_text(&h->field,shown,CHAT_MODEL_TEXT);
      CHECK(!wcscmp(shown,L"anthropic/claude-3")); }

    /* Cancelling applies nothing. */
    begin_model_palette(h);
    CHECK(h->open_palette && catalog_request_calls==1);
    palette_popup_cancel(h->open_palette);
    h->dirty=false;
    end_palette(h);
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
    begin_model_palette(h);
    CHECK(catalog_request_calls==1 && h->open_palette);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_NETWORK_ERROR,NULL,L"offline"));
    CHECK(h->catalog_failed[CHAT_BACKEND_OPENROUTER] && !h->catalog_loading && h->catalog[CHAT_BACKEND_OPENROUTER].count==0);
    CHECK(h->open_palette && palette_popup_row_count(h->open_palette)>=2);
    CHECK(!wcscmp(palette_row_id(h->open_palette,0),L"typed/model"));
    palette_popup_cancel(h->open_palette);
    end_palette(h);
    catalog_request_calls=0;
    begin_model_palette(h);
    CHECK(catalog_request_calls==1 && h->open_palette);
    palette_popup_cancel(h->open_palette);
    end_palette(h);

    /* A worker whose completion was lost (the event could not be allocated or
       posted) must be reaped so the next open starts a fresh request, without
       any manual model_catalog_complete. The wrapper leaves an already-finished
       thread handle, exactly as that worker does. */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]);
    h->catalog_loaded[CHAT_BACKEND_OPENROUTER]=false; h->catalog_failed[CHAT_BACKEND_OPENROUTER]=false;
    h->catalog_loading=false; h->catalog_generation=0;
    catalog_request_lost_worker=true;
    catalog_request_calls=0;
    begin_model_palette(h);
    CHECK(catalog_request_calls==1 && h->catalog_loading && h->open_palette);
    CHECK(h->catalog_client.thread!=NULL);   /* lost completion still held */
    palette_popup_cancel(h->open_palette);
    end_palette(h);
    begin_model_palette(h);
    CHECK(catalog_request_calls==2 && h->catalog_loading && h->open_palette);
    CHECK(h->catalog_client.thread!=NULL);   /* request #2's worker is unjoined */
    palette_popup_cancel(h->open_palette);
    end_palette(h);
    catalog_request_lost_worker=false;

    /* A close arriving while the picker is live is deferred until the picker
       has unwound, so the parent is never destroyed under the modal loop. */
    begin_model_palette(h);
    CHECK(h->open_palette);
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(IsWindow(window) && h->close_pending && !h->generating);
    end_palette(h);
    pump_messages(30);
    CHECK(!IsWindow(window));
    CHECK(!h->open_palette && !h->close_pending);

    saver_shutdown(&h->saver); storage_close(&h->storage);
    model_catalog_shutdown(&h->catalog_client);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript); /* Msftedit stays loaded for the process (see main). */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OPENROUTER]);
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
    /* Both storm widths stay below the readable-column cap, so each step
       genuinely re-wraps: a width at or above the cap would leave the
       content measure unchanged by design. */
    SetWindowPos(h->view,NULL,0,0,640,510,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
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
    /* Msftedit stays loaded for the process (see main). */
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
    catalog_request_calls=0; catalog_request_generation=0; palette_pump_calls=0;

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

    /* Composer button tooltips: the text mirrors each button's current action
       and hovering activates the tracking tooltip. */
    CHECK(h->tooltip!=NULL);
    CHECK(wcsstr(h->tooltip_text[0],L"Send the message")!=NULL);
    CHECK(wcsstr(h->tooltip_text[1],L"Reasoning is on")!=NULL);
    command(h,CHAT_COMMAND_TOGGLE_REASONING,-1);
    CHECK(wcsstr(h->tooltip_text[1],L"Reasoning is off")!=NULL);
    command(h,CHAT_COMMAND_TOGGLE_REASONING,-1);
    {
        UiRect r=chat_ui_rect(&h->chat_ui,h->chat_ui.reasoning);
        LPARAM at=MAKELPARAM(px(h,r.x+r.w/2),px(h,r.y+r.h/2));
        window_proc(h->window,WM_MOUSEMOVE,0,at);
        CHECK(h->tooltip_shown==1);
        CHECK(IsWindowVisible(h->tooltip));
        window_proc(h->window,WM_MOUSELEAVE,0,0);
        CHECK(h->tooltip_shown==-1);
        CHECK(!IsWindowVisible(h->tooltip));
        UiRect s=chat_ui_rect(&h->chat_ui,h->chat_ui.send);
        at=MAKELPARAM(px(h,s.x+s.w/2),px(h,s.y+s.h/2));
        window_proc(h->window,WM_MOUSEMOVE,0,at);
        CHECK(h->tooltip_shown==0);
        window_proc(h->window,WM_LBUTTONDOWN,0,at);
        CHECK(h->tooltip_shown==-1);
        /* Cancel the press so no send action fires and no capture is held. */
        window_proc(h->window,WM_CANCELMODE,0,0);
    }

    /* chat_actions_sync is the single enabled-state authority. Routing sync
       runs first purely for check/radio marks; the availability pass must
       still win, including during an OpenRouter generation. */
    {
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,ACTION_BACKEND_OPENROUTER,L"OpenRouter");
        AppendMenuW(menu,MF_STRING,ACTION_ROUTING_ZDR,L"zdr");
        ChatActionContext context;
        chat->backend=CHAT_BACKEND_OPENROUTER;
        chat_action_context_init(&context,chat);
        chat_actions_sync(menu,&context);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)==0);
        context.generating=true;
        chat_actions_sync(menu,&context);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)!=0);
        CHECK((GetMenuState(menu,ACTION_BACKEND_OPENROUTER,MF_BYCOMMAND)&MF_GRAYED)!=0);
        chat->backend=CHAT_BACKEND_OLLAMA;
        chat_action_context_init(&context,chat);
        chat_actions_sync(menu,&context);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)!=0);
        context.generating=true;
        chat_actions_sync(menu,&context);
        CHECK((GetMenuState(menu,ACTION_ROUTING_ZDR,MF_BYCOMMAND)&MF_GRAYED)!=0);
        chat->backend=CHAT_BACKEND_OPENROUTER;
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
    /* A conversation that already carries an Ollama override must not
       swallow the model the user picks for the first switch: the switch
       seeds the global slot (storage rejects an Ollama-active snapshot with
       an empty one) and aligns the existing override, so after the commit
       the effective model is exactly the selection. */
    CHECK(chat_conversation_set_model(chat,chat->active,CHAT_BACKEND_OLLAMA,
        L"stale/local"));
    h->backend_target=CHAT_BACKEND_OLLAMA; h->backend_switch_pending=true;
    begin_model_palette(h);
    CHECK(h->open_palette);
    CHECK(!wcscmp(global_model_for(chat,CHAT_BACKEND_OLLAMA),L""));
    palette_popup_set_selected_model(h->open_palette,L"llama3.2:latest");
    palette_popup_accept(h->open_palette);
    end_palette(h);
    CHECK(chat->backend==CHAT_BACKEND_OLLAMA);
    CHECK(!h->backend_switch_pending);
    CHECK(!wcscmp(chat->ollama_model,L"llama3.2:latest"));
    CHECK(!wcscmp(chat->conversations[chat->active].ollama_model,
        L"llama3.2:latest"));
    CHECK(!wcscmp(chat_effective_model(chat,chat_active(chat)),
        L"llama3.2:latest"));
    { wchar_t shown[CHAT_MODEL_TEXT]; rich_text_get_text(&h->field,shown,CHAT_MODEL_TEXT);
      CHECK(!wcscmp(shown,L"llama3.2:latest")); }
    /* The aligned override is conversation state; drop it so the following
       remember-model checks resolve through the global slots. */
    CHECK(chat_conversation_set_model(chat,chat->active,CHAT_BACKEND_OLLAMA,
        L""));
    /* Complete the accept's fetch so the client is not left busy. */
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));

    /* Separate last-good catalogs and status per backend. */
    chat->backend=CHAT_BACKEND_OPENROUTER; h->backend_target=CHAT_BACKEND_OPENROUTER;
    catalog_request_calls=0;
    begin_model_palette(h);
    CHECK(catalog_request_calls==1);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
    CHECK(h->catalog[CHAT_BACKEND_OPENROUTER].count==3 &&
        h->catalog_loaded[CHAT_BACKEND_OPENROUTER]);
    CHECK(h->open_palette &&
        !wcscmp(palette_row_id(h->open_palette,0),L"openrouter/model"));
    palette_popup_cancel(h->open_palette);
    end_palette(h);
    /* A fresh Ollama open fetches Ollama's own catalogue and leaves the
       OpenRouter one untouched. */
    chat_model_catalog_dispose(&h->catalog[CHAT_BACKEND_OLLAMA]);
    h->catalog_loaded[CHAT_BACKEND_OLLAMA]=false;
    h->catalog_failed[CHAT_BACKEND_OLLAMA]=false;
    chat->backend=CHAT_BACKEND_OLLAMA; h->backend_target=CHAT_BACKEND_OLLAMA;
    catalog_request_calls=0;
    begin_model_palette(h);
    CHECK(catalog_request_calls==1);
    catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));
    CHECK(h->catalog[CHAT_BACKEND_OLLAMA].count==2 &&
        h->catalog[CHAT_BACKEND_OPENROUTER].count==3);
    CHECK(!wcscmp(h->catalog[CHAT_BACKEND_OLLAMA].items[0].id,L"llama3.2:latest"));
    CHECK(!wcscmp(h->catalog[CHAT_BACKEND_OPENROUTER].items[0].id,L"openai/gpt-4"));
    CHECK(h->open_palette &&
        !wcscmp(palette_row_id(h->open_palette,0),L"llama3.2:latest"));
    palette_popup_cancel(h->open_palette);
    end_palette(h);

    /* Cross-backend history isolation: a backend's palette never shows the
       other backend's recent models, and chat_remember_model tags the active
       backend while preserving each backend's MRU subsequence. The host
       filters the history before the palette ever sees it. */
    {
        wchar_t history[CHAT_MODEL_HISTORY][CHAT_MODEL_TEXT];
        wcsncpy(chat->model_history[0],L"openrouter/only",CHAT_MODEL_TEXT-1);
        chat->model_history_backend[0]=CHAT_BACKEND_OPENROUTER;
        wcsncpy(chat->model_history[1],L"local/only",CHAT_MODEL_TEXT-1);
        chat->model_history_backend[1]=CHAT_BACKEND_OLLAMA;
        chat->model_history_count=2;
        int count=build_backend_history(h,CHAT_BACKEND_OPENROUTER,history);
        CHECK(count==1 && !wcscmp(history[0],L"openrouter/only"));
        count=build_backend_history(h,CHAT_BACKEND_OLLAMA,history);
        CHECK(count==1 && !wcscmp(history[0],L"local/only"));
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
        begin_model_palette(h);
        CHECK(catalog_request_calls==1 &&
            h->catalog_loading && h->catalog_backend==CHAT_BACKEND_OPENROUTER);
        /* The OR worker is still running while the user opens Ollama. */
        parked_release=CreateEventW(NULL,TRUE,FALSE,NULL);
        CHECK(parked_release);
        { uintptr_t thread=_beginthreadex(NULL,0,parked_worker,parked_release,0,NULL);
          CHECK(thread);
          h->catalog_client.thread=(HANDLE)thread; }
        palette_popup_cancel(h->open_palette);
        end_palette(h);
        h->backend_target=CHAT_BACKEND_OLLAMA;
        begin_model_palette(h);
        CHECK(h->open_palette && catalog_request_calls==1);
        /* The OpenRouter completion settles; Ollama's fetch is now queued. */
        SetEvent(parked_release); CloseHandle(parked_release); parked_release=NULL;
        catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_first_json,NULL));
        CHECK(h->catalog[CHAT_BACKEND_OPENROUTER].count==3);
        CHECK(catalog_request_calls==2 &&
            h->catalog_loading && h->catalog_backend==CHAT_BACKEND_OLLAMA);
        catalog_event(h,catalog_fixture(h,MODEL_CATALOG_OK,catalog_ollama_json,NULL));
        CHECK(h->catalog[CHAT_BACKEND_OLLAMA].count==2);
        bool found_llama=false, found_qwen=false;
        if (h->open_palette)
            for (size_t i=0;i<palette_popup_row_count(h->open_palette);i++) {
                const wchar_t *id=palette_row_id(h->open_palette,i);
                if (id && !wcscmp(id,L"llama3.2:latest")) found_llama=true;
                if (id && !wcscmp(id,L"qwen2.5:7b")) found_qwen=true;
            }
        CHECK(found_llama && found_qwen);
        palette_popup_cancel(h->open_palette);
        end_palette(h);
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
    transcript_dispose(&h->transcript); /* Msftedit stays loaded for the process (see main). */
    for (int i=0;i<CHAT_BACKEND_COUNT;i++) chat_model_catalog_dispose(&h->catalog[i]);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Command palette suite (separate clean fixture) ------------------------ */

/* Counts an element's children by walking the provider tree; Navigate
   returns S_OK with NULL when there are no more siblings. */
static size_t uia_children(IRawElementProviderFragment *from) {
    if (!from) return 0;
    IRawElementProviderFragment *child=NULL;
    if (FAILED(IRawElementProviderFragment_Navigate(from,
            NavigateDirection_FirstChild,&child)) || !child) return 0;
    size_t count=1;
    for (;;) {
        IRawElementProviderFragment *next=NULL;
        HRESULT hr=IRawElementProviderFragment_Navigate(child,
            NavigateDirection_NextSibling,&next);
        IRawElementProviderFragment_Release(child);
        if (FAILED(hr) || !next) return count;
        child=next; ++count;
    }
}

/* Resolves the popup's UI_SCROLL fragment: the scroll container is always the
   root's last child (command mode hides the model-only status label, so the
   visible sibling count differs between modes). Returns NULL when the tree
   does not have that shape. */
static IRawElementProviderFragment *palette_scroll_fragment(
    IRawElementProviderFragment *root_fragment) {
    if (!root_fragment) return NULL;
    IRawElementProviderFragment *scroll=NULL;
    if (FAILED(IRawElementProviderFragment_Navigate(root_fragment,
            NavigateDirection_LastChild,&scroll)) || !scroll) return NULL;
    return scroll;
}

static int palette_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Palette host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-palette-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Palette integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));
    palette_pump_calls=0;

    /* Ctrl+K opens the palette, pumps once, and closes it: the wrapped pump
       never finishes, so end_palette destroys the popup directly. */
    CHECK(host_shortcut(h,L'K',false,true,false));
    CHECK(palette_pump_calls==1);
    CHECK(!h->open_palette);
    /* An action that merely opens a popup must leave no dirty state. */
    CHECK(!h->dirty);

    /* Open/filter/accept/cancel through the seams: only available commands
       become rows, the filter narrows by label, and accept resolves the
       stable action id. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette && palette_popup_window(palette));
        CHECK(palette_popup_row_count(palette)>0);
        /* Empty conversation: ACTION_COPY has no response to copy, so it is
           excluded up front and navigation can never land on it. */
        bool copy_row=false;
        for (size_t i=0;i<palette_popup_row_count(palette);i++)
            if (!wcscmp(palette_popup_row_label(palette,i),L"Copy response"))
                copy_row=true;
        CHECK(!copy_row);
        /* Filtering is by label; clearing the query restores every row and
           every previously available row keeps its stable identity. */
        size_t before=palette_popup_row_count(palette);
        palette_popup_set_query(palette,L"rename");
        CHECK(palette_popup_row_count(palette)==1);
        CHECK(!wcscmp(palette_popup_row_label(palette,0),L"Rename..."));
        palette_popup_set_query(palette,L"");
        CHECK(palette_popup_row_count(palette)==before);
        /* Repeated filter/clear cycles must not accumulate section headers
           in the retained tree: every child of the scroll container is
           rebuilt, so the child census is stable. */
        {
            IRawElementProviderSimple *root=palette_popup_root_provider(palette);
            CHECK(root);
            IRawElementProviderFragment *fragment=NULL;
            CHECK(SUCCEEDED(IRawElementProviderSimple_QueryInterface(root,
                &IID_IRawElementProviderFragment,(void **)&fragment)));
            IRawElementProviderFragment *scroll=palette_scroll_fragment(fragment);
            CHECK(scroll);
            size_t stable=uia_children(scroll);
            CHECK(stable>0);
            for (int cycle=0;cycle<4;cycle++) {
                palette_popup_set_query(palette,cycle%2?L"":L"new");
            }
            CHECK(palette_popup_row_count(palette)==before);
            size_t after=uia_children(scroll);
            CHECK(after==stable);
            IRawElementProviderFragment_Release(scroll);
            IRawElementProviderFragment_Release(fragment);
            IRawElementProviderSimple_Release(root);
        }
        /* No match: the palette cannot be accepted. */
        palette_popup_set_query(palette,L"zzzznope");
        CHECK(palette_popup_row_count(palette)==0);
        palette_popup_accept(palette);
        CHECK(!palette_popup_accepted(palette));
        /* Accept after typing resolves the row's action id exactly once. */
        palette_popup_set_query(palette,L"sidebar width");
        CHECK(palette_popup_row_count(palette)==1);
        palette_popup_accept(palette);
        CHECK(palette_popup_accepted(palette));
        CHECK(palette_popup_action_id(palette)==ACTION_SIDEBAR);
        palette_popup_accept(palette);            /* idempotent */
        CHECK(palette_popup_action_id(palette)==ACTION_SIDEBAR);
        /* The pump re-enables the owner even when it never ran (the wrapped
           seam skipped it): a stranded disabled owner would dead-end input.
           This assertion drives the real pump directly. */
        EnableWindow(window,FALSE);
        __real_palette_popup_pump(palette);
        CHECK(IsWindowEnabled(window));
        palette_popup_destroy(palette);
    }

    /* The dimmer overlay is created with the palette: owned, layered,
       click-through, non-activating, hidden until the pump shows it, and
       destroyed on every exit path. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        HWND overlay=palette_popup_overlay(palette);
        CHECK(overlay && IsWindow(overlay) && overlay!=window &&
            overlay!=palette_popup_window(palette));
        LONG exstyle=(LONG)GetWindowLongW(overlay,GWL_EXSTYLE);
        CHECK((exstyle&WS_EX_LAYERED) && (exstyle&WS_EX_TRANSPARENT) &&
            (exstyle&WS_EX_NOACTIVATE) && (exstyle&WS_EX_TOOLWINDOW));
        CHECK(!IsWindowVisible(overlay));     /* parked until the pump */
        /* Showing the overlay can never take focus from the palette. */
        SetFocus(h->composer.window);
        ShowWindow(overlay,SW_SHOWNA);
        CHECK(GetFocus()==h->composer.window);
        { BYTE alpha=0; DWORD flags=0;
          CHECK(GetLayeredWindowAttributes(overlay,NULL,&alpha,&flags));
          CHECK((flags&LWA_ALPHA) && alpha==30); }   /* ~12% black */
        /* Every exit path removes it: direct destroy covers cancel/accept
           teardown; the pump tail hides it. */
        palette_popup_destroy(palette);
        CHECK(!IsWindow(overlay));
    }

    /* Placement: 580x420 DIP centered over the owner's client area, biased
       above vertical center; a small owner clamps the palette inside its own
       client area, and a DPI change re-places it around the new scale. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        UINT dpi=palette_popup_dpi(palette);
        CHECK(dpi==96);
        int margin=MulDiv(16,(int)dpi,96);
        int width=MulDiv(580,(int)dpi,96), height=MulDiv(420,(int)dpi,96);
        RECT client; GetClientRect(window,&client);
        POINT origin={0,0}; ClientToScreen(window,&origin);
        RECT area={origin.x,origin.y,origin.x+client.right,
            origin.y+client.bottom};
        int area_w=area.right-area.left, area_h=area.bottom-area.top;
        CHECK(width<=area_w-2*margin);      /* no clamp on the fixture size */
        RECT bounds=palette_popup_bounds(palette);
        CHECK(bounds.left==area.left+(area_w-width)/2);
        CHECK(bounds.top==area.top+(int)((float)(area_h-height)*0.40f));
        CHECK(bounds.right-bounds.left==width);
        CHECK(bounds.bottom-bounds.top==height);
        /* DPI change while open: re-scaled, re-centered, still usable. */
        SendMessageW(palette_popup_window(palette),WM_DPICHANGED,
            MAKEWPARAM(96,192),0);
        CHECK(palette_popup_dpi(palette)==192);
        margin=MulDiv(16,192,96);
        width=MulDiv(580,192,96); height=MulDiv(420,192,96);
        if (width>area_w-2*margin) width=area_w-2*margin;
        if (height>area_h-2*margin) height=area_h-2*margin;
        bounds=palette_popup_bounds(palette);
        CHECK(bounds.left==area.left+(area_w-width)/2);
        CHECK(bounds.top==area.top+(int)((float)(area_h-height)*0.40f));
        CHECK(bounds.right-bounds.left==width);
        CHECK(bounds.bottom-bounds.top==height);
        CHECK(palette_popup_overlay(palette) &&
            IsWindow(palette_popup_overlay(palette)));
        CHECK(palette_popup_row_count(palette)>0);
        /* Capture the overlay handle before teardown: the popup owns its
           whole Ui arena, so destroy returns it to the OS and any later
           dereference would fault. */
        HWND dying_overlay=palette_popup_overlay(palette);
        palette_popup_destroy(palette);
        CHECK(!IsWindow(dying_overlay));

        /* A small owner clamps the palette inside its own client area and
           keeps it centered above vertical center. */
        WNDCLASSW small_cls={0}; small_cls.lpfnWndProc=DefWindowProcW;
        small_cls.lpszClassName=L"DarkChat.PaletteSmall";
        CHECK(RegisterClassW(&small_cls) ||
            GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
        HWND small=CreateWindowW(L"DarkChat.PaletteSmall",L"Small",
            WS_OVERLAPPEDWINDOW,40,40,420,320,NULL,NULL,NULL,NULL);
        CHECK(small);
        palette=palette_popup_create(small,&context);
        CHECK(palette);
        dpi=palette_popup_dpi(palette);
        margin=MulDiv(16,(int)dpi,96);
        GetClientRect(small,&client);
        origin.x=origin.y=0; ClientToScreen(small,&origin);
        area.right=area.left=origin.x; area.bottom=area.top=origin.y;
        area.right+=client.right; area.bottom+=client.bottom;
        area_w=area.right-area.left; area_h=area.bottom-area.top;
        CHECK(MulDiv(580,(int)dpi,96)>area_w-2*margin);   /* clamp engages */
        width=area_w-2*margin; height=area_h-2*margin;
        bounds=palette_popup_bounds(palette);
        CHECK(bounds.left==area.left+margin);
        CHECK(bounds.top==area.top+(int)((float)(area_h-height)*0.40f));
        CHECK(bounds.right-bounds.left==width);
        CHECK(bounds.bottom-bounds.top==height);
        palette_popup_destroy(palette);
        DestroyWindow(small);
    }

    /* A disabled/unavailable command cannot be invoked through the popup:
       during generation the registry excludes it, so an accept lands on the
       nearest available row instead. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        context.generating=true;
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        bool stop_row=false;
        for (size_t i=0;i<palette_popup_row_count(palette);i++)
            if (!wcscmp(palette_popup_row_label(palette,i),L"Retry unsuccessful response"))
                stop_row=true;
        CHECK(!stop_row);
        palette_popup_destroy(palette);
    }

    /* Accepting through the popup dispatches exactly one action through the
       unchanged action() dispatcher. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        palette_popup_set_query(palette,L"new conversation");
        CHECK(palette_popup_row_count(palette)==1);
        palette_popup_accept(palette);
        palette_popup_destroy(palette);
        /* Applied by hand exactly as end_palette does (the wrapped pump skips
           the unwind), proving the dispatcher path once. */
        int conversations_before=chat->conversation_count;
        action(h,ACTION_NEW);
        CHECK(chat->conversation_count==conversations_before+1);
    }

    /* UIA exposure: the popup's own provider names the root and exposes the
       row buttons with Invoke, and activating a row through the provider
       fires exactly once. */
    {
        add_turn(chat,L"u1",L"a1",NULL,-1);
        ChatActionContext context;
        palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        IRawElementProviderSimple *root=palette_popup_root_provider(palette);
        CHECK(root);
        VARIANT value; VariantInit(&value);
        CHECK(SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(root,
            UIA_ControlTypePropertyId,&value)));
        CHECK(V_VT(&value)==VT_I4 &&
            V_I4(&value)==UIA_WindowControlTypeId);
        VariantClear(&value);
        IRawElementProviderFragment *fragment=NULL;
        CHECK(SUCCEEDED(IRawElementProviderSimple_QueryInterface(root,
            &IID_IRawElementProviderFragment,(void **)&fragment)));
        /* Rows live under the scroll container, not among the root's direct
           children: root -> query -> separator -> scroll -> rows. */
        IRawElementProviderFragment *scroll=palette_scroll_fragment(fragment);
        CHECK(scroll);
        IRawElementProviderFragment *child=NULL;
        CHECK(SUCCEEDED(IRawElementProviderFragment_Navigate(scroll,
            NavigateDirection_FirstChild,&child)) && child);
        /* Find the "New conversation" row by walking the scroll's children
           (the first child is the section header label). */
        IRawElementProviderFragment *row=NULL;
        for (IRawElementProviderFragment *it=child; it && !row;) {
            IRawElementProviderSimple *simple=NULL;
            CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(it,
                &IID_IRawElementProviderSimple,(void **)&simple)));
            VARIANT name; VariantInit(&name);
            bool is_new=false;
            if (SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(simple,
                    UIA_NamePropertyId,&name)) && V_VT(&name)==VT_BSTR)
                is_new=!wcscmp(V_BSTR(&name),L"New conversation");
            VariantClear(&name);
            if (is_new) row=it;
            else {
                IRawElementProviderFragment *next=NULL;
                CHECK(SUCCEEDED(IRawElementProviderFragment_Navigate(it,
                    NavigateDirection_NextSibling,&next)));
                IRawElementProviderSimple_Release(simple);
                IRawElementProviderFragment_Release(it);
                it=next;
            }
            if (is_new) IRawElementProviderSimple_Release(simple);
        }
        CHECK(row);
        IRawElementProviderSimple *row_simple=NULL;
        CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(row,
            &IID_IRawElementProviderSimple,(void **)&row_simple)));
        IUnknown *pattern=NULL;
        CHECK(SUCCEEDED(IRawElementProviderSimple_GetPatternProvider(
            row_simple,UIA_InvokePatternId,&pattern)) && pattern);
        IInvokeProvider *invoke=NULL;
        CHECK(SUCCEEDED(IUnknown_QueryInterface(pattern,
            &IID_IInvokeProvider,(void **)&invoke)));
        int conversations_before=chat->conversation_count;
        CHECK(SUCCEEDED(IInvokeProvider_Invoke(invoke)));
        /* The invoke is posted; run the popup's message handling. */
        MSG message;
        while (PeekMessageW(&message,palette_popup_window(palette),0,0,
                PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        CHECK(palette_popup_accepted(palette));
        CHECK(palette_popup_action_id(palette)==ACTION_NEW);
        palette_popup_destroy(palette);
        /* Applied by hand exactly as end_palette does: the dispatch fires
           exactly once, so the delta is one conversation. */
        action(h,ACTION_NEW);
        CHECK(chat->conversation_count==conversations_before+1);
        IInvokeProvider_Release(invoke); IUnknown_Release(pattern);
        IRawElementProviderSimple_Release(row_simple);
        IRawElementProviderFragment_Release(row);
        IRawElementProviderFragment_Release(child);
        IRawElementProviderFragment_Release(scroll);
        IRawElementProviderFragment_Release(fragment);
        IRawElementProviderSimple_Release(root);
    }

    /* A close arriving while the palette is live parks on close_pending and
       is reposted once end_palette has unwound. */
    {
        ChatActionContext context;
        palette_context(h,&context);
        h->open_palette=palette_popup_create(window,&context);
        CHECK(h->open_palette);
        SendMessageW(window,WM_CLOSE,0,0);
        CHECK(IsWindow(window) && h->close_pending);
        h->palette_pumping=true;
        palette_popup_cancel(h->open_palette);
        h->palette_pumping=false;
        end_palette(h);
        pump_messages(30);
        CHECK(!IsWindow(window));
        CHECK(!h->open_palette && !h->close_pending);
    }

    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Deliberate keyboard navigation suite (separate clean fixture) -------- */

static int navigation_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Navigation host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    /* Production wiring: retained events reach the host command dispatcher and
       native key activation therefore selects the row under focus. */
    ui->on_event=host_event; ui->event_user=h;
    h->chat_ui.command=command; h->chat_ui.command_user=h;
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-nav-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Navigation integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    /* A conversation with realized turns gives the transcript region a body
       to focus. */
    for (int t=0;t<8;t++) add_turn(chat,L"nav question",L"nav answer body",NULL,-1);
    render_transcript(h);
    flush(h);

    /* Region cycle order and focus ownership: Sidebar -> Transcript ->
       Composer -> Header, with F6 and Ctrl+T sharing one registry binding. */
    {
        SetFocus(h->composer.window);
        CHECK(focus_region_of(h)==CHAT_REGION_COMPOSER);
        CHECK(host_shortcut(h,VK_F6,false,false,false));
        CHECK(focus_region_of(h)==CHAT_REGION_HEADER && GetFocus()==window &&
            ui->focus==h->chat_ui.hamburger);
        CHECK(host_shortcut(h,VK_F6,false,false,false));
        CHECK(focus_region_of(h)==CHAT_REGION_SIDEBAR && GetFocus()==window &&
            focused_sidebar_id(h)==chat->conversations[chat->active].id);
        CHECK(host_shortcut(h,VK_F6,false,false,false));
        CHECK(focus_region_of(h)==CHAT_REGION_TRANSCRIPT &&
            is_transcript_window(h,GetFocus()));
        CHECK(host_shortcut(h,VK_F6,false,false,false));
        CHECK(focus_region_of(h)==CHAT_REGION_COMPOSER &&
            GetFocus()==h->composer.window);
        CHECK(host_shortcut(h,L'T',false,true,false));
        CHECK(focus_region_of(h)==CHAT_REGION_HEADER);
    }

    /* Entering the transcript focuses the reader's anchor turn body when it
       is realized, and never a stale record slot. */
    {
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(h->transcript.anchor.valid);
        int anchor_index=chat_message_index_by_id(chat,chat->active,
            h->transcript.anchor.message);
        CHECK(anchor_index>=0);
        focus_region(h,CHAT_REGION_TRANSCRIPT);
        CHECK(GetFocus()==body_window(h,anchor_index));
        CHECK(h->transcript.focus_window==body_window(h,anchor_index));
        /* A stale anchor resolves to a realized body of the active
           conversation instead of a recycled slot. */
        h->transcript.anchor.valid=true;
        h->transcript.anchor.conversation=chat->conversations[chat->active].id;
        h->transcript.anchor.message=0x7fffffffffffffffULL;
        focus_region(h,CHAT_REGION_TRANSCRIPT);
        HWND focused=GetFocus();
        CHECK(focused && is_transcript_window(h,focused));
        bool bound=false;
        for (int i=0;i<h->transcript.record_count;i++) {
            RichTextControl *body=transcript_surface(&h->transcript,i,
                TRANSCRIPT_BODY);
            if (body && body->window==focused &&
                h->transcript.records[i].conversation==
                    chat->conversations[chat->active].id) bound=true;
        }
        CHECK(bound);
    }

    /* A record at the right index that describes an older message is not a
       valid candidate even while its body window is still bound: entering the
       region must focus a different, current body. */
    {
        transcript_note_user_scroll(&h->transcript,0);
        transcript_position(&h->transcript,feed_arg(h),false);
        CHECK(h->transcript.anchor.valid);
        int stale_index=chat_message_index_by_id(chat,chat->active,
            h->transcript.anchor.message);
        CHECK(stale_index>=0);
        HWND stale_body=body_window(h,stale_index);
        CHECK(stale_body);
        uint64_t saved_message=h->transcript.records[stale_index].message;
        h->transcript.records[stale_index].message=0x7fffffffffffffffULL;
        SetFocus(h->composer.window);
        focus_region(h,CHAT_REGION_TRANSCRIPT);
        CHECK(GetFocus()!=stale_body);
        h->transcript.records[stale_index].message=saved_message;
    }

    /* Escape from a transcript surface returns to the composer through the
       existing focus-release callback. */
    {
        SetFocus(body_window(h,0));
        CHECK(h->transcript.focus_window==body_window(h,0));
        CHECK(surface_key(h,VK_ESCAPE,false,false,true));
        CHECK(GetFocus()==h->composer.window && h->transcript.focus_window==NULL);
    }

    /* Tab still spans the native fields and the retained chrome; the
       transcript is deliberately not part of that ring. */
    {
        SetFocus(h->field.window);
        CHECK(surface_key(h,VK_TAB,false,false,true));
        CHECK(GetFocus()==h->search.window);
        CHECK(surface_key(h,VK_TAB,false,false,true));
        CHECK(GetFocus()==h->composer.window);
        CHECK(surface_key(h,VK_TAB,false,false,true));
        CHECK(GetFocus()==window && ui->focus==h->chat_ui.hamburger);
        ui_focus_edge(ui,true);                    /* last retained stop */
        SetFocus(window);
        CHECK(ui_focus_boundary(ui,false));
        SendMessageW(window,WM_KEYDOWN,VK_TAB,0);
        CHECK(GetFocus()==h->field.window);
    }

    /* Sidebar arrows rove by conversation identity across the windowed pool,
       and Enter selects the row under focus. */
    {
        while (chat->conversation_count<60) chat_new_conversation(chat);
        command(h,CHAT_COMMAND_SELECT,0);
        pump_messages(20);
        focus_region(h,CHAT_REGION_SIDEBAR);
        CHECK(GetFocus()==window);
        CHECK(focused_sidebar_id(h)==chat->conversations[0].id);
        int start_offset=h->chat_ui.window_offset;
        int limit=h->chat_ui.pool_count+4;
        bool advanced=false;
        for (int step=1;step<=limit;step++) {
            CHECK(sidebar_roving(h,1));
            CHECK(focused_sidebar_id(h)==chat->conversations[step].id);
            if (h->chat_ui.window_offset>start_offset) advanced=true;
        }
        CHECK(advanced);
        int focused_index=chat_index_of_id(chat,focused_sidebar_id(h));
        CHECK(focused_index>0 && focused_index!=chat->active);
        SendMessageW(window,WM_KEYDOWN,VK_RETURN,0);
        SendMessageW(window,WM_KEYUP,VK_RETURN,0);
        CHECK(chat->active==focused_index);
        /* Roving back up preserves identity across the boundary too. */
        for (int step=0;step<6;step++) {
            int before=chat_index_of_id(chat,focused_sidebar_id(h));
            CHECK(before>0);
            CHECK(sidebar_roving(h,-1));
            CHECK(focused_sidebar_id(h)==chat->conversations[before-1].id);
        }
    }

    /* Focused-row actions resolve their target and check availability before
       touching the active conversation: a rejection must not select/render/
       save a different conversation first, and an unresolvable row must never
       fall through to the active one. */
    {
        command(h,CHAT_COMMAND_SELECT,0);
        pump_messages(20);
        focus_region(h,CHAT_REGION_SIDEBAR);
        CHECK(sidebar_roving(h,1));            /* focus conversation 1 */
        CHECK(focused_sidebar_id(h)==chat->conversations[1].id);
        int active_before=chat->active;
        CHECK(active_before==0);
        /* Generating: F2 and Delete are unavailable, so neither the selection
           nor history may change; action() reports the existing status. */
        h->generating=true;
        SendMessageW(window,WM_KEYDOWN,VK_F2,0);
        CHECK(chat->active==active_before);
        CHECK(wcsstr(chat->status,L"Stop generation")!=NULL);
        SendMessageW(window,WM_KEYDOWN,VK_DELETE,0);
        CHECK(chat->active==active_before);
        CHECK(wcsstr(chat->status,L"Stop generation")!=NULL);
        h->generating=false;
        /* An unresolvable focused-row tag is ignored outright: a non-modal
           probe action is never invoked on the active conversation. */
        UiNode *row=ui_node(ui,ui->focus);
        CHECK(row && row->tag!=0);
        uintptr_t saved_tag=row->tag;
        row->tag=0;
        int count_before=chat->conversation_count;
        wchar_t status_before[CHAT_STATUS_TEXT];
        wcscpy(status_before,chat->status);
        action_focused_conversation(h,ACTION_COPY);
        CHECK(chat->active==active_before);
        CHECK(chat->conversation_count==count_before);
        CHECK(!wcscmp(chat->status,status_before));
        row=ui_node(ui,ui->focus);
        if (row) row->tag=saved_tag;
    }

    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window));
    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Code-block copy suite (separate clean fixture) ------------------------ */

/* The suite's top-level window is never shown, so a child's visibility must
   be read from its own style bit, not IsWindowVisible (which walks ancestors). */
static bool copy_pill_shown(ChatHost *h) {
    return h->copy_pill &&
        (GetWindowLongW(h->copy_pill, GWL_STYLE) & WS_VISIBLE) != 0;
}

/* Intercepts WM_COPY so the selection path can be asserted without touching
   the real clipboard. */
static WNDPROC copy_probe_previous;
static int copy_probe_wm_copy;
static LRESULT CALLBACK copy_probe_proc(HWND window, UINT message,
    WPARAM w, LPARAM l) {
    if (message == WM_COPY) { ++copy_probe_wm_copy; return 0; }
    return CallWindowProcW(copy_probe_previous, window, message, w, l);
}

static int code_copy_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Copy host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-copy-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Code copy integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    /* One completed assistant turn whose Markdown body renders exactly one
       code block, preceded and followed by prose. */
    int index=add_turn(chat,L"copy question",
        L"Before the block.\n\n```basic\nline one\nline two\n```\n\n"
        L"After the block.",NULL,-1);
    render_transcript(h);
    flush(h);
    RichTextControl *body=transcript_surface(&h->transcript,index,
        TRANSCRIPT_BODY);
    CHECK(body && body->window);
    CHECK(rich_text_code_block_count(body)==1);
    size_t start=0,length=0;
    CHECK(rich_text_code_block_range(body,0,&start,&length));

    /* The pill appears over the block, anchored at its first-line right
       edge. */
    POINTL first;
    SendMessageW(body->window,EM_POSFROMCHAR,(WPARAM)&first,(LPARAM)start);
    CHECK(first.x>=0 && first.y>=0);
    SendMessageW(body->window,WM_MOUSEMOVE,0,
        MAKELPARAM(first.x+2,first.y+2));
    CHECK(h->copy_pill && copy_pill_shown(h));
    CHECK(h->copy_control==body && h->copy_block==0);
    {
        int width=px(h,COPY_PILL_WIDTH_DIPS);
        int height=px(h,COPY_PILL_HEIGHT_DIPS);
        RECT client; GetClientRect(body->window,&client);
        POINT anchor={ client.right-px(h,COPY_PILL_INSET_DIPS)-width,
            first.y+px(h,8)-height/2 };
        MapWindowPoints(body->window,h->view,&anchor,1);
        POINT origin={0,0}; MapWindowPoints(h->view,NULL,&origin,1);
        RECT pill; CHECK(GetWindowRect(h->copy_pill,&pill));
        CHECK(pill.left==origin.x+anchor.x && pill.top==origin.y+anchor.y);
        CHECK(pill.right-pill.left==width && pill.bottom-pill.top==height);
    }

    /* Prose hides it again. */
    {
        POINTL prose;
        SendMessageW(body->window,EM_POSFROMCHAR,(WPARAM)&prose,(LPARAM)0);
        SendMessageW(body->window,WM_MOUSEMOVE,0,
            MAKELPARAM(prose.x+2,prose.y+2));
        CHECK(!copy_pill_shown(h));
    }

    /* Moving onto the pill arms its own tracking: the surface's leave keeps
       the pill, and the pill's own leave hides it. */
    SendMessageW(body->window,WM_MOUSEMOVE,0,
        MAKELPARAM(first.x+2,first.y+2));
    CHECK(copy_pill_shown(h));
    SendMessageW(h->copy_pill,WM_MOUSEMOVE,0,MAKELPARAM(2,2));
    CHECK(h->copy_hovering);
    SendMessageW(body->window,WM_MOUSELEAVE,0,0);
    CHECK(copy_pill_shown(h));
    SendMessageW(h->copy_pill,WM_MOUSELEAVE,0,0);
    CHECK(!copy_pill_shown(h) && !h->copy_hovering);

    /* A pill click copies exactly the block's rendered text (fence markers
       excluded, LF normalized to CRLF) with status feedback. */
    SendMessageW(body->window,WM_MOUSEMOVE,0,
        MAKELPARAM(first.x+2,first.y+2));
    CHECK(copy_pill_shown(h));
    copied_text[0]=0; copy_calls=0;
    SendMessageW(h->copy_pill,WM_LBUTTONUP,0,0);
    CHECK(copy_calls==1 && !wcscmp(copied_text,L"line one\r\nline two"));
    CHECK(!wcscmp(chat->status,L"Code copied"));

    /* Container scroll hides the pill; the hover point stays valid because
       the block's position inside its own surface never moved. */
    SendMessageW(h->view,WM_VSCROLL,MAKEWPARAM(SB_PAGEDOWN,0),0);
    CHECK(!copy_pill_shown(h));
    SendMessageW(body->window,WM_MOUSEMOVE,0,
        MAKELPARAM(first.x+2,first.y+2));
    CHECK(copy_pill_shown(h));

    /* A rebuild (whole re-render) hides the pill. */
    render_transcript(h);
    CHECK(!copy_pill_shown(h));

    /* Verbatim bodies (the user turn) expose no pill. */
    {
        RichTextControl *user_body=transcript_surface(&h->transcript,index-1,
            TRANSCRIPT_BODY);
        CHECK(user_body && user_body->window);
        SendMessageW(user_body->window,WM_MOUSEMOVE,0,MAKELPARAM(4,4));
        CHECK(!copy_pill_shown(h));
    }

    /* Ctrl+Shift+C: no selection and no transcript focus -> the hint, and
       the clipboard helper is never called. */
    copy_calls=0;
    CHECK(host_shortcut(h,L'C',true,true,false));
    CHECK(copy_calls==0);
    CHECK(!wcscmp(chat->status,
        L"Select text, or rest the caret in a code block, to copy."));

    /* A transcript selection wins over the block at the caret: WM_COPY runs
       through the interceptor, never the real clipboard. */
    copy_probe_wm_copy=0;
    copy_probe_previous=(WNDPROC)SetWindowLongPtrW(body->window,GWLP_WNDPROC,
        (LONG_PTR)copy_probe_proc);
    CHARRANGE selection={(LONG)start,(LONG)(start+4)};
    SendMessageW(body->window,EM_EXSETSEL,0,(LPARAM)&selection);
    CHECK(host_shortcut(h,L'C',true,true,false));
    CHECK(copy_probe_wm_copy==1 && copy_calls==0);
    CHECK(!wcscmp(chat->status,L"Selection copied"));
    SetWindowLongPtrW(body->window,GWLP_WNDPROC,
        (LONG_PTR)copy_probe_previous);
    CHARRANGE none={0,0};
    SendMessageW(body->window,EM_EXSETSEL,0,(LPARAM)&none);

    /* The caret inside the block copies the block. */
    h->transcript.focus_window=body->window;
    CHARRANGE caret={(LONG)(start+2),(LONG)(start+2)};
    SendMessageW(body->window,EM_EXSETSEL,0,(LPARAM)&caret);
    CHECK(host_shortcut(h,L'C',true,true,false));
    CHECK(copy_calls==1 && !wcscmp(copied_text,L"line one\r\nline two"));
    CHECK(!wcscmp(chat->status,L"Code copied"));

    /* The caret outside any block gets the hint again. */
    SendMessageW(body->window,EM_EXSETSEL,0,(LPARAM)&none);
    CHECK(host_shortcut(h,L'C',true,true,false));
    CHECK(copy_calls==1);
    CHECK(!wcscmp(chat->status,
        L"Select text, or rest the caret in a code block, to copy."));

    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window));
    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* Returns the first scroll-fragment child whose UIA Name equals `name`; the
   caller owns the returned reference. NULL when absent. */
static IRawElementProviderFragment *palette_named_child(PalettePopup *popup,
    const wchar_t *name) {
    IRawElementProviderSimple *root=palette_popup_root_provider(popup);
    if (!root) return NULL;
    IRawElementProviderFragment *fragment=NULL;
    IRawElementProviderFragment *found=NULL;
    if (SUCCEEDED(IRawElementProviderSimple_QueryInterface(root,
            &IID_IRawElementProviderFragment,(void **)&fragment))) {
        IRawElementProviderFragment *scroll=palette_scroll_fragment(fragment);
        if (scroll) {
            IRawElementProviderFragment *child=NULL;
            if (SUCCEEDED(IRawElementProviderFragment_Navigate(scroll,
                    NavigateDirection_FirstChild,&child)))
                while (child) {
                    IRawElementProviderSimple *simple=NULL;
                    if (SUCCEEDED(IRawElementProviderFragment_QueryInterface(
                            child,&IID_IRawElementProviderSimple,
                            (void **)&simple))) {
                        VARIANT value; VariantInit(&value);
                        if (SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(
                                simple,UIA_NamePropertyId,&value)) &&
                            V_VT(&value)==VT_BSTR &&
                            !wcscmp(V_BSTR(&value),name)) {
                            found=child;
                            child=NULL;
                        }
                        VariantClear(&value);
                        IRawElementProviderSimple_Release(simple);
                    }
                    if (!child) break;
                    IRawElementProviderFragment *next=NULL;
                    if (FAILED(IRawElementProviderFragment_Navigate(child,
                            NavigateDirection_NextSibling,&next))) next=NULL;
                    IRawElementProviderFragment_Release(child);
                    child=next;
                }
            IRawElementProviderFragment_Release(scroll);
        }
        IRawElementProviderFragment_Release(fragment);
    }
    IRawElementProviderSimple_Release(root);
    return found;
}

/* True when the scroll fragment exposes a child with this accessible name. */
static bool palette_has_named_child(PalettePopup *popup, const wchar_t *name) {
    IRawElementProviderFragment *found=palette_named_child(popup,name);
    if (!found) return false;
    IRawElementProviderFragment_Release(found);
    return true;
}

/* Resolves the scroll container's screen rectangle through the popup's own
   UIA provider (the same geometry assistive tech sees). */
static bool palette_scroll_bounds(PalettePopup *popup, RECT *out) {
    bool ok=false;
    IRawElementProviderSimple *root=palette_popup_root_provider(popup);
    if (!root) return false;
    IRawElementProviderFragment *fragment=NULL;
    if (SUCCEEDED(IRawElementProviderSimple_QueryInterface(root,
            &IID_IRawElementProviderFragment,(void **)&fragment))) {
        IRawElementProviderFragment *scroll=palette_scroll_fragment(fragment);
        if (scroll) {
            struct UiaRect bounds={0};
            if (SUCCEEDED(IRawElementProviderFragment_get_BoundingRectangle(scroll,
                    &bounds)) && bounds.width>0 && bounds.height>0) {
                out->left=(LONG)bounds.left;
                out->top=(LONG)bounds.top;
                out->right=(LONG)(bounds.left+bounds.width);
                out->bottom=(LONG)(bounds.top+bounds.height);
                ok=true;
            }
            IRawElementProviderFragment_Release(scroll);
        }
        IRawElementProviderFragment_Release(fragment);
    }
    IRawElementProviderSimple_Release(root);
    return ok;
}

/* Activates a retained row with real mouse messages at its center: a single
   click must accept, exactly as assistive tech and the mouse behave. */
static bool palette_click_named(PalettePopup *popup, const wchar_t *name) {
    IRawElementProviderFragment *found=palette_named_child(popup,name);
    if (!found) return false;
    struct UiaRect bounds={0};
    bool clicked=false;
    if (SUCCEEDED(IRawElementProviderFragment_get_BoundingRectangle(found,
            &bounds)) && bounds.width>0 && bounds.height>0) {
        POINT point={ (LONG)(bounds.left+bounds.width/2),
            (LONG)(bounds.top+bounds.height/2) };
        HWND window=palette_popup_window(popup);
        ScreenToClient(window,&point);
        LPARAM lparam=MAKELPARAM(point.x,point.y);
        SendMessageW(window,WM_MOUSEMOVE,0,lparam);
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,lparam);
        SendMessageW(window,WM_LBUTTONUP,0,lparam);
        clicked=true;
    }
    IRawElementProviderFragment_Release(found);
    return clicked;
}

/* Walks the scroll fragment's children, returning the count and whether every
   one exposed a non-empty accessible name. Layout-only spacers are hidden from
   UIA, so they must not appear at all. */
static size_t palette_scroll_children_named(PalettePopup *popup,
    bool *all_named) {
    *all_named=true;
    size_t count=0;
    IRawElementProviderSimple *root=palette_popup_root_provider(popup);
    if (!root) { *all_named=false; return 0; }
    IRawElementProviderFragment *fragment=NULL;
    if (SUCCEEDED(IRawElementProviderSimple_QueryInterface(root,
            &IID_IRawElementProviderFragment,(void **)&fragment))) {
        IRawElementProviderFragment *scroll=palette_scroll_fragment(fragment);
        if (scroll) {
            IRawElementProviderFragment *child=NULL;
            if (SUCCEEDED(IRawElementProviderFragment_Navigate(scroll,
                    NavigateDirection_FirstChild,&child)))
                while (child) {
                    IRawElementProviderSimple *simple=NULL;
                    if (SUCCEEDED(IRawElementProviderFragment_QueryInterface(
                            child,&IID_IRawElementProviderSimple,
                            (void **)&simple))) {
                        VARIANT value; VariantInit(&value);
                        bool named=false;
                        if (SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(
                                simple,UIA_NamePropertyId,&value)) &&
                            V_VT(&value)==VT_BSTR && V_BSTR(&value)[0])
                            named=true;
                        VariantClear(&value);
                        if (!named) *all_named=false;
                        IRawElementProviderSimple_Release(simple);
                    }
                    ++count;
                    IRawElementProviderFragment *next=NULL;
                    if (FAILED(IRawElementProviderFragment_Navigate(child,
                            NavigateDirection_NextSibling,&next))) next=NULL;
                    IRawElementProviderFragment_Release(child);
                    child=next;
                }
            IRawElementProviderFragment_Release(scroll);
        }
        IRawElementProviderFragment_Release(fragment);
    }
    IRawElementProviderSimple_Release(root);
    return count;
}

/* True when `text` (length `n`) contains an unpaired UTF-16 surrogate. */
static bool has_lone_surrogate(const wchar_t *text, size_t n) {
    for (size_t i=0;i<n;i++) {
        if (text[i]>=0xD800 && text[i]<=0xDBFF) {
            if (i+1>=n || text[i+1]<0xDC00 || text[i+1]>0xDFFF) return true;
            ++i;
        } else if (text[i]>=0xDC00 && text[i]<=0xDFFF) return true;
    }
    return false;
}

/* ---- Export + native file dialog suite ---------------------------------- */

static bool export_file_exists(const wchar_t *path) {
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}
static size_t export_read(const wchar_t *path,char *out,size_t capacity) {
    out[0]=0;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return 0;
    DWORD got=0;
    bool ok=ReadFile(file,out,(DWORD)(capacity-1),&got,NULL)!=0;
    CloseHandle(file);
    if (!ok) return 0;
    out[got]=0;
    return got;
}
static bool export_contains(const char *haystack,const char *needle) {
    return strstr(haystack,needle)!=NULL;
}

static int export_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Export host",1100,720,720,480,NULL,false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-export-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Export integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    wchar_t md_path[512],json_path[512],all_path[512],direct_path[512];
    wchar_t empty_path[512],big_path[512],unrelated_tmp[512],unrelated_target[512];
    wchar_t subdir[512];
    swprintf(md_path,512,L"%ls\\chat.md",dir);
    swprintf(json_path,512,L"%ls\\chat.json",dir);
    swprintf(all_path,512,L"%ls\\all.json",dir);
    swprintf(direct_path,512,L"%ls\\direct.json",dir);
    swprintf(empty_path,512,L"%ls\\empty.json",dir);
    swprintf(big_path,512,L"%ls\\big.json",dir);
    swprintf(unrelated_tmp,512,L"%ls\\report.json.tmp",dir);
    swprintf(unrelated_target,512,L"%ls\\report.json",dir);
    swprintf(subdir,512,L"%ls\\adir",dir);

    /* ---- Direct real-writer guarantees (no host, no wrapper) ---- */
    {
        const char *first="first";
        CHECK(__real_chat_write_file_utf8(direct_path,first,strlen(first)));
        char read[512];
        CHECK(export_read(direct_path,read,sizeof read)==strlen(first));
        CHECK(!strcmp(read,first));
        /* An existing destination is replaced atomically. */
        const char *second="a longer replacement payload";
        CHECK(__real_chat_write_file_utf8(direct_path,second,strlen(second)));
        CHECK(export_read(direct_path,read,sizeof read)==strlen(second));
        CHECK(!strcmp(read,second));
    }
    /* Zero-length output is a valid empty file. */
    CHECK(__real_chat_write_file_utf8(empty_path,"",0));
    CHECK(export_file_exists(empty_path));
    { char read[8]; CHECK(export_read(empty_path,read,sizeof read)==0); CHECK(read[0]==0); }
    /* A length above the DWORD WriteFile limit is rejected, not truncated. */
    if (sizeof(size_t) > sizeof(DWORD))
        CHECK(!__real_chat_write_file_utf8(direct_path,"x",(size_t)MAXDWORD+1));
    /* An unrelated temp-like sibling survives untouched. */
    {
        const char *keep="do not touch";
        CHECK(__real_chat_write_file_utf8(unrelated_tmp,keep,strlen(keep)));
        const char *payload="exported payload";
        CHECK(__real_chat_write_file_utf8(unrelated_target,payload,
            strlen(payload)));
        char read[64];
        CHECK(export_read(unrelated_tmp,read,sizeof read)==strlen(keep));
        CHECK(!strcmp(read,keep));
    }
    /* Failure after the temporary exists: moving onto an existing directory
       fails, and only our owned temporary is removed. */
    CHECK(CreateDirectoryW(subdir,NULL)!=0);
    {
        const char *payload="cannot land here";
        CHECK(!__real_chat_write_file_utf8(subdir,payload,strlen(payload)));
        wchar_t pattern[512]; swprintf(pattern,512,L"%ls\\.darkchat-*",dir);
        WIN32_FIND_DATAW found; HANDLE find=FindFirstFileW(pattern,&found);
        CHECK(find==INVALID_HANDLE_VALUE);
        if (find!=INVALID_HANDLE_VALUE) FindClose(find);
    }
    RemoveDirectoryW(subdir);
    /* A large payload round-trips byte-exact. */
    {
        size_t length=200000;
        char *big=malloc(length); CHECK(big);
        for (size_t i=0;i<length;i++) big[i]=(char)(' '+(i%95));
        CHECK(__real_chat_write_file_utf8(big_path,big,length));
        char *back=malloc(length+1); CHECK(back);
        CHECK(export_read(big_path,back,length+1)==length);
        CHECK(memcmp(big,back,length)==0);
        free(big); free(back);
    }

    /* Default-name sanitizer: reserved characters, trailing trim, reserved
       DOS device basenames (prefixed), and the empty-title fallback. */
    {
        wchar_t name[64];
        chat_export_default_name(L"a/b:c*d?e\"f<g>h|i",L".md",name,64);
        CHECK(!wcscmp(name,L"a_b_c_d_e_f_g_h_i.md"));
        chat_export_default_name(L"trailing... ",L".json",name,64);
        CHECK(!wcscmp(name,L"trailing.json"));
        chat_export_default_name(L"CON",L".json",name,64);
        CHECK(!wcscmp(name,L"_CON.json"));
        chat_export_default_name(L"nul",L".md",name,64);
        CHECK(!wcscmp(name,L"_nul.md"));
        chat_export_default_name(L"com1",L".txt",name,64);
        CHECK(!wcscmp(name,L"_com1.txt"));
        chat_export_default_name(L"lpt9",L".txt",name,64);
        CHECK(!wcscmp(name,L"_lpt9.txt"));
        /* A device stem is reserved even with an internal extension. */
        chat_export_default_name(L"CON.txt",L".json",name,64);
        CHECK(!wcscmp(name,L"_CON.txt.json"));
        chat_export_default_name(L"lpt1.log",L".md",name,64);
        CHECK(!wcscmp(name,L"_lpt1.log.md"));
        /* Only the stem before the first dot is compared. */
        chat_export_default_name(L"CONSOLE",L".md",name,64);
        CHECK(!wcscmp(name,L"CONSOLE.md"));
        /* COM10 is not a reserved device name. */
        chat_export_default_name(L"com10",L".txt",name,64);
        CHECK(!wcscmp(name,L"com10.txt"));
        chat_export_default_name(L"",L".md",name,64);
        CHECK(!wcscmp(name,L"conversation.md"));
        chat_export_default_name(L"   ",L".md",name,64);
        CHECK(!wcscmp(name,L"conversation.md"));
    }

    /* ---- Host actions through the dialog seam ---- */
    add_turn(chat,L"export question",L"exported answer body",NULL,-1);
    render_transcript(h); flush(h);
    CHECK(chat_rename(chat,L"Chat/Export: \"Test\"?"));

    /* Markdown export, with the session deliberately staged to look dirty:
       the export must not capture the draft/model/geometry or save. */
    rich_text_set_text(&h->composer,L"staged unsaved draft");
    rich_text_set_text(&h->field,L"staged/model");
    h->dirty=false;
    wchar_t draft_before[CHAT_COMPOSER_TEXT];
    wcscpy(draft_before,chat->conversations[chat->active].draft);
    wchar_t model_before[CHAT_MODEL_TEXT], override_before[CHAT_MODEL_TEXT];
    wcscpy(model_before,chat->model);
    wcscpy(override_before,chat->conversations[chat->active].model);
    int64_t modified_before=chat->conversations[chat->active].modified_at;
    int wx=chat->window_x,wy=chat->window_y,ww=chat->window_width,wh=chat->window_height;
    int maximized=chat->maximized;
    LONG saves_before=storage_save_calls;
    save_queue_clear(); write_file_calls=0; write_file_fail=false;
    save_queue_push(md_path,CHAT_FILE_DIALOG_ACCEPTED);
    action(h,ACTION_EXPORT_MARKDOWN);
    CHECK(save_dialog_calls==1 && write_file_calls==1);
    CHECK(!wcscmp(write_file_last_path,md_path));
    /* The proposed name is sanitized and typed. */
    {
        size_t n=wcslen(save_dialog_last_default);
        CHECK(n>3 && !wcscmp(save_dialog_last_default+n-3,L".md"));
        CHECK(!wcschr(save_dialog_last_default,L'/'));
        CHECK(!wcschr(save_dialog_last_default,L':'));
        CHECK(!wcschr(save_dialog_last_default,L'"'));
    }
    CHECK(export_file_exists(md_path));
    {
        char read[8192];
        CHECK(export_read(md_path,read,sizeof read)>0);
        CHECK(export_contains(read,"# "));
        CHECK(export_contains(read,"exported answer body"));
        CHECK(export_contains(read,"<!-- darkchat.export:"));
    }
    CHECK(h->dirty==false);
    CHECK(!wcscmp(chat->conversations[chat->active].draft,draft_before));
    CHECK(!wcscmp(chat->model,model_before));
    CHECK(!wcscmp(chat->conversations[chat->active].model,override_before));
    CHECK(chat->conversations[chat->active].modified_at==modified_before);
    CHECK(chat->window_x==wx && chat->window_y==wy &&
        chat->window_width==ww && chat->window_height==wh &&
        chat->maximized==maximized);
    CHECK(storage_save_calls==saves_before);

    /* JSON export of the active conversation. */
    save_queue_clear(); write_file_calls=0;
    save_queue_push(json_path,CHAT_FILE_DIALOG_ACCEPTED);
    action(h,ACTION_EXPORT_JSON);
    CHECK(write_file_calls==1 && export_file_exists(json_path));
    {
        char read[8192];
        CHECK(export_read(json_path,read,sizeof read)>0);
        CHECK(json_validate(read));
        CHECK(export_contains(read,"darkchat.export"));
        CHECK(export_contains(read,"exported answer body"));
    }

    /* Export all: a second conversation is included in one JSON file. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    add_turn(chat,L"second question",L"second answer body",NULL,-1);
    render_transcript(h); flush(h);
    save_queue_clear(); write_file_calls=0;
    save_queue_push(all_path,CHAT_FILE_DIALOG_ACCEPTED);
    action(h,ACTION_EXPORT_ALL);
    CHECK(write_file_calls==1 && export_file_exists(all_path));
    {
        char read[16384];
        CHECK(export_read(all_path,read,sizeof read)>0);
        CHECK(json_validate(read));
        CHECK(export_contains(read,"exported answer body"));
        CHECK(export_contains(read,"second answer body"));
    }

    /* Cancel is silent and writes nothing. */
    save_queue_clear(); write_file_calls=0;
    action(h,ACTION_EXPORT_JSON);
    CHECK(save_dialog_calls==1 && write_file_calls==0);

    /* A dialog error is reported, not mistaken for a cancel. */
    save_queue_clear(); write_file_calls=0;
    save_queue_push(L"",CHAT_FILE_DIALOG_ERROR);
    action(h,ACTION_EXPORT_MARKDOWN);
    CHECK(save_dialog_calls==1 && write_file_calls==0);
    CHECK(!wcscmp(chat->status,L"Could not open the save dialog."));

    /* A writer failure is surfaced and leaves no target. */
    DeleteFileW(json_path);
    save_queue_clear(); write_file_calls=0; write_file_fail=true;
    save_queue_push(json_path,CHAT_FILE_DIALOG_ACCEPTED);
    action(h,ACTION_EXPORT_JSON);
    write_file_fail=false;
    CHECK(write_file_calls==1 && !export_file_exists(json_path));
    CHECK(!wcscmp(chat->status,L"Could not write the export file."));

    /* While generating the existing guard runs first: no dialog, no write. */
    save_queue_clear(); write_file_calls=0;
    h->generating=true;
    action(h,ACTION_EXPORT_JSON);
    h->generating=false;
    CHECK(save_dialog_calls==0 && write_file_calls==0);
    CHECK(!wcscmp(chat->status,
        L"Stop generation before changing history or settings."));

    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window));
    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(md_path); DeleteFileW(json_path); DeleteFileW(all_path);
    DeleteFileW(direct_path); DeleteFileW(empty_path); DeleteFileW(big_path);
    DeleteFileW(unrelated_tmp); DeleteFileW(unrelated_target);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup);
    DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir);
    DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Model palette / virtualized rows suite ------------------------------ */

static int model_palette_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Model palette host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-modelpal-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Model palette integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    /* 4,096-entry catalog: only a bounded window of retained nodes is ever
       created, the last entry is reachable, and acceptance returns its exact
       id. The UIA census stays constant across deep scrolling and every
       exposed child is named. */
    {
        ChatModelCatalog big; chat_model_catalog_init(&big);
        for (unsigned i=0;i<4096;i++) {
            ChatModelInfo info; memset(&info,0,sizeof info);
            info.context_length=-1;
            swprintf(info.id,CHAT_MODEL_TEXT,L"vendor/model-%04u",i);
            swprintf(info.name,CHAT_MODEL_NAME_TEXT,L"Model %04u",i);
            CHECK(chat_model_catalog_append(&big,&info));
        }
        CHECK(big.count==4096);
        PalettePopup *palette=palette_popup_create_models(window,&big,
            L"vendor/model-0000",NULL,0,L"4096 models",L"vendor/model-0000");
        CHECK(palette && palette_popup_mode(palette)==PALETTE_MODE_MODELS);
        CHECK(palette_popup_row_count(palette)==4096);
        CHECK(!wcscmp(palette_popup_status(palette),L"4096 models"));
        bool all_named=true;
        size_t census=palette_scroll_children_named(palette,&all_named);
        CHECK(all_named && census>0 && census<=80);
        /* Deep path: End reaches the final row and rebinds the bounded pool. */
        SendMessageW(palette_popup_window(palette),WM_KEYDOWN,VK_END,0);
        CHECK(!wcscmp(palette_highlighted_id(palette),L"vendor/model-4095"));
        bool all_named2=true;
        size_t census2=palette_scroll_children_named(palette,&all_named2);
        /* The window is bounded (never growing with the catalog) and stays
           named; it can be smaller at the very end of the list. */
        CHECK(all_named2 && census2>0 && census2<=80);
        /* Virtualization correctness, not just controller correctness: the
           final row's composed label is actually present in the accessibility
           tree after the deep rebind. */
        { wchar_t expected[UI_TEXT_CAPACITY];
          palette_popup_format_model_row(expected,L"Model 4095",
              L"vendor/model-4095");
          CHECK(palette_has_named_child(palette,expected)); }
        palette_popup_accept(palette);
        { wchar_t id[CHAT_MODEL_TEXT];
          CHECK(palette_popup_accepted_model(palette,id) &&
              !wcscmp(id,L"vendor/model-4095")); }
        palette_popup_destroy(palette);
        chat_model_catalog_dispose(&big);
    }

    /* Duplicate display names stay distinguishable: the composed row keeps the
       complete id, and activating each returns its own id. */
    {
        ChatModelCatalog cat; chat_model_catalog_init(&cat);
        ChatModelInfo one; memset(&one,0,sizeof one); one.context_length=-1;
        wcscpy(one.id,L"dup/one"); wcscpy(one.name,L"Same Name");
        ChatModelInfo two=one; wcscpy(two.id,L"dup/two");
        CHECK(chat_model_catalog_append(&cat,&one));
        CHECK(chat_model_catalog_append(&cat,&two));
        /* No current id: both catalog entries keep their display name, so the
           two rows really do share one name and differ only by id. */
        PalettePopup *palette=palette_popup_create_models(window,&cat,
            NULL,NULL,0,NULL,NULL);
        CHECK(palette && palette_popup_row_count(palette)==2);
        wchar_t label_a[UI_TEXT_CAPACITY], label_b[UI_TEXT_CAPACITY];
        palette_popup_format_model_row(label_a,L"Same Name",L"dup/one");
        palette_popup_format_model_row(label_b,L"Same Name",L"dup/two");
        CHECK(wcscmp(label_a,label_b)!=0);
        CHECK(wcsstr(label_a,L"dup/one") && wcsstr(label_b,L"dup/two"));
        /* Both same-name rows are exposed to UIA, and a real single click on
           each retained node accepts that exact id. */
        CHECK(palette_has_named_child(palette,label_a));
        CHECK(palette_has_named_child(palette,label_b));
        CHECK(palette_click_named(palette,label_b));
        { wchar_t id[CHAT_MODEL_TEXT];
          CHECK(palette_popup_accepted_model(palette,id) &&
              !wcscmp(id,L"dup/two")); }
        palette_popup_destroy(palette);
        /* The other duplicate activates to its own id in a fresh popup. */
        palette=palette_popup_create_models(window,&cat,NULL,NULL,0,
            NULL,NULL);
        CHECK(palette && palette_click_named(palette,label_a));
        { wchar_t id[CHAT_MODEL_TEXT];
          CHECK(palette_popup_accepted_model(palette,id) &&
              !wcscmp(id,L"dup/one")); }
        palette_popup_destroy(palette);
        chat_model_catalog_dispose(&cat);
    }

    /* Long names are cut so the complete id always survives, without splitting
       a UTF-16 surrogate pair. */
    {
        wchar_t name[CHAT_MODEL_NAME_TEXT], id[CHAT_MODEL_TEXT];
        wchar_t out[UI_TEXT_CAPACITY];
        for (int i=0;i<CHAT_MODEL_NAME_TEXT-1;i++) name[i]=L'A';
        name[CHAT_MODEL_NAME_TEXT-1]=0;
        for (int i=0;i<CHAT_MODEL_TEXT-1;i++) id[i]=L'b';
        id[CHAT_MODEL_TEXT-1]=0;
        palette_popup_format_model_row(out,name,id);
        CHECK(wcslen(out)<=UI_TEXT_CAPACITY-1);
        size_t id_length=wcslen(id);
        CHECK(wcslen(out)>=id_length &&
            !wcscmp(out+wcslen(out)-id_length,id));   /* id tail intact */
        /* A name made of surrogate pairs is never split at the cut. */
        wchar_t emoji[CHAT_MODEL_NAME_TEXT];
        int at=0;
        while (at+2<CHAT_MODEL_NAME_TEXT-1) { emoji[at++]=0xD83D; emoji[at++]=0xDE00; }
        emoji[at]=0;
        palette_popup_format_model_row(out,emoji,L"vendor/emoji");
        CHECK(wcslen(out)<=UI_TEXT_CAPACITY-1);
        wchar_t *separator=wcsstr(out,L" \u2014 ");
        CHECK(separator!=NULL);
        CHECK(!has_lone_surrogate(out,(size_t)(separator-out)));
        CHECK(!wcscmp(separator+3,L"vendor/emoji"));
    }

    /* A source refresh failure keeps the old source, query, selection, scroll
       and display fully intact; a later success commits. */
    {
        ChatModelCatalog cat; chat_model_catalog_init(&cat);
        for (unsigned i=0;i<50;i++) {
            ChatModelInfo info; memset(&info,0,sizeof info); info.context_length=-1;
            swprintf(info.id,CHAT_MODEL_TEXT,L"stable/%02u",i);
            swprintf(info.name,CHAT_MODEL_NAME_TEXT,L"Stable %02u",i);
            CHECK(chat_model_catalog_append(&cat,&info));
        }
        PalettePopup *palette=palette_popup_create_models(window,&cat,
            L"stable/00",NULL,0,L"50 models",NULL);
        CHECK(palette && palette_popup_row_count(palette)==50);
        palette_popup_set_query(palette,L"Stable 1");
        size_t filtered=palette_popup_row_count(palette);
        palette_popup_set_selected_model(palette,L"stable/10");
        float offset=palette_popup_scroll_offset(palette);
        ChatModelCatalog more; chat_model_catalog_init(&more);
        for (unsigned i=0;i<60;i++) {
            ChatModelInfo info; memset(&info,0,sizeof info); info.context_length=-1;
            swprintf(info.id,CHAT_MODEL_TEXT,L"extra/%02u",i);
            swprintf(info.name,CHAT_MODEL_NAME_TEXT,L"Extra %02u",i);
            CHECK(chat_model_catalog_append(&more,&info));
        }
        alloc_fail_malloc=0;
        CHECK(!palette_popup_set_models(palette,&more,NULL,NULL,0,L"oom"));
        alloc_fail_malloc=-1;
        CHECK(!wcscmp(palette_popup_query(palette),L"Stable 1"));
        CHECK(palette_popup_row_count(palette)==filtered);
        CHECK(!wcscmp(palette_highlighted_id(palette),L"stable/10"));
        CHECK(palette_popup_scroll_offset(palette)==offset);
        CHECK(!wcscmp(palette_popup_status(palette),L"50 models"));
        CHECK(palette_popup_set_models(palette,&more,NULL,NULL,0,L"60 models"));
        CHECK(!wcscmp(palette_popup_query(palette),L"Stable 1")); /* query kept */
        CHECK(!wcscmp(palette_popup_status(palette),L"60 models"));
        palette_popup_destroy(palette);
        chat_model_catalog_dispose(&more);
        chat_model_catalog_dispose(&cat);
    }

    /* A stale handle from before a rebind cannot fire: ui_remove advanced the
       generation, so the queued UIA invoke fails instead of resolving a
       rebound slot. */
    {
        ChatActionContext context; palette_context(h,&context);
        PalettePopup *palette=palette_popup_create(window,&context);
        CHECK(palette);
        IRawElementProviderFragment *row=palette_named_child(palette,
            L"New conversation");
        CHECK(row);
        IRawElementProviderSimple *simple=NULL;
        CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(row,
            &IID_IRawElementProviderSimple,(void **)&simple)));
        IUnknown *pattern=NULL;
        CHECK(SUCCEEDED(IRawElementProviderSimple_GetPatternProvider(simple,
            UIA_InvokePatternId,&pattern)) && pattern);
        IInvokeProvider *invoke=NULL;
        CHECK(SUCCEEDED(IUnknown_QueryInterface(pattern,&IID_IInvokeProvider,
            (void **)&invoke)));
        int conversations_before=chat->conversation_count;
        /* Rebuild the visible list, invalidating every realized handle. */
        palette_popup_set_query(palette,L"zzzznope");
        CHECK(palette_popup_row_count(palette)==0);
        CHECK(FAILED(IInvokeProvider_Invoke(invoke)));
        /* Drain any posted accessibility message; nothing may fire. */
        MSG message;
        while (PeekMessageW(&message,palette_popup_window(palette),0,0,
                PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        CHECK(!palette_popup_accepted(palette));
        CHECK(chat->conversation_count==conversations_before);
        IInvokeProvider_Release(invoke); IUnknown_Release(pattern);
        IRawElementProviderSimple_Release(simple);
        IRawElementProviderFragment_Release(row);
        palette_popup_destroy(palette);
    }

    /* Wheel scrolling rebinds the window and a DPI change re-places and
       rebinds without losing the source. */
    {
        ChatModelCatalog cat; chat_model_catalog_init(&cat);
        for (unsigned i=0;i<200;i++) {
            ChatModelInfo info; memset(&info,0,sizeof info); info.context_length=-1;
            swprintf(info.id,CHAT_MODEL_TEXT,L"scroll/%03u",i);
            swprintf(info.name,CHAT_MODEL_NAME_TEXT,L"Scroll %03u",i);
            CHECK(chat_model_catalog_append(&cat,&info));
        }
        PalettePopup *palette=palette_popup_create_models(window,&cat,
            L"scroll/000",NULL,0,L"200 models",NULL);
        CHECK(palette && palette_popup_row_count(palette)==200);
        float before=palette_popup_scroll_offset(palette);
        RECT bounds=palette_popup_bounds(palette);
        int sx=bounds.left+100, sy=bounds.top+300;
        SendMessageW(palette_popup_window(palette),WM_MOUSEWHEEL,
            MAKEWPARAM(0,(WPARAM)(-WHEEL_DELTA)),MAKELPARAM(sx,sy));
        CHECK(palette_popup_scroll_offset(palette)>before);
        SendMessageW(palette_popup_window(palette),WM_DPICHANGED,
            MAKEWPARAM(96,192),0);
        CHECK(palette_popup_dpi(palette)==192);
        CHECK(palette_popup_row_count(palette)==200);
        palette_popup_destroy(palette);
        chat_model_catalog_dispose(&cat);
    }

    /* Dragging the scrollbar thumb rebinds the window: the drag path mutates
       the offset through ui_pointer_move(), not the wheel handler. */
    {
        ChatModelCatalog cat; chat_model_catalog_init(&cat);
        for (unsigned i=0;i<200;i++) {
            ChatModelInfo info; memset(&info,0,sizeof info); info.context_length=-1;
            swprintf(info.id,CHAT_MODEL_TEXT,L"drag/%03u",i);
            swprintf(info.name,CHAT_MODEL_NAME_TEXT,L"Drag %03u",i);
            CHECK(chat_model_catalog_append(&cat,&info));
        }
        PalettePopup *palette=palette_popup_create_models(window,&cat,
            L"drag/000",NULL,0,L"200 models",NULL);
        CHECK(palette && palette_popup_row_count(palette)==200);
        RECT scroll; CHECK(palette_scroll_bounds(palette,&scroll));
        HWND popup_window=palette_popup_window(palette);
        POINT grab={ scroll.right-4, scroll.top+8 };
        POINT drop={ scroll.right-4, scroll.top+(scroll.bottom-scroll.top)/2 };
        ScreenToClient(popup_window,&grab);
        ScreenToClient(popup_window,&drop);
        float before=palette_popup_scroll_offset(palette);
        SendMessageW(popup_window,WM_MOUSEMOVE,0,MAKELPARAM(grab.x,grab.y));
        SendMessageW(popup_window,WM_LBUTTONDOWN,MK_LBUTTON,
            MAKELPARAM(grab.x,grab.y));
        SendMessageW(popup_window,WM_MOUSEMOVE,MK_LBUTTON,
            MAKELPARAM(drop.x,drop.y));
        SendMessageW(popup_window,WM_LBUTTONUP,0,MAKELPARAM(drop.x,drop.y));
        CHECK(palette_popup_scroll_offset(palette)>before);
        /* The rebind left a usable, named window at the new position. */
        bool all_named=true;
        CHECK(palette_scroll_children_named(palette,&all_named)>0 && all_named);
        palette_popup_destroy(palette);
        chat_model_catalog_dispose(&cat);
    }

    /* Mixed-mode lifecycle: the single popup serves commands and models; each
       open pumps once, applies nothing on cancel, and leaves no dirty state. */
    {
        palette_pump_calls=0; h->dirty=false;
        CHECK(host_shortcut(h,L'K',false,true,false));
        CHECK(palette_pump_calls==1 && !h->open_palette);
        CHECK(!h->dirty);
        /* Ctrl+Space routes through action(), which captures pending settings
           first; the palette itself applies nothing on cancel. */
        CHECK(host_shortcut(h,L' ',false,true,false));
        CHECK(palette_pump_calls==2 && !h->open_palette);
        CHECK(!h->model_applied);
        /* A command palette opened right after is still commands mode: the
           single popup never leaks the previous mode. */
        h->dirty=false;
        CHECK(host_shortcut(h,L'K',false,true,false));
        CHECK(palette_pump_calls==3 && !h->open_palette);
        CHECK(!h->dirty);
    }

    /* IME yield: when the thread's active input locale is an IME, Ctrl+Space
       belongs to the IME on/off toggle and must neither open the model palette
       nor be consumed. Ctrl+K remains the canonical palette shortcut. */
    {
        palette_pump_calls=0; h->dirty=false;
        ime_forced=true; ime_value=true;
        CHECK(!host_shortcut(h,L' ',false,true,false));
        CHECK(palette_pump_calls==0 && !h->open_palette);
        CHECK(!h->model_applied && !h->dirty);
        CHECK(host_shortcut(h,L'K',false,true,false));
        CHECK(palette_pump_calls==1 && !h->open_palette);
        /* A non-IME locale keeps the original Ctrl+Space behavior. */
        ime_value=false;
        CHECK(host_shortcut(h,L' ',false,true,false));
        CHECK(palette_pump_calls==2 && !h->open_palette);
        ime_forced=false;
    }

    /* VK_PROCESSKEY -- what Windows delivers while an IME is composing -- is
       unmapped by the retained key map and passes through both shortcut paths
       untouched: no palette, no command, no dirty state. */
    {
        palette_pump_calls=0; h->dirty=false;
        UiKey key=UI_KEY_ENTER;
        CHECK(!key_from_win32(VK_PROCESSKEY,&key));
        CHECK(key==UI_KEY_ENTER);                       /* left unchanged */
        CHECK(!host_shortcut(h,VK_PROCESSKEY,false,false,false));
        CHECK(!surface_key(h,VK_PROCESSKEY,false,false,true));
        SendMessageW(window,WM_KEYDOWN,VK_PROCESSKEY,0);
        SendMessageW(h->composer.window,WM_KEYDOWN,VK_PROCESSKEY,0);
        CHECK(palette_pump_calls==0 && !h->open_palette);
        CHECK(!h->dirty);
    }

    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup); DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer); DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Import + native open/read suite ------------------------------------ */

static int import_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Import host",1100,720,720,480,NULL,false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-import-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Import integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    wchar_t bom_path[512],empty_path[512],exact_path[512],over_path[512];
    swprintf(bom_path,512,L"%ls\\bom.json",dir);
    swprintf(empty_path,512,L"%ls\\empty.json",dir);
    swprintf(exact_path,512,L"%ls\\exact.json",dir);
    swprintf(over_path,512,L"%ls\\over.json",dir);

    /* ---- Direct real-reader boundary guarantees ---- */
    {
        const char *bom="\xEF\xBB\xBF{}";
        CHECK(__real_chat_write_file_utf8(bom_path,bom,strlen(bom)));
        char *data=(char *)0x1; size_t length=123;
        CHECK(chat_read_file_utf8_limited(bom_path,64,&data,&length)==
            CHAT_FILE_READ_OK);
        CHECK(data && length==2 && !strcmp(data,"{}"));
        free(data);

        CHECK(__real_chat_write_file_utf8(empty_path,"",0));
        data=(char *)0x1; length=123;
        CHECK(chat_read_file_utf8_limited(empty_path,64,&data,&length)==
            CHAT_FILE_READ_OK);
        CHECK(data && length==0 && data[0]==0);
        free(data);

        /* Exact limit succeeds; one byte past it is classified, not read. */
        CHECK(__real_chat_write_file_utf8(exact_path,"abcd",4));
        data=(char *)0x1; length=123;
        CHECK(chat_read_file_utf8_limited(exact_path,4,&data,&length)==
            CHAT_FILE_READ_OK);
        CHECK(data && length==4 && !strcmp(data,"abcd"));
        free(data);

        CHECK(__real_chat_write_file_utf8(over_path,"abcde",5));
        data=(char *)0x1; length=123;
        CHECK(chat_read_file_utf8_limited(over_path,4,&data,&length)==
            CHAT_FILE_READ_TOO_LARGE);
        CHECK(data==NULL && length==0);

        data=(char *)0x1; length=123;
        CHECK(chat_read_file_utf8_limited(L"Z:\\darkchat\\missing.json",4,
            &data,&length)==CHAT_FILE_READ_IO_ERROR);
        CHECK(data==NULL && length==0);
        /* Rejected arguments clear the outputs too. */
        data=(char *)0x1; length=123;
        CHECK(chat_read_file_utf8_limited(NULL,4,&data,&length)==
            CHAT_FILE_READ_IO_ERROR);
        CHECK(data==NULL && length==0);
    }

    /* ---- A source chat whose export is the import payload ---- */
    Chat *source=calloc(1,sizeof *source); CHECK(source);
    chat_init(source); chat_clear(source);
    ChatConversation *sc=&source->conversations[0];
    sc->id=7; sc->created_at=1000; sc->modified_at=2000; sc->renamed=true;
    wcscpy(sc->title,L"Imported one");
    int mi=chat_append_at(source,0,CHAT_ROLE_USER,L"hello");
    CHECK(mi>=0);
    source->conversations[0].messages[mi].created_at=1500;
    source->conversations[0].messages[mi].modified_at=1500;
    mi=chat_append_at(source,0,CHAT_ROLE_ASSISTANT,L"world");
    CHECK(mi>=0);
    source->conversations[0].messages[mi].created_at=1600;
    source->conversations[0].messages[mi].modified_at=1600;
    source->conversations[0].messages[mi].generation.state=CHAT_GENERATION_COMPLETE;
    JsonBuf out; CHECK(chat_export_json(source,0,true,7,&out));
    JsonBuf markdown; CHECK(chat_export_markdown(source,0,true,7,&markdown));
    chat_dispose(source); free(source);

    /* ---- Cancel is a silent no-op: no read, no capture, no status ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); read_set(NULL,0,CHAT_FILE_READ_OK);
        open_queue_push(L"ignored.json",CHAT_FILE_DIALOG_CANCELLED);
        action(h,ACTION_IMPORT_JSON);
        CHECK(chat->conversation_count==count);
        CHECK(open_dialog_calls==1 && read_calls==0);
    }

    /* ---- Success appends and persists; active selection is untouched ---- */
    {
        int count=chat->conversation_count;
        int active=chat->active;
        open_queue_clear(); open_queue_push(L"import.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(out.data,out.length,CHAT_FILE_READ_OK);
        /* Unsaved composer text proves capture_settings() runs on the import
           path: it must land in the untouched active conversation and in the
           durable snapshot. */
        rich_text_set_text(&h->composer,L"unsaved draft before import");
        uint64_t mutations=h->mutations;
        action(h,ACTION_IMPORT_JSON);
        CHECK(open_dialog_calls==1 && read_calls==1);
        CHECK(chat->conversation_count==count+1);
        CHECK(chat->active==active);
        CHECK(h->mutations>mutations);
        CHECK(!wcscmp(chat->status,L"Imported 1 conversation."));
        CHECK(!wcscmp(chat->conversations[active].draft,
            L"unsaved draft before import"));
        {
            char saved[65536];
            size_t saved_length=export_read(h->storage.path,saved,sizeof saved);
            CHECK(saved_length>0);
            CHECK(strstr(saved,"unsaved draft before import")!=NULL);
        }
        const ChatConversation *imp=&chat->conversations[count];
        CHECK(!wcscmp(imp->title,L"Imported one"));
        CHECK(imp->message_count==2);
        CHECK(!wcscmp(chat_message_text(&imp->messages[0]),L"hello"));
        CHECK(imp->messages[1].generation.state==CHAT_GENERATION_COMPLETE);
        CHECK(imp->messages[1].generation.cost==-1);
    }

    /* ---- Markdown cancel is a silent no-op ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); read_set(NULL,0,CHAT_FILE_READ_OK);
        open_queue_push(L"ignored.md",CHAT_FILE_DIALOG_CANCELLED);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(chat->conversation_count==count);
        CHECK(open_dialog_calls==1 && read_calls==0);
    }

    /* ---- Markdown payload round trip ---- */
    {
        int count=chat->conversation_count;
        int active=chat->active;
        open_queue_clear(); open_queue_push(L"import.md",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(markdown.data,markdown.length,CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(open_dialog_calls==1 && read_calls==1);
        CHECK(chat->conversation_count==count+1);
        CHECK(chat->active==active);
        CHECK(!wcscmp(chat->status,L"Imported 1 conversation."));
        const ChatConversation *imp=&chat->conversations[count];
        CHECK(!wcscmp(imp->title,L"Imported one"));
        CHECK(imp->message_count==2);
        CHECK(!wcscmp(chat_message_text(&imp->messages[0]),L"hello"));
        CHECK(!wcscmp(chat_message_text(&imp->messages[1]),L"world"));
        CHECK(imp->messages[1].generation.state==CHAT_GENERATION_COMPLETE);
        CHECK(imp->messages[1].generation.cost==-1);
    }

    /* ---- Payload-less Markdown: one verbatim message, heading title ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); open_queue_push(L"notes.md",CHAT_FILE_DIALOG_ACCEPTED);
        const char *plain="# Heading\n\n## User\n\nbody text\n";
        read_set(plain,(size_t)strlen(plain),CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(chat->conversation_count==count+1);
        const ChatConversation *imp=&chat->conversations[count];
        CHECK(!wcscmp(imp->title,L"Heading"));
        CHECK(imp->message_count==1);
        CHECK(imp->messages[0].role==CHAT_ROLE_USER);
        CHECK(!wcscmp(chat_message_text(&imp->messages[0]),
            L"# Heading\n\n## User\n\nbody text\n"));
    }

    /* ---- Filename fallback title, surrogate-safe at the buffer boundary ---- */
    {
        int count=chat->conversation_count;
        wchar_t path[128];
        size_t ep=0;
        for (int i=0;i<62;i++) path[ep++]=L'a';
        path[ep++]=(wchar_t)0xd83d;
        path[ep++]=(wchar_t)0xde00;
        path[ep++]=L'.'; path[ep++]=L'm'; path[ep++]=L'd'; path[ep]=0;
        open_queue_clear(); open_queue_push(path,CHAT_FILE_DIALOG_ACCEPTED);
        const char *plain="plain body\n";
        read_set(plain,(size_t)strlen(plain),CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(chat->conversation_count==count+1);
        const ChatConversation *imp=&chat->conversations[count];
        CHECK(wcslen(imp->title)==62);
        CHECK(imp->title[61]==L'a');

        /* An extension-only basename falls through to the default title. */
        open_queue_clear(); open_queue_push(L".md",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(plain,(size_t)strlen(plain),CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(!wcscmp(chat->conversations[chat->conversation_count-1].title,
            L"Imported conversation"));
    }

    /* ---- Invalid Markdown reports its own error text ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); open_queue_push(L"x.md",CHAT_FILE_DIALOG_ACCEPTED);
        const char invalid[]={ '#',(char)0xff,'x' };
        read_set(invalid,sizeof invalid,CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_MARKDOWN);
        CHECK(chat->conversation_count==count);
        CHECK(wcsstr(chat->status,L"not valid UTF-8 Markdown")!=NULL);
    }

    /* ---- Read failures and malformed payloads never mutate ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); open_queue_push(L"x.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(NULL,0,CHAT_FILE_READ_TOO_LARGE);
        action(h,ACTION_IMPORT_JSON);
        CHECK(chat->conversation_count==count);
        CHECK(wcsstr(chat->status,L"larger than")!=NULL);

        open_queue_clear(); open_queue_push(L"x.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(NULL,0,CHAT_FILE_READ_IO_ERROR);
        action(h,ACTION_IMPORT_JSON);
        CHECK(chat->conversation_count==count);
        CHECK(wcsstr(chat->status,L"could not be read")!=NULL);

        open_queue_clear(); open_queue_push(L"x.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(NULL,0,CHAT_FILE_READ_OOM);
        action(h,ACTION_IMPORT_JSON);
        CHECK(chat->conversation_count==count);
        CHECK(wcsstr(chat->status,L"memory")!=NULL);

        open_queue_clear(); open_queue_push(L"x.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set("not a darkchat export",21,CHAT_FILE_READ_OK);
        action(h,ACTION_IMPORT_JSON);
        CHECK(chat->conversation_count==count);
        CHECK(wcsstr(chat->status,L"not a valid")!=NULL);
    }

    /* ---- Generating disables the action before any dialog opens ---- */
    {
        int count=chat->conversation_count;
        int calls=open_dialog_calls;
        h->generating=true;
        action(h,ACTION_IMPORT_JSON);
        h->generating=false;
        CHECK(open_dialog_calls==calls);
        CHECK(chat->conversation_count==count);
    }

    /* ---- A failed durability flush is not overwritten by success ---- */
    {
        int count=chat->conversation_count;
        open_queue_clear(); open_queue_push(L"x.json",CHAT_FILE_DIALOG_ACCEPTED);
        read_set(out.data,out.length,CHAT_FILE_READ_OK);
        bool writable=h->storage.writable;
        h->storage.writable=false;
        action(h,ACTION_IMPORT_JSON);
        h->storage.writable=writable;
        CHECK(chat->conversation_count==count+1);
        CHECK(!wcscmp(chat->status,CHAT_SAVE_FAILED));
    }

    open_queue_clear(); read_set(NULL,0,CHAT_FILE_READ_OK);
    json_buf_free(&out);
    json_buf_free(&markdown);
    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(bom_path); DeleteFileW(empty_path);
    DeleteFileW(exact_path); DeleteFileW(over_path);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup);
    DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir);
    DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

/* ---- Tray identity and completion notifications --------------------------- */

static int notify_suite(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Notify host",1100,720,720,480,NULL,false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-notify-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    /* The test executable links no app.rc, so supply a shared stock icon the
       way chat_host_run's resource load would. */
    h->icon=LoadIconW(NULL,(LPCWSTR)IDI_APPLICATION);
    CHECK(h->icon);
    notify_reset();
    HWND window=CreateWindowW(cls.lpszClassName,L"Notify integration",WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    /* WM_CREATE added the tray with a callback message, an icon and a tooltip,
       then set the version the shell needs to deliver those events. */
    CHECK(h->tray_visible);
    CHECK(notify_count>=2);
    CHECK(notify_calls[0].operation==NIM_ADD);
    CHECK(notify_calls[0].data.hWnd==window);
    CHECK(notify_calls[0].data.uID==CHAT_TRAY_ICON_ID);
    CHECK(notify_calls[0].data.uCallbackMessage==CHAT_WM_TRAY);
    CHECK(notify_calls[0].data.hIcon==h->icon);
    CHECK((notify_calls[0].data.uFlags&(NIF_ICON|NIF_MESSAGE|NIF_TIP))==
        (NIF_ICON|NIF_MESSAGE|NIF_TIP));
    /* Version 4 suppresses the standard tooltip unless NIF_SHOWTIP is set. */
    CHECK((notify_calls[0].data.uFlags&NIF_SHOWTIP)!=0);
    CHECK(notify_calls[0].data.szTip[0]!=0);
    CHECK(notify_calls[1].operation==NIM_SETVERSION);
    CHECK(notify_calls[1].data.uVersion==NOTIFYICON_VERSION_4);
    CHECK((notify_calls[1].data.uFlags&NIF_SHOWTIP)!=0);

    /* A failed version call rolls the just-added icon back so no untracked
       icon can outlive a false tray_visible. */
    notify_reset();
    notify_fail_index=1;
    CHECK(!chat_tray_show(window,h->icon,CHAT_WM_TRAY,L"DarkChat"));
    CHECK(notify_op_count(NIM_ADD)==1);
    CHECK(notify_op_count(NIM_SETVERSION)==1);
    CHECK(notify_op_count(NIM_DELETE)==1);

    /* Explorer restart: the broadcast re-adds and re-sets the version. */
    notify_reset();
    UINT taskbar_msg=RegisterWindowMessageW(L"TaskbarCreated");
    CHECK(taskbar_msg);
    SendMessageW(window,taskbar_msg,0,0);
    CHECK(notify_op_count(NIM_ADD)==1);
    CHECK(notify_op_count(NIM_SETVERSION)==1);
    CHECK(h->tray_visible);

    /* Trigger matrix. An eligible long completion of an unfocused window
       raises exactly one balloon naming the conversation and state. A real
       keyless send first establishes the conversation and its user turn;
       each iteration regenerates the latest response. */
    rich_text_set_text(&h->composer,L"ask A");
    perform_send(h);
    CHECK(!h->generating &&
        pending(h)->generation.state==CHAT_GENERATION_FAILED);
    ChatConversation *conv_a=&chat->conversations[chat->active];
    uint64_t id_a=conv_a->id;
    wchar_t title_a[CHAT_TITLE_TEXT]; wcscpy(title_a,conv_a->title);
    foreground_window_forced=true; foreground_window_override=(HWND)1;
    notify_reset();
    begin_regenerate(h);
    h->started_tick=GetTickCount64()-6000;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"long answer"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(notify_op_count(NIM_MODIFY)==1);
    CHECK(h->notified_conversation_id==id_a);
    {
        NOTIFYICONDATAW *balloon=last_balloon();
        CHECK(balloon);
        CHECK(!wcscmp(balloon->szInfoTitle,L"DarkChat"));
        CHECK(wcsstr(balloon->szInfo,L"Complete")!=NULL);
        CHECK(wcsstr(balloon->szInfo,title_a)!=NULL);
    }
    /* A short completion is silent. */
    notify_reset();
    begin_regenerate(h); h->started_tick=GetTickCount64();
    handle_event(h,fixture(h,COMPLETION_DELTA,L"quick"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(notify_op_count(NIM_MODIFY)==0);
    /* A foreground completion is silent. */
    notify_reset();
    foreground_window_override=window;
    begin_regenerate(h); h->started_tick=GetTickCount64()-6000;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"focused"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(notify_op_count(NIM_MODIFY)==0);
    /* The preference disables it. */
    foreground_window_override=(HWND)1;
    chat->notify_disabled=1;
    notify_reset();
    begin_regenerate(h); h->started_tick=GetTickCount64()-6000;
    handle_event(h,fixture(h,COMPLETION_DELTA,L"ignored"));
    handle_event(h,fixture(h,COMPLETION_DONE,NULL));
    CHECK(notify_op_count(NIM_MODIFY)==0);
    chat->notify_disabled=0;
    /* A user cancellation is not announced. */
    notify_reset();
    begin_regenerate(h); h->started_tick=GetTickCount64()-6000;
    handle_event(h,fixture(h,COMPLETION_CANCELLED,NULL));
    CHECK(notify_op_count(NIM_MODIFY)==0);
    /* A failed long completion is announced. */
    notify_reset();
    begin_regenerate(h); h->started_tick=GetTickCount64()-6000;
    handle_event(h,fixture(h,COMPLETION_ERROR,L"boom"));
    CHECK(notify_op_count(NIM_MODIFY)==1);
    CHECK(wcsstr(last_balloon()->szInfo,L"Failed")!=NULL);

    /* Balloon lifetime: a newer request must not retarget an older balloon.
       Show A, start B, click the tray, and A is selected. */
    command(h,CHAT_COMMAND_NEW_CONVERSATION,-1);
    CHECK(chat_active(chat)->id!=id_a);
    set_foreground_calls=0;
    SendMessageW(window,CHAT_WM_TRAY,0,(LPARAM)WM_LBUTTONUP);
    CHECK(chat_active(chat)->id==id_a);
    CHECK(set_foreground_calls==1);

    /* Destroying the window removes the tray exactly once. */
    notify_reset();
    SendMessageW(window,WM_CLOSE,0,0);
    CHECK(!IsWindow(window));
    CHECK(notify_op_count(NIM_DELETE)==1);

    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup);
    DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir);
    DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    foreground_window_forced=false;
    return 0;
}

/* ---- DPI reflow + saved-geometry hardening suite ------------------------ */

static int hardening_suite(void) {
    /* Saved-geometry fallback is a pure predicate: the minimized sentinel and
       an off-monitor rectangle both force default placement, while an ordinary
       on-screen rectangle is kept. */
    CHECK(!saved_geometry_visible(-32000,0,800,600));
    CHECK(saved_geometry_visible(0,0,800,600));
    CHECK(!saved_geometry_visible(300000,-300000,800,600));

    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ChatHost *h=calloc(1,sizeof *h); Ui *ui=calloc(1,sizeof *ui); Chat *chat=calloc(1,sizeof *chat);
    CHECK(h && ui && chat); ui_init(ui,NULL,NULL); chat_init(chat); chat_clear(chat);
    h->config=(ChatHostConfig){ui,chat,L"Hardening host",1100,720,720,480,NULL,
        false};
    h->dpi=96; CHECK(chat_ui_init(&h->chat_ui,ui,chat));
    CHECK(SUCCEEDED(renderer_init(&h->renderer,&ui->theme)));
    h->background=CreateSolidBrush(RGB(20,20,20));
    wchar_t dir[256]; swprintf(dir,256,L"build\\host-harden-%lu",GetCurrentProcessId());
    CHECK(storage_open(&h->storage,dir));
    WNDCLASSW cls={0}; cls.lpfnWndProc=window_proc; cls.lpszClassName=L"DarkChat.HostTest";
    CHECK(register_class_once(&cls));
    WNDCLASSW view_cls={0}; view_cls.lpfnWndProc=view_proc; view_cls.lpszClassName=L"DarkChat.Transcript";
    CHECK(register_class_once(&view_cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Hardening integration",
        WS_OVERLAPPEDWINDOW,100,100,1100,720,NULL,NULL,NULL,h);
    CHECK(window); KillTimer(window,2);
    CHECK(saver_init(&h->saver,window,CHAT_WM_SAVER_RESULT,&h->storage));

    /* A DPI change carries the new scale in both words of wParam and a
       suggested rectangle in lParam. The host adopts the DPI on itself and
       re-derives every native surface (the fields, the transcript and the
       renderer), and takes the suggested rectangle. */
    CHECK(h->dpi==96);
    CHECK(h->field.window && h->search.window && h->composer.window);
    RECT suggested={0,0,2000,1400};
    SendMessageW(window,WM_DPICHANGED,MAKELONG(192,192),(LPARAM)&suggested);
    CHECK(h->dpi==192);
    CHECK(h->renderer.dpi==192);
    CHECK(h->field.dpi==192);
    CHECK(h->search.dpi==192);
    CHECK(h->composer.dpi==192);
    CHECK(h->transcript.dpi==192);
    RECT grown; GetWindowRect(window,&grown);
    CHECK(grown.left==0 && grown.top==0 &&
        grown.right==2000 && grown.bottom==1400);

    saver_shutdown(&h->saver); storage_close(&h->storage);
    DeleteFileW(h->storage.path); DeleteFileW(h->storage.backup);
    DeleteFileW(h->storage.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir);
    DeleteFileW(lock); RemoveDirectoryW(dir);
    ui_accessibility_destroy(h->accessibility); renderer_dispose(&h->renderer);
    DeleteObject(h->background);
    transcript_dispose(&h->transcript);
    chat_dispose(chat); free(chat); free(ui); free(h); CoUninitialize();
    return 0;
}

int main(void) {
    /* The fixtures share one process and never unload Msftedit: repeated
        unload/reload cycles across fixtures can fail its DllMain with
        ERROR_DLL_INIT_FAILED (1114), so the first fixture's load is kept for
        the whole run (each fixture frees only its own windows and bookkeeping). */
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
    failed=palette_suite();
    if (failed) return failed;
    failed=model_palette_suite();
    if (failed) return failed;
    failed=navigation_suite();
    if (failed) return failed;
    failed=code_copy_suite();
    if (failed) return failed;
    failed=export_suite();
    if (failed) return failed;
    failed=import_suite();
    if (failed) return failed;
    failed=notify_suite();
    if (failed) return failed;
    failed=hardening_suite();
    if (failed) return failed;
    puts("Hidden host (default + bounded + catalog + backend + palette + model palette + navigation + code copy + export + import + notifications + hardening fixtures) passed");
    return failed;
}
