#include "chat_host_win32.h"
#include "chat_ui.h"
#include "rich_text_win32.h"
#include "transcript_win32.h"
#include "openrouter_winhttp.h"
#include "storage.h"
#include "actions_win32.h"
#include "../platform/renderer.h"
#include "../platform/accessibility.h"
#include <windowsx.h>
#include <richedit.h>
#include <dwmapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* One-shot flush for dirty streaming content; the status sweep and the
   paint-retry timers use ids 2 and 1. */
#define CHAT_TIMER_BODY_FLUSH 3

typedef struct {
    ChatHostConfig config;
    UiRenderer renderer;
    UiAccessibility *accessibility;
    HWND window;
    ChatUi chat_ui;
    RichTextControl composer, field;
    HWND view;                          /* transcript container child window */
    Transcript transcript;              /* per-turn update bookkeeping */
    RichTextTheme rich_theme;
    HBRUSH background;
    float dpi;
    bool tracking, minimized;
    unsigned retries;
    UiId accessibility_focus;
    OpenRouterClient client;
    int request_generation, request_conversation, request_message;
    bool generating, stopping, accepting, dirty, editing, content_started;
    bool reasoning_streaming;
    /* Appended text not yet written to the live body; a scheduled flush
       guarantees it renders even when the stream pauses. */
    bool body_flush_pending;
    ULONGLONG reasoning_started_tick;
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
static bool surface_key(void *user, WPARAM key, bool shift, bool control,
    bool down);
static void refresh_turn(ChatHost *host, int index);
static void stream_body_markdown(ChatHost *host, int index);
static void position_turns(ChatHost *host, bool follow);
static void schedule_body_flush(ChatHost *host);
static void cancel_body_flush(ChatHost *host);
static void flush_stream_body(ChatHost *host);
static bool turn_row_click(void *user, RichTextControl *control, int line,
    bool down);

static int px(ChatHost *host, float dips) {
    return (int)lroundf(dips * host->dpi / 96.0f);
}

static float dip(ChatHost *host, int pixels) {
    return pixels * 96.0f / host->dpi;
}

static COLORREF rgb(UiColor color) { return RGB(color.r, color.g, color.b); }

/* Transcript entry points; the streaming context is fed on every call so the
   update bookkeeping never reads host fields directly. */
static TranscriptFeed transcript_feed(ChatHost *host) {
    Chat *chat = host->config.chat;
    TranscriptFeed feed;
    feed.chat = chat;
    feed.generating = host->generating;
    feed.content_started = host->content_started;
    feed.reasoning_streaming = host->reasoning_streaming;
    feed.request_conversation = host->request_conversation;
    feed.request_message = host->request_message;
    return feed;
}
static void render_transcript(ChatHost *host) {
    TranscriptFeed feed = transcript_feed(host);
    transcript_render(&host->transcript, &feed);
}
static void refresh_turn(ChatHost *host, int index) {
    TranscriptFeed feed = transcript_feed(host);
    transcript_refresh_turn(&host->transcript, &feed, index);
}
static void stream_body_markdown(ChatHost *host, int index) {
    TranscriptFeed feed = transcript_feed(host);
    transcript_stream_body(&host->transcript, &feed, index);
}
static void position_turns(ChatHost *host, bool follow) {
    transcript_position(&host->transcript, follow);
}
/* Arms a one-shot flush so deltas appended inside the throttle window reach
   the body even if the stream then pauses with no further delta to cross it.
   body_flush_pending always reflects whether text is still unrendered: it stays
   set only while a flush is actually armed, so a failed arm can never block
   later deltas from trying again. */
static void schedule_body_flush(ChatHost *host) {
    if (!host->window) {
        host->body_flush_pending = false;
        return;
    }
    ULONGLONG elapsed = GetTickCount64() - host->transcript.body_render_tick;
    UINT delay = elapsed >= CHAT_BODY_RENDER_MS ? 1
        : (UINT)(CHAT_BODY_RENDER_MS - elapsed);
    if (SetTimer(host->window, CHAT_TIMER_BODY_FLUSH, delay, NULL)) return;
    /* Timer creation can fail under resource pressure. Render immediately
       instead of stranding the dirty text until another delta or completion;
       one unthrottled render is the accepted cost. */
    flush_stream_body(host);
}
/* Drops any scheduled or pending body flush; used when content is rendered
   whole (first token, terminal render) or the target turn is no longer live. */
static void cancel_body_flush(ChatHost *host) {
    host->body_flush_pending = false;
    if (host->window) KillTimer(host->window, CHAT_TIMER_BODY_FLUSH);
}
/* Applies the scheduled rebuild. A body holding a selection is not rewritten:
   transcript_stream_body records the debt as a pending write and returns, so
   the reader's range survives and the deferred render lands when it clears. */
static void flush_stream_body(ChatHost *host) {
    if (host->window) KillTimer(host->window, CHAT_TIMER_BODY_FLUSH);
    if (!host->body_flush_pending || !host->generating) return;
    if (host->request_conversation != host->config.chat->active) return;
    host->body_flush_pending = false;
    host->transcript.body_render_tick = GetTickCount64();
    stream_body_markdown(host, host->request_message);
}
/* Whole-row click on one turn's reasoning row: toggles only that turn, whose
   expansion is stored on its own message. */
static bool turn_row_click(void *user, RichTextControl *control, int line,
    bool down) {
    ChatHost *host = (ChatHost *)user;
    Chat *chat = host->config.chat;
    for (int i = 0; i < host->transcript.turn_count; i++) {
        if (&host->transcript.turns[i].head != control) continue;
        if (line != 1) return false;          /* only the reasoning row line */
        if (!down &&
            chat->active >= 0 && chat->active < chat->conversation_count &&
            i < chat->conversations[chat->active].message_count) {
            ChatMessage *m = &chat->conversations[chat->active].messages[i];
            m->reasoning_open = !m->reasoning_open;
            refresh_turn(host, i);
        }
        return true;
    }
    return false;
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

/* Ends the visible-reasoning window and records its duration once. */
static void end_reasoning(ChatHost *host) {
    if (!host->reasoning_streaming) return;
    ChatMessage *m = pending(host);
    if (m->generation.reasoning_ms < 0)
        m->generation.reasoning_ms =
            (double)(GetTickCount64() - host->reasoning_started_tick);
    host->reasoning_streaming = false;
}

static void place(ChatHost *host, RichTextControl *control, UiId id,
    float inset) {
    UiNode *item = ui_node(host->config.ui, id);
    if (!item || !control->window) return;
    if (!ui_visible(host->config.ui, id)) {
        ShowWindow(control->window, SW_HIDE);
        return;
    }
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

/* Sizes the transcript container over its placeholder, inside the drawn border. */
static void place_container(ChatHost *host) {
    UiNode *item = ui_node(host->config.ui, host->chat_ui.transcript);
    if (!item || !host->view) return;
    if (!ui_visible(host->config.ui, host->chat_ui.transcript)) {
        ShowWindow(host->view, SW_HIDE);
        return;
    }
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += 1;
    area.y += 1;
    area.w -= 2;
    area.h -= 2;
    if (area.w <= 2 || area.h <= 2) { ShowWindow(host->view, SW_HIDE); return; }
    int x = px(host, area.x), y = px(host, area.y);
    SetWindowPos(host->view, NULL, x, y, px(host, area.x + area.w) - x,
        px(host, area.y + area.h) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(host->view, SW_SHOWNOACTIVATE);
}

static void layout(ChatHost *host) {
    if (host->minimized) return;
    RECT client;
    GetClientRect(host->window, &client);
    chat_ui_resize(&host->chat_ui, dip(host, client.right), dip(host, client.bottom));
    place_container(host);
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
    /* The composer and model field remain the two explicit Tab stops; Tab from
       a transcript turn returns to the field. */
    HWND order[2] = { host->field.window, host->composer.window };
    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < 2; i++) if (order[i] == focus) current = i;
    int next = current < 0 ? (reverse ? 1 : 0) : (current + (reverse ? -1 : 1) + 2) % 2;
    SetFocus(order[next]);
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
    /* Retry/regenerate/edit-resend reuse turn slots: their stale identity,
       pending updates and selections are dropped so the next render replaces
       the affected surfaces without deferral. */
    transcript_invalidate_from(&host->transcript, index);
    host->started_tick=GetTickCount64();
    /* The running turn shows a temporary pending row; it is removed once answer
       text begins if the provider never supplies reasoning. */
    host->reasoning_streaming=false; host->content_started=false;
    host->transcript.body_render_tick=0;
    cancel_body_flush(host);
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
        messages[used++]=(OpenRouterMessage){c->messages[i].role,
            chat_message_text(&c->messages[i])};
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
        chat_message_touch(m);
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
            chat_message_touch(pending(host));
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
    double keep_reasoning_ms=m->generation.reasoning_ms;
    m->generation=event->metadata; m->generation.state=CHAT_GENERATION_RUNNING;
    m->generation.reasoning_ms=keep_reasoning_ms;
    /* The first answer token ends any reasoning window. If no reasoning ever
       arrived, the pending Thinking row is removed from that turn. */
    bool first=!host->content_started;
    if (first) {
        host->content_started=true;
        end_reasoning(host);
    }
    size_t incoming=wcslen(event->text);
    if (!chat_message_append_text(m,event->text)) {
        host->stop_state=CHAT_GENERATION_INTERRUPTED;
        host->accepting=false; host->stopping=true;
        openrouter_cancel(&host->client,host->request_generation);
        chat_ui_set_generation(&host->chat_ui,true,true);
        set_status(host,L"Not enough memory to continue the response.");
        return;
    }
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->dirty=true;
    if (incoming>0 && host->request_conversation==host->config.chat->active) {
        int index=host->request_message;
        TranscriptTurn *turn=&host->transcript.turns[index];
        if (first) {
            /* Rebuilds this turn's body and refreshes/removes its row. */
            refresh_turn(host,index);
            host->transcript.body_render_tick=GetTickCount64();
            cancel_body_flush(host);
        } else if (turn->body_live && turn->body.window) {
            /* Deltas accumulate in the message; the visible body is rebuilt
               as Markdown at most once per interval, so a token storm never
               reparses per token. A delta inside the window only marks the
               body dirty and arms a one-shot flush, so a burst that then
               pauses still renders without waiting for another delta. The
               rebuild follows the transcript scroll only while the reader
               stays pinned to the bottom. */
            ULONGLONG now=GetTickCount64();
            if (now-host->transcript.body_render_tick>=CHAT_BODY_RENDER_MS) {
                host->transcript.body_render_tick=now;
                cancel_body_flush(host);
                stream_body_markdown(host,index);
            } else if (!host->body_flush_pending) {
                host->body_flush_pending=true;
                schedule_body_flush(host);
            }
        }
    }
}

static void append_reasoning_delta(ChatHost *host, OpenRouterEvent *event) {
    if (!event->text || !event->text[0] || !host->accepting) return;
    ChatMessage *m=pending(host);
    m->generation=event->metadata; m->generation.state=CHAT_GENERATION_RUNNING;
    size_t incoming=wcslen(event->text);
    if (incoming) {
        if (!chat_message_append_reasoning(m,event->text)) {
            host->stop_state=CHAT_GENERATION_INTERRUPTED;
            host->accepting=false; host->stopping=true;
            openrouter_cancel(&host->client,host->request_generation);
            chat_ui_set_generation(&host->chat_ui,true,true);
            set_status(host,L"Not enough memory to continue the reasoning.");
            return;
        }
        if (host->request_conversation==host->config.chat->active) {
            TranscriptTurn *turn=&host->transcript.turns[host->request_message];
            if (m->reasoning_open && !turn->reason_live)
                refresh_turn(host,host->request_message);
            else if (m->reasoning_open && turn->reason_live &&
                turn->reasoning.window)
                rich_text_append_reasoning(&turn->reasoning,event->text);
        }
    }
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->dirty=true;
    if (!host->reasoning_streaming) {
        host->reasoning_streaming=true;
        host->reasoning_started_tick=GetTickCount64();
    }
}

static void finish_request(ChatHost *host, OpenRouterEvent *event) {
    end_reasoning(host);
    double reasoning_ms=pending(host)->generation.reasoning_ms;
    openrouter_complete(&host->client,event->generation);
    ChatMessage *m=pending(host);
    m->generation=event->metadata;
    m->generation.reasoning_ms=reasoning_ms;
    ChatGeneration *g=&m->generation;
    g->state=host->stopping ? host->stop_state : event->type==OPENROUTER_DONE ?
        CHAT_GENERATION_COMPLETE : event->type==OPENROUTER_CANCELLED ?
        CHAT_GENERATION_CANCELLED : event->type==OPENROUTER_INTERRUPTED ?
        CHAT_GENERATION_INTERRUPTED : CHAT_GENERATION_FAILED;
    if (event->text) wcsncpy(g->error,event->text,511);
    if (g->state==CHAT_GENERATION_COMPLETE && !wcscmp(g->finish_reason,L"error")) g->state=CHAT_GENERATION_FAILED;
    if (g->state==CHAT_GENERATION_COMPLETE && !chat_message_text(m)[0]) {
        g->state=CHAT_GENERATION_FAILED; wcscpy(g->error,L"OpenRouter completed without text.");
    }
    /* Terminal generation metadata is observable: mark the message changed. */
    chat_message_touch(m);
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->generating=false; host->stopping=false; host->accepting=false;
    chat_ui_set_generation(&host->chat_ui,false,false); EnableWindow(host->field.window,TRUE);
    set_status(host,chat_generation_name(g->state));
    /* The full transcript render flushes any body rebuild the streaming
       throttle had deferred, so the terminal Markdown is always visible. */
    cancel_body_flush(host);
    host->dirty=true; save(host);
    if (host->request_conversation==host->config.chat->active) render_transcript(host);
    chat_ui_sync(&host->chat_ui); flush(host);
}

static void handle_event(ChatHost *host, OpenRouterEvent *event) {
    if (!event) return;
    if (event->generation==host->request_generation && host->generating) {
        if (event->type==OPENROUTER_DELTA) append_stream_delta(host,event);
        else if (event->type==OPENROUTER_REASONING) append_reasoning_delta(host,event);
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
            cancel_body_flush(host);
            transcript_invalidate(&host->transcript);
            render_transcript(host);
            rich_text_set_text(&host->composer, L"");
            chat_ui_sync(&host->chat_ui);
        }
    } else if (code == CHAT_COMMAND_SELECT) {
        if (chat_select_conversation(chat, index)) {
            cancel_body_flush(host);
            transcript_invalidate(&host->transcript);
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
            set_status(host,chat_copy_text(host->window,
                chat_message_text(&c->messages[i])) ? L"Response copied" :
                L"Copy failed"); return;
        }
        return;
    }
    if (code==ACTION_SELECTION) {
        /* Copy whichever turn viewport/block currently holds a selection. */
        for (int i=0;i<host->transcript.turn_count;i++) {
            RichTextControl *controls[3]={&host->transcript.turns[i].head,
                &host->transcript.turns[i].body,
                &host->transcript.turns[i].reasoning};
            for (int k=0;k<3;k++) {
                if (!controls[k]->window) continue;
                if (rich_text_has_selection(controls[k])) {
                    SendMessageW(controls[k]->window,WM_COPY,0,0);
                    set_status(host,L"Transcript selection copied");
                    return;
                }
            }
        }
        return;
    }
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
        host->editing=true; rich_text_set_text(&host->composer,
            chat_message_text(&c->messages[user]));
        SetFocus(host->composer.window); set_status(host,L"Editing latest user message. Send replaces its response; Response > Cancel edit restores the draft.");
    } else if (code==ACTION_RENAME) {
        wchar_t title[CHAT_TITLE_TEXT]; wcscpy(title,c->title);
        if (chat_edit_dialog(host->window,L"Rename conversation",title,CHAT_TITLE_TEXT,false)) chat_rename(chat,title);
    } else if (code==ACTION_DELETE || code==ACTION_DELETE_ALL || code==ACTION_CLEAR) {
        const wchar_t *message=code==ACTION_DELETE ? L"Delete this conversation?" :
            code==ACTION_DELETE_ALL ? L"Delete all conversations? This cannot be undone." :
            L"Clear all messages in this conversation?";
        if (MessageBoxW(host->window,message,
            L"DarkChat",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        if (code==ACTION_DELETE) chat_delete(chat);
        else if (code==ACTION_DELETE_ALL) chat_delete_all(chat);
        else chat_clear(chat);
        transcript_invalidate(&host->transcript);
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
    /* Route the wheel to the transcript container when the pointer is over it,
       even if a different child (e.g. the composer) holds focus. */
    if (host->view) {
        RECT bounds;
        GetWindowRect(host->view, &bounds);
        if (PtInRect(&bounds, point)) {
            SendMessageW(host->view, WM_MOUSEWHEEL, w, l);
            return;
        }
    }
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

/* True for any child that keeps the window active while it holds focus. */
static bool owns_child_window(ChatHost *host, HWND window) {
    if (window == host->composer.window || window == host->field.window)
        return true;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        TranscriptTurn *turn = &host->transcript.turns[i];
        if (window == turn->head.window || window == turn->body.window ||
            window == turn->reasoning.window || window == turn->meta.window)
            return true;
    }
    return false;
}

/* Transcript container: owns the single outer scroll for the whole
   conversation and repositions each turn's controls as it scrolls. Its
   WS_CLIPCHILDREN style clips turns that scroll out of view so they never draw
   over the composer or toolbar. */
static LRESULT CALLBACK view_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    ChatHost *host = (ChatHost *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        host = ((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)host);
    }
    if (!host) return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_ERASEBKGND: {
        RECT bounds;
        GetClientRect(window, &bounds);
        FillRect((HDC)w, &bounds, host->background);
        return 1;
    }
    case WM_SIZE:
        render_transcript(host);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info;
        memset(&info, 0, sizeof info);
        info.cbSize = sizeof info;
        info.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &info);
        int position = info.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: position -= px(host, 28); break;
        case SB_LINEDOWN: position += px(host, 28); break;
        case SB_PAGEUP: position -= (int)info.nPage; break;
        case SB_PAGEDOWN: position += (int)info.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = info.nMin; break;
        case SB_BOTTOM: position = info.nMax; break;
        default: return 0;
        }
        int maximum = host->transcript.view_content - host->transcript.view_page;
        if (maximum < 0) maximum = 0;
        if (position < 0) position = 0;
        if (position > maximum) position = maximum;
        host->transcript.view_scroll = position;
        position_turns(host, false);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        /* An expanded reasoning viewport scrolls itself first; once it reaches
           an end the transcript scrolls, so chaining feels natural and the
           target no longer depends on which control holds focus. */
        POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(window, &point);
        HWND child = ChildWindowFromPointEx(window, point,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (child && child != window && delta) {
            for (int i = 0; i < host->transcript.turn_count; i++) {
                if (host->transcript.turns[i].reasoning.window != child)
                    continue;
                SCROLLINFO inner;
                memset(&inner, 0, sizeof inner);
                inner.cbSize = sizeof inner;
                inner.fMask = SIF_ALL;
                GetScrollInfo(child, SB_VERT, &inner);
                int inner_max = inner.nMax - (int)inner.nPage + 1;
                if (inner_max < 0) inner_max = 0;
                bool can = delta > 0 ? inner.nPos > 0 : inner.nPos < inner_max;
                if (can) {
                    UINT lines = 3;
                    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                    int code = lines == WHEEL_PAGESCROLL
                        ? (delta > 0 ? SB_PAGEUP : SB_PAGEDOWN)
                        : (delta > 0 ? SB_LINEUP : SB_LINEDOWN);
                    SendMessageW(child, WM_VSCROLL, MAKEWPARAM(code, 0), 0);
                    return 0;
                }
                break;
            }
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int step = lines == WHEEL_PAGESCROLL ? host->transcript.view_page
                                             : (int)lines * px(host, 28);
        int position = host->transcript.view_scroll - delta / WHEEL_DELTA * step;
        int maximum = host->transcript.view_content - host->transcript.view_page;
        if (maximum < 0) maximum = 0;
        if (position < 0) position = 0;
        if (position > maximum) position = maximum;
        host->transcript.view_scroll = position;
        position_turns(host, false);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)l;
        RichTextControl *control = (RichTextControl *)GetWindowLongPtrW(
            header->hwndFrom, GWLP_USERDATA);
        if (header->code == EN_REQUESTRESIZE) {
            transcript_measure_notify(&host->transcript, control,
                &((REQRESIZE *)l)->rc);
            return 0;
        }
        if (header->code == EN_SELCHANGE && control) {
            /* A reader selection ended: apply writes the transcript deferred
               for this surface. Programmatic writes are suppressed inside. */
            TranscriptFeed feed = transcript_feed(host);
            transcript_selection_changed(&host->transcript, &feed, control);
            return 0;
        }
        if (control && rich_text_handle_notify(control, l)) return 0;
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, w, l);
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
        if (!rich_text_create_composer(&host->composer, window, 2,
            &host->rich_theme, host->dpi)) return -1;
        if (!rich_text_create_field(&host->field, window, 3, &host->rich_theme,
            host->dpi, host->config.chat->model)) return -1;
        host->view = CreateWindowExW(0, L"DarkChat.Transcript", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, 10, 10, window, NULL, GetModuleHandleW(NULL), host);
        if (!host->view) return -1;
        transcript_create(&host->transcript, host->view, &host->rich_theme,
            host->dpi);
        host->transcript.callbacks.surface_key = surface_key;
        host->transcript.callbacks.row_click = turn_row_click;
        host->transcript.callbacks.user = host;
        host->composer.on_submit = composer_submit;
        host->composer.on_key = surface_key;
        host->composer.user = host;
        host->field.on_submit = field_submit;
        host->field.on_key = surface_key;
        host->field.on_blur = field_blur;
        host->field.user = host;
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
        if (w == CHAT_TIMER_BODY_FLUSH) {
            /* The stream went quiet with text still dirty: render it now. */
            flush_stream_body(host);
            return 0;
        }
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
            { TranscriptFeed feed = transcript_feed(host);
              transcript_apply_pending(&host->transcript, &feed); }
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
        transcript_set_dpi(&host->transcript, host->dpi);
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
        if (!owns_child_window(host, (HWND)w)) {
            ui_set_active(u, false);
            flush(host);
        }
        return 0;
    case CHAT_WM_OPENROUTER_EVENT:
        handle_event(host, (OpenRouterEvent *)l);
        return 0;
    case WM_CLOSE:
        capture_settings(host);
        cancel_body_flush(host);
        if (host->generating) {
            ChatGeneration *g=&pending(host)->generation;
            g->state=host->stopping ? host->stop_state : CHAT_GENERATION_INTERRUPTED;
            g->finished_at=chat_now();
            g->latency_ms=(double)(GetTickCount64()-host->started_tick);
            wcscpy(g->error,L"Window closed before generation finished.");
            chat_message_touch(pending(host));
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
        KillTimer(window, CHAT_TIMER_BODY_FLUSH);
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
    WNDCLASSEXW view_cls = { 0 };
    view_cls.cbSize = sizeof view_cls;
    view_cls.lpfnWndProc = view_proc;
    view_cls.hInstance = instance;
    view_cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    view_cls.lpszClassName = L"DarkChat.Transcript";
    if (!RegisterClassExW(&view_cls)) goto cleanup;
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
    UnregisterClassW(view_cls.lpszClassName, instance);
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





