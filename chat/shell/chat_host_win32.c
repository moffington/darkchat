#include "chat/shell/chat_host_win32.h"
#include "chat/shell/chat_ui.h"
#include "chat/transcript/rich_text_win32.h"
#include "chat/transcript/transcript_win32.h"
#include "chat/generation/completion_winhttp.h"
#include "chat/generation/completion_request.h"
#include "chat/generation/context.h"
#include "chat/core/search.h"
#include "chat/persistence/storage.h"
#include "chat/persistence/saver.h"
#include "chat/shell/actions_win32.h"
#include "chat/models/model_catalog.h"
#include "chat/models/model_catalog_winhttp.h"
#include "chat/shell/palette_win32.h"
#include "platform/renderer.h"
#include "platform/accessibility.h"
#include <windowsx.h>
#include <richedit.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One-shot flush for dirty streaming content; the status sweep and the
   paint-retry timers use ids 2 and 1. */
#define CHAT_TIMER_BODY_FLUSH 3
#define CHAT_SEARCH_QUERY_TEXT 256
/* Posted (never sent) so the overflow popup opens after the button-up input
   cycle completes and mouse capture is released; opening it synchronously
   inside the click handler lets the popup's own input loop see the held
   capture and dismiss it immediately. */
#define CHAT_WM_ACTIONS_MENU (WM_APP + 0x52)

typedef struct {
    ChatHostConfig config;
    UiRenderer renderer;
    UiAccessibility *accessibility;
    HWND window;
    ChatUi chat_ui;
    RichTextControl composer, field, search;
    HWND view;                          /* transcript container child window */
    Transcript transcript;              /* per-turn update bookkeeping */
    RichTextTheme rich_theme;
    HBRUSH background;
    float dpi;
    bool tracking, minimized;
    unsigned retries;
    UiId accessibility_focus;
    CompletionClient client;
    int request_generation, request_conversation, request_message;
    /* Request-scoped: how many older messages the live request omitted to fit
       the context budget. Shown by the one-second generating status sweep. */
    int context_dropped;
    bool generating, stopping, accepting, dirty, editing, content_started;
    bool reasoning_streaming;
    /* Appended text not yet written to the live body; a scheduled flush
        guarantees it renders even when the stream pauses. */
    bool body_flush_pending;
    /* Reasoning pane appends batched the same way: fragments accumulate in
        the message and the pane repaints on the flush timer, never once per
        fragment (each repaint is a synchronous Rich Edit update). The
        painted-tail mark lives on the transcript record, stamped by every
        full pane reload, so a mid-stream re-render cannot desync it.
        reasoning_paint_tick is the reasoning flush's own throttle deadline:
        it advances after every reasoning flush, so a reasoning-only stream
        schedules real-interval timers instead of 1 ms ones. */
    bool reason_paint_pending;
    ULONGLONG reasoning_paint_tick;
    ULONGLONG reasoning_started_tick;
    ChatStorage storage;
    /* Background snapshot writer. Mutations is the counter every save handoff
       captures; last_submitted_attempt is the newest accepted handoff and
       handled_attempt the newest attempt whose result has been interpreted,
       so a superseded failure can never report and a stale result can never
       re-interpret older completions. */
    ChatSaver saver;
    uint64_t mutations, last_submitted_attempt, handled_attempt;
    bool save_failed;
    ChatSearchResults search_results;
    size_t search_selected;
    bool search_has_selection;
    ULONGLONG started_tick;
    ChatGenerationState stop_state;
    /* Latest sidebar remap change report, translated into UIA notifications
       by the host (chat_ui.c itself knows nothing about accessibility). */
    ChatUiRemapReport remap;
    /* Transient model catalogs: the last-good parsed list and status per
       backend and the one-shot fetch worker. Never persisted. When no catalog
       is available the palette falls back to the model history.
       `backend_target` is the backend the model palette is choosing for; it
       equals the active backend except while an Ollama switch waits for a
       model selection. */
    ChatModelCatalog catalog[CHAT_BACKEND_COUNT];
    ChatModelParseStats catalog_stats[CHAT_BACKEND_COUNT];
    ModelCatalogClient catalog_client;
    int catalog_generation;
    ChatBackend catalog_backend;
    ULONGLONG catalog_success_tick[CHAT_BACKEND_COUNT];
    bool catalog_loaded[CHAT_BACKEND_COUNT], catalog_loading;
    bool catalog_failed[CHAT_BACKEND_COUNT];
    wchar_t catalog_error[CHAT_BACKEND_COUNT][CHAT_STATUS_TEXT];
    ChatBackend backend_target;
    bool backend_switch_pending;
    bool close_pending, model_applied;
    /* One retained popup serves commands (Ctrl+K) and models (Ctrl+Space):
       `open_palette` is the single open-popup field and its mode is read from
       the popup itself. `palette_pumping` guards against freeing it under a
       live modal loop; a close that lands while the pump is live parks on
       `close_pending` and is reposted once the pump has unwound. */
    PalettePopup *open_palette;
    bool palette_pumping;

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
static bool stream_body_markdown(ChatHost *host, int index);
static void position_turns(ChatHost *host, bool follow);
static void schedule_body_flush(ChatHost *host);
static void cancel_body_flush(ChatHost *host);
static void flush_stream_body(ChatHost *host);
static void flush_reasoning_paint(ChatHost *host);
static ChatMessage *pending(ChatHost *host);
static bool turn_row_click(void *user, RichTextControl *control, int line,
    bool down);
static bool search_submit(void *user);
static bool search_refresh(ChatHost *host, bool reverse);
static bool search_step(ChatHost *host, bool reverse);
static void place_container(ChatHost *host);
static void open_actions_menu(ChatHost *host);
static void open_palette(ChatHost *host);
static void end_palette(ChatHost *host);
static void end_model_palette(ChatHost *host, bool accepted,
    const wchar_t *id);
static RichTextControl *transcript_selected_surface(ChatHost *host);

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

/* Reconciles the native transcript container's visibility with the active
   conversation before a transition render (new/select/clear), so a
   conversation that is empty shows the retained hero instead and one that
   just gained its first turn is measured in a shown, correctly sized
   container. Ordinary renders and direct container resizes do not call
   this: the container tracks the placeholder on the next layout. */
static void sync_transcript_container(ChatHost *host) {
    place_container(host);
}
static void refresh_turn(ChatHost *host, int index) {
    TranscriptFeed feed = transcript_feed(host);
    transcript_refresh_turn(&host->transcript, &feed, index);
}
static bool stream_body_markdown(ChatHost *host, int index) {
    TranscriptFeed feed = transcript_feed(host);
    return transcript_stream_body(&host->transcript, &feed, index);
}
static void position_turns(ChatHost *host, bool follow) {
    TranscriptFeed feed = transcript_feed(host);
    transcript_position(&host->transcript, &feed, follow);
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
    /* The pending family owns the deadline: a reasoning-only burst must not
        ride the body's tick (which only advances when the body flushes), or
        its elapsed time stays huge and every fragment schedules the 1 ms
        fallback -- nearly one synchronous repaint per fragment. */
    ULONGLONG now = GetTickCount64();
    ULONGLONG tick = host->body_flush_pending
        ? host->transcript.body_render_tick : host->reasoning_paint_tick;
    ULONGLONG elapsed = now - tick;
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
    host->reason_paint_pending = false;
    if (host->window) KillTimer(host->window, CHAT_TIMER_BODY_FLUSH);
}
/* Paints the reasoning accumulated since the last flush into the live pane.
    One append per flush, never one per fragment: each append is a
    synchronous Rich Edit update with a forced repaint. The painted-tail
    mark lives on the transcript record (stamped by every full pane
    reload), so a wholesale rewrite rebases it automatically; a closed or
    missing pane leaves the mark untouched until such a reload. */
static void flush_reasoning_paint(ChatHost *host) {
    if (!host->reason_paint_pending) return;
    host->reason_paint_pending = false;
    /* Advance the reasoning deadline whether or not a pane existed: the
        throttle measures the flush cadence, not the paint's success. */
    host->reasoning_paint_tick = GetTickCount64();
    if (!host->generating) return;
    if (host->request_conversation != host->config.chat->active) return;
    ChatMessage *m = pending(host);
    const wchar_t *text = chat_message_reasoning(m);
    size_t length = wcslen(text);
    TranscriptRecord *rec = &host->transcript.records[host->request_message];
    RichTextControl *reason = transcript_surface(&host->transcript,
        host->request_message, TRANSCRIPT_REASON);
    if (m->reasoning_open && rec->reason_live && reason) {
        if (length > rec->reason_painted)
            rich_text_append_reasoning(reason, text + rec->reason_painted);
        rec->reason_painted = length;
    }
}
/* Applies the scheduled rebuild. A body holding a selection is not rewritten:
   transcript_stream_body records the debt as a pending write and returns, so
   the reader's range survives and the deferred render lands when it clears.
   A missing body surface (creation failed) is retried inside
   transcript_stream_body; a false return keeps the flush armed and re-arms
   the one-shot timer at the throttle interval, so the retry cadence
   continues without waiting for another delta. The re-arm uses a raw
   SetTimer and never schedule_body_flush, whose SetTimer-failure fallback
   calls this function -- re-entering the scheduler would recurse. A failed
   re-arm simply falls back to the external retry paths (next incoming
   delta, the 1 Hz sweep, the next render). Batched reasoning appends share
   this timer. */
static void flush_stream_body(ChatHost *host) {
    if (host->window) KillTimer(host->window, CHAT_TIMER_BODY_FLUSH);
    if ((!host->body_flush_pending && !host->reason_paint_pending) ||
        !host->generating) return;
    if (host->request_conversation != host->config.chat->active) return;
    flush_reasoning_paint(host);
    if (!host->body_flush_pending) return;
    host->body_flush_pending = false;
    host->transcript.body_render_tick = GetTickCount64();
    if (stream_body_markdown(host, host->request_message)) return;
    if (!host->transcript.bounded) return;
    host->body_flush_pending = true;
    if (host->window)
        SetTimer(host->window, CHAT_TIMER_BODY_FLUSH, CHAT_BODY_RENDER_MS,
            NULL);
}
/* Whole-row click on one turn's reasoning row: toggles only that turn, whose
   expansion is stored on its own message. */
static bool turn_row_click(void *user, RichTextControl *control, int line,
    bool down) {
    ChatHost *host = (ChatHost *)user;
    Chat *chat = host->config.chat;
    for (int i = 0; i < host->transcript.record_count; i++) {
        if (transcript_surface(&host->transcript, i, TRANSCRIPT_HEAD) !=
            control) continue;
        if (line != 1) return false;          /* only the reasoning row line */
        if (!down &&
            chat->active >= 0 && chat->active < chat->conversation_count &&
            (size_t)i < chat->conversations[chat->active].message_count) {
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

#define CHAT_SAVE_FAILED L"Save failed: changes are in memory; check storage permissions or disk space."

/* Interprets one completed save. Results arrive with the attempt id and the
   captured mutation counter of that specific handoff. Two guards keep the
   report truthful:
   - attempt <= handled_attempt: stale — the result was already interpreted
     (synchronously by a flush, or as an earlier dispatch of itself).
   - a failure whose attempt is older than the newest submitted attempt is
     superseded: that newer attempt's snapshot contains all of this one's
     state and is guaranteed to complete and post its own authoritative
     result, so the older failure must not latch the failure latch, write the
     failure status or suppress the status sweep.
   A success clears the failure latch and clears dirty only when no mutation
   happened after that snapshot was taken; a processed failure latches and
   explicitly keeps dirty set so the one-second autosave retries. */
static void saver_completed(ChatHost *host, bool ok, uint64_t attempt,
    uint64_t captured) {
    if (attempt<=host->handled_attempt) return;
    if (!ok && attempt<host->last_submitted_attempt) return;
    host->handled_attempt=attempt;
    if (ok) {
        host->save_failed=false;
        if (host->mutations==captured) host->dirty=false;
    } else {
        host->save_failed=true;
        host->dirty=true;
        set_status(host,CHAT_SAVE_FAILED);
    }
}

/* Builds the immutable snapshot a save hands off. NULL on allocation failure:
   dirty stays set so the one-second autosave retries, and the failure is
   reported exactly like a storage failure. `captured` receives the mutation
   counter at handoff time, which travels with the job. */
static Chat *save_snapshot(ChatHost *host, uint64_t *captured) {
    *captured=host->mutations;
    Chat *snapshot=chat_snapshot(host->config.chat);
    if (!snapshot) {
        host->save_failed=true;
        set_status(host,CHAT_SAVE_FAILED);
    }
    return snapshot;
}

/* Asynchronous autosave: hands the snapshot to the background writer and
   returns. The writer alone calls the storage module; with no writer there is
   nothing honest to report except failure. */
static bool save(ChatHost *host) {
    if (!host->dirty) return true;
    uint64_t captured;
    Chat *snapshot=save_snapshot(host,&captured);
    if (!snapshot) return false;
    if (!saver_ready(&host->saver)) {
        chat_dispose(snapshot); free(snapshot);
        host->save_failed=true; host->dirty=true;
        set_status(host,CHAT_SAVE_FAILED);
        return false;
    }
    uint64_t attempt=saver_submit(&host->saver,snapshot,captured);
    host->last_submitted_attempt=attempt;
    return true;
}

/* Flush-and-wait durability gate: hands the current state to the writer and
   blocks until its own attempt is durable or failed. Used where the
   synchronous contract must hold: before a network request and on close. */
static bool save_sync(ChatHost *host) {
    if (!host->dirty) return true;
    uint64_t captured;
    Chat *snapshot=save_snapshot(host,&captured);
    if (!snapshot) return false;
    if (!saver_ready(&host->saver)) {
        chat_dispose(snapshot); free(snapshot);
        host->save_failed=true; host->dirty=true;
        set_status(host,CHAT_SAVE_FAILED);
        return false;
    }
    uint64_t attempt;
    bool ok=saver_flush(&host->saver,snapshot,captured,&attempt);
    /* The flush's attempt is the newest submission while it completes; the
       result is interpreted here, immediately, and its later queued
       duplicate is ignored as stale. */
    host->last_submitted_attempt=attempt;
    saver_completed(host,ok,attempt,captured);
    return ok;
}

/* Every observable change that makes state newer than the last handed-off
   snapshot goes through here; the counter it bumps is what save handoffs
   capture and completions are judged against. */
static void mark_dirty(ChatHost *host) {
    host->dirty = true;
    ++host->mutations;
}

/* ---- Model catalog and picker ----------------------------------------- */

/* The remembered model slot of one backend. OpenRouter keeps the historical
   `model`; Ollama has its own additive slot. */
static wchar_t *model_slot(Chat *chat, ChatBackend backend) {
    return backend == CHAT_BACKEND_OLLAMA ? chat->ollama_model : chat->model;
}
static const wchar_t *current_model(const Chat *chat, ChatBackend backend) {
    return backend == CHAT_BACKEND_OLLAMA ? chat->ollama_model : chat->model;
}

/* Writes the selected id into the target backend's slot and the visible field
   atomically. Rejects empty and over-capacity ids. The model is added to
   history only when a request actually begins (chat_begin_response), never
   here, and manual entry through the field remains a fully supported
   secondary path. During a pending backend switch the field is not updated
   until the switch commits. */
static bool apply_model(ChatHost *host, const wchar_t *id) {
    if (!id || !id[0] || wcslen(id) >= CHAT_MODEL_TEXT) return false;
    Chat *chat = host->config.chat;
    wchar_t *slot = model_slot(chat, host->backend_target);
    bool changed = wcscmp(slot, id) != 0;
    if (changed) {
        wcsncpy(slot, id, CHAT_MODEL_TEXT - 1);
        slot[CHAT_MODEL_TEXT - 1] = 0;
        mark_dirty(host);
    }
    /* The visible field always mirrors the active backend, changed or not. */
    if (!host->backend_switch_pending)
        rich_text_set_text(&host->field, chat_active_model(chat));
    return changed;
}

/* Concise, nonfatal status for the picker's status line, for one backend. */
static void picker_status(ChatHost *host, ChatBackend backend, wchar_t *out,
    size_t capacity) {
    if (!out || !capacity) return;
    out[0] = 0;
    const ChatModelCatalog *catalog = &host->catalog[(int)backend];
    const ChatModelParseStats *stats = &host->catalog_stats[(int)backend];
    if (host->catalog_failed[backend] && catalog->count)
        wcsncpy(out, L"Refresh failed; showing cached catalog.", capacity - 1);
    else if (host->catalog_failed[backend]) {
        if (host->catalog_error[backend][0])
            _snwprintf(out, capacity,
                L"Catalog unavailable: %ls. Showing recent models.",
                host->catalog_error[backend]);
        else
            wcsncpy(out, L"Catalog unavailable; showing recent models.",
                capacity - 1);
    } else if (host->catalog_loading && host->catalog_backend == backend &&
        !catalog->count)
        _snwprintf(out, capacity, L"Loading %ls models\u2026",
            chat_backend_name(backend));
    else if (stats->too_long)
        _snwprintf(out, capacity, L"%lu models hidden (id too long).",
            (unsigned long)stats->too_long);
    else if (stats->truncated)
        _snwprintf(out, capacity, L"%lu models omitted (catalog limit).",
            (unsigned long)stats->truncated);
    else if (host->catalog_loaded[backend])
        _snwprintf(out, capacity, L"%lu models",
            (unsigned long)catalog->count);
    out[capacity - 1] = 0;
}

/* OpenRouter needs a key to fetch; Ollama never does. */
static bool should_fetch_catalog(ChatHost *host, ChatBackend backend) {
    if (backend == CHAT_BACKEND_OPENROUTER &&
        (!host->config.api_key_utf8 || !host->config.api_key_utf8[0]))
        return false;
    const ChatModelCatalog *catalog = &host->catalog[(int)backend];
    if (catalog->count == 0 || host->catalog_failed[backend]) return true;
    return GetTickCount64() - host->catalog_success_tick[(int)backend] >= 3600000ULL;
}

/* Copies the remembered history for `backend` only. The palette builds its own
   merged view internally, so it never receives the combined history: an
   OpenRouter model can never appear in the Ollama model list or vice versa. */
static int build_backend_history(const ChatHost *host, ChatBackend backend,
    wchar_t out[][CHAT_MODEL_TEXT]) {
    const Chat *chat = host->config.chat;
    int count = 0;
    for (int i = 0; i < chat->model_history_count &&
         count < CHAT_MODEL_HISTORY; i++)
        if (chat->model_history_backend[i] == backend)
            wcscpy(out[count++], chat->model_history[i]);
    return count;
}

/* True when a catalog completion is already queued for the host window: it will
   clear catalog_loading when dispatched and must not be mistaken for a lost
   completion. */
static bool catalog_event_pending(const ChatHost *host) {
    MSG message;
    return PeekMessageW(&message, host->window, CHAT_WM_CATALOG_EVENT,
        CHAT_WM_CATALOG_EVENT, PM_NOREMOVE) != FALSE;
}

/* Starts a fetch for `backend` now; the caller guarantees the client is idle. */
static void request_catalog(ChatHost *host, ChatBackend backend) {
    int generation = model_catalog_request(&host->catalog_client, backend,
        host->config.api_key_utf8);
    if (generation) {
        host->catalog_generation = generation;
        host->catalog_backend = backend;
        host->catalog_loading = true;
        host->catalog_failed[backend] = false;
    } else {
        host->catalog_failed[backend] = true;
        wcsncpy(host->catalog_error[backend],
            L"Could not start the catalog request.", CHAT_STATUS_TEXT - 1);
        host->catalog_error[backend][CHAT_STATUS_TEXT - 1] = 0;
    }
}

/* Reaps a worker that finished without posting its completion (a failed post),
   treating it as a retryable failure for the backend that owned it, so a
   loading flag no event will clear can never block a later fetch. */
static void recover_lost_catalog(ChatHost *host) {
    if (!host->catalog_loading || catalog_event_pending(host) ||
        model_catalog_busy(&host->catalog_client)) return;
    ChatBackend lost = host->catalog_backend;
    host->catalog_loading = false;
    host->catalog_failed[lost] = true;
    wcsncpy(host->catalog_error[lost],
        L"The model catalog request did not complete.", CHAT_STATUS_TEXT - 1);
    host->catalog_error[lost][CHAT_STATUS_TEXT - 1] = 0;
}

/* Starts the target backend's fetch when one is wanted and none is in flight.
   Called after any completion settles, which closes the switch gap: an
   OpenRouter result that lands while the Ollama picker is open queues Ollama's
   own fetch instead of leaving the picker stale until it is reopened. */
static void maybe_start_catalog_fetch(ChatHost *host, ChatBackend backend) {
    if (host->catalog_loading || model_catalog_busy(&host->catalog_client))
        return;
    if (!should_fetch_catalog(host, backend)) return;
    request_catalog(host, backend);
}

/* Opens the picker for the backend `backend_target`: builds the merged view,
   starts at most one fetch, and creates the popup. The caller drives the
   pump. */
static void begin_model_palette(ChatHost *host) {
    if (host->open_palette) return;
    Chat *chat = host->config.chat;
    ChatBackend backend = host->backend_target;
    bool has_key = host->config.api_key_utf8 && host->config.api_key_utf8[0];
    if (backend == CHAT_BACKEND_OPENROUTER && !has_key) {
        host->catalog_failed[backend] = true;
        wcsncpy(host->catalog_error[backend],
            L"Set OPENROUTER_API_KEY to load the model catalog.",
            CHAT_STATUS_TEXT - 1);
        host->catalog_error[backend][CHAT_STATUS_TEXT - 1] = 0;
    } else {
        recover_lost_catalog(host);
        maybe_start_catalog_fetch(host, backend);
    }
    wchar_t history[CHAT_MODEL_HISTORY][CHAT_MODEL_TEXT];
    int history_count = build_backend_history(host, backend, history);
    wchar_t status[CHAT_STATUS_TEXT];
    picker_status(host, backend, status, CHAT_STATUS_TEXT);
    host->open_palette = palette_popup_create_models(host->window,
        &host->catalog[(int)backend], current_model(chat, backend), history,
        history_count, status, current_model(chat, backend));
    if (!host->open_palette)
        set_status(host, L"Could not open the model palette.");
}

/* Applies the model palette's result and reposts a close that arrived while
   the modal loop was live. A confirmed selection for a pending backend switch
   commits the switch only now; cancelling leaves the target uncommitted. */
static void end_model_palette(ChatHost *host, bool accepted, const wchar_t *id) {
    Chat *chat = host->config.chat;
    bool changed = false;
    if (accepted && id && id[0]) {
        changed = apply_model(host, id);
        if (host->backend_switch_pending) {
            chat->backend = host->backend_target;
            rich_text_set_text(&host->field, chat_active_model(chat));
            changed = true;
        }
    }
    /* Cancelling a pending switch leaves no pending state behind; the palette
       never leaves the target pointing at an uncommitted backend. */
    host->backend_switch_pending = false;
    host->backend_target = chat->backend;
    host->model_applied = changed;
    if (changed) {
        save(host);
        chat_ui_sync(&host->chat_ui);
    }
    flush(host);
}

static void open_model_palette(ChatHost *host) {
    if (!host->backend_switch_pending)
        host->backend_target = host->config.chat->backend;
    begin_model_palette(host);
    if (host->open_palette) {
        host->palette_pumping = true;
        palette_popup_pump(host->open_palette);
        host->palette_pumping = false;
    }
    end_palette(host);
}

/* Builds the command palette's availability context from live host state:
    the same fields WM_INITMENUPOPUP feeds the menu sync, so the palette and
    the overflow menu can never disagree about what is runnable. */
static void palette_context(ChatHost *host, ChatActionContext *context) {
    chat_action_context_init(context, host->config.chat);
    context->generating = host->generating;
    context->editing = host->editing;
    context->has_transcript_selection =
        transcript_selected_surface(host) != NULL;
}

/* Destroys the palette and applies its result exactly once. Command mode
    dispatches the highlighted command through the unchanged action()
    dispatcher; model mode applies the accepted model (and commits a pending
    backend switch). A close that arrived while the modal loop was live is
    reposted once, after the palette call has unwound. */
static void end_palette(ChatHost *host) {
    PalettePopup *palette = host->open_palette;
    if (!palette) return;
    PaletteMode mode = palette_popup_mode(palette);
    bool accepted = palette_popup_accepted(palette);
    int action_id = palette_popup_action_id(palette);
    wchar_t model[CHAT_MODEL_TEXT];
    model[0] = 0;
    if (accepted && mode == PALETTE_MODE_MODELS)
        palette_popup_accepted_model(palette, model);
    palette_popup_destroy(palette);
    host->open_palette = NULL;
    if (mode == PALETTE_MODE_COMMANDS) {
        if (accepted && action_id) action(host, action_id);
    } else {
        end_model_palette(host, accepted, model);
    }
    if (host->close_pending) {
        host->close_pending = false;
        PostMessageW(host->window, WM_CLOSE, 0, 0);
    }
}

/* Opens the command palette (Ctrl+K) and runs its modal pump. */
static void open_palette(ChatHost *host) {
    if (host->open_palette) return;
    ChatActionContext context;
    palette_context(host, &context);
    host->open_palette = palette_popup_create(host->window, &context);
    if (!host->open_palette) {
        set_status(host, L"Could not open the command palette.");
        return;
    }
    host->palette_pumping = true;
    palette_popup_pump(host->open_palette);
    host->palette_pumping = false;
    end_palette(host);
}

/* Switches the active backend. Switching to Ollama when no local model has
   been remembered opens the Ollama picker instead and commits the switch only
   after a model is selected (or does nothing when the picker is cancelled). */
static void select_backend(ChatHost *host, ChatBackend backend) {
    Chat *chat = host->config.chat;
    if (chat->backend == backend && !host->backend_switch_pending) {
        set_status(host, backend == CHAT_BACKEND_OLLAMA ?
            L"Ollama is already the active backend." :
            L"OpenRouter is already the active backend.");
        return;
    }
    if (backend == CHAT_BACKEND_OLLAMA && !chat->ollama_model[0]) {
        host->backend_target = CHAT_BACKEND_OLLAMA;
        host->backend_switch_pending = true;
        begin_model_palette(host);
        if (host->open_palette) {
            host->palette_pumping = true;
            palette_popup_pump(host->open_palette);
            host->palette_pumping = false;
        }
        end_palette(host);
        if (chat->backend == CHAT_BACKEND_OLLAMA)
            set_status(host, L"Switched to Ollama.");
        return;
    }
    chat->backend = backend;
    host->backend_target = backend;
    mark_dirty(host);
    rich_text_set_text(&host->field, chat_active_model(chat));
    save(host);
    chat_ui_sync(&host->chat_ui);
    set_status(host, backend == CHAT_BACKEND_OLLAMA ?
        L"Switched to Ollama." : L"Switched to OpenRouter.");
}

/* Interprets one fetch completion. Stale generations are freed and dropped;
   success replaces that backend's catalog, failure keeps its last good one and
   records a nonfatal reason. An open picker is refreshed in place with its
   filter and selection preserved. */
static void catalog_event(ChatHost *host, ModelCatalogEvent *event) {
    if (!event) return;
    if (event->generation != host->catalog_generation) {
        model_catalog_event_free(event);
        return;
    }
    ChatBackend backend = host->catalog_backend;
    ChatModelCatalog *catalog = &host->catalog[(int)backend];
    host->catalog_loading = false;
    if (event->result == MODEL_CATALOG_OK && event->json) {
        ChatModelParseStats stats;
        if (chat_model_catalog_parse(catalog, event->json, &stats)) {
            host->catalog_stats[backend] = stats;
            host->catalog_loaded[backend] = true;
            host->catalog_failed[backend] = false;
            host->catalog_error[backend][0] = 0;
            host->catalog_success_tick[backend] = GetTickCount64();
        } else {
            host->catalog_failed[backend] = true;
            wcsncpy(host->catalog_error[backend],
                L"The catalog response could not be parsed.",
                CHAT_STATUS_TEXT - 1);
            host->catalog_error[backend][CHAT_STATUS_TEXT - 1] = 0;
        }
    } else {
        host->catalog_failed[backend] = true;
        wcsncpy(host->catalog_error[backend],
            event->error ? event->error : L"Model catalog request failed.",
            CHAT_STATUS_TEXT - 1);
        host->catalog_error[backend][CHAT_STATUS_TEXT - 1] = 0;
    }
    int generation = event->generation;
    model_catalog_event_free(event);
    model_catalog_complete(&host->catalog_client, generation);
    host->catalog_generation = 0;
    if (host->open_palette &&
        palette_popup_mode(host->open_palette) == PALETTE_MODE_MODELS) {
        ChatBackend target = host->backend_target;
        /* Only a completion for the other backend leaves the open palette
           without its own fetch; queue that fetch so a switch completes in
           place. A fetch for the target itself is never auto-retried here, so
           a failure cannot spin. */
        if (target != backend)
            maybe_start_catalog_fetch(host, target);
        wchar_t history[CHAT_MODEL_HISTORY][CHAT_MODEL_TEXT];
        int history_count = build_backend_history(host, target, history);
        wchar_t status[CHAT_STATUS_TEXT];
        picker_status(host, target, status, CHAT_STATUS_TEXT);
        if (!palette_popup_set_models(host->open_palette,
                &host->catalog[(int)target],
                current_model(host->config.chat, target), history,
                history_count, status))
            set_status(host, L"Could not refresh the model list.");
    }
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

/* Places a native child exactly over an arranged placeholder rectangle and
   keeps visibility in step. Identical geometry and visibility are a no-op,
   so the placement pass can run on every flush without touching child
   windows (and without provoking relayout churn). */
/* Whether the last placement left the child shown. The WS_VISIBLE style is
   authoritative even when the top-level window is not shown (the hidden test
   harness), unlike IsWindowVisible, which folds in ancestor visibility. */
static bool child_shown(HWND child) {
    return child && (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0;
}

static void place_native(ChatHost *host, HWND child, UiRect area, bool show) {
    if (!child) return;
    if (!show || area.w <= 2 || area.h <= 2) {
        if (child_shown(child)) ShowWindow(child, SW_HIDE);
        return;
    }
    int x = px(host, area.x), y = px(host, area.y);
    int w = px(host, area.x + area.w) - x;
    int h = px(host, area.y + area.h) - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    RECT current;
    POINT origin = { 0, 0 };
    ClientToScreen(host->window, &origin);
    if (child_shown(child) &&
        GetWindowRect(child, &current) &&
        current.left == origin.x + x && current.top == origin.y + y &&
        current.right == origin.x + x + w &&
        current.bottom == origin.y + y + h) return;
    SetWindowPos(child, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(child, SW_SHOWNOACTIVATE);
}

static void place(ChatHost *host, RichTextControl *control, UiId id,
    float inset) {
    UiNode *item = ui_node(host->config.ui, id);
    if (!item || !control->window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += inset;
    area.y += inset;
    area.w -= 2 * inset;
    area.h -= 2 * inset;
    place_native(host, control->window, area,
        ui_visible(host->config.ui, id));
}

/* A single-line field is vertically centered inside its placeholder. The
   horizontal inset leaves room for a retained glyph drawn inside the
   placeholder (the search field's magnifier). */
static void place_field_inset(ChatHost *host, RichTextControl *control, UiId id,
    float left_inset) {
    UiNode *item = ui_node(host->config.ui, id);
    if (!item || !control->window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += left_inset;
    area.w -= left_inset + 1;
    area.y += 1;
    area.h -= 2;
    float line = host->rich_theme.ui_size * 1.8f;
    float top = area.y + (area.h - line) / 2;
    if (top < area.y) top = area.y;
    float height = line < area.h ? line : area.h;
    place_native(host, control->window,
        (UiRect){area.x, top, area.w, height},
        ui_visible(host->config.ui, id));
}

/* Sizes the transcript container over its placeholder, inside the drawn
   border. An empty conversation has nothing to realize, so the container is
   hidden and the retained empty-state hero shows through instead. */
static void place_container(ChatHost *host) {
    UiNode *item = ui_node(host->config.ui, host->chat_ui.transcript);
    if (!item || !host->view) return;
    const ChatConversation *active = chat_active(host->config.chat);
    bool empty = !active || active->message_count == 0;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += 1;
    area.y += 1;
    area.w -= 2;
    area.h -= 2;
    place_native(host, host->view, area,
        ui_visible(host->config.ui, host->chat_ui.transcript) && !empty);
}

static void layout(ChatHost *host) {
    if (host->minimized) return;
    RECT client;
    GetClientRect(host->window, &client);
    /* chat_ui_resize lays out only when the client size changed, the
       responsive sidebar state flipped, or a relayout is owed. */
    bool laid_out = chat_ui_resize(&host->chat_ui, dip(host, client.right),
        dip(host, client.bottom));
    /* The transcript container's desired visibility also depends on the
       active conversation being empty, which changes without a layout, so it
       is reconciled on every pass (a no-op when nothing moved). The other
       native overlays can only move when the layout ran. */
    place_container(host);
    if (!laid_out) return;
    place(host, &host->composer, host->chat_ui.composer, 1.0f);
    place_field_inset(host, &host->field, host->chat_ui.model, 1.0f);
    place_field_inset(host, &host->search, host->chat_ui.search,
        CHAT_UI_SEARCH_ICON_INSET);
}

/* Translates the sidebar remap change report into UIA notifications. Row
   bindings are compared by stable id, never by title text. */
static void sidebar_accessibility(ChatHost *host, const ChatUiRemapReport *report) {
    if (!host->accessibility) return;
    if (report->mapping_changed)
        ui_accessibility_children_invalidated(host->accessibility,
            host->chat_ui.list);
    for (int i = 0; i < report->name_changed_count; i++) {
        const wchar_t *current = ui_accessible_name(host->config.ui,
            report->name_changed[i].id);
        ui_accessibility_property_changed(host->accessibility,
            report->name_changed[i].id,
            report->name_changed[i].old_title, current);
    }
    if (report->focus_binding_changed)
        ui_accessibility_focus_changed(host->accessibility,
            host->config.ui->focus ? host->config.ui->focus
                                   : host->config.ui->root);
}

static void flush(ChatHost *host) {
    layout(host);
    /* A reveal runs a second remap within this flush; the report
       accumulates so the first remap's findings are never overwritten. */
    chat_ui_remap_report_clear(&host->remap);
    chat_ui_sidebar_remap(&host->chat_ui, &host->remap);
    if (ui_layout_pending(host->config.ui)) layout(host);
    /* Reveal is applied after the remap so the scroll range reflects the
       current conversation count; a moved scroll rebinds the pool once more. */
    if (chat_ui_apply_reveal(&host->chat_ui)) {
        layout(host);
        chat_ui_sidebar_remap(&host->chat_ui, &host->remap);
        if (ui_layout_pending(host->config.ui)) layout(host);
    }
    sidebar_accessibility(host, &host->remap);
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
    Chat *chat = host->config.chat;
    wchar_t buffer[CHAT_MODEL_TEXT];
    rich_text_get_text(&host->field, buffer, CHAT_MODEL_TEXT);
    wchar_t *model = buffer;
    while (*model == L' ' || *model == L'\t') ++model;
    size_t length = wcslen(model);
    while (length && (model[length - 1] == L' ' || model[length - 1] == L'\t'))
        model[--length] = 0;
    if (length) {
        wchar_t *slot = model_slot(chat, chat->backend);
        wcsncpy(slot, model, CHAT_MODEL_TEXT - 1);
        slot[CHAT_MODEL_TEXT - 1] = 0;
    }
    /* Never let the visible field and the stored model disagree: an empty field
       falls back to the active backend's last valid model, which is written
       back into the field. */
    rich_text_set_text(&host->field, chat_active_model(chat));
    mark_dirty(host);
}

static void field_blur(void *user) { sync_model((ChatHost *)user); }


/* A native child that can receive focus: shown by the last placement (the
   WS_VISIBLE style, which is meaningful even when the top-level is not
   shown, as in the hidden test harness) and not disabled. */
static bool native_child_usable(HWND window) {
    return window &&
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0 &&
        IsWindowEnabled(window);
}

/* The native Tab stops in visual order. A hidden or disabled field is not a
   candidate: the collapsed sidebar hides the search edit and generation
   disables the model edit, so including them would strand focus on a window
   that cannot take it. */
static int native_focus_order(ChatHost *host, HWND *order, int capacity) {
    HWND candidates[3] = { host->field.window, host->search.window,
        host->composer.window };
    int count = 0;
    for (int i = 0; i < 3 && count < capacity; i++)
        if (native_child_usable(candidates[i])) order[count++] = candidates[i];
    return count;
}

/* Moves focus into the retained DarkUI tree at its first (forward) or last
   (reverse) focusable control, so Tab continues there instead of wrapping
   inside the native fields. */
static void focus_retained_edge(ChatHost *host, bool reverse) {
    if (!ui_focus_edge(host->config.ui, reverse)) return;
    SetFocus(host->window);
    flush(host);
}

/* Moves focus to the first (forward) or last (reverse) usable native field. */
static void focus_native_edge(ChatHost *host, bool reverse) {
    HWND order[3];
    int count = native_focus_order(host, order, 3);
    if (!count) return;
    SetFocus(order[reverse ? count - 1 : 0]);
}

/* Tab traversal that spans the native fields and the retained controls: the
   two form one cycle (model -> search -> composer -> hamburger -> ... ->
   send -> model) rather than two disconnected rings. */
static void focus_surface(ChatHost *host, bool reverse) {
    HWND order[3];
    int count = native_focus_order(host, order, 3);
    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < count; i++) if (order[i] == focus) current = i;
    if (current < 0) { focus_native_edge(host, reverse); return; }
    int next = current + (reverse ? -1 : 1);
    if (next < 0 || next >= count) {
        focus_retained_edge(host, reverse);
        return;
    }
    SetFocus(order[next]);
}

/* After the sidebar changes, a focused native window that is now hidden
   (the search edit when the sidebar collapses) must hand focus back to the
   composer, exactly like a transcript surface that is about to be hidden. */
static void ensure_native_focus(ChatHost *host) {
    HWND focus = GetFocus();
    if (focus && (focus == host->field.window || focus == host->search.window ||
        focus == host->composer.window) && !native_child_usable(focus) &&
        native_child_usable(host->composer.window))
        SetFocus(host->composer.window);
}
static void capture_settings(ChatHost *host) {
    Chat *chat=host->config.chat;
    wchar_t draft[CHAT_COMPOSER_TEXT];
    rich_text_get_text(&host->composer,draft,CHAT_COMPOSER_TEXT);
    ChatConversation *c=&chat->conversations[chat->active];
    if (!host->editing && wcscmp(draft,c->draft)) {
        wcscpy(c->draft,draft); c->modified_at=chat_now(); mark_dirty(host);
    }
    wchar_t model[CHAT_MODEL_TEXT];
    rich_text_get_text(&host->field,model,CHAT_MODEL_TEXT);
    wchar_t *slot=model_slot(chat,chat->backend);
    if (model[0] && wcscmp(model,slot)) { wcscpy(slot,model); mark_dirty(host); }
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
            chat->maximized=maximized; mark_dirty(host);
        }
    }
}

static void start_response(ChatHost *host, ChatSendMode mode, const wchar_t *prompt) {
    if (host->generating) return;
    Chat *chat=host->config.chat;
    sync_model(host);
    int index=chat_begin_response(chat,mode,prompt);
    if (index<0) { set_status(host,L"Action unavailable: check the latest turn and conversation capacity."); return; }
    /* Resolved only once the response exists, so a rejected action cannot
       touch a conversation it never used. */
    ChatConversation *c=&chat->conversations[chat->active];
    host->request_conversation=chat->active; host->request_message=index;
    /* A toolbar-triggered send/retry must not destroy a focused transcript
       surface: move focus to the composer through the transcript's
       focus-release callback before the replacement invalidation. A no-op
       when no transcript surface holds focus. */
    transcript_focus_release(&host->transcript);
    /* Retry/regenerate/edit-resend reuse turn slots: their stale identity,
       pending updates and selections are dropped so the next render replaces
       the affected surfaces without deferral. */
    transcript_invalidate_from(&host->transcript, index);
    host->started_tick=GetTickCount64();
    /* The running turn shows a temporary pending row; it is removed once answer
       text begins if the provider never supplies reasoning. */
    host->reasoning_streaming=false; host->content_started=false;
    host->reason_paint_pending=false; host->reasoning_paint_tick=0;
    host->transcript.body_render_tick=0;
    cancel_body_flush(host);
    ChatMessage *m=pending(host);
    if (mode==CHAT_SEND) { rich_text_set_text(&host->composer,L""); chat->conversations[chat->active].draft[0]=0; }
    if (host->editing && mode!=CHAT_SEND) {
        host->editing=false;
        rich_text_set_text(&host->composer,chat->conversations[chat->active].draft);
    }
    mark_dirty(host);
    /* Durability gate: the user turn and pending response are durable before
       network work starts. This flushes through the background writer and
       keeps today's synchronous guarantee; a save failure outranks every
       context diagnostic and the request is not sent. */
    bool saved=save_sync(host);
    /* The request context is a bounded projection of the conversation: the
       system prompt, the triggering user message and the newest eligible
       history that fits CHAT_CONTEXT_BUDGET_BYTES. Persisted history is never
       changed by this. A save failure outranks every context diagnostic. */
    ChatRequestContext context;
    ChatContextResult built=chat_context_build(chat,c,index-1,
        CHAT_CONTEXT_BUDGET_BYTES,&context);
    host->context_dropped=built==CHAT_CONTEXT_OK ? context.dropped_messages : 0;
    /* OpenRouter keeps its credentials and provider routing; Ollama needs
       neither, so a missing OPENROUTER_API_KEY never blocks it. */
    host->request_generation=saved && built==CHAT_CONTEXT_OK ?
        completion_request(&host->client,chat->backend,
            host->config.api_key_utf8,chat_active_model(chat),
            context.messages,context.count,
            chat->backend==CHAT_BACKEND_OPENROUTER ?
                &chat->provider_routing : NULL) : 0;
    if (!host->request_generation) {
        m->generation.state=CHAT_GENERATION_FAILED;
        m->generation.finished_at=chat_now();
        m->generation.latency_ms=0;
        if (!saved) wcscpy(m->generation.error,
            L"Could not save pending response; request was not sent.");
        else if (built==CHAT_CONTEXT_INVALID) wcscpy(m->generation.error,
            L"The request context could not be built; the latest turn is inconsistent.");
        else if (built!=CHAT_CONTEXT_OK) {
            /* required_bytes is the complete body the indispensable content
               needs, so the diagnostic states the cause and the real size
               instead of implying a single offending message. */
            const wchar_t *cause = built==CHAT_CONTEXT_OVERSIZE_SYSTEM
                ? L"the system prompt is too large; shorten it in Settings"
                : built==CHAT_CONTEXT_OVERSIZE_USER
                ? L"this message is too large; shorten it to send"
                : L"the system prompt and this message are too large together; shorten either";
            const size_t bound=sizeof m->generation.error/sizeof *m->generation.error;
            swprintf(m->generation.error,bound,
                L"Request not sent: %ls. The request body needs %lu bytes; the budget is %lu.",
                cause,(unsigned long)context.required_bytes,
                (unsigned long)CHAT_CONTEXT_BUDGET_BYTES);
            /* Truncation semantics of a full buffer are unspecified for
               swprintf: terminate explicitly so every reader is safe. */
            m->generation.error[bound-1]=0;
        }
        else if (chat->backend==CHAT_BACKEND_OPENROUTER &&
            (!host->config.api_key_utf8 || !host->config.api_key_utf8[0]))
            wcscpy(m->generation.error,
            L"OPENROUTER_API_KEY is unavailable. Set the Windows User environment variable and restart.");
        else if (chat->backend==CHAT_BACKEND_OLLAMA)
            wcscpy(m->generation.error,L"Could not start Ollama request.");
        else wcscpy(m->generation.error,L"Could not start OpenRouter request.");
        chat_message_touch(m);
        mark_dirty(host); save(host); set_status(host,L"Request failed; use Response > Retry.");
    } else {
        host->generating=true; host->stopping=false; host->accepting=true;
        host->stop_state=CHAT_GENERATION_CANCELLED;
        chat_ui_set_generation(&host->chat_ui,true,false);
        EnableWindow(host->field.window,FALSE);
        if (host->context_dropped) {
            wchar_t status[CHAT_STATUS_TEXT];
            swprintf(status,CHAT_STATUS_TEXT,L"Generating... (%d older messages omitted to fit the request budget)",host->context_dropped);
            status[CHAT_STATUS_TEXT-1]=0;   /* truncation must still terminate */
            set_status(host,status);
        } else set_status(host,L"Generating...");
    }
    sync_transcript_container(host);
    render_transcript(host); chat_ui_sync(&host->chat_ui); flush(host);
}

static void perform_send(ChatHost *host) {
    if (host->generating) {
        if (!host->stopping && completion_cancel(&host->client,host->request_generation)) {
            host->stopping=true; host->accepting=false;
            pending(host)->generation.state=CHAT_GENERATION_CANCELLED;
            pending(host)->generation.finished_at=chat_now();
            pending(host)->generation.latency_ms=(double)(GetTickCount64()-host->started_tick);
            chat_message_touch(pending(host));
            mark_dirty(host); save(host);
            chat_ui_set_generation(&host->chat_ui,true,true);
            set_status(host,L"Stopping generation...");
        }
        return;
    }
    wchar_t prompt[CHAT_COMPOSER_TEXT];
    rich_text_get_text(&host->composer,prompt,CHAT_COMPOSER_TEXT);
    start_response(host,host->editing ? CHAT_EDIT_RESEND : CHAT_SEND,prompt);
}

static void append_stream_delta(ChatHost *host, CompletionEvent *event) {
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
        /* Paint any reasoning tail still pending before the streaming
            window ends and the flush timer is cancelled. */
        flush_reasoning_paint(host);
        end_reasoning(host);
    }
    size_t incoming=wcslen(event->text);
    if (!chat_message_append_text(m,event->text)) {
        host->stop_state=CHAT_GENERATION_INTERRUPTED;
        host->accepting=false; host->stopping=true;
        completion_cancel(&host->client,host->request_generation);
        chat_ui_set_generation(&host->chat_ui,true,true);
        set_status(host,L"Not enough memory to continue the response.");
        return;
    }
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    mark_dirty(host);
    if (incoming>0 && host->request_conversation==host->config.chat->active) {
        int index=host->request_message;
        TranscriptRecord *rec=&host->transcript.records[index];
        if (first) {
            /* Rebuilds this turn's body and refreshes/removes its row. */
            refresh_turn(host,index);
            host->transcript.body_render_tick=GetTickCount64();
            cancel_body_flush(host);
        } else if (rec->body_live &&
            transcript_surface(&host->transcript,index,TRANSCRIPT_BODY)) {
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
        } else if (host->transcript.bounded && !host->body_flush_pending) {
            /* The body surface is missing (never created, or its creation
               failed): arm the flush so the retry paths — this delta's
               schedule, the 1 Hz sweep, the terminal render — recreate it.
               Each retry is one bounded attempt; no path recurses. */
            host->body_flush_pending=true;
            schedule_body_flush(host);
        }
    }
}

static void append_reasoning_delta(ChatHost *host, CompletionEvent *event) {
    if (!event->text || !event->text[0] || !host->accepting) return;
    ChatMessage *m=pending(host);
    m->generation=event->metadata; m->generation.state=CHAT_GENERATION_RUNNING;
    size_t incoming=wcslen(event->text);
    if (incoming) {
        if (!chat_message_append_reasoning(m,event->text)) {
            host->stop_state=CHAT_GENERATION_INTERRUPTED;
            host->accepting=false; host->stopping=true;
            completion_cancel(&host->client,host->request_generation);
            chat_ui_set_generation(&host->chat_ui,true,true);
            set_status(host,L"Not enough memory to continue the reasoning.");
            return;
        }
        if (host->request_conversation==host->config.chat->active) {
            TranscriptRecord *rec=&host->transcript.records[host->request_message];
            RichTextControl *reason=transcript_surface(&host->transcript,
                host->request_message,TRANSCRIPT_REASON);
            if (m->reasoning_open && !rec->reason_live) {
                /* Creating or reopening the pane reloads the accumulated
                    reasoning whole (the record's painted mark is stamped
                    by that reload). */
                refresh_turn(host,host->request_message);
                host->reason_paint_pending=false;
            } else if (m->reasoning_open && rec->reason_live && reason) {
                /* Fragments accumulate in the message; the pane repaints
                    at most once per flush interval. One repaint is a
                    synchronous Rich Edit update with a forced repaint, so
                    per-fragment appends would freeze the UI under a
                    word-sized reasoning stream. */
                host->reason_paint_pending=true;
                if (!host->body_flush_pending) schedule_body_flush(host);
            }
        }
    }
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    mark_dirty(host);
    if (!host->reasoning_streaming) {
        host->reasoning_streaming=true;
        host->reasoning_started_tick=GetTickCount64();
    }
}

static void finish_request(ChatHost *host, CompletionEvent *event) {
    end_reasoning(host);
    double reasoning_ms=pending(host)->generation.reasoning_ms;
    completion_complete(&host->client,event->generation);
    ChatMessage *m=pending(host);
    m->generation=event->metadata;
    m->generation.reasoning_ms=reasoning_ms;
    ChatGeneration *g=&m->generation;
    g->state=host->stopping ? host->stop_state : event->type==COMPLETION_DONE ?
        CHAT_GENERATION_COMPLETE : event->type==COMPLETION_CANCELLED ?
        CHAT_GENERATION_CANCELLED : event->type==COMPLETION_INTERRUPTED ?
        CHAT_GENERATION_INTERRUPTED : CHAT_GENERATION_FAILED;
    if (event->text) wcsncpy(g->error,event->text,511);
    if (g->state==CHAT_GENERATION_COMPLETE && !wcscmp(g->finish_reason,L"error")) g->state=CHAT_GENERATION_FAILED;
    if (g->state==CHAT_GENERATION_COMPLETE && !chat_message_text(m)[0]) {
        g->state=CHAT_GENERATION_FAILED;
        swprintf(g->error,512,L"%ls completed without text.",
            chat_backend_name(g->backend));
    }
    /* Terminal generation metadata is observable: mark the message changed. */
    chat_message_touch(m);
    m->modified_at=chat_now();
    host->config.chat->conversations[host->request_conversation].modified_at=m->modified_at;
    host->generating=false; host->stopping=false; host->accepting=false;
    host->context_dropped=0;   /* the omission count belongs to the live request */
    chat_ui_set_generation(&host->chat_ui,false,false); EnableWindow(host->field.window,TRUE);
    set_status(host,chat_generation_name(g->state));
    /* The full transcript render flushes any body rebuild the streaming
       throttle had deferred, so the terminal Markdown is always visible. */
    cancel_body_flush(host);
    mark_dirty(host); save(host);
    if (host->request_conversation==host->config.chat->active) render_transcript(host);
    chat_ui_sync(&host->chat_ui); flush(host);
}

static void handle_event(ChatHost *host, CompletionEvent *event) {
    if (!event) return;
    if (event->generation==host->request_generation && host->generating) {
        if (event->type==COMPLETION_DELTA) append_stream_delta(host,event);
        else if (event->type==COMPLETION_REASONING) append_reasoning_delta(host,event);
        else finish_request(host,event);
    }
    completion_event_free(event);
}

/* Processes the batch one wake delivered. The per-wake work is naturally
   bounded (fragments are coalesced by the worker and the heavy body and
   reasoning flushes stay on the flush timer), so the handler returns to
   the normal pump immediately instead of re-entering a filtered loop that
   would starve queued input messages. Events for an older generation are
   freed unprocessed. */
static void handle_events(ChatHost *host) {
    CompletionEvent *batch=completion_take(&host->client);
    while (batch) {
        CompletionEvent *next=batch->next;
        batch->next=NULL;
        handle_event(host,batch);
        batch=next;
    }
}

static void command(void *user, ChatCommand code, int index) {
    ChatHost *host = (ChatHost *)user;
    Chat *chat = host->config.chat;
    if (code == CHAT_COMMAND_TOGGLE_SIDEBAR) {
        /* The wide-width preference is persisted; the narrow-width drawer is
           session state, so only a preference flip marks the store dirty. */
        if (chat_ui_toggle_sidebar(&host->chat_ui)) {
            mark_dirty(host);
            save(host);
        }
        flush(host);
        /* Collapsing can hide the focused search edit; never strand focus. */
        ensure_native_focus(host);
        return;
    }
    if (code == CHAT_COMMAND_OVERFLOW) {
        PostMessageW(host->window, CHAT_WM_ACTIONS_MENU, 0, 0);
        return;
    }
    if (code == CHAT_COMMAND_MODEL_PICKER) { open_model_palette(host); return; }
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
            sync_transcript_container(host);
            render_transcript(host);
            rich_text_set_text(&host->composer, L"");
            chat_ui_sync(&host->chat_ui);
            /* The new row sits below the window until it is revealed. */
            chat_ui_request_reveal(&host->chat_ui,
                chat->conversations[chat->active].id);
        }
    } else if (code == CHAT_COMMAND_SELECT) {
        if (chat_select_conversation(chat, index)) {
            cancel_body_flush(host);
            transcript_invalidate(&host->transcript);
            sync_transcript_container(host);
            render_transcript(host);
            rich_text_set_text(&host->composer,chat->conversations[chat->active].draft);
            chat_ui_sync(&host->chat_ui);
            /* Identity-based selection may target a row outside the window
               (search navigation, UIA SetFocus on a list summary). */
            chat_ui_request_reveal(&host->chat_ui,
                chat->conversations[chat->active].id);
        }
    }
    mark_dirty(host); save(host);
    ui_invalidate(host->config.ui, false);
    flush(host);
}

static const wchar_t *search_role_name(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_USER: return L"User";
    case CHAT_ROLE_ASSISTANT: return L"Assistant";
    case CHAT_ROLE_SYSTEM: return L"System";
    case CHAT_ROLE_ERROR: return L"Error";
    }
    return L"Message";
}

/* Resolves ids immediately before navigation; retained array indices are never
   trusted across conversation deletion, message replacement, or growth. */
static bool jump_search_result(ChatHost *host, size_t index) {
    ChatSearchTarget target;
    if (!chat_search_resolve(host->config.chat, &host->search_results, index,
            &target)) return false;
    Chat *chat = host->config.chat;
    bool different_conversation = target.conversation != chat->active;
    ChatMessage *message =
        &chat->conversations[target.conversation].messages[target.message];
    bool open_reasoning = target.field == CHAT_SEARCH_REASONING &&
        !message->reasoning_open;
    if (open_reasoning) message->reasoning_open = true;
    if (different_conversation) {
        /* Selection invalidates and renders once. Set per-turn view state first
           so a reasoning result is realized by that render. */
        command(host, CHAT_COMMAND_SELECT, target.conversation);
    } else if (open_reasoning && !host->transcript.bounded) {
        /* Only the newly opened reasoning turn needs synchronization. Body
           results and already-open reasoning results need no render at all.
           Bounded reveal performs this reconciliation in its one epoch. */
        refresh_turn(host, target.message);
    }
    if (target.conversation != chat->active || target.message < 0 ||
        (size_t)target.message >= chat->conversations[chat->active].message_count)
        return false;
    {
        TranscriptFeed feed = transcript_feed(host);
        if (!transcript_reveal_turn(&host->transcript, &feed, target.message))
            return false;
    }

    host->search_selected = index;
    host->search_has_selection = true;
    /* Every successful navigation reveals the active conversation's sidebar
       row, including navigation within the already-active conversation. */
    chat_ui_request_reveal(&host->chat_ui,
        chat->conversations[chat->active].id);
    const ChatSearchResult *result = &host->search_results.items[index];
    wchar_t status[UI_TEXT_CAPACITY];
    swprintf(status, UI_TEXT_CAPACITY, L"%llu/%llu %ls %ls: %ls",
        (unsigned long long)(index + 1),
        (unsigned long long)host->search_results.count,
        search_role_name(result->role),
        result->field == CHAT_SEARCH_REASONING ? L"reasoning" : L"message",
        result->snippet);
    status[UI_TEXT_CAPACITY - 1] = 0;
    chat_ui_set_search_status(&host->chat_ui, status);
    flush(host);
    return true;
}

static bool search_step(ChatHost *host, bool reverse) {
    size_t count = host->search_results.count;
    if (!count) {
        chat_ui_set_search_status(&host->chat_ui, L"No results");
        flush(host);
        return false;
    }
    size_t candidate = host->search_has_selection ? host->search_selected :
        (reverse ? 0 : count - 1);
    for (size_t checked = 0; checked < count; checked++) {
        candidate = reverse ? (candidate ? candidate - 1 : count - 1) :
            (candidate + 1) % count;
        if (jump_search_result(host, candidate)) return true;
    }
    host->search_has_selection = false;
    chat_ui_set_search_status(&host->chat_ui,
        L"Results changed; press Enter to refresh");
    flush(host);
    return false;
}

static bool search_refresh(ChatHost *host, bool reverse) {
    wchar_t query[CHAT_SEARCH_QUERY_TEXT];
    rich_text_get_text(&host->search, query, CHAT_SEARCH_QUERY_TEXT);
    ChatSearchOptions options = { true };
    if (!chat_search_build(host->config.chat, query, options,
            &host->search_results)) {
        chat_ui_set_search_status(&host->chat_ui, L"Search could not allocate results");
        flush(host);
        return true;
    }
    host->search_has_selection = false;
    if (!query[0]) {
        chat_ui_set_search_status(&host->chat_ui,
            L"Enter to search messages and reasoning");
        flush(host);
        return true;
    }
    search_step(host, reverse);
    return true;
}

static bool search_submit(void *user) {
    return search_refresh((ChatHost *)user, false);
}

/* True when any realized transcript surface currently holds a selection. */
static RichTextControl *transcript_selected_surface(ChatHost *host) {
    const TranscriptSurface surfaces[3]={TRANSCRIPT_HEAD,TRANSCRIPT_BODY,
        TRANSCRIPT_REASON};
    for (int i=0;i<host->transcript.record_count;i++)
        for (int k=0;k<3;k++) {
            RichTextControl *control=transcript_surface(&host->transcript,
                i,surfaces[k]);
            if (control && rich_text_has_selection(control)) return control;
        }
    return NULL;
}

/* Anchors the complete retained command menu under the overflow button. The
   default presentation has no menu bar, but every Conversation, Response and
   Settings command stays reachable here (and through its keyboard shortcut).
   TrackPopupMenu's WM_INITMENUPOPUP runs the live availability and
   routing/backend sync. */
static void open_actions_menu(ChatHost *host) {
    UiRect r = chat_ui_rect(&host->chat_ui, host->chat_ui.overflow);
    /* The arranged rectangle is in 96-DPI DIPs; ClientToScreen expects
       physical pixels, so both coordinates scale before the conversion. */
    POINT point = { px(host, r.x + r.w), px(host, r.y + r.h + 4) };
    ClientToScreen(host->window, &point);
    HMENU menu = chat_actions_menu(host->config.chat);
    if (!menu) return;
    SetForegroundWindow(host->window);
    int command_id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN |
        TPM_TOPALIGN, point.x, point.y, 0, host->window, NULL);
    DestroyMenu(menu);
    if (command_id) action(host, command_id);
    /* MSDN: a queued null message lets the menu dismiss cleanly. */
    PostMessageW(host->window, WM_NULL, 0, 0);
}

static void action(ChatHost *host, int code) {
    Chat *chat=host->config.chat;
    ChatConversation *c=&chat->conversations[chat->active];
    if (code==ACTION_SEARCH) {
        /* The search field lives in the sidebar: reveal it (opening the
           narrow-width drawer, or restoring the wide-width preference)
           before placing and focusing the hidden native edit. */
        if (chat_ui_reveal_sidebar(&host->chat_ui)) { mark_dirty(host); save(host); }
        flush(host);
        SetFocus(host->search.window);
        SendMessageW(host->search.window,EM_SETSEL,0,-1);
        return;
    }
    if (code==ACTION_COPY) {
        for (size_t i=c->message_count;i>0;i--) if (c->messages[i-1].role==CHAT_ROLE_ASSISTANT) {
            set_status(host,chat_copy_text(host->window,
                chat_message_text(&c->messages[i-1])) ? L"Response copied" :
                L"Copy failed"); return;
        }
        return;
    }
    if (code==ACTION_SELECTION) {
        /* Copy whichever turn viewport/block currently holds a selection. */
        RichTextControl *control=transcript_selected_surface(host);
        if (control) {
            SendMessageW(control->window,WM_COPY,0,0);
            set_status(host,L"Transcript selection copied");
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
        sync_transcript_container(host);
        host->editing=false; rich_text_set_text(&host->composer,chat->conversations[chat->active].draft); render_transcript(host);
        /* The active conversation may have shifted above or below the
           sidebar window after deletion. */
        if (chat->active >= 0 && chat->active < chat->conversation_count)
            chat_ui_request_reveal(&host->chat_ui,
                chat->conversations[chat->active].id);
    } else if (code==ACTION_SYSTEM) {
        wchar_t prompt[CHAT_COMPOSER_TEXT]; wcscpy(prompt,chat->system_prompt);
        if (chat_edit_dialog(host->window,L"System prompt (applies to future requests)",prompt,CHAT_COMPOSER_TEXT,true))
            wcscpy(chat->system_prompt,prompt);
    } else if (code==ACTION_SIDEBAR) {
        wchar_t width[16]; swprintf(width,16,L"%d",chat->sidebar_width);
        if (chat_edit_dialog(host->window,L"Sidebar width in DIPs (160-360)",width,16,false)) {
            wchar_t *end; long value=wcstol(width,&end,10);
            if (!*end && value>=160 && value<=360) chat->sidebar_width=(int)value;
            else set_status(host,L"Sidebar width must be 160-360");
        }
    } else if (code==ACTION_MODELS) {
        /* The picker owns its own conditional save so a mere browse never
           marks the session dirty; returning here skips the common tail. */
        open_model_palette(host);
        return;
    } else if (code==ACTION_BACKEND_OPENROUTER || code==ACTION_BACKEND_OLLAMA) {
        select_backend(host,code==ACTION_BACKEND_OLLAMA ?
            CHAT_BACKEND_OLLAMA : CHAT_BACKEND_OPENROUTER);
        return;
    } else if (code>=ACTION_ROUTING_SORT_DEFAULT && code<=ACTION_ROUTING_ZDR) {
        /* OpenRouter provider routing has no meaning for a local Ollama
           request. The menu items are grayed while Ollama is active; a stale
           menu activation is ignored here with a clear reason. */
        if (chat->backend==CHAT_BACKEND_OLLAMA) {
            set_status(host,L"Provider routing applies to OpenRouter only.");
            return;
        }
        if (code==ACTION_ROUTING_SORT_DEFAULT || code==ACTION_ROUTING_SORT_PRICE ||
            code==ACTION_ROUTING_SORT_THROUGHPUT || code==ACTION_ROUTING_SORT_LATENCY) {
            chat->provider_routing.sort =
                code==ACTION_ROUTING_SORT_PRICE ? CHAT_PROVIDER_SORT_PRICE :
                code==ACTION_ROUTING_SORT_THROUGHPUT ? CHAT_PROVIDER_SORT_THROUGHPUT :
                code==ACTION_ROUTING_SORT_LATENCY ? CHAT_PROVIDER_SORT_LATENCY :
                CHAT_PROVIDER_SORT_DEFAULT;
            set_status(host,L"Provider sorting updated for future requests.");
        } else if (code==ACTION_ROUTING_ALLOW_FALLBACKS) {
            chat->provider_routing.disallow_fallbacks=
                !chat->provider_routing.disallow_fallbacks;
            set_status(host,chat->provider_routing.disallow_fallbacks ?
                L"Fallback providers disabled; a request may fail if the primary is unavailable." :
                L"Fallback providers allowed.");
        } else if (code==ACTION_ROUTING_DATA_COLLECTION) {
            chat->provider_routing.data_collection =
                chat->provider_routing.data_collection==CHAT_DATA_COLLECTION_DENY ?
                CHAT_DATA_COLLECTION_ALLOW : CHAT_DATA_COLLECTION_DENY;
            set_status(host,chat->provider_routing.data_collection==CHAT_DATA_COLLECTION_DENY ?
                L"Routing restricted to providers that do not store data." :
                L"Providers that may store data are allowed.");
        } else if (code==ACTION_ROUTING_ZDR) {
            chat->provider_routing.zdr=!chat->provider_routing.zdr;
            set_status(host,chat->provider_routing.zdr ?
                L"Request-level zero data retention required; account settings may also apply." :
                L"DarkChat adds no request-level zero data retention requirement.");
        }
    }
    mark_dirty(host); save(host); chat_ui_sync(&host->chat_ui); flush(host);
}

/* ---- Deliberate keyboard navigation -------------------------------------- */

/* Named focus regions for F6 / Ctrl+T. Regions are not Tab stops: Tab still
   spans the native fields and the retained chrome, and the transcript stays a
   deliberate region reached only by region navigation or a mouse focus. */
typedef enum {
    CHAT_REGION_SIDEBAR,
    CHAT_REGION_TRANSCRIPT,
    CHAT_REGION_COMPOSER,
    CHAT_REGION_HEADER,
    CHAT_REGION_COUNT
} ChatRegion;

static bool ui_descends_from(Ui *ui, UiId id, UiId ancestor) {
    for (UiId p = id; p; ) {
        if (p == ancestor) return true;
        UiNode *item = ui_node(ui, p);
        p = item ? item->parent : UI_NONE;
    }
    return false;
}

static bool is_transcript_window(const ChatHost *host, HWND window) {
    if (!window) return false;
    for (int s = 0; s < host->transcript.slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            if (host->transcript.slots[s].surface[k].window == window)
                return true;
    return false;
}

/* True while the retained tree owns keyboard focus (the top-level window has
   it). A stale ui->focus from a native field must not be mistaken for it. */
static bool retained_focus_active(const ChatHost *host) {
    return GetFocus() == host->window;
}

static ChatRegion focus_region_of(const ChatHost *host) {
    HWND focus = GetFocus();
    if (focus == host->composer.window) return CHAT_REGION_COMPOSER;
    if (focus == host->field.window) return CHAT_REGION_HEADER;
    if (focus == host->search.window) return CHAT_REGION_SIDEBAR;
    if (is_transcript_window(host, focus)) return CHAT_REGION_TRANSCRIPT;
    UiId id = host->config.ui->focus;
    if (id) {
        if (ui_descends_from(host->config.ui, id, host->chat_ui.sidebar))
            return CHAT_REGION_SIDEBAR;
        if (ui_descends_from(host->config.ui, id, host->chat_ui.header))
            return CHAT_REGION_HEADER;
        if (ui_descends_from(host->config.ui, id, host->chat_ui.composer_area))
            return CHAT_REGION_COMPOSER;
    }
    return CHAT_REGION_COMPOSER;
}

/* The conversation id bound to the focused retained row, or 0 when focus is
   not on a realized sidebar row. */
static uint64_t focused_sidebar_id(const ChatHost *host) {
    UiId focus = host->config.ui->focus;
    for (int j = 0; j < host->chat_ui.pool_count; j++)
        if (host->chat_ui.rows[j] == focus) {
            UiNode *item = ui_node(host->config.ui, focus);
            return item ? (uint64_t)item->tag : 0;
        }
    return 0;
}

static bool sidebar_row_focused(const ChatHost *host) {
    UiId focus = host->config.ui->focus;
    for (int j = 0; j < host->chat_ui.pool_count; j++)
        if (host->chat_ui.rows[j] == focus) return true;
    return false;
}

static bool focus_sidebar_row_by_id(ChatHost *host, uint64_t id) {
    if (!id) return false;
    for (int j = 0; j < host->chat_ui.pool_count; j++) {
        UiNode *item = ui_node(host->config.ui, host->chat_ui.rows[j]);
        if (item && (uint64_t)item->tag == id) {
            ui_focus(host->config.ui, host->chat_ui.rows[j], true);
            return true;
        }
    }
    return false;
}

/* Focuses the realized row bound to a conversation index, advancing the
   windowed pool first when the row is outside it. Identity, never a recycled
   pool slot, is what is focused: a handling flush may rebind every row, so
   the identity is re-verified after the flush settles and the reveal is
   always applied before the focus (a focus on a partly visible row would
   itself scroll and rebind the row away from its conversation). */
static bool focus_conversation_row(ChatHost *host, int target) {
    Chat *chat = host->config.chat;
    if (target < 0 || target >= chat->conversation_count) return false;
    uint64_t id = chat->conversations[target].id;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (focused_sidebar_id(host) == id) return true;
        chat_ui_request_reveal(&host->chat_ui, id);
        flush(host);
        if (!focus_sidebar_row_by_id(host, id)) continue;
        flush(host);
        if (focused_sidebar_id(host) == id) return true;
    }
    return focused_sidebar_id(host) == id;
}

/* Roving focus within the sidebar: one conversation step per key. Inside the
   realized pool the toolkit's ui_focus_move supplies the movement and the
   reveal (with identity verified afterwards, since the reveal can rebind the
   pool); at the pool edge the window is advanced or retreated by identity. */
static bool sidebar_roving(ChatHost *host, int delta) {
    Chat *chat = host->config.chat;
    if (!delta || !sidebar_row_focused(host)) return false;
    int index = chat_index_of_id(chat, focused_sidebar_id(host));
    if (index < 0) return false;
    int target = index + delta;
    if (target < 0 || target >= chat->conversation_count) return false;
    uint64_t wanted = chat->conversations[target].id;
    int offset = host->chat_ui.window_offset;
    int end = offset + host->chat_ui.pool_count;
    if (target >= offset && target < end) {
        ui_focus_move(host->config.ui, delta);
        /* The remap runs at the next flush, so row tags still name the
           pre-move binding until then: flush before verifying identity. */
        flush(host);
        if (focused_sidebar_id(host) == wanted) return true;
    }
    return focus_conversation_row(host, target);
}

static bool sidebar_roving_edge(ChatHost *host, bool last) {
    Chat *chat = host->config.chat;
    if (!sidebar_row_focused(host) || chat->conversation_count <= 0)
        return false;
    return focus_conversation_row(host,
        last ? chat->conversation_count - 1 : 0);
}

/* Validates one realized transcript body as a focus candidate. A record at
   the right array index can still describe an older message (replacement,
   invalidation, or rebinding), so the record's identity must name both the
   active conversation and the live message at that index, the surface must be
   live (not merely bound), and its window must be the visible one. */
static RichTextControl *transcript_focus_body(ChatHost *host, int index) {
    Transcript *t = &host->transcript;
    const ChatConversation *active = chat_active(host->config.chat);
    if (!active || index < 0 || (size_t)index >= active->message_count)
        return NULL;
    if (index >= t->record_count) return NULL;
    TranscriptRecord *rec = &t->records[index];
    if (rec->conversation != active->id) return NULL;
    if (rec->message != active->messages[index].id) return NULL;
    if (!rec->body_live) return NULL;
    RichTextControl *body = transcript_surface(t, index, TRANSCRIPT_BODY);
    if (!body || !body->window) return NULL;
    if (!(GetWindowLongPtrW(body->window, GWL_STYLE) & WS_VISIBLE))
        return NULL;
    return body;
}

/* The turn whose body entering the Transcript region should focus: the
   reader's anchor turn when its body is realized, otherwise the nearest
   realized body. Identity is resolved from the live message list; a record
   slot is never trusted. Traversal is bounded by both the record list and
   the live message list. */
static int transcript_focus_index(ChatHost *host) {
    Transcript *t = &host->transcript;
    Chat *chat = host->config.chat;
    const ChatConversation *active = chat_active(chat);
    if (!active || active->message_count == 0) return -1;
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    if ((size_t)count > active->message_count)
        count = (int)active->message_count;
    if (t->anchor.valid && t->anchor.conversation == active->id) {
        int index = chat_message_index_by_id(chat, chat->active,
            t->anchor.message);
        if (index >= 0 && index < count && transcript_focus_body(host, index))
            return index;
    }
    int best = -1, best_distance = 0;
    for (int i = 0; i < count; i++) {
        if (!transcript_focus_body(host, i)) continue;
        int top = t->records[i].y;
        int bottom = t->records[i].y + t->records[i].height;
        int distance = 0;
        if (bottom <= t->view_scroll) distance = t->view_scroll - bottom;
        else if (top >= t->view_scroll + t->view_page)
            distance = top - (t->view_scroll + t->view_page);
        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static void focus_transcript_region(ChatHost *host) {
    int index = transcript_focus_index(host);
    RichTextControl *body =
        index >= 0 ? transcript_focus_body(host, index) : NULL;
    if (body) { SetFocus(body->window); return; }
    if (native_child_usable(host->composer.window))
        SetFocus(host->composer.window);
}

static void focus_region(ChatHost *host, ChatRegion region) {
    Ui *ui = host->config.ui;
    switch (region) {
    case CHAT_REGION_SIDEBAR: {
        if (!chat_ui_sidebar_visible(&host->chat_ui)) {
            if (chat_ui_reveal_sidebar(&host->chat_ui)) {
                mark_dirty(host);
                save(host);
            }
            flush(host);
        }
        SetFocus(host->window);
        const ChatConversation *active = chat_active(host->config.chat);
        uint64_t id = active ? active->id : 0;
        if (id) chat_ui_request_reveal(&host->chat_ui, id);
        flush(host);
        if (!focus_sidebar_row_by_id(host, id)) {
            if (host->chat_ui.pool_count > 0)
                ui_focus(ui, host->chat_ui.rows[0], true);
            else
                ui_focus(ui, host->chat_ui.new_conversation, true);
        }
        flush(host);
        return;
    }
    case CHAT_REGION_TRANSCRIPT:
        focus_transcript_region(host);
        return;
    case CHAT_REGION_COMPOSER:
        if (native_child_usable(host->composer.window))
            SetFocus(host->composer.window);
        return;
    case CHAT_REGION_HEADER:
        SetFocus(host->window);
        ui_focus(ui, host->chat_ui.hamburger, true);
        flush(host);
        return;
    default:
        return;
    }
}

static void cycle_region(ChatHost *host, bool reverse) {
    ChatRegion current = focus_region_of(host);
    int next = ((int)current + (reverse ? -1 : 1) + CHAT_REGION_COUNT) %
        CHAT_REGION_COUNT;
    focus_region(host, (ChatRegion)next);
}

/* F2 / Delete act on the conversation under sidebar focus. The target is
   resolved first and an unresolvable row is ignored outright: falling through
   to the active conversation would break the stable-identity promise (and is
   especially dangerous for Delete). Availability is checked before any
   selection change, so an action the host would reject -- during generation,
   say -- never renders, saves, or leaves edit mode for a different
   conversation first; action() itself reports the existing status. */
static void action_focused_conversation(ChatHost *host, int code) {
    Chat *chat = host->config.chat;
    int index = chat_index_of_id(chat, focused_sidebar_id(host));
    if (index < 0) return;
    if (index != chat->active) {
        if (host->generating) {
            action(host, code);
            return;
        }
        command(host, CHAT_COMMAND_SELECT, index);
    }
    action(host, code);
}

/* The single shortcut registry consulted by both key paths: surface_key for
   keys arriving from the native edits and transcript surfaces, and window_proc
   for keys arriving at the top level. `from_surface` distinguishes the Tab
   semantics the two paths have always had. */
static bool host_shortcut(ChatHost *host, WPARAM key, bool shift,
    bool control, bool from_surface) {
    if (control && (key == L'F' || key == L'f')) {
        action(host, ACTION_SEARCH);
        return true;
    }
    if (key == VK_F3) {
        wchar_t query[CHAT_SEARCH_QUERY_TEXT];
        rich_text_get_text(&host->search, query, CHAT_SEARCH_QUERY_TEXT);
        const wchar_t *searched = host->search_results.query
            ? host->search_results.query : L"";
        if (wcscmp(query, searched)) return search_refresh(host, shift);
        search_step(host, shift);
        return true;
    }
    if (control && key == VK_SPACE) { action(host, ACTION_MODELS); return true; }
    /* Ctrl+K opens the retained command palette: the same registry the
       overflow menu renders, filtered as you type. */
    if (control && (key == L'K' || key == L'k')) {
        open_palette(host);
        return true;
    }
    /* Ctrl+B toggles the sidebar; F10 / the context-menu key open the
       retained command menu, the replacement route to every command that the
       removed menu bar used to carry. */
    if (control && (key == L'B' || key == L'b')) {
        command(host, CHAT_COMMAND_TOGGLE_SIDEBAR, -1);
        return true;
    }
    if (key == VK_F10 || key == VK_APPS) {
        command(host, CHAT_COMMAND_OVERFLOW, -1);
        return true;
    }
    /* F6 / Ctrl+T cycle the named regions. Shift reverses the direction. */
    if (key == VK_F6 || (control && (key == L'T' || key == L't'))) {
        cycle_region(host, shift);
        return true;
    }
    if (key == VK_ESCAPE) {
        if (chat_ui_narrow_drawer_open(&host->chat_ui)) {
            chat_ui_close_drawer(&host->chat_ui);
            flush(host);
            return true;
        }
        HWND before = GetFocus();
        transcript_focus_release(&host->transcript);
        return GetFocus() != before;
    }
    if (key == VK_TAB) {
        if (from_surface) { focus_surface(host, shift); return true; }
        if (ui_focus_boundary(host->config.ui, shift)) {
            focus_native_edge(host, shift);
            return true;
        }
        return false;
    }
    if (!from_surface && retained_focus_active(host) &&
        sidebar_row_focused(host)) {
        if (key == VK_DOWN) { sidebar_roving(host, 1); return true; }
        if (key == VK_UP) { sidebar_roving(host, -1); return true; }
        if (key == VK_HOME) { sidebar_roving_edge(host, false); return true; }
        if (key == VK_END) { sidebar_roving_edge(host, true); return true; }
        if (key == VK_F2) {
            action_focused_conversation(host, ACTION_RENAME);
            return true;
        }
        if (key == VK_DELETE) {
            action_focused_conversation(host, ACTION_DELETE);
            return true;
        }
    }
    return false;
}

static bool surface_key(void *user, WPARAM key, bool shift, bool control,
    bool down) {
    if (!down) return false;
    return host_shortcut((ChatHost *)user, key, shift, control, true);
}

/* Ends a scrollbar thumb drag: the final position is reader input, so it
   runs the qualifying transition (bottom re-enters follow, anything else
   leaves it) and is captured as the fresh anchor at the next placement --
   the same path the release event uses, whether the drag ended normally
   or was cancelled (WM_CANCELMODE). */
static void end_scroll_drag(ChatHost *host, int position) {
    transcript_set_drag(&host->transcript, false);
    int maximum = host->transcript.view_content - host->transcript.view_page;
    if (maximum < 0) maximum = 0;
    if (position < 0) position = 0;
    if (position > maximum) position = maximum;
    transcript_note_user_scroll(&host->transcript, position);
    position_turns(host, false);
}

/* The position a cancelled drag ends at: while the drag owns the position
   it lives in nTrackPos; otherwise the settled nPos. */
static int drag_position(HWND window) {
    SCROLLINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SIF_ALL;
    GetScrollInfo(window, SB_VERT, &info);
    return info.nTrackPos;
}

static bool composer_submit(void *user) {
    perform_send((ChatHost *)user);
    return true;
}

/* The transcript's focus-transfer destination: when a surface is about to be
   hidden, invalidated, rebound, switched away, or torn down, the reader
   lands in the composer -- ordinary reader flow continues at the input, and
   no focused child ever disappears under the reader. */
static void transcript_focus_to_composer(void *user) {
    ChatHost *host = (ChatHost *)user;
    if (host->composer.window && IsWindow(host->composer.window))
        SetFocus(host->composer.window);
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
    if (window == host->composer.window || window == host->field.window ||
        window == host->search.window)
        return true;
    for (int s = 0; s < host->transcript.slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            if (host->transcript.slots[s].surface[k].window == window)
                return true;
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
    case WM_SETCURSOR:
        /* The whole transcript is a text-selection surface. Without this,
           the pointer over a gap or margin is an arrow while a turn control
           is an I-beam, and a streaming turn resizing under the pointer
           flips between them as its edges move. One stable cursor for the
           container and its read-only surfaces removes the flicker. */
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32513)));
            return TRUE;
        }
        break;
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
        case SB_THUMBTRACK:
            /* The drag owns the position: anchor capture/restore and follow
                transitions are suspended until the release event. */
            transcript_set_drag(&host->transcript, true);
            position = info.nTrackPos;
            break;
        case SB_THUMBPOSITION:
            transcript_set_drag(&host->transcript, false);
            position = info.nTrackPos;
            break;
        case SB_TOP: position = info.nMin; break;
        case SB_BOTTOM: position = info.nMax; break;
        case SB_ENDSCROLL:
            transcript_set_drag(&host->transcript, false);
            position = info.nPos;
            break;
        default: return 0;
        }
        int maximum = host->transcript.view_content - host->transcript.view_page;
        if (maximum < 0) maximum = 0;
        if (position < 0) position = 0;
        if (position > maximum) position = maximum;
        /* Every scrollbar event is reader input: the position is the truth,
            and only a release landing at the bottom re-enters follow. */
        transcript_note_user_scroll(&host->transcript, position);
        position_turns(host, false);
        return 0;
    }
    case WM_CANCELMODE:
        /* The transcript child can receive the cancellation of its own
            scrollbar drag: finish it through the same end-of-drag path the
            release event uses, so the final position is qualified and
            captured as the fresh anchor instead of leaving the drag
            suspended. */
        if (host->transcript.thumb_drag)
            end_scroll_drag(host, drag_position(window));
        return 0;
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
            for (int i = 0; i < host->transcript.record_count; i++) {
                RichTextControl *reason = transcript_surface(&host->transcript,
                    i, TRANSCRIPT_REASON);
                if (!reason || reason->window != child)
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
        transcript_note_user_scroll(&host->transcript, position);
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
    case WM_COMMAND: {
        /* Rich Edit focus notifications (EN_SETFOCUS/EN_KILLFOCUS, sent
            through WM_COMMAND without any special event mask): the reader's
            focused transcript surface is tracked here instead of being
            probed with GetFocus() at decision time, so hiding, invalidating,
            rebinding, switching and teardown can transfer focus deliberately
            before they act. */
        HWND source = (HWND)l;
        if (!source) break;
        RichTextControl *control = (RichTextControl *)GetWindowLongPtrW(
            source, GWLP_USERDATA);
        if (!control) break;
        switch (HIWORD(w)) {
        case EN_SETFOCUS:
            transcript_focus_notify(&host->transcript, control, true);
            return 0;
        case EN_KILLFOCUS:
            transcript_focus_notify(&host->transcript, control, false);
            return 0;
        default:
            break;
        }
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
            host->dpi, chat_active_model(host->config.chat))) return -1;
        if (!rich_text_create_field_limit(&host->search, window, 4,
            &host->rich_theme, host->dpi, CHAT_SEARCH_QUERY_TEXT - 1, L""))
            return -1;
        host->view = CreateWindowExW(0, L"DarkChat.Transcript", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, 10, 10, window, NULL, GetModuleHandleW(NULL), host);
        if (!host->view) return -1;
        if (!transcript_create(&host->transcript, host->view,
                &host->rich_theme, host->dpi)) return -1;
        /* Realization mode from the init-time config: retain-all unless the
            caller flips the bounded production activation. */
        transcript_set_bounded(&host->transcript,
            host->config.bounded_transcript);
        host->transcript.callbacks.surface_key = surface_key;
        host->transcript.callbacks.row_click = turn_row_click;
        host->transcript.callbacks.focus_release = transcript_focus_to_composer;
        host->transcript.callbacks.user = host;
        host->composer.on_submit = composer_submit;
        host->composer.on_key = surface_key;
        host->composer.user = host;
        host->field.on_submit = field_submit;
        host->field.on_key = surface_key;
        host->field.on_blur = field_blur;
        host->field.user = host;
        host->search.on_submit = search_submit;
        host->search.on_key = surface_key;
        host->search.user = host;
        completion_init(&host->client, window, CHAT_WM_COMPLETION_EVENT);
        model_catalog_client_init(&host->catalog_client, window,
            CHAT_WM_CATALOG_EVENT);
        for (int i = 0; i < CHAT_BACKEND_COUNT; i++)
            chat_model_catalog_init(&host->catalog[i]);
        host->backend_target = host->config.chat->backend;
        if (!ui_accessible_name(u, u->root)[0])
            ui_set_accessible_name(u, u->root, host->config.title);
        host->accessibility = ui_accessibility_create(window, u);
        if (!host->accessibility) return -1;
        /* No menu bar: the header's overflow button (and the retained
           keyboard shortcuts) carries every command. */
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
    case WM_INITMENUPOPUP: {
        /* Re-derive enabled state and the routing check/radio marks from live
           state each time a popup opens, so the menu can never disagree with
           what the host would actually do. A no-op for absent items. */
        ChatActionContext context;
        chat_action_context_init(&context,host->config.chat);
        context.generating = host->generating;
        context.editing = host->editing;
        context.has_transcript_selection =
            transcript_selected_surface(host) != NULL;
        chat_actions_sync((HMENU)w,&context);
        break;
    }
    case WM_COMMAND:
        if (!l) { action(host,LOWORD(w)); return 0; }
        /* Rich Edit focus notifications from the native fields (no event
           mask is required). They drive the retained focus ring on the
           matching placeholder surface. */
        if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) {
            bool focused = HIWORD(w) == EN_SETFOCUS;
            HWND source = (HWND)l;
            if (source == host->composer.window)
                chat_ui_set_focus_ring(&host->chat_ui,
                    host->chat_ui.composer_card, focused);
            else if (source == host->field.window)
                chat_ui_set_focus_ring(&host->chat_ui, host->chat_ui.model,
                    focused);
            else if (source == host->search.window)
                chat_ui_set_focus_ring(&host->chat_ui, host->chat_ui.search,
                    focused);
            flush(host);
            return 0;
        }
        break;

    case WM_TIMER:
        if (w == CHAT_TIMER_BODY_FLUSH) {
            /* The stream went quiet with text still dirty: render it now. */
            flush_stream_body(host);
            return 0;
        }
        if (w == 2) {
            /* If allocation/queueing failed for the terminal event, the
                worker can finish without notifying us. Take the queued
                batch before declaring this failure so a queued DONE always
                wins. */
            if (host->generating && host->client.thread &&
                WaitForSingleObject(host->client.thread,0)==WAIT_OBJECT_0) {
                handle_events(host);
                if (host->generating) {
                    CompletionEvent lost={0}; lost.generation=host->request_generation;
                    lost.type=COMPLETION_ERROR; lost.metadata=pending(host)->generation;
                    lost.metadata.finished_at=chat_now();
                    lost.metadata.latency_ms=(double)(GetTickCount64()-host->started_tick);
                    lost.text=L"Worker ended without a completion event.";
                    finish_request(host,&lost);
                }
            }
            capture_settings(host);
            { TranscriptFeed feed = transcript_feed(host);
              transcript_apply_pending(&host->transcript, &feed); }
            /* Bounded 1 Hz retry for an armed, still-missing streaming body
                or an unpainted reasoning pane: timer context, one attempt,
                no re-arming loop. */
            if (host->transcript.bounded &&
                (host->body_flush_pending || host->reason_paint_pending) &&
                host->generating)
                flush_stream_body(host);
            /* One-second autosave: hand a snapshot to the background writer.
               While a save failure is latched the sweep stays silent so the
               failure report remains visible, exactly like the synchronous
               save's failure return did. */
            save(host);
            if (!host->save_failed && host->generating) {
                wchar_t status[CHAT_STATUS_TEXT];
                if (host->context_dropped)
                    swprintf(status,CHAT_STATUS_TEXT,
                        L"%ls | %ls | %.1f s elapsed | %d older messages omitted",
                        host->stopping ? L"Stopping" : L"Generating",
                        pending(host)->generation.requested_model,
                        (GetTickCount64()-host->started_tick)/1000.0,
                        host->context_dropped);
                else
                    swprintf(status,CHAT_STATUS_TEXT,L"%ls | %ls | %.1f s elapsed",
                        host->stopping ? L"Stopping" : L"Generating",
                        pending(host)->generation.requested_model,
                        (GetTickCount64()-host->started_tick)/1000.0);
                status[CHAT_STATUS_TEXT-1]=0;   /* truncation must still terminate */
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
    case WM_ENTERSIZEMOVE:
        /* Interactive drag: defer off-screen re-measurement (the realize
           loop keeps last-known off-screen heights; visible records still
           live-measure each step). The settle render below restores exact
           geometry. Behavior-neutral in retain-all mode. */
        if (host->transcript.bounded) host->transcript.resizing = true;
        return 0;
    case WM_EXITSIZEMOVE:
        if (host->transcript.bounded) {
            host->transcript.resizing = false;
            render_transcript(host);
        }
        return 0;
    case WM_DPICHANGED: {
        host->dpi = (float)HIWORD(w);
        RECT *r = (RECT *)l;
        rich_text_set_dpi(&host->field, host->dpi);
        rich_text_set_dpi(&host->search, host->dpi);
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
        /* There is no menu bar (commands live on the overflow button). */
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
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
        /* A cancelled scrollbar drag still ends the drag: the release event
            may never arrive, so the drag is finished through the same
            end-of-drag path (qualification plus fresh-anchor capture), not
            merely unflagged. */
        if (host->transcript.thumb_drag)
            end_scroll_drag(host, drag_position(host->view));
        flush(host);
        return 0;
    case WM_MOUSEWHEEL: wheel(host, w, l); return 0;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        bool down = message == WM_KEYDOWN;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        /* One registry consulted here and by surface_key: every shortcut,
           the Escape/drawer dismissal, region navigation and the Tab
           hand-off between retained chrome and native fields. */
        if (down && host_shortcut(host, w, shift, control, false)) return 0;
        UiKey key;
        if (key_from_win32(w, &key)) {
            ui_key(u, key, down, shift, (l & (1L << 30)) != 0);
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
    case CHAT_WM_COMPLETION_EVENT:
        handle_events(host);
        return 0;
    case CHAT_WM_CATALOG_EVENT:
        catalog_event(host, (ModelCatalogEvent *)l);
        return 0;
    case CHAT_WM_ACTIONS_MENU:
        open_actions_menu(host);
        return 0;
    case CHAT_WM_SAVER_RESULT:
        /* wParam carries the result bit and this job's attempt id; lParam
           the mutation counter captured at that handoff. The completion is
           interpreted against its own attempt and counter, never against the
           latest handoff, and results at or below the newest
           already-interpreted attempt are stale and ignored. */
        saver_completed(host,(w&1)!=0,(uint64_t)w>>1,(uint64_t)l);
        return 0;
    case WM_CLOSE:
        /* A close that lands while a modal popup is live must not destroy
           the parent under the nested loop. Close the popup, park the close,
           and repost it once the popup call has unwound. */
        if (host->open_palette) {
            host->close_pending = true;
            palette_popup_cancel(host->open_palette);
            return 0;
        }
        capture_settings(host);
        cancel_body_flush(host);
        if (host->generating) {
            ChatGeneration *g=&pending(host)->generation;
            g->state=host->stopping ? host->stop_state : CHAT_GENERATION_INTERRUPTED;
            g->finished_at=chat_now();
            g->latency_ms=(double)(GetTickCount64()-host->started_tick);
            wcscpy(g->error,L"Window closed before generation finished.");
            chat_message_touch(pending(host));
            mark_dirty(host);
            save_sync(host);
            /* Stop this request without finalizing the reusable client. The
               save-failure prompt below can keep the window open; destroying
               the client's critical section here would leave that live
               window unable to start another request. Final shutdown belongs
               to chat_host_run's cleanup after the window really closes. */
            completion_cancel(&host->client,host->request_generation);
            completion_complete(&host->client,host->request_generation);
            host->generating = false;
            host->context_dropped = 0;
            /* Discard anything the joined worker queued. generating is false,
               so handle_event frees every node without applying it. A stale
               wake message can safely find the still-live queue empty. */
            handle_events(host);
        }
        /* Join and drain the catalog worker while the main window still
           exists, so a late completion is freed here instead of leaking into a
           queue whose window is gone. */
        model_catalog_shutdown(&host->catalog_client);
        {
            MSG queued;
            while (PeekMessageW(&queued, window, CHAT_WM_CATALOG_EVENT,
                CHAT_WM_CATALOG_EVENT, PM_REMOVE))
                model_catalog_event_free((ModelCatalogEvent *)queued.lParam);
        }
        /* Final flush-and-wait: the close prompt must describe real
           durability, not a handoff still in flight. */
        if (!save_sync(host) && MessageBoxW(window,L"Changes could not be saved. Close anyway and lose unsaved changes?",
            L"DarkChat",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) {
            chat_ui_set_generation(&host->chat_ui,false,false); EnableWindow(host->field.window,TRUE);
            render_transcript(host); return 0;
        }
        /* Teardown focus safety: while the whole hierarchy still exists,
            move keyboard focus to the top-level window. No focused child
            is destroyed, and the transcript's composer transfer target
            dies with the window it belongs to. */
        SetFocus(window);
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
        /* Never free the palette underneath its active nested pump: cancel it
            so the pump unwinds, and let its completion path destroy it. Only a
            palette with no live pump is safe to destroy here. The end_palette
            path (close repost, action dispatch) is skipped in WM_DESTROY, since
            the hierarchy is already collapsing. */
        if (host->open_palette) {
            if (host->palette_pumping) {
                palette_popup_cancel(host->open_palette);
            } else {
                palette_popup_destroy(host->open_palette);
                host->open_palette = NULL;
            }
        }
        chat_search_results_dispose(&host->search_results);
        renderer_drop_target(&host->renderer);
        /* Transfer focus off any transcript surface before the hierarchy
            collapses under it: the child teardown never destroys a focused
            window. */
        transcript_focus_release(&host->transcript);
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
    mark_dirty(host);
    if (host->storage.recovered) wcscpy(config->chat->status,L"Recovered the last valid snapshot from backup.");
    host->dpi = (float)GetDpiForSystem();
    if (!chat_ui_init(&host->chat_ui, config->ui, config->chat)) goto cleanup;
    host->chat_ui.command = command;
    host->chat_ui.command_user = host;
    /* The list has no extent before the first layout, so the storage load's
       active conversation is revealed by pending id on the first flush. */
    if (config->chat->active >= 0 &&
        config->chat->active < config->chat->conversation_count)
        chat_ui_request_reveal(&host->chat_ui,
            config->chat->conversations[config->chat->active].id);
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
        /* The writer alone calls the storage module after startup, so a
           failed saver_init fails startup cleanly rather than degrading to
           UI-thread storage access. */
        if (!saver_init(&host->saver,window,CHAT_WM_SAVER_RESULT,
                &host->storage)) {
            MessageBoxW(window,
                L"DarkChat could not start the background snapshot writer.",
                L"DarkChat",MB_OK|MB_ICONERROR);
            DestroyWindow(window);
            result=1;
        } else {
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
    }
    UnregisterClassW(cls.lpszClassName, instance);
    UnregisterClassW(view_cls.lpszClassName, instance);
cleanup:
    /* Never exit under a running worker: it still owns its request snapshot. */
    completion_shutdown(&host->client);
    /* Cancel and join the catalog fetch, then drain any completion it managed
       to post before the window went away, so no event body leaks. */
    model_catalog_shutdown(&host->catalog_client);
    {
        MSG queued;
        while (PeekMessageW(&queued, host->window, CHAT_WM_CATALOG_EVENT,
            CHAT_WM_CATALOG_EVENT, PM_REMOVE))
            model_catalog_event_free((ModelCatalogEvent *)queued.lParam);
    }
    if (host->open_palette) {
        palette_popup_destroy(host->open_palette);
        host->open_palette = NULL;
    }
    /* Join the snapshot writer before the storage lock is released: it alone
       uses the ChatStorage after startup, and pending jobs must be drained
       (or disposed) while the store is still valid. */
    saver_shutdown(&host->saver);
    ui_accessibility_destroy(host->accessibility);
    config->ui->measure = NULL;
    config->ui->measure_user = NULL;
    renderer_dispose(&host->renderer);
    if (host->background) DeleteObject(host->background);
    /* Lifetime order: the window hierarchy is fully destroyed by now (the
       message loop exits from WM_DESTROY, and a failed WM_CREATE reaches
       cleanup only after CreateWindowExW completed its teardown), so every
       child surface is gone and each surface's GWLP_USERDATA is no longer
       reachable. Only now may the slot pool be freed; never dispose from the
       parent window procedure, whose WM_DESTROY runs while children exist. */
    transcript_dispose(&host->transcript);
    for (int i = 0; i < CHAT_BACKEND_COUNT; i++)
        chat_model_catalog_dispose(&host->catalog[i]);
    rich_text_library_close();
    storage_close(&host->storage);
    free(host);
    CoUninitialize();
    if (result) MessageBoxW(NULL,
        L"DarkChat could not initialize. Check the graphics runtime and available resources.",
        L"DarkChat", MB_OK | MB_ICONERROR);
    return result;
}




