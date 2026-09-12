#include "chat_host_win32.h"
#include "chat_ui.h"
#include "rich_text_win32.h"
#include "openrouter_winhttp.h"
#include "storage.h"
#include "actions_win32.h"
#include "../platform/renderer.h"
#include "../platform/accessibility.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ChatHostConfig config;
    UiRenderer renderer;
    UiAccessibility *accessibility;
    HWND window;
    ChatUi chat_ui;
    RichTextControl transcript, composer, field;
    RichTextTheme rich_theme;
    HBRUSH background;
    float dpi;
    bool tracking, minimized;
    unsigned retries;
    UiId accessibility_focus;
    OpenRouterClient client;
    int request_generation, request_conversation, request_message;
    bool generating, stopping, accepting, stream_visible, dirty, editing;
    ChatStorage storage;
    ULONGLONG started_tick;
    ChatGenerationState stop_state;

} ChatHost;

static void flush(ChatHost *host);
static void render_transcript(ChatHost *host);
static void perform_send(ChatHost *host);
static void action(ChatHost *host, int code);
static bool save(ChatHost *host);
static void capture_settings(ChatHost *host);

static int px(ChatHost *host, float dips) {
    return (int)lroundf(dips * host->dpi / 96.0f);
}

static float dip(ChatHost *host, int pixels) {
    return pixels * 96.0f / host->dpi;
}

static COLORREF rgb(UiColor color) { return RGB(color.r, color.g, color.b); }

/* Sizes a native surface to its placeholder, insetting for the drawn border. */
static void place(ChatHost *host, RichTextControl *control, UiId id,
    float inset) {
    UiNode *item = ui_node(host->config.ui, id);
    if (!item || !control->window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += inset;
    area.y += inset;
    area.w -= 2 * inset;
    area.h -= 2 * inset;
    if (area.w <= 2 || area.h <= 2) { ShowWindow(control->window, SW_HIDE); return; }
    int x = px(host, area.x), y = px(host, area.y);
    SetWindowPos(control->window, NULL, x, y, px(host, area.x + area.w) - x,
        px(host, area.y + area.h) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(control->window, SW_SHOWNOACTIVATE);
}

/* The single-line field is vertically centered inside its placeholder. */
static void place_field(ChatHost *host) {
    UiNode *item = ui_node(host->config.ui, host->chat_ui.model);
    if (!item || !host->field.window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += 1;
    area.w -= 2;
    float line = host->rich_theme.ui_size * 1.8f;
    float top = area.y + (area.h - line) / 2;
    if (top < area.y) top = area.y;
    float height = line < area.h ? line : area.h;
    if (area.w <= 2 || height <= 2) { ShowWindow(host->field.window, SW_HIDE); return; }
    int x = px(host, area.x), y = px(host, top);
    SetWindowPos(host->field.window, NULL, x, y, px(host, area.x + area.w) - x,
        px(host, top + height) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(host->field.window, SW_SHOWNOACTIVATE);
}

static void layout(ChatHost *host) {
    if (host->minimized) return;
    RECT client;
    GetClientRect(host->window, &client);
    chat_ui_resize(&host->chat_ui, dip(host, client.right), dip(host, client.bottom));
    place(host, &host->transcript, host->chat_ui.transcript, 1.0f);
    place(host, &host->composer, host->chat_ui.composer, 1.0f);
    place_field(host);
}

static void flush(ChatHost *host) {
    layout(host);
    if (host->accessibility &&
        host->accessibility_focus != host->config.ui->focus) {
        host->accessibility_focus = host->config.ui->focus;
        ui_accessibility_focus_changed(host->accessibility,
            host->accessibility_focus ? host->accessibility_focus
                                      : host->config.ui->root);
    }
    if (GetCapture() == host->window && !host->config.ui->pressed &&
        !host->config.ui->drag_scroll) ReleaseCapture();
    if (host->config.ui->paint_dirty) InvalidateRect(host->window, NULL, FALSE);
}

static void sync_model(ChatHost *host) {
    wchar_t buffer[CHAT_MODEL_TEXT];
    rich_text_get_text(&host->field, buffer, CHAT_MODEL_TEXT);
    wchar_t *model = buffer;
    while (*model == L' ' || *model == L'\t') ++model;
    size_t length = wcslen(model);
    while (length && (model[length - 1] == L' ' || model[length - 1] == L'\t'))
        model[--length] = 0;
    if (length) {
        wcsncpy(host->config.chat->model, model, CHAT_MODEL_TEXT - 1);
        host->config.chat->model[CHAT_MODEL_TEXT - 1] = 0;
    }
    /* Never let the visible field and the stored model disagree: an empty field
       falls back to the last valid model, which is written back into the field. */
    rich_text_set_text(&host->field, host->config.chat->model);
    host->dirty = true;
}

static void field_blur(void *user) { sync_model((ChatHost *)user); }


static void focus_surface(ChatHost *host, bool reverse) {
    HWND order[3] = { host->field.window, host->composer.window,
        host->transcript.window };
    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < 3; i++) if (order[i] == focus) current = i;
    int next = current < 0 ? 0 : (current + (reverse ? -1 : 1) + 3) % 3;
    SetFocus(order[next]);
}

static void render_transcript(ChatHost *host) {
    host->stream_visible = false;
    rich_text_clear(&host->transcript);
    const ChatConversation *c = chat_active(host->config.chat);
    if (!c) return;
    for (int i = 0; i < c->message_count; i++) {
        const ChatMessage *m = &c->messages[i];
        const ChatGeneration *g = &m->generation;
        if (host->generating && i == host->request_message &&
            host->config.chat->active == host->request_conversation) {
            rich_text_begin_stream(&host->transcript);
            rich_text_append_stream(&host->transcript, m->text);
            host->stream_visible = true;
        } else {
            rich_text_append_message(&host->transcript, m->role, m->text);
            if (m->role == CHAT_ROLE_ASSISTANT && g->state != CHAT_GENERATION_NONE) {
                wchar_t info[768], usage[160], timing[120], cost[64], ttft[40], latency[40];
                if (g->total_tokens >= 0)
                    swprintf(usage,160,L"Tokens: %.0f input / %.0f output / %.0f total",
                        g->prompt_tokens,g->completion_tokens,g->total_tokens);
                else wcscpy(usage,L"Tokens: unavailable");
                if (g->ttft_ms >= 0) swprintf(ttft,40,L"%.0f ms",g->ttft_ms);
                else wcscpy(ttft,L"unavailable");
                if (g->latency_ms >= 0) swprintf(latency,40,L"%.0f ms",g->latency_ms);
                else wcscpy(latency,L"unavailable");
                swprintf(timing,120,L"TTFT: %ls | Latency: %ls",ttft,latency);
                if (g->cost >= 0) swprintf(cost,64,L"Cost: $%.8f",g->cost);
                else wcscpy(cost,L"Cost: unavailable");
                swprintf(info,768,L"%ls | Requested: %ls | Actual: %ls\n%ls | %ls | %ls\nFinish: %ls",
                    chat_generation_name(g->state),g->requested_model,
                    g->actual_model[0] ? g->actual_model : L"unavailable",timing,usage,cost,
                    g->finish_reason[0] ? g->finish_reason : L"unavailable");
                rich_text_append_message(&host->transcript,CHAT_ROLE_SYSTEM,info);
                if (g->error[0]) rich_text_append_message(&host->transcript,CHAT_ROLE_ERROR,g->error);
            }
        }
    }
    rich_text_scroll_to_end(&host->transcript);
}

static void set_status(ChatHost *host, const wchar_t *text) {
    wcsncpy(host->config.chat->status, text, CHAT_STATUS_TEXT - 1);
    host->config.chat->status[CHAT_STATUS_TEXT - 1] = 0;
    chat_ui_sync(&host->chat_ui);
    flush(host);
}

static ChatMessage *pending(ChatHost *host) {
    return &host->config.chat->conversations[host->request_conversation].messages[host->request_message];
}

static bool save(ChatHost *host) {
    if (!host->dirty) return true;
    if (storage_save(&host->storage,host->config.chat)) { host->dirty=false; return true; }
    set_status(host,L"Save failed: changes are in memory; check storage permissions or disk space.");
    return false;
}

static void capture_settings(ChatHost *host) {
    Chat *chat=host->config.chat;
    wchar_t draft[CHAT_MESSAGE_TEXT];
    rich_text_get_text(&host->composer,draft,CHAT_MESSAGE_TEXT);
    ChatConversation *c=&chat->conversations[chat->active];
    if (!host->editing && wcscmp(draft,c->draft)) {
        wcscpy(c->draft,draft); c->modified_at=chat_now(); host->dirty=true;
    }
    wchar_t model[CHAT_MODEL_TEXT];
    rich_text_get_text(&host->field,model,CHAT_MODEL_TEXT);
    if (model[0] && wcscmp(model,chat->model)) { wcscpy(chat->model,model); host->dirty=true; }
    WINDOWPLACEMENT placement={0}; placement.length=sizeof placement;
    if (GetWindowPlacement(host->window,&placement)) {
        RECT r=placement.rcNormalPosition;
        int width=(int)dip(host,r.right-r.left), height=(int)dip(host,r.bottom-r.top);
        int maximized=IsIconic(host->window) ? chat->maximized : IsZoomed(host->window)!=0;
        if (chat->window_x!=r.left || chat->window_y!=r.top || chat->window_width!=width ||
            chat->window_height!=height || chat->maximized!=maximized) {
            chat->window_x=r.left; chat->window_y=r.top;
            chat->window_width=width<720 ? 720 : width;
            chat->window_height=height<480 ? 480 : height;
            chat->maximized=maximized; host->dirty=true;
        }
    }
}

static void start_response(ChatHost *host, ChatSendMode mode, const wchar_t *prompt) {
    if (host->generating) return;
    Chat *chat=host->config.chat;
    sync_model(host);
    int index=chat_begin_response(chat,mode,prompt);
    if (index<0) { set_status(host,L"Action unavailable: check the latest turn and conversation capacity."); return; }
    host->request_conversation=chat->active; host->request_message=index;
    host->started_tick=GetTickCount64();
    ChatMessage *m=pending(host);
    if (mode==CHAT_SEND) { rich_text_set_text(&host->composer,L""); chat->conversations[chat->active].draft[0]=0; }
    if (host->editing && mode!=CHAT_SEND) {
        host->editing=false;
        rich_text_set_text(&host->composer,chat->conversations[chat->active].draft);
    }
    host->dirty=true;
    /* Persist the user turn and pending response before starting network work. */
    bool saved=save(host);
    OpenRouterMessage messages[CHAT_MAX_MESSAGES+1]; int used=0;
    if (chat->system_prompt[0]) messages[used++]=(OpenRouterMessage){CHAT_ROLE_SYSTEM,chat->system_prompt};
    ChatConversation *c=&chat->conversations[chat->active];
    for (int i=0;i<index;i++) if (chat_history_message(&c->messages[i]))
        messages[used++]=(OpenRouterMessage){c->messages[i].role,c->messages[i].text};
    host->request_generation=saved ? openrouter_request(&host->client,
        host->config.api_key_utf8,chat->model,messages,used) : 0;
    if (!host->request_generation) {
        m->generation.state=CHAT_GENERATION_FAILED;
        m->generation.finished_at=chat_now();
        m->generation.latency_ms=0;
        wcscpy(m->generation.error,!saved ? L"Could not save pending response; request was not sent." :
            !host->config.api_key_utf8 || !host->config.api_key_utf8[0] ?
            L"OPENROUTER_API_KEY is unavailable. Set the Windows User environment variable and restart." :
            L"Could not start OpenRouter request.");
        host->dirty=true; save(host); set_status(host,L"Request failed; use Response > Retry.");
    } else {
        host->generating=true; host->stopping=false; host->accepting=true;
        host->stop_state=CHAT_GENERATION_CANCELLED;
        chat_ui_set_generation(&host->chat_ui,true,false);
        EnableWindow(host->field.window,FALSE);
        set_status(host,L"Generating...");
    }
    render_transcript(host); chat_ui_sync(&host->chat_ui); flush(host);
}

static void perform_send(ChatHost *host) {
    if (host->generating) {
        if (!host->stopping && openrouter_cancel(&host->client,host->request_generation)) {
            host->stopping=true; host->accepting=false;
            pending(host)->generation.state=CHAT_GENERATION_CANCELLED;
            pending(host)->generation.finished_at=chat_now();
            pending(host)->generation.latency_ms=(double)(GetTickCount64()-host->started_tick);
            host->dirty=true; save(host);
            chat_ui_set_generation(&host->chat_ui,true,true);
            set_status(host,L"Stopping generation...");
        }
        return;
    }
    wchar_t prompt[CHAT_MESSAGE_TEXT];
    rich_text_get_text(&host->composer,prompt,CHAT_MESSAGE_TEXT);
    start_response(host,host->editing ? CHAT_EDIT_RESEND : CHAT_SEND,prompt);
}

static void append_stream_delta(ChatHost *host, OpenRouterEvent *event) {
    if (!event->text || !event->text[0] || !host->accepting) return;
    ChatMessage *m=pending(host);
    m->generation=event->metadata; m->generation.state=CHAT_GENERATION_RUNNING;
    size_t length=wcslen(m->text), total=wcslen(event->text), incoming=total;
    if (incoming>CHAT_MESSAGE_TEXT-1-length) incoming=CHAT_MESSAGE_TEXT-1-length;
    if (incoming && event->text[incoming-1]>=0xd800 && event->text[incoming-1]<=0xdbff) --incoming;
    wmemcpy(m->text+length,event->text,incoming); m->text[length+incoming]=0;
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->dirty=true;
    if (host->stream_visible && host->request_conversation==host->config.chat->active) {
        event->text[incoming]=0; rich_text_append_stream(&host->transcript,event->text);
    }
    if (incoming<total) {
        host->stop_state=CHAT_GENERATION_INTERRUPTED;
        host->accepting=false; host->stopping=true;
        openrouter_cancel(&host->client,host->request_generation);
        chat_ui_set_generation(&host->chat_ui,true,true);
        set_status(host,L"Response reached the message limit; partial text preserved.");
    }
}

static void finish_request(ChatHost *host, OpenRouterEvent *event) {
    openrouter_complete(&host->client,event->generation);
    ChatMessage *m=pending(host);
    m->generation=event->metadata;
    ChatGeneration *g=&m->generation;
    g->state=host->stopping ? host->stop_state : event->type==OPENROUTER_DONE ?
        CHAT_GENERATION_COMPLETE : event->type==OPENROUTER_CANCELLED ?
        CHAT_GENERATION_CANCELLED : event->type==OPENROUTER_INTERRUPTED ?
        CHAT_GENERATION_INTERRUPTED : CHAT_GENERATION_FAILED;
    if (event->text) wcsncpy(g->error,event->text,511);
    if (g->state==CHAT_GENERATION_COMPLETE && !wcscmp(g->finish_reason,L"error")) g->state=CHAT_GENERATION_FAILED;
    if (g->state==CHAT_GENERATION_COMPLETE && !m->text[0]) {
        g->state=CHAT_GENERATION_FAILED; wcscpy(g->error,L"OpenRouter completed without text.");
    }
    if (host->stopping && host->stop_state==CHAT_GENERATION_INTERRUPTED) wcscpy(g->error,L"Response exceeded the local message limit.");
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->generating=false; host->stopping=false; host->accepting=false;
    chat_ui_set_generation(&host->chat_ui,false,false); EnableWindow(host->field.window,TRUE);
    set_status(host,chat_generation_name(g->state));
    host->dirty=true; save(host);
    if (host->request_conversation==host->config.chat->active) render_transcript(host);
    chat_ui_sync(&host->chat_ui); flush(host);
}

static void handle_event(ChatHost *host, OpenRouterEvent *event) {
    if (!event) return;
    if (event->generation==host->request_generation && host->generating) {
        if (event->type==OPENROUTER_DELTA) append_stream_delta(host,event);
        else finish_request(host,event);
    }
    openrouter_event_free(event);
}

static void command(void *user, ChatCommand code, int index) {
    ChatHost *host = (ChatHost *)user;
    Chat *chat = host->config.chat;
    if (code != CHAT_COMMAND_SEND) {
        capture_settings(host);
        host->editing=false;
    }
    if (code == CHAT_COMMAND_SEND) {
        perform_send(host);
    } else if (code == CHAT_COMMAND_NEW_CONVERSATION) {
        if (chat_new_conversation(chat) >= 0) {
            render_transcript(host);
            rich_text_set_text(&host->composer, L"");
            chat_ui_sync(&host->chat_ui);
        }
    } else if (code == CHAT_COMMAND_SELECT) {
        if (chat_select_conversation(chat, index)) {
            render_transcript(host);
            rich_text_set_text(&host->composer,chat->conversations[chat->active].draft);
            chat_ui_sync(&host->chat_ui);
        }
    }
    host->dirty=true; save(host);
    ui_invalidate(host->config.ui, false);
    flush(host);
}

static void action(ChatHost *host, int code) {
    Chat *chat=host->config.chat;
    ChatConversation *c=&chat->conversations[chat->active];
    if (code==ACTION_COPY) {
        for (int i=c->message_count-1;i>=0;i--) if (c->messages[i].role==CHAT_ROLE_ASSISTANT) {
            set_status(host,chat_copy_text(host->window,c->messages[i].text) ? L"Response copied" : L"Copy failed"); return;
        }
        return;
    }
    if (code==ACTION_SELECTION) { SendMessageW(host->transcript.window,WM_COPY,0,0); return; }
    if (code==ACTION_NEW) { command(host,CHAT_COMMAND_NEW_CONVERSATION,-1); return; }
    if (host->generating) { set_status(host,L"Stop generation before changing history or settings."); return; }
    capture_settings(host);
    if (code==ACTION_CANCEL_EDIT) {
        host->editing=false; rich_text_set_text(&host->composer,c->draft); set_status(host,L"Edit cancelled");
    } else if (code==ACTION_RETRY || code==ACTION_REGENERATE) {
        start_response(host,code==ACTION_RETRY ? CHAT_RETRY : CHAT_REGENERATE,NULL); return;
    } else if (code==ACTION_EDIT) {
        int user=chat_latest_user(c);
        if (user<0) { set_status(host,L"No user message to edit"); return; }
        host->editing=true; rich_text_set_text(&host->composer,c->messages[user].text);
        SetFocus(host->composer.window); set_status(host,L"Editing latest user message. Send replaces its response; Response > Cancel edit restores the draft.");
    } else if (code==ACTION_RENAME) {
        wchar_t title[CHAT_TITLE_TEXT]; wcscpy(title,c->title);
        if (chat_edit_dialog(host->window,L"Rename conversation",title,CHAT_TITLE_TEXT,false)) chat_rename(chat,title);
    } else if (code==ACTION_DELETE || code==ACTION_CLEAR) {
        if (MessageBoxW(host->window,code==ACTION_DELETE ? L"Delete this conversation?" : L"Clear all messages in this conversation?",
            L"DarkChat",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        if (code==ACTION_DELETE) chat_delete(chat); else chat_clear(chat);
        host->editing=false; rich_text_set_text(&host->composer,chat->conversations[chat->active].draft); render_transcript(host);
    } else if (code==ACTION_SYSTEM) {
        wchar_t prompt[CHAT_MESSAGE_TEXT]; wcscpy(prompt,chat->system_prompt);
        if (chat_edit_dialog(host->window,L"System prompt (applies to future requests)",prompt,CHAT_MESSAGE_TEXT,true))
            wcscpy(chat->system_prompt,prompt);
    } else if (code==ACTION_SIDEBAR) {
        wchar_t width[16]; swprintf(width,16,L"%d",chat->sidebar_width);
        if (chat_edit_dialog(host->window,L"Sidebar width in DIPs (160-360)",width,16,false)) {
            wchar_t *end; long value=wcstol(width,&end,10);
            if (!*end && value>=160 && value<=360) chat->sidebar_width=(int)value;
            else set_status(host,L"Sidebar width must be 160-360");
        }
    } else if (code==ACTION_MODELS) {
        wchar_t prefix[CHAT_MODEL_TEXT]; rich_text_get_text(&host->field,prefix,CHAT_MODEL_TEXT);
        HMENU menu=CreatePopupMenu(); int matches=0;
        for (int i=0;i<chat->model_history_count;i++) if (!prefix[0] || !wcsncmp(chat->model_history[i],prefix,wcslen(prefix))) {
            AppendMenuW(menu,MF_STRING,1000+i,chat->model_history[i]); ++matches;
        }
        if (!matches) for (int i=0;i<chat->model_history_count;i++) AppendMenuW(menu,MF_STRING,1000+i,chat->model_history[i]);
        if (!chat->model_history_count) AppendMenuW(menu,MF_GRAYED,0,L"Model history is empty; send using a model first.");
        RECT r; GetWindowRect(host->field.window,&r);
        int selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.left,r.bottom,0,host->window,NULL);
        DestroyMenu(menu);
        if (selected>=1000 && selected<1000+chat->model_history_count) {
            wcscpy(chat->model,chat->model_history[selected-1000]); rich_text_set_text(&host->field,chat->model);
        }
    }
    host->dirty=true; save(host); chat_ui_sync(&host->chat_ui); flush(host);
}

static bool surface_key(void *user, WPARAM key, bool shift, bool control,
    bool down) {
    ChatHost *host = (ChatHost *)user;
    if (control && key == VK_SPACE && down) { action(host,ACTION_MODELS); return true; }
    if (key == VK_TAB && down) { focus_surface(host, shift); return true; }
    return false;
}

static bool composer_submit(void *user) {
    perform_send((ChatHost *)user);
    return true;
}

static bool field_submit(void *user) {
    ChatHost *host = (ChatHost *)user;
    sync_model(host);
    SetFocus(host->composer.window);
    return true;
}

static bool key_from_win32(WPARAM key, UiKey *out) {
    switch (key) {
    case VK_TAB: *out = UI_KEY_TAB; break;
    case VK_RETURN: *out = UI_KEY_ENTER; break;
    case VK_SPACE: *out = UI_KEY_SPACE; break;
    case VK_ESCAPE: *out = UI_KEY_ESCAPE; break;
    case VK_LEFT: *out = UI_KEY_LEFT; break;
    case VK_RIGHT: *out = UI_KEY_RIGHT; break;
    case VK_UP: *out = UI_KEY_UP; break;
    case VK_DOWN: *out = UI_KEY_DOWN; break;
    case VK_HOME: *out = UI_KEY_HOME; break;
    case VK_END: *out = UI_KEY_END; break;
    case VK_PRIOR: *out = UI_KEY_PAGE_UP; break;
    case VK_NEXT: *out = UI_KEY_PAGE_DOWN; break;
    default: return false;
    }
    return true;
}

static void wheel(ChatHost *host, WPARAM w, LPARAM l) {
    Ui *u = host->config.ui;
    POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(host->window, &point);
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    float step = lines == WHEEL_PAGESCROLL ? u->height * .9f
                                           : lines * u->theme.control_height;
    float delta = -(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * step;
    ui_scroll(u, dip(host, point.x), dip(host, point.y), delta);
    flush(host);
}

static void host_event(void *user, Ui *ui, UiEvent event) {
    (void)ui;
    ChatHost *host = (ChatHost *)user;
    chat_ui_event(&host->chat_ui, host->config.ui, event);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    ChatHost *host = (ChatHost *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        host = ((CREATESTRUCTW *)l)->lpCreateParams;
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)host);
    }
    if (!host) return DefWindowProcW(window, message, w, l);
    Ui *u = host->config.ui;
    switch (message) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(window);
        host->dpi = dpi ? (float)dpi : 96;
        host->renderer.dpi = host->dpi;
        if (!rich_text_library_open()) return -1;
        rich_text_theme(&host->rich_theme, &u->theme);
        if (!rich_text_create_transcript(&host->transcript, window, 1,
            &host->rich_theme, host->dpi)) return -1;
        if (!rich_text_create_composer(&host->composer, window, 2,
            &host->rich_theme, host->dpi)) return -1;
        if (!rich_text_create_field(&host->field, window, 3, &host->rich_theme,
            host->dpi, host->config.chat->model)) return -1;
        host->composer.on_submit = composer_submit;
        host->composer.on_key = surface_key;
        host->composer.user = host;
        host->field.on_submit = field_submit;
        host->field.on_key = surface_key;
        host->field.on_blur = field_blur;
        host->field.user = host;
        host->transcript.on_key = surface_key;
        host->transcript.user = host;
        openrouter_init(&host->client, window, CHAT_WM_OPENROUTER_EVENT);
        if (!ui_accessible_name(u, u->root)[0])
            ui_set_accessible_name(u, u->root, host->config.title);
        host->accessibility = ui_accessibility_create(window, u);
        if (!host->accessibility) return -1;
        SetMenu(window,chat_actions_menu());
        SetTimer(window,2,1000,NULL);
        rich_text_set_text(&host->composer,host->config.chat->conversations[host->config.chat->active].draft);
        render_transcript(host);
        return 0;
    }
    case WM_GETOBJECT: {
        LRESULT result = ui_accessibility_get_object(host->accessibility, w, l);
        if (result) return result;
        break;
    }
    case UI_WM_ACCESSIBILITY_INVOKE:
        ui_accessibility_handle_message(host->accessibility, message, w);
        flush(host);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        if (!host->minimized) {
            layout(host);
            HRESULT hr = renderer_paint(&host->renderer, window, u, UI_NONE);
            if (FAILED(hr)) {
                FillRect(paint.hdc, &paint.rcPaint, host->background);
                wchar_t error[96];
                swprintf(error, 96, L"DarkChat: rendering failed (0x%08lX)\n",
                    (unsigned long)hr);
                OutputDebugStringW(error);
                if (host->retries++ < 3) SetTimer(window, 1, 250, NULL);
            } else {
                host->retries = 0;
                KillTimer(window, 1);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_COMMAND:
        if (!l) { action(host,LOWORD(w)); return 0; }
        break;
    case WM_TIMER:
        if (w == 2) {
            /* If allocation/PostMessage failed for the terminal event, the
               worker can finish without notifying us. Drain queued events
               before declaring this failure so a queued DONE always wins. */
            if (host->generating && host->client.thread &&
                WaitForSingleObject(host->client.thread,0)==WAIT_OBJECT_0) {
                MSG queued;
                while (PeekMessageW(&queued,window,CHAT_WM_OPENROUTER_EVENT,CHAT_WM_OPENROUTER_EVENT,PM_REMOVE))
                    handle_event(host,(OpenRouterEvent *)queued.lParam);
                if (host->generating) {
                    OpenRouterEvent lost={0}; lost.generation=host->request_generation;
                    lost.type=OPENROUTER_ERROR; lost.metadata=pending(host)->generation;
                    lost.metadata.finished_at=chat_now();
                    lost.metadata.latency_ms=(double)(GetTickCount64()-host->started_tick);
                    lost.text=L"Worker ended without a completion event.";
                    finish_request(host,&lost);
                }
            }
            capture_settings(host);
            if (save(host) && host->generating) {
                wchar_t status[CHAT_STATUS_TEXT];
                swprintf(status,CHAT_STATUS_TEXT,L"%ls | %ls | %.1f s elapsed",
                    host->stopping ? L"Stopping" : L"Generating",pending(host)->generation.requested_model,
                    (GetTickCount64()-host->started_tick)/1000.0);
                set_status(host,status);
            }
        }
        if (w == 1) { KillTimer(window, 1); InvalidateRect(window, NULL, FALSE); }
        return 0;
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)l;
        if (host->transcript.window &&
            header->hwndFrom == host->transcript.window &&
            rich_text_handle_notify(&host->transcript, l)) return 0;
        break;
    }
    case WM_SIZE:
        host->minimized = w == SIZE_MINIMIZED;
        if (!host->minimized) {
            RECT r;
            GetClientRect(window, &r);
            renderer_resize(&host->renderer, (UINT)r.right, (UINT)r.bottom,
                host->dpi);
            ui_invalidate(u, true);
            flush(host);
        }
        return 0;
    case WM_DPICHANGED: {
        host->dpi = (float)HIWORD(w);
        RECT *r = (RECT *)l;
        rich_text_set_dpi(&host->field, host->dpi);
        rich_text_set_dpi(&host->composer, host->dpi);
        rich_text_set_dpi(&host->transcript, host->dpi);
        renderer_resize(&host->renderer, 0, 0, host->dpi);
        ui_invalidate(u, true);
        SetWindowPos(window, NULL, r->left, r->top, r->right - r->left,
            r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        flush(host);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)l;
        UINT dpi = host->dpi > 0 ? (UINT)host->dpi : GetDpiForSystem();
        RECT r = { 0, 0, MulDiv(host->config.min_width, (int)dpi, 96),
            MulDiv(host->config.min_height, (int)dpi, 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
        info->ptMinTrackSize.x = r.right - r.left;
        info->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!host->tracking) {
            TRACKMOUSEEVENT tracking = { sizeof tracking, TME_LEAVE, window, 0 };
            host->tracking = TrackMouseEvent(&tracking) != FALSE;
        }
        ui_pointer_move(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        flush(host);
        return 0;
    case WM_MOUSELEAVE:
        host->tracking = false; ui_pointer_leave(u); flush(host); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        layout(host);
        ui_pointer_down(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        if (u->pressed) SetCapture(window);
        flush(host);
        return 0;
    case WM_LBUTTONUP:
        ui_pointer_up(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        flush(host);
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != window) { ui_cancel_input(u); InvalidateRect(window, NULL, FALSE); }
        return 0;
    case WM_CANCELMODE:
        ui_cancel_input(u);
        if (GetCapture() == window) ReleaseCapture();
        flush(host);
        return 0;
    case WM_MOUSEWHEEL: wheel(host, w, l); return 0;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        UiKey key;
        if (key_from_win32(w, &key)) {
            ui_key(u, key, message == WM_KEYDOWN,
                (GetKeyState(VK_SHIFT) & 0x8000) != 0, (l & (1L << 30)) != 0);
            flush(host);
            return 0;
        }
        break;
    }
    case WM_CHAR: return 0;
    case WM_SETFOCUS: ui_set_active(u, true); flush(host); return 0;
    case WM_KILLFOCUS:
        if ((HWND)w != host->field.window &&
            (HWND)w != host->composer.window &&
            (HWND)w != host->transcript.window) {
            ui_set_active(u, false);
            flush(host);
        }
        return 0;
    case CHAT_WM_OPENROUTER_EVENT:
        handle_event(host, (OpenRouterEvent *)l);
        return 0;
    case WM_CLOSE:
        capture_settings(host);
        if (host->generating) {
            ChatGeneration *g=&pending(host)->generation;
            g->state=host->stopping ? host->stop_state : CHAT_GENERATION_INTERRUPTED;
            g->finished_at=chat_now();
            g->latency_ms=(double)(GetTickCount64()-host->started_tick);
            wcscpy(g->error,L"Window closed before generation finished.");
            host->dirty=true;
            save(host);
            openrouter_shutdown(&host->client);
            host->generating = false;
            MSG queued;
            while (PeekMessageW(&queued, window, CHAT_WM_OPENROUTER_EVENT,
                CHAT_WM_OPENROUTER_EVENT, PM_REMOVE))
                openrouter_event_free((OpenRouterEvent *)queued.lParam);
        }
        if (!save(host) && MessageBoxW(window,L"Changes could not be saved. Close anyway and lose unsaved changes?",
            L"DarkChat",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) {
            chat_ui_set_generation(&host->chat_ui,false,false); EnableWindow(host->field.window,TRUE);
            render_transcript(host); return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_ACTIVATE:
        ui_set_active(u, LOWORD(w) != WA_INACTIVE);
        flush(host);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        KillTimer(window, 2);
        renderer_drop_target(&host->renderer);
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, w, l);
}

int chat_host_run(HINSTANCE instance, int show, const ChatHostConfig *config) {
    if (!config || !config->ui || !config->chat) return 1;
    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) return 1;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    ChatHost *host = (ChatHost *)calloc(1, sizeof *host);
    int result = 1;
    if (!host) { CoUninitialize(); return result; }
    host->config = *config;
    if (!storage_open(&host->storage,NULL)) {
        MessageBoxW(NULL,L"Cannot open DarkChat storage. Another instance may be running, or the directory is unavailable.",L"DarkChat",MB_OK|MB_ICONERROR);
        goto cleanup;
    }
    int loaded=storage_load(&host->storage,config->chat);
    if (loaded<0) {
        MessageBoxW(NULL,L"No valid supported DarkChat snapshot was found. Storage files have been preserved. Restore a valid state.jsonl before restarting.",L"DarkChat",MB_OK|MB_ICONERROR);
        goto cleanup;
    }
    host->dirty=true;
    if (host->storage.recovered) wcscpy(config->chat->status,L"Recovered the last valid snapshot from backup.");
    host->dpi = (float)GetDpiForSystem();
    if (!chat_ui_init(&host->chat_ui, config->ui, config->chat)) goto cleanup;
    host->chat_ui.command = command;
    host->chat_ui.command_user = host;
    config->ui->on_event = host_event;
    config->ui->event_user = host;
    HRESULT hr = renderer_init(&host->renderer, &config->ui->theme);
    if (FAILED(hr)) goto cleanup;
    config->ui->measure = renderer_measure;
    config->ui->measure_user = &host->renderer;
    host->background = CreateSolidBrush(rgb(config->ui->theme.colors[UI_BG]));
    if (!host->background) goto cleanup;
    WNDCLASSEXW cls = { 0 };
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = L"DarkChat.Window";
    if (!RegisterClassExW(&cls)) goto cleanup;
    RECT bounds = { 0, 0, px(host, (float)config->chat->window_width),
        px(host, (float)config->chat->window_height) };
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0,
        (UINT)host->dpi);
    int x=config->chat->window_x, y=config->chat->window_y;
    RECT saved_rect={x,y,x+px(host,(float)config->chat->window_width),y+px(host,(float)config->chat->window_height)};
    if (x==-32000 || !MonitorFromRect(&saved_rect,MONITOR_DEFAULTTONULL)) x=y=CW_USEDEFAULT;
    HWND window = CreateWindowExW(0, cls.lpszClassName, config->title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y,
        loaded ? px(host,(float)config->chat->window_width) : bounds.right - bounds.left,
        loaded ? px(host,(float)config->chat->window_height) : bounds.bottom - bounds.top, NULL, NULL,
        instance, host);
    if (window) {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
        ShowWindow(window, config->chat->maximized ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(window);
        if (host->composer.window) SetFocus(host->composer.window);
        MSG message;
        BOOL got;
        while ((got = GetMessageW(&message, NULL, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        result = got < 0 ? 1 : (int)message.wParam;
        if (IsWindow(window)) DestroyWindow(window);
    }
    UnregisterClassW(cls.lpszClassName, instance);
cleanup:
    /* Never exit under a running worker: it still owns its request snapshot. */
    openrouter_shutdown(&host->client);
    ui_accessibility_destroy(host->accessibility);
    config->ui->measure = NULL;
    config->ui->measure_user = NULL;
    renderer_dispose(&host->renderer);
    if (host->background) DeleteObject(host->background);
    rich_text_library_close();
    storage_close(&host->storage);
    free(host);
    CoUninitialize();
    if (result) MessageBoxW(NULL,
        L"DarkChat could not initialize. Check the graphics runtime and available resources.",
        L"DarkChat", MB_OK | MB_ICONERROR);
    return result;
}





