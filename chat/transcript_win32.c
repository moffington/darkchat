#include "transcript_win32.h"
#include "transcript_policy.h"
#include <assert.h>
#include <richedit.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int px(const Transcript *t, float dips) {
    return (int)lroundf(dips * t->dpi / 96.0f);
}

/* ---- Record/slot plumbing ------------------------------------------------ */

RichTextControl *transcript_surface(Transcript *t, int index,
    TranscriptSurface surface) {
    if (!t || index < 0 || index >= CHAT_MAX_MESSAGES) return NULL;
    if ((int)surface < 0 || surface >= TRANSCRIPT_SURFACE_COUNT) return NULL;
    TranscriptRecord *rec = &t->records[index];
    if (rec->slot < 0 || rec->slot >= t->slot_capacity) return NULL;
    RichTextControl *control = &t->slots[rec->slot].surface[surface];
    return control->window ? control : NULL;
}

/* Decision-time debt: pending deferred writes OR a live selection in any
   bound surface. Every TranscriptPolicyItem built for a production decision
   (the capacity checkpoint and the dormant victim path) takes debt from
   here, so an unfocused turn with a live selection is class-protected and
   can never be chosen for eviction. */
bool transcript_record_debt(Transcript *t, int index) {
    if (!t || index < 0 || index >= CHAT_MAX_MESSAGES) return false;
    TranscriptRecord *rec = &t->records[index];
    if (rec->head_pending || rec->body_pending || rec->meta_pending ||
        rec->reason_pending) return true;
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
        RichTextControl *control = transcript_surface(t, index,
            (TranscriptSurface)s);
        if (control && rich_text_has_selection(control)) return true;
    }
    return false;
}

/* True when the record's cached identity certifies the content currently in
   its bound slot: the association must be the exact one the identity was
   stamped from -- same slot index AND same binding generation, because a
   slot number can be reused by another record after an unbind. */
static bool record_certified(const Transcript *t, const TranscriptRecord *rec) {
    if (rec->slot < 0 || rec->slot >= t->slot_capacity) return false;
    return rec->slot == rec->rendered_slot &&
        rec->rendered_generation == t->slots[rec->slot].generation;
}

/* Clears the reader selection on every surface of one slot, regardless of
   which record certified them. Used when a record acquires surfaces whose
   content it cannot certify: the selection belonging to the acquired
   surfaces is dropped before the replacement writes. */
static void clear_slot_selection(Transcript *t, int slot) {
    if (slot < 0 || slot >= t->slot_capacity) return;
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++) {
        RichTextControl *control = &t->slots[slot].surface[k];
        if (!control->window) continue;
        CHARRANGE none = { 0, 0 };
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&none);
    }
}

/* Hides one surface if it is realized. */
static void hide_surface(Transcript *t, int index, TranscriptSurface surface) {
    RichTextControl *control = transcript_surface(t, index, surface);
    if (control) ShowWindow(control->window, SW_HIDE);
}

/* Hides every surface of one record and clears its live flags; the slot
   binding is retained. */
static void hide_turn(Transcript *t, int index) {
    TranscriptRecord *rec = &t->records[index];
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++)
        hide_surface(t, index, (TranscriptSurface)s);
    rec->head_live = rec->body_live = rec->reason_live = rec->meta_live = false;
}

/* Detaches a slot from its record: hides every surface and clears the
   association on both sides. Debt and rendered identity stay on the record.
   Rebinding always creates a NEW association with a fresh binding
   generation, so certification against the retained identity fails for any
   newly acquired surfaces -- same-message debt is preserved and applied by
   the forced rewrite, while replacement-class debt is dropped. The policy
   must never select a must-keep record; this is the dormant unbind path a
   capacity-governed pass activates. */
static void unbind_slot(Transcript *t, int slot) {
    TranscriptSlot *s = &t->slots[slot];
    if (s->record < 0) return;
    TranscriptRecord *rec = &t->records[s->record];
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
        if (s->surface[k].window) ShowWindow(s->surface[k].window, SW_HIDE);
    rec->head_live = rec->body_live = rec->reason_live = rec->meta_live = false;
    rec->slot = -1;
    s->record = -1;
}

/* Returns the record's bound slot, binding one if needed. Retain-all: the
   pool holds CHAT_MAX_MESSAGES slots and records never exceed that, so a
   free slot always exists and the policy victim path is unreachable this
   pass. Returns -1 (record stays unrealized) only if binding fails closed. */
static int ensure_slot(Transcript *t, const TranscriptFeed *feed, int index) {
    TranscriptRecord *rec = &t->records[index];
    if (rec->slot >= 0) return rec->slot;
    int slot = -1;
    for (int s = 0; s < t->slot_capacity; s++) {
        if (t->slots[s].record < 0) { slot = s; break; }
    }
    if (slot < 0) {
        /* Dormant policy path: no free slot. Under retain-all this is
           unreachable (slot_capacity == CHAT_MAX_MESSAGES >= record count).
           The policy refuses to select a must-keep slot; a -1 result fails
           closed and the record stays unrealized -- a capacity-governed pass
           must answer it by raising capacity, never by evicting a must-keep
           record. The slot position is the array index; item.index is the
           represented record. */
        TranscriptPolicyItem bound[CHAT_MAX_MESSAGES];
        HWND focus = GetFocus();
        int h_min = px(t, 8);
        for (int s = 0; s < t->slot_capacity; s++) {
            TranscriptRecord *other = &t->records[t->slots[s].record];
            bound[s].index = t->slots[s].record;
            bound[s].y = other->y;
            bound[s].height = other->height;
            bound[s].streaming = feed->generating &&
                feed->request_conversation == feed->chat->active &&
                t->slots[s].record == feed->request_message;
            bound[s].debt = transcript_record_debt(t, t->slots[s].record);
            bound[s].expanded = other->reason_live;
            bound[s].focused = false;
            bound[s].last_used = other->last_used;
            if (focus)
                for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT &&
                    !bound[s].focused; k++)
                    bound[s].focused = other->slot >= 0 &&
                        s == other->slot &&
                        t->slots[s].surface[k].window == focus;
        }
        slot = transcript_policy_pick_victim(bound, t->slot_capacity,
            t->view_scroll, t->view_page, h_min);
        if (slot < 0) return -1;
        unbind_slot(t, slot);
    }
    t->slots[slot].record = index;
    /* Every new association receives a fresh, nonzero binding generation:
       slot-number reuse by another record can never be mistaken for the
       same surfaces. */
    t->slots[slot].generation = ++t->clock;
    rec->slot = slot;
    rec->last_used = t->slots[slot].generation;
    return slot;
}

/* Returns the record's bound surface, creating it on first use. Creation is
   per surface, exactly as before: an independent failure leaves the window
   NULL and is retried on the next prepare. The control id is slot-based
   (100 + slot*4 + surface); with this pass's permanent binding slot equals
   record index, so emitted ids are numerically unchanged. */
static RichTextControl *ensure_surface(Transcript *t,
    const TranscriptFeed *feed, int index, TranscriptSurface surface,
    bool viewport) {
    if (!t->view) return NULL;
    int slot = ensure_slot(t, feed, index);
    if (slot < 0) return NULL;
    RichTextControl *control = &t->slots[slot].surface[surface];
    if (!control->window) {
        int id = 100 + slot * 4 + (int)surface;
        if (viewport)
            rich_text_create_viewport(control, t->view, id, &t->theme, t->dpi);
        else
            rich_text_create_block(control, t->view, id, &t->theme, t->dpi);
        if (!control->window) return NULL;
        control->on_key = t->callbacks.surface_key;
        control->on_line_click = t->callbacks.row_click;
        control->user = t->callbacks.user;
    }
    return control;
}

/* Clears one record's reader selection, deferred debt and cached rendered
   identity; the slot binding and the surfaces' content stay. Declared here
   because the replacement-class rules in catch_up and prepare_turn run it
   before their replacement writes. */
static void reset_slot(Transcript *t, int index);

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

/* Required client height of a block at the given width. The control sends
    EN_REQUESTRESIZE to its parent (the transcript container), which records
    the value in t->measured while t->measuring points at it. */
static int measure_control(Transcript *t, RichTextControl *control,
    int width) {
    if (!control || !control->window || width <= 0) return 0;
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
    if (!control) return;
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
    for (int i = 0; i < t->record_count; i++) {
        TranscriptRecord *rec = &t->records[i];
        place_turn_control(t, transcript_surface(t, i, TRANSCRIPT_HEAD),
            rec->head_live, rec->head_y, rec->head_h, t->view_scroll, page, 0);
        place_turn_control(t, transcript_surface(t, i, TRANSCRIPT_REASON),
            rec->reason_live, rec->reason_y, rec->reason_h, t->view_scroll,
            page, t->view_reason_inset);
        place_turn_control(t, transcript_surface(t, i, TRANSCRIPT_BODY),
            rec->body_live, rec->body_y, rec->body_h, t->view_scroll, page, 0);
        place_turn_control(t, transcript_surface(t, i, TRANSCRIPT_META),
            rec->meta_live, rec->meta_y, rec->meta_h, t->view_scroll, page, 0);
    }
    /* Moving children does not repaint the background they uncover; the
       container paints every gap and margin itself. */
    InvalidateRect(t->view, NULL, FALSE);
}

bool transcript_reveal_turn(Transcript *t, int index) {
    if (!t || !t->view || index < 0 || index >= t->record_count) return false;
    TranscriptRecord *rec = &t->records[index];
    int top = rec->y;
    int bottom = rec->y + rec->height;
    if (top < t->view_scroll) {
        t->view_scroll = top;
    } else if (bottom > t->view_scroll + t->view_page) {
        t->view_scroll = rec->height > t->view_page
            ? top : bottom - t->view_page;
    }
    transcript_position(t, false);
    return true;
}

/* Measures and stacks turns from start onward, then repositions them. */
void transcript_layout_from(Transcript *t, int start, bool follow) {
    if (!t->view) return;
    if (start < 0 || start >= t->record_count) start = 0;
    int y = start == 0 ? t->view_margin : t->records[start].y;
    for (int i = start; i < t->record_count; i++) {
        TranscriptRecord *rec = &t->records[i];
        rec->y = y;
        int cursor = y;
        if (rec->head_live) {
            rec->head_h = measure_control(t,
                transcript_surface(t, i, TRANSCRIPT_HEAD), t->view_width);
            rec->head_y = cursor;
            cursor += rec->head_h;
        }
        if (rec->reason_live) {
            cursor += t->view_reason_gap;
            rec->reason_h = px(t, 150);
            rec->reason_y = cursor;
            cursor += rec->reason_h + t->view_reason_gap;
        }
        if (rec->body_live) {
            rec->body_h = measure_control(t,
                transcript_surface(t, i, TRANSCRIPT_BODY), t->view_width);
            rec->body_y = cursor;
            cursor += rec->body_h;
        }
        if (rec->meta_live) {
            cursor += t->view_meta_gap;
            rec->meta_h = measure_control(t,
                transcript_surface(t, i, TRANSCRIPT_META), t->view_width);
            rec->meta_y = cursor;
            cursor += rec->meta_h;
        }
        rec->height = cursor - y;
        /* Measurement stamp: heights are valid only under (width, dpi, theme
           epoch) with the rendered identity current and no debt outstanding
           (the record's other fields). Recorded, never consumed this pass:
           layout re-measures unconditionally, so rendered output is
           identical to a transcript without the stamp. */
        rec->measured_width = t->view_width;
        rec->measured_dpi = t->dpi;
        rec->measured_theme = t->theme_epoch;
        y = cursor + t->view_gap;
    }
    t->view_content = y;
    transcript_position(t, follow);
}

/* ---- Revision-tracked updates -------------------------------------------- */

/* True when the record's surfaces were already built from exactly this
    message state. Identity includes the conversation and message instance
    ids plus the revision, so a replaced slot or an edited message never
    matches. */
static bool rendered_current(const TranscriptRecord *rec, uint64_t conversation,
    uint64_t message, uint64_t revision, ChatRole role,
    ChatGenerationState state, bool running, bool content_started,
    bool reasoning_open, bool has_row, const wchar_t *row) {
    if (!rec->rendered_valid) return false;
    if (rec->conversation != conversation || rec->message != message ||
        rec->revision != revision || rec->role != role ||
        rec->state != state || rec->running != running ||
        rec->reasoning_open != reasoning_open) return false;
    /* content_started only shapes the row of the turn that is currently
       streaming; a completed historical turn's rendered row does not depend on
       it, so starting another response must not stale that turn. */
    if (running && rec->content_started != content_started) return false;
    return has_row ? !wcscmp(rec->row, row) : !rec->row[0];
}

/* True when the record's body already holds exactly this message's answer
    text. Keyed on the text-only revision so a metadata-only generation
    update (or a reasoning append) does not rewrite the body. */
static bool body_current(const TranscriptRecord *rec, uint64_t conversation,
    uint64_t message, uint64_t body_revision, ChatRole role) {
    return rec->rendered_valid && rec->conversation == conversation &&
        rec->message == message && rec->body_revision == body_revision &&
        rec->role == role;
}

/* Writes one surface, deferring the destructive write while it holds a
   selection. Returns true only when a write was actually applied; a deferred
   write changes nothing and leaves geometry untouched until it is applied.
   A MISSING surface is distinguished by why it is missing: an unrealized
   record (slot -1) KEEPS its debt, which re-applies when the record binds
   again; a bound surface without a window stays the existing rule (the
   surface is inapplicable or its creation failed, so the debt clears).
   Programmatic selection changes fired inside the write are suppressed by
   the guard so a deferred application cannot recurse. */
static bool write_head(Transcript *t, TranscriptRecord *rec, int index,
    ChatRole role, const wchar_t *row, bool has_row) {
    RichTextControl *control = transcript_surface(t, index, TRANSCRIPT_HEAD);
    if (!control) {
        if (rec->slot < 0) return false;    /* unrealized: debt retained */
        rec->head_pending = false;
        return false;
    }
    if (rich_text_has_selection(control)) { rec->head_pending = true; return false; }
    t->applying = true;
    rich_text_set_head(control, role, has_row ? row : NULL);
    t->applying = false;
    rec->head_pending = false;
    return true;
}

static bool write_body(Transcript *t, TranscriptRecord *rec, int index,
    ChatRole role, const wchar_t *text) {
    RichTextControl *control = transcript_surface(t, index, TRANSCRIPT_BODY);
    if (!control) {
        if (rec->slot < 0) return false;    /* unrealized: debt retained */
        rec->body_pending = false;
        return false;
    }
    if (rich_text_has_selection(control)) { rec->body_pending = true; return false; }
    t->applying = true;
    if (role == CHAT_ROLE_ASSISTANT)
        rich_text_set_markdown(control, role, text);
    else
        rich_text_set_block(control, role, text);
    t->applying = false;
    rec->body_pending = false;
    return true;
}

static bool write_meta(Transcript *t, TranscriptRecord *rec, int index,
    const ChatGeneration *g) {
    RichTextControl *control = transcript_surface(t, index, TRANSCRIPT_META);
    if (!control) {
        if (rec->slot < 0) return false;    /* unrealized: debt retained */
        rec->meta_pending = false;
        return false;
    }
    if (rich_text_has_selection(control)) { rec->meta_pending = true; return false; }
    wchar_t info[768];
    format_stats(g, info, 768);
    t->applying = true;
    rich_text_set_meta(control, info, g->error);
    t->applying = false;
    rec->meta_pending = false;
    return true;
}

static bool write_reasoning(Transcript *t, TranscriptRecord *rec, int index,
    const wchar_t *text) {
    RichTextControl *control = transcript_surface(t, index, TRANSCRIPT_REASON);
    if (!control) {
        if (rec->slot < 0) return false;    /* unrealized: debt retained */
        rec->reason_pending = false;
        return false;
    }
    if (rich_text_has_selection(control)) { rec->reason_pending = true; return false; }
    t->applying = true;
    rich_text_set_reasoning(control, text);
    t->applying = false;
    rec->reason_pending = false;
    return true;
}

/* Applies deferred writes for one record whose surfaces no longer hold a
   selection. Only surfaces with a pending debt are touched; each write
   re-checks its own selection and re-defers if the reader is still selecting.
   A record may only be touched when its identity certifies the bound
   surfaces: the message instance must match (through the pure policy seam)
   AND the binding generation must be current. On message replacement the
   debt refers to content the surfaces no longer render and is refused --
   the selection is cleared, the stale debt and cached identity are dropped,
   nothing is rewritten here, and the next render replaces the content
   immediately (the selection is already gone, so no deferral). On a
   same-message binding mismatch nothing is touched at all and the debt is
   preserved for prepare_turn, which rebinds, rewrites completely and
   applies it. Returns true when any write was applied, which changes
   content geometry and requires a relayout from this turn. */
static bool catch_up(Transcript *t, const TranscriptFeed *feed, int index) {
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count) return false;
    const ChatConversation *c = &chat->conversations[chat->active];
    if (index < 0 || (size_t)index >= c->message_count) return false;
    if (index >= CHAT_MAX_MESSAGES) return false;
    const ChatMessage *m = &c->messages[index];
    TranscriptRecord *rec = &t->records[index];
    if (!rec->rendered_valid) return false;   /* nothing certifies the surfaces */
    if (!transcript_policy_same_message(rec->conversation, rec->message,
            c->id, m->id)) {
        reset_slot(t, index);   /* replacement class: drop stale debt, refuse */
        return false;
    }
    if (!record_certified(t, rec)) return false;   /* stale incarnation: keep debt */
    bool assistant = m->role == CHAT_ROLE_ASSISTANT;
    bool running = feed->generating &&
        feed->request_conversation == chat->active &&
        index == feed->request_message;
    wchar_t row[48];
    row[0] = 0;
    bool has_row = row_for(feed, m, assistant, running, row, 48);
    bool changed = false;
    if (rec->head_pending) {
        if (write_head(t, rec, index, m->role, row, has_row)) changed = true;
    }
    if (rec->body_pending) {
        if (write_body(t, rec, index, m->role, chat_message_text(m))) {
            rec->body_revision = m->body_revision;
            changed = true;
        }
    }
    bool terminal = m->generation.state != CHAT_GENERATION_NONE &&
        m->generation.state != CHAT_GENERATION_RUNNING;
    if (rec->meta_pending) {
        if (terminal) {
            if (write_meta(t, rec, index, &m->generation)) changed = true;
        } else {
            hide_surface(t, index, TRANSCRIPT_META);
            rec->meta_pending = false;
            rec->meta_live = false;
            changed = true;
        }
    }
    if (rec->reason_pending) {
        if (write_reasoning(t, rec, index, chat_message_reasoning(m)))
            changed = true;
    }
    return changed;
}

/* Synchronizes one record's surfaces with its message. A message whose
   identity still matches what was rendered skips every destructive write;
   control realization, callback wiring and visibility reconciliation always
   run. */
static void prepare_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    if (index < 0 || index >= CHAT_MAX_MESSAGES) return;
    const Chat *chat = feed->chat;
    const ChatConversation *c = &chat->conversations[chat->active];
    const ChatMessage *m = &c->messages[index];
    TranscriptRecord *rec = &t->records[index];
    /* Bind before any freshness decision: the cached identity can only
       certify content in the slot it will actually render into. */
    ensure_slot(t, feed, index);
    bool assistant = m->role == CHAT_ROLE_ASSISTANT;
    bool running = feed->generating &&
        feed->request_conversation == chat->active &&
        index == feed->request_message;
    bool terminal = m->generation.state != CHAT_GENERATION_NONE &&
        m->generation.state != CHAT_GENERATION_RUNNING;
    wchar_t row[48];
    row[0] = 0;
    bool has_row = row_for(feed, m, assistant, running, row, 48);
    /* The cached identity certifies content only for the exact association
       it was stamped from: same slot index AND same binding generation. A
       slot number reused by another record fails the generation check. */
    bool certified = record_certified(t, rec);
    bool fresh = certified && rendered_current(rec, c->id, m->id,
        m->revision, m->role, m->generation.state, running,
        feed->content_started, m->reasoning_open, has_row, row);
    /* The answer body is keyed on its text alone, so a terminal metadata
       update refreshes the footer without rebuilding the body. */
    bool body_fresh = certified && body_current(rec, c->id, m->id,
        m->body_revision, m->role);
    /* Does the surface still represent this exact message instance? The
       instance-level comparison goes through the pure policy seam. Captured
       before the invalidations below overwrite it. */
    bool same_message = rec->rendered_valid &&
        transcript_policy_same_message(rec->conversation, rec->message,
            c->id, m->id);
    /* Two separated invalidation cases. Both clear the selection belonging
       to the surfaces about to be rewritten and invalidate the cached
       state, forcing a complete rewrite; they differ in the record-owned
       debt. Captured before the invalidations. */
    bool stale_binding = rec->rendered_valid && !certified;
    bool replacing = rec->rendered_valid && !same_message;
    if (stale_binding) {
        /* Case 2 -- a NEW binding incarnation (freshly assigned generation)
           for the SAME message: the acquired surfaces may hold another
           record's content and reader selection. Clear that selection,
           invalidate the cached certification, and rewrite completely --
           but PRESERVE the pending debt, which then applies to the newly
           bound surfaces below. */
        clear_slot_selection(t, rec->slot);
        rec->rendered_valid = false;
        rec->rendered_slot = -1;
        rec->rendered_generation = 0;
    }
    if (replacing) {
        /* Case 1 -- another conversation or message instance: clear the
           selection, DROP the stale debt and invalidate the cached state;
           the writes below replace content immediately, without selection
           deferral. */
        reset_slot(t, index);
    }
    if (!fresh) {
        rec->conversation = c->id;
        rec->message = m->id;
        rec->revision = m->revision;
        rec->role = m->role;
        rec->state = m->generation.state;
        rec->running = running;
        rec->content_started = feed->content_started;
        rec->reasoning_open = m->reasoning_open;
        wcsncpy(rec->row, row, sizeof rec->row / sizeof *rec->row - 1);
        rec->row[sizeof rec->row / sizeof *rec->row - 1] = 0;
        /* The stamped identity certifies exactly this association. */
        rec->rendered_slot = rec->slot;
        rec->rendered_generation = rec->slot >= 0
            ? t->slots[rec->slot].generation : 0;
        rec->rendered_valid = true;
    }

    if (assistant) {
        bool created = transcript_surface(t, index, TRANSCRIPT_HEAD) == NULL;
        RichTextControl *head = ensure_surface(t, feed, index,
            TRANSCRIPT_HEAD, false);
        if (head) rec->head_live = true;
        if (!fresh || rec->head_pending || created)
            write_head(t, rec, index, m->role, row, has_row);
    } else {
        hide_surface(t, index, TRANSCRIPT_HEAD);
        rec->head_live = false;
        rec->head_pending = false;
    }
    RichTextControl *body = ensure_surface(t, feed, index, TRANSCRIPT_BODY,
        false);
    if (body) rec->body_live = true;
    /* Record the revision only once the write actually lands, so the field
       always names the answer text present in the control. A write deferred by
       a selection leaves body_pending set and the revision untouched. */
    if ((!body_fresh || rec->body_pending) &&
        write_body(t, rec, index, m->role, chat_message_text(m)))
        rec->body_revision = m->body_revision;
    if (assistant && terminal) {
        bool created = transcript_surface(t, index, TRANSCRIPT_META) == NULL;
        ensure_surface(t, feed, index, TRANSCRIPT_META, false);
        if (!fresh || rec->meta_pending || created)
            write_meta(t, rec, index, &m->generation);
    }

    /* Metadata is a terminal-state footer: visibility is reconciled on every
       pass, whether or not the content write was skipped. */
    if (assistant && terminal) {
        rec->meta_live = transcript_surface(t, index, TRANSCRIPT_META) != NULL;
    } else {
        hide_surface(t, index, TRANSCRIPT_META);
        rec->meta_live = false;
        if (!assistant) rec->meta_pending = false;
    }

    bool open = assistant && has_row && m->reasoning_open;
    if (open) {
        bool created = transcript_surface(t, index, TRANSCRIPT_REASON) == NULL;
        RichTextControl *reason = ensure_surface(t, feed, index,
            TRANSCRIPT_REASON, true);
        if (reason) {
            /* Reopened on this pass: the viewport kept its window while
               collapsed and so missed the appends that arrived in the
               meantime. It must reload the accumulated reasoning before the
               stream resumes appending into it. */
            bool resumed = !rec->reason_live;
            rec->reason_live = true;
            /* A live stream is appended to, never rebuilt, so its viewport
               keeps its own scroll position. A freshly created viewport always
               loads the reasoning accumulated so far, and a stale binding
               incarnation must reload it too (the viewport's content is not
               certified). */
            bool streaming = running && feed->reasoning_streaming;
            if (created || !streaming || resumed || !same_message ||
                stale_binding) {
                if (!fresh || rec->reason_pending || created || resumed ||
                    !same_message)
                    write_reasoning(t, rec, index, chat_message_reasoning(m));
            }
        }
    } else {
        hide_surface(t, index, TRANSCRIPT_REASON);
        rec->reason_live = false;
    }
}

/* The single documented capacity checkpoint. The policy's required slot
   count is computed every render and asserted against the pool: a P-CAP
   breach (needed > slot_capacity) is a contract error and fails fast -- it
   is never clamped away or discarded. Retain-all satisfies the precondition
   by construction (slot_capacity == CHAT_MAX_MESSAGES >= record count >=
   |V u P|), so the assertion cannot fire this pass; a future capacity-
   governed pass activates here and answers a breach by raising capacity (or
   evicting under the policy's P-CAP), never by leaving a visible record
   unrealized. Debt is the full decision-time predicate (pending writes OR a
   live selection), so an unfocused selected turn is protected. Geometry
   comes from the previous layout, so unmeasured records claim h_min of
   space -- the same conservative bound the policy documents. */
static void capacity_checkpoint(Transcript *t, const TranscriptFeed *feed,
    int count) {
    if (count <= 0) { t->policy_needed = 0; return; }
    TranscriptPolicyItem items[CHAT_MAX_MESSAGES];
    HWND focus = GetFocus();
    for (int i = 0; i < count; i++) {
        TranscriptRecord *rec = &t->records[i];
        items[i].index = i;
        items[i].y = rec->y;
        items[i].height = rec->height;
        items[i].streaming = feed->generating &&
            feed->request_conversation == feed->chat->active &&
            i == feed->request_message;
        items[i].debt = transcript_record_debt(t, i);
        items[i].expanded = rec->reason_live;
        items[i].focused = false;
        items[i].last_used = rec->last_used;
        if (focus)
            for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT && !items[i].focused;
                s++) {
                RichTextControl *control = transcript_surface(t, i,
                    (TranscriptSurface)s);
                items[i].focused = control && control->window == focus;
            }
    }
    int needed = transcript_policy_needed_slots(items, count, t->view_scroll,
        t->view_page, px(t, 8), TRANSCRIPT_SPARE_SLOTS);
    /* Fail-fast P-CAP (invariant I10). */
    assert(needed <= t->slot_capacity);
    t->policy_needed = needed;
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
    int count = c ? (int)c->message_count : 0;
    t->record_count = count;
    for (int i = 0; i < count; i++) prepare_turn(t, feed, i);
    for (int i = count; i < CHAT_MAX_MESSAGES; i++) hide_turn(t, i);
    capacity_checkpoint(t, feed, count);
    transcript_layout_from(t, 0, pinned);
}

/* Rebuilds one turn only, so toggling a row never disturbs another turn's
   viewport or the transcript scroll. */
void transcript_refresh_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count) return;
    if (index < 0 || (size_t)index >= chat->conversations[chat->active].message_count)
        return;
    bool pinned = transcript_pinned(t);
    prepare_turn(t, feed, index);
    transcript_layout_from(t, index, pinned);
}

void transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index) {
    if (index < 0 || index >= CHAT_MAX_MESSAGES) return;
    TranscriptRecord *rec = &t->records[index];
    RichTextControl *body = transcript_surface(t, index, TRANSCRIPT_BODY);
    if (!body) return;
    const ChatConversation *c =
        &feed->chat->conversations[feed->request_conversation];
    const ChatMessage *m = &c->messages[index];
    if (rich_text_has_selection(body)) {
        /* The reader holds a selection in the live answer: the destructive
           Markdown rebuild is deferred until the selection clears. */
        rec->body_pending = true;
        return;
    }
    bool pinned = transcript_pinned(t);
    t->applying = true;
    rich_text_set_markdown(body, m->role, chat_message_text(m));
    t->applying = false;
    rec->body_pending = false;
    /* This path bypasses prepare_turn(); keep the recorded revision in step. */
    rec->body_revision = m->body_revision;
    transcript_layout_from(t, index, pinned);
}

/* Explicit invalidation: on conversation change or slot reuse the record's
   pending updates and selections are dropped and the next render replaces
   content without deferral. */
static void reset_slot(Transcript *t, int index) {
    TranscriptRecord *rec = &t->records[index];
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
        RichTextControl *control = transcript_surface(t, index,
            (TranscriptSurface)s);
        if (!control) continue;
        CHARRANGE none = { 0, 0 };
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&none);
    }
    rec->rendered_valid = false;
    rec->head_pending = rec->body_pending = false;
    rec->meta_pending = rec->reason_pending = false;
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
    for (int i = 0; i < t->record_count; i++) {
        for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
            if (transcript_surface(t, i, (TranscriptSurface)s) != control)
                continue;
            if (!rich_text_has_selection(control)) {
                /* Applied deferred content changes geometry: capture the
                   reader's pin state first, then relayout from the affected
                   turn so following positions, view_content and the scrollbar
                   range follow the new content, keeping bottom-following when
                   pinned. */
                bool pinned = transcript_pinned(t);
                if (catch_up(t, feed, i)) transcript_layout_from(t, i, pinned);
            }
            return;
        }
    }
}

void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed) {
    if (t->applying) return;
    bool pinned = transcript_pinned(t);
    int first = -1;
    for (int i = 0; i < t->record_count; i++) {
        TranscriptRecord *rec = &t->records[i];
        if (rec->head_pending || rec->body_pending || rec->meta_pending ||
            rec->reason_pending) {
            if (catch_up(t, feed, i) && first < 0) first = i;
        }
    }
    /* One relayout from the earliest changed turn covers every applied write. */
    if (first >= 0) transcript_layout_from(t, first, pinned);
}

bool transcript_create(Transcript *t, HWND view, const RichTextTheme *theme,
    float dpi) {
    memset(t, 0, sizeof *t);
    t->view = view;
    t->dpi = dpi;
    t->theme = *theme;
    t->theme_epoch = 1;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        t->records[i].slot = -1;
        t->records[i].rendered_slot = -1;
    }
    /* Exactly one allocation for the transcript's lifetime: the realized-slot
       pool. Failure leaves the transcript safe but unrealized and fails host
       startup (fail closed, like the other host subsystems). */
    t->slots = calloc(CHAT_MAX_MESSAGES, sizeof *t->slots);
    if (!t->slots) { t->slot_capacity = 0; return false; }
    t->slot_capacity = CHAT_MAX_MESSAGES;
    for (int i = 0; i < t->slot_capacity; i++) t->slots[i].record = -1;
    return true;
}

void transcript_dispose(Transcript *t) {
    /* Bookkeeping only: the child surfaces are destroyed with the container
       window, which must already be gone when this runs (see the header). */
    if (!t || !t->slots) return;
    free(t->slots);
    t->slots = NULL;
    t->slot_capacity = 0;
}

void transcript_set_dpi(Transcript *t, float dpi) {
    t->dpi = dpi;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++)
        for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
            RichTextControl *control = transcript_surface(t, i,
                (TranscriptSurface)s);
            if (control) rich_text_set_dpi(control, dpi);
        }
}

void transcript_measure_notify(Transcript *t, RichTextControl *control,
    const RECT *required) {
    if (control && control == t->measuring)
        t->measured = required->bottom - required->top;
}
