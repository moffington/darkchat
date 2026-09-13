#include "transcript_win32.h"
#include <richedit.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int px(const Transcript *t, float dips) {
    return (int)lroundf(dips * t->dpi / 96.0f);
}

/* ---- Terminal metadata footer ------------------------------------------- */

/* Writes a grouped integer (e.g. 1,365) into out. */
static void group_integer(wchar_t *out, size_t cap, double value) {
    wchar_t digits[32];
    swprintf(digits, 32, L"%.0f", value);
    size_t length = wcslen(digits);
    size_t commas = length > 1 ? (length - 1) / 3 : 0;
    size_t needed = length + commas;
    if (needed + 1 > cap) {
        wcsncpy(out, digits, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    out[needed] = 0;
    size_t write = needed;
    int count = 0;
    for (size_t read = length; read > 0; ) {
        out[--write] = digits[--read];
        if (++count == 3 && read > 0) { count = 0; out[--write] = L','; }
    }
}

/* Appends a " · " separated segment, skipping empty ones. */
static void stats_append(wchar_t *out, size_t cap, const wchar_t *part) {
    if (!part || !part[0]) return;
    size_t used = wcslen(out);
    if (used) {
        if (used + 3 >= cap) return;
        wmemcpy(out + used, L" \u00b7 ", 3);
        used += 3;
        out[used] = 0;
    }
    size_t room = cap - 1 - used;
    size_t length = wcslen(part);
    if (length > room) length = room;
    wmemcpy(out + used, part, length);
    out[used + length] = 0;
}

/* Compact two-line footer: "Complete · TTFT 18.0s · 19.4s · 44 in / 1,365 out
   · $0.00165" followed by the model, deduplicated when requested == actual.
   Normal "stop" is omitted; unusual finish reasons are surfaced. */
static void format_stats(const ChatGeneration *g, wchar_t *info, size_t cap) {
    info[0] = 0;
    wchar_t piece[192];
    stats_append(info, cap, chat_generation_name(g->state));
    if (g->ttft_ms >= 0) {
        swprintf(piece, 192, L"TTFT %.1fs", g->ttft_ms / 1000.0);
        stats_append(info, cap, piece);
    }
    if (g->latency_ms >= 0) {
        swprintf(piece, 192, L"%.1fs", g->latency_ms / 1000.0);
        stats_append(info, cap, piece);
    }
    if (g->total_tokens >= 0 && g->prompt_tokens >= 0 &&
        g->completion_tokens >= 0) {
        wchar_t in[32], out[32];
        group_integer(in, 32, g->prompt_tokens);
        group_integer(out, 32, g->completion_tokens);
        swprintf(piece, 192, L"%ls in / %ls out", in, out);
        stats_append(info, cap, piece);
    }
    if (g->cost >= 0) {
        swprintf(piece, 192, L"$%.5f", g->cost);
        stats_append(info, cap, piece);
    }
    if (g->finish_reason[0] && wcscmp(g->finish_reason, L"stop") != 0) {
        swprintf(piece, 192, L"finish: %ls", g->finish_reason);
        stats_append(info, cap, piece);
    }
    const wchar_t *requested = g->requested_model[0] ? g->requested_model : NULL;
    const wchar_t *actual = g->actual_model[0] ? g->actual_model : NULL;
    wchar_t model[224];
    model[0] = 0;
    if (requested && actual) {
        if (!wcscmp(requested, actual)) wcsncpy(model, actual, 223);
        else swprintf(model, 224, L"%ls \u2192 %ls", requested, actual);
    } else if (actual) {
        wcsncpy(model, actual, 223);
    } else if (requested) {
        wcsncpy(model, requested, 223);
    }
    model[223] = 0;
    if (model[0]) {
        size_t used = wcslen(info);
        if (used + 2 < cap) {
            info[used++] = L'\n';
            info[used] = 0;
            size_t room = cap - 1 - used;
            size_t length = wcslen(model);
            if (length > room) length = room;
            wmemcpy(info + used, model, length);
            info[used + length] = 0;
        }
    }
}

/* ---- Turn content -------------------------------------------------------- */

/* Head-row text for one turn's reasoning affordance. */
static void reasoning_row_text(const ChatMessage *m, bool running,
    bool content_started, wchar_t *out, size_t capacity) {
    bool pending = running && !content_started;
    if (pending) {
        wcsncpy(out, L"Thinking\u2026 \u2304", capacity - 1);
        out[capacity - 1] = 0;
    } else {
        double ms = m->generation.reasoning_ms;
        if (ms < 0) ms = 0;
        swprintf(out, capacity, L"\u2304 Thought for %.1fs", ms / 1000.0);
    }
}

static bool row_for(const TranscriptFeed *feed, const ChatMessage *m,
    bool assistant, bool running, wchar_t *row, size_t cap) {
    row[0] = 0;
    if (!assistant) return false;
    bool pending_row = running && !feed->content_started;
    bool has = pending_row || chat_message_reasoning(m)[0];
    if (has) reasoning_row_text(m, running, feed->content_started, row, cap);
    return has;
}

/* Builds one control from its message. Doing this per turn is what makes each
   turn own its reasoning affordance and viewport: no control is shared, and
   none follows a "latest" message. */
static void ensure_control(Transcript *t, RichTextControl *control, int id,
    bool viewport) {
    if (control->window || !t->view) return;
    if (viewport)
        rich_text_create_viewport(control, t->view, id, &t->theme, t->dpi);
    else
        rich_text_create_block(control, t->view, id, &t->theme, t->dpi);
    if (!control->window) return;
    control->on_key = t->callbacks.surface_key;
    control->on_line_click = t->callbacks.row_click;
    control->user = t->callbacks.user;
}

static void hide_turn(Transcript *t, int index) {
    TranscriptTurn *turn = &t->turns[index];
    if (turn->head.window) ShowWindow(turn->head.window, SW_HIDE);
    if (turn->body.window) ShowWindow(turn->body.window, SW_HIDE);
    if (turn->reasoning.window) ShowWindow(turn->reasoning.window, SW_HIDE);
    if (turn->meta.window) ShowWindow(turn->meta.window, SW_HIDE);
    turn->head_live = turn->body_live = turn->reason_live = turn->meta_live = false;
}

/* Required client height of a block at the given width. The control sends
   EN_REQUESTRESIZE to its parent (the transcript container), which records the
   value in t->measured while t->measuring points at it. */
static int measure_control(Transcript *t, RichTextControl *control,
    int width) {
    if (!control->window || width <= 0) return 0;
    RECT bounds;
    GetWindowRect(control->window, &bounds);
    /* Keep the current height while measuring. Shrinking a live block to a
       probe height on every token scrolls/clips its text before layout. */
    if (bounds.right - bounds.left != width)
        SetWindowPos(control->window, NULL, 0, 0, width,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
    t->measuring = control;
    t->measured = 0;
    SendMessageW(control->window, EM_REQUESTRESIZE, 0, 0);
    t->measuring = NULL;
    int height = t->measured;
    if (height <= 0) {
        int lines = (int)SendMessageW(control->window, EM_GETLINECOUNT, 0, 0);
        if (lines < 1) lines = 1;
        height = lines * px(t, t->theme.ui_size * 1.45f);
    }
    int minimum = px(t, 8);
    return height < minimum ? minimum : height;
}

bool transcript_pinned(const Transcript *t) {
    int maximum = t->view_content - t->view_page;
    if (maximum < 0) maximum = 0;
    return t->view_scroll >= maximum - 1;
}

static void place_turn_control(Transcript *t, RichTextControl *control,
    bool live, int y, int height, int scroll, int page, int inset) {
    if (!control->window) return;
    if (!live) { ShowWindow(control->window, SW_HIDE); return; }
    int top = y - scroll;
    if (top >= page || top + height <= 0) {
        ShowWindow(control->window, SW_HIDE);
        return;
    }
    SetWindowPos(control->window, NULL, t->view_margin + inset, top,
        t->view_width - 2 * inset, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(control->window, SW_SHOWNOACTIVATE);
}

/* Re-clamps the scroll and moves every turn control to its scrolled position.
   Only turns that intersect the viewport are shown, so a turn above or below
   the visible transcript is clipped away rather than drawn over other UI. */
void transcript_position(Transcript *t, bool follow) {
    if (!t->view) return;
    RECT client;
    GetClientRect(t->view, &client);
    int page = client.bottom;
    int maximum = t->view_content - page;
    if (maximum < 0) maximum = 0;
    if (follow) t->view_scroll = maximum;
    else if (t->view_scroll > maximum) t->view_scroll = maximum;
    else if (t->view_scroll < 0) t->view_scroll = 0;
    t->view_page = page;
    SCROLLINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    info.nMin = 0;
    info.nMax = t->view_content > 0 ? t->view_content - 1 : 0;
    info.nPage = (UINT)page;
    info.nPos = t->view_scroll;
    SetScrollInfo(t->view, SB_VERT, &info, TRUE);
    for (int i = 0; i < t->turn_count; i++) {
        TranscriptTurn *turn = &t->turns[i];
        place_turn_control(t, &turn->head, turn->head_live, turn->head_y,
            turn->head_h, t->view_scroll, page, 0);
        place_turn_control(t, &turn->reasoning, turn->reason_live,
            turn->reason_y, turn->reason_h, t->view_scroll, page,
            t->view_reason_inset);
        place_turn_control(t, &turn->body, turn->body_live, turn->body_y,
            turn->body_h, t->view_scroll, page, 0);
        place_turn_control(t, &turn->meta, turn->meta_live, turn->meta_y,
            turn->meta_h, t->view_scroll, page, 0);
    }
    /* Moving children does not repaint the background they uncover; the
       container paints every gap and margin itself. */
    InvalidateRect(t->view, NULL, FALSE);
}

/* Measures and stacks turns from start onward, then repositions them. */
void transcript_layout_from(Transcript *t, int start, bool follow) {
    if (!t->view) return;
    if (start < 0 || start >= t->turn_count) start = 0;
    int y = start == 0 ? t->view_margin : t->turns[start].y;
    for (int i = start; i < t->turn_count; i++) {
        TranscriptTurn *turn = &t->turns[i];
        turn->y = y;
        int cursor = y;
        if (turn->head_live) {
            turn->head_h = measure_control(t, &turn->head, t->view_width);
            turn->head_y = cursor;
            cursor += turn->head_h;
        }
        if (turn->reason_live) {
            cursor += t->view_reason_gap;
            turn->reason_h = px(t, 150);
            turn->reason_y = cursor;
            cursor += turn->reason_h + t->view_reason_gap;
        }
        if (turn->body_live) {
            turn->body_h = measure_control(t, &turn->body, t->view_width);
            turn->body_y = cursor;
            cursor += turn->body_h;
        }
        if (turn->meta_live) {
            cursor += t->view_meta_gap;
            turn->meta_h = measure_control(t, &turn->meta, t->view_width);
            turn->meta_y = cursor;
            cursor += turn->meta_h;
        }
        turn->height = cursor - y;
        y = cursor + t->view_gap;
    }
    t->view_content = y;
    transcript_position(t, follow);
}

/* ---- Revision-tracked updates -------------------------------------------- */

/* True when the turn's surfaces were already built from exactly this message
   state. Identity includes the conversation and message instance ids plus the
   revision, so a replaced slot or an edited message never matches. */
static bool rendered_current(const TranscriptTurn *turn, uint64_t conversation,
    uint64_t message, uint64_t revision, ChatRole role,
    ChatGenerationState state, bool running, bool content_started,
    bool reasoning_open, bool has_row, const wchar_t *row) {
    if (!turn->rendered_valid) return false;
    if (turn->conversation != conversation || turn->message != message ||
        turn->revision != revision || turn->role != role ||
        turn->state != state || turn->running != running ||
        turn->content_started != content_started ||
        turn->reasoning_open != reasoning_open) return false;
    return has_row ? !wcscmp(turn->row, row) : !turn->row[0];
}

/* Writes one surface, deferring the destructive write while it holds a
   selection. Returns true only when a write was actually applied; a deferred
   write changes nothing and leaves geometry untouched until it is applied.
   Programmatic selection changes fired inside the write are suppressed by the
   guard so a deferred application cannot recurse. */
static bool write_head(Transcript *t, TranscriptTurn *turn, ChatRole role,
    const wchar_t *row, bool has_row) {
    if (!turn->head.window) { turn->head_pending = false; return false; }
    if (rich_text_has_selection(&turn->head)) { turn->head_pending = true; return false; }
    t->applying = true;
    rich_text_set_head(&turn->head, role, has_row ? row : NULL);
    t->applying = false;
    turn->head_pending = false;
    return true;
}

static bool write_body(Transcript *t, TranscriptTurn *turn, ChatRole role,
    const wchar_t *text) {
    if (!turn->body.window) { turn->body_pending = false; return false; }
    if (rich_text_has_selection(&turn->body)) { turn->body_pending = true; return false; }
    t->applying = true;
    if (role == CHAT_ROLE_ASSISTANT)
        rich_text_set_markdown(&turn->body, role, text);
    else
        rich_text_set_block(&turn->body, role, text);
    t->applying = false;
    turn->body_pending = false;
    return true;
}

static bool write_meta(Transcript *t, TranscriptTurn *turn,
    const ChatGeneration *g) {
    if (!turn->meta.window) { turn->meta_pending = false; return false; }
    if (rich_text_has_selection(&turn->meta)) { turn->meta_pending = true; return false; }
    wchar_t info[768];
    format_stats(g, info, 768);
    t->applying = true;
    rich_text_set_meta(&turn->meta, info, g->error);
    t->applying = false;
    turn->meta_pending = false;
    return true;
}

static bool write_reasoning(Transcript *t, TranscriptTurn *turn,
    const wchar_t *text) {
    if (!turn->reasoning.window) { turn->reason_pending = false; return false; }
    if (rich_text_has_selection(&turn->reasoning)) { turn->reason_pending = true; return false; }
    t->applying = true;
    rich_text_set_reasoning(&turn->reasoning, text);
    t->applying = false;
    turn->reason_pending = false;
    return true;
}

/* Applies deferred writes for one turn whose surfaces no longer hold a
   selection. Only surfaces with a pending debt are touched; each write
   re-checks its own selection and re-defers if the reader is still selecting.
   Returns true when any write was applied, which changes content geometry and
   requires a relayout from this turn. */
static bool catch_up(Transcript *t, const TranscriptFeed *feed, int index) {
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count) return false;
    const ChatConversation *c = &chat->conversations[chat->active];
    if (index < 0 || index >= c->message_count) return false;
    const ChatMessage *m = &c->messages[index];
    TranscriptTurn *turn = &t->turns[index];
    bool assistant = m->role == CHAT_ROLE_ASSISTANT;
    bool running = feed->generating &&
        feed->request_conversation == chat->active &&
        index == feed->request_message;
    wchar_t row[48];
    row[0] = 0;
    bool has_row = row_for(feed, m, assistant, running, row, 48);
    bool changed = false;
    if (turn->head_pending) {
        if (write_head(t, turn, m->role, row, has_row)) changed = true;
    }
    if (turn->body_pending) {
        if (write_body(t, turn, m->role, chat_message_text(m))) changed = true;
    }
    bool terminal = m->generation.state != CHAT_GENERATION_NONE &&
        m->generation.state != CHAT_GENERATION_RUNNING;
    if (turn->meta_pending) {
        if (terminal) {
            if (write_meta(t, turn, &m->generation)) changed = true;
        } else {
            if (turn->meta.window) ShowWindow(turn->meta.window, SW_HIDE);
            turn->meta_pending = false;
            turn->meta_live = false;
            changed = true;
        }
    }
    if (turn->reason_pending) {
        if (write_reasoning(t, turn, chat_message_reasoning(m))) changed = true;
    }
    return changed;
}

/* Synchronizes one turn's surfaces with its message. A message whose identity
   still matches what was rendered skips every destructive write; control
   realization, callback wiring and visibility reconciliation always run. */
static void prepare_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    const Chat *chat = feed->chat;
    const ChatConversation *c = &chat->conversations[chat->active];
    const ChatMessage *m = &c->messages[index];
    TranscriptTurn *turn = &t->turns[index];
    bool assistant = m->role == CHAT_ROLE_ASSISTANT;
    bool running = feed->generating &&
        feed->request_conversation == chat->active &&
        index == feed->request_message;
    bool terminal = m->generation.state != CHAT_GENERATION_NONE &&
        m->generation.state != CHAT_GENERATION_RUNNING;
    wchar_t row[48];
    row[0] = 0;
    bool has_row = row_for(feed, m, assistant, running, row, 48);
    bool fresh = rendered_current(turn, c->id, m->id, m->revision, m->role,
        m->generation.state, running, feed->content_started, m->reasoning_open,
        has_row, row);
    /* Does the surface still represent this exact message instance? A
       conversation switch invalidates every slot and a reused slot may hold a
       different message, so streaming may skip a destructive rebuild only
       while this identity is unchanged. Captured before the identity update
       below overwrites it. */
    bool same_message = turn->rendered_valid &&
        turn->conversation == c->id && turn->message == m->id;
    if (!fresh) {
        turn->conversation = c->id;
        turn->message = m->id;
        turn->revision = m->revision;
        turn->role = m->role;
        turn->state = m->generation.state;
        turn->running = running;
        turn->content_started = feed->content_started;
        turn->reasoning_open = m->reasoning_open;
        wcsncpy(turn->row, row, sizeof turn->row / sizeof *turn->row - 1);
        turn->row[sizeof turn->row / sizeof *turn->row - 1] = 0;
        turn->rendered_valid = true;
    }

    if (assistant) {
        bool created = turn->head.window == NULL;
        ensure_control(t, &turn->head, 100 + index * 4, false);
        if (turn->head.window) turn->head_live = true;
        if (!fresh || turn->head_pending || created)
            write_head(t, turn, m->role, row, has_row);
    } else {
        if (turn->head.window) ShowWindow(turn->head.window, SW_HIDE);
        turn->head_live = false;
        turn->head_pending = false;
    }
    ensure_control(t, &turn->body, 100 + index * 4 + 1, false);
    if (turn->body.window) turn->body_live = true;
    if (!fresh || turn->body_pending)
        write_body(t, turn, m->role, chat_message_text(m));
    if (assistant && terminal) {
        bool created = turn->meta.window == NULL;
        ensure_control(t, &turn->meta, 100 + index * 4 + 3, false);
        if (!fresh || turn->meta_pending || created)
            write_meta(t, turn, &m->generation);
    }

    /* Metadata is a terminal-state footer: visibility is reconciled on every
       pass, whether or not the content write was skipped. */
    if (assistant && terminal) {
        turn->meta_live = turn->meta.window != NULL;
    } else {
        if (turn->meta.window) ShowWindow(turn->meta.window, SW_HIDE);
        turn->meta_live = false;
        if (!assistant) turn->meta_pending = false;
    }

    bool open = assistant && has_row && m->reasoning_open;
    if (open) {
        bool created = turn->reasoning.window == NULL;
        ensure_control(t, &turn->reasoning, 100 + index * 4 + 2, true);
        if (turn->reasoning.window) {
            /* Reopened on this pass: the viewport kept its window while
               collapsed and so missed the appends that arrived in the
               meantime. It must reload the accumulated reasoning before the
               stream resumes appending into it. */
            bool resumed = !turn->reason_live;
            turn->reason_live = true;
            /* A live stream is appended to, never rebuilt, so its viewport
               keeps its own scroll position. A freshly created viewport always
               loads the reasoning accumulated so far. */
            bool streaming = running && feed->reasoning_streaming;
            if (created || !streaming || resumed || !same_message) {
                if (!fresh || turn->reason_pending || created || resumed ||
                    !same_message)
                    write_reasoning(t, turn, chat_message_reasoning(m));
            }
        }
    } else {
        if (turn->reasoning.window) ShowWindow(turn->reasoning.window, SW_HIDE);
        turn->reason_live = false;
    }
}

void transcript_render(Transcript *t, const TranscriptFeed *feed) {
    if (!t->view) return;
    const Chat *chat = feed->chat;
    RECT client;
    GetClientRect(t->view, &client);
    t->view_margin = px(t, 12);
    t->view_gap = px(t, 10);
    t->view_reason_gap = px(t, 8);
    t->view_meta_gap = px(t, 8);
    t->view_reason_inset = px(t, 10);
    t->view_width = client.right - 2 * t->view_margin;
    int minimum = px(t, 40);
    if (t->view_width < minimum) t->view_width = minimum;
    bool pinned = transcript_pinned(t);
    const ChatConversation *c = chat_active(chat);
    int count = c ? c->message_count : 0;
    t->turn_count = count;
    for (int i = 0; i < count; i++) prepare_turn(t, feed, i);
    for (int i = count; i < CHAT_MAX_MESSAGES; i++) hide_turn(t, i);
    transcript_layout_from(t, 0, pinned);
}

/* Rebuilds one turn only, so toggling a row never disturbs another turn's
   viewport or the transcript scroll. */
void transcript_refresh_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count) return;
    if (index < 0 || index >= chat->conversations[chat->active].message_count)
        return;
    bool pinned = transcript_pinned(t);
    prepare_turn(t, feed, index);
    transcript_layout_from(t, index, pinned);
}

void transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index) {
    if (index < 0 || index >= CHAT_MAX_MESSAGES) return;
    TranscriptTurn *turn = &t->turns[index];
    if (!turn->body.window) return;
    const ChatConversation *c =
        &feed->chat->conversations[feed->request_conversation];
    const ChatMessage *m = &c->messages[index];
    if (rich_text_has_selection(&turn->body)) {
        /* The reader holds a selection in the live answer: the destructive
           Markdown rebuild is deferred until the selection clears. */
        turn->body_pending = true;
        return;
    }
    bool pinned = transcript_pinned(t);
    t->applying = true;
    rich_text_set_markdown(&turn->body, m->role, chat_message_text(m));
    t->applying = false;
    turn->body_pending = false;
    transcript_layout_from(t, index, pinned);
}

/* Explicit invalidation: on conversation change or slot reuse the slot's
   pending updates and selections are dropped and the next render replaces
   content without deferral. */
static void reset_slot(Transcript *t, int index) {
    TranscriptTurn *turn = &t->turns[index];
    RichTextControl *controls[4] = { &turn->head, &turn->body,
        &turn->reasoning, &turn->meta };
    for (int k = 0; k < 4; k++) {
        if (!controls[k]->window) continue;
        CHARRANGE none = { 0, 0 };
        SendMessageW(controls[k]->window, EM_EXSETSEL, 0, (LPARAM)&none);
    }
    turn->rendered_valid = false;
    turn->head_pending = turn->body_pending = false;
    turn->meta_pending = turn->reason_pending = false;
}

void transcript_invalidate(Transcript *t) {
    bool guard = t->applying;
    t->applying = true;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) reset_slot(t, i);
    t->applying = guard;
}

void transcript_invalidate_from(Transcript *t, int from) {
    if (from < 0) from = 0;
    if (from >= CHAT_MAX_MESSAGES) return;
    bool guard = t->applying;
    t->applying = true;
    for (int i = from; i < CHAT_MAX_MESSAGES; i++) reset_slot(t, i);
    t->applying = guard;
}

void transcript_selection_changed(Transcript *t, const TranscriptFeed *feed,
    RichTextControl *control) {
    /* Programmatic writes fire EN_SELCHANGE too; only reader actions may
       trigger deferred applications. */
    if (t->applying) return;
    for (int i = 0; i < t->turn_count; i++) {
        TranscriptTurn *turn = &t->turns[i];
        if (control != &turn->head && control != &turn->body &&
            control != &turn->reasoning && control != &turn->meta) continue;
        if (!rich_text_has_selection(control)) {
            /* Applied deferred content changes geometry: capture the reader's
               pin state first, then relayout from the affected turn so
               following positions, view_content and the scrollbar range follow
               the new content, keeping bottom-following when pinned. */
            bool pinned = transcript_pinned(t);
            if (catch_up(t, feed, i)) transcript_layout_from(t, i, pinned);
        }
        return;
    }
}

void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed) {
    if (t->applying) return;
    bool pinned = transcript_pinned(t);
    int first = -1;
    for (int i = 0; i < t->turn_count; i++) {
        TranscriptTurn *turn = &t->turns[i];
        if (turn->head_pending || turn->body_pending || turn->meta_pending ||
            turn->reason_pending) {
            if (catch_up(t, feed, i) && first < 0) first = i;
        }
    }
    /* One relayout from the earliest changed turn covers every applied write. */
    if (first >= 0) transcript_layout_from(t, first, pinned);
}

void transcript_create(Transcript *t, HWND view, const RichTextTheme *theme,
    float dpi) {
    memset(t, 0, sizeof *t);
    t->view = view;
    t->dpi = dpi;
    t->theme = *theme;
}

void transcript_set_dpi(Transcript *t, float dpi) {
    t->dpi = dpi;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        TranscriptTurn *turn = &t->turns[i];
        if (turn->head.window) rich_text_set_dpi(&turn->head, dpi);
        if (turn->body.window) rich_text_set_dpi(&turn->body, dpi);
        if (turn->reasoning.window) rich_text_set_dpi(&turn->reasoning, dpi);
        if (turn->meta.window) rich_text_set_dpi(&turn->meta, dpi);
    }
}

void transcript_measure_notify(Transcript *t, RichTextControl *control,
    const RECT *required) {
    if (control && control == t->measuring)
        t->measured = required->bottom - required->top;
}






