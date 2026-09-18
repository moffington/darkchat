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

/* Required client height of a block at the given width (defined with the
    measurement helpers; needed by the public diagnostic seam). */
static int measure_control_q(Transcript *t, RichTextControl *control,
    int width, TranscriptMeasureQuality *quality);

/* Head-row text for one turn's reasoning affordance (defined with the other
    content helpers; needed by the shared shape derivation). */
static bool row_for(const TranscriptFeed *feed, const ChatMessage *m,
    bool assistant, bool running, wchar_t *row, size_t cap);

/* Placement half of transcript_position (defined with the bounded engine;
    retain-all layout reaches it through transcript_position with a NULL
    feed, which skips realization). */
static void place_and_scroll(Transcript *t, const TranscriptFeed *feed,
    bool follow);

typedef struct {
    int rounds;
    bool stable, degraded, fallback;
} RealizeResult;

static void realize_loop(Transcript *t, const TranscriptFeed *feed,
    bool follow, int forced_index, bool forced_reveal, RealizeResult *out);

/* ---- Record/slot plumbing ------------------------------------------------ */

/* A non-NULL destroyed HWND is not a surface. Clear all cached control state
   before a later reconciliation recreates it. */
static bool control_window_live(RichTextControl *control) {
    if (control && control->window && IsWindow(control->window)) return true;
    if (control && control->window) memset(control, 0, sizeof *control);
    return false;
}

RichTextControl *transcript_surface(Transcript *t, int index,
    TranscriptSurface surface) {
    if (!t || index < 0 || index >= CHAT_MAX_MESSAGES) return NULL;
    if ((int)surface < 0 || surface >= TRANSCRIPT_SURFACE_COUNT) return NULL;
    TranscriptRecord *rec = &t->records[index];
    if (rec->slot < 0 || rec->slot >= t->slot_capacity) return NULL;
    RichTextControl *control = &t->slots[rec->slot].surface[surface];
    return control_window_live(control) ? control : NULL;
}

/* Reader focus transfer (defined with the reader-intent section); declared
    here because the hide/unbind paths below must transfer focus first. */
static void focus_release_surface(Transcript *t, HWND window);
/* Decision-time reader focus (defined with the reader-intent section): the
    tracked WM_COMMAND state, with GetFocus() as the fallback. */
static HWND decision_focus(const Transcript *t);
/* Capacity-governance helpers (defined with the bounded engine); the
    bind path consults the forced-victim seam when the Tier-B allowance
    leaves no free or evictable slot. */
static void fill_bound_items(Transcript *t, const TranscriptFeed *feed,
    TranscriptPolicyItem *bound);

/* Arena diagnostics (see the header): created counts native windows across
    the pool plus the measurer; bound counts slots currently attached to a
    record. Both re-derive from live state, so forged or failed states stay
    honest. */
int transcript_created_windows(const Transcript *t) {
    if (!t) return 0;
    int count = 0;
    for (int s = 0; s < t->slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            if (t->slots[s].surface[k].window &&
                IsWindow(t->slots[s].surface[k].window)) ++count;
    if (t->measurer.window && IsWindow(t->measurer.window)) ++count;
    return count;
}

int transcript_bound_slots(const Transcript *t) {
    if (!t || !t->slots) return 0;
    int count = 0;
    for (int s = 0; s < t->slot_capacity; s++)
        if (t->slots[s].record >= 0) ++count;
    return count;
}

void transcript_set_bounded(Transcript *t, bool bounded) {
    if (!t) return;
    if (bounded && !t->bounded) t->bounded_prune_pending = true;
    if (!bounded) t->bounded_prune_pending = false;
    t->bounded = bounded;
}

static void update_created_peak(Transcript *t) {
    int current = transcript_created_windows(t);
    if (current > t->stat.created_peak) t->stat.created_peak = current;
}

void transcript_set_theme(Transcript *t, const RichTextTheme *theme) {
    if (!t || !theme) return;
    t->theme = *theme;
    ++t->theme_epoch;
    if (!t->theme_epoch) ++t->theme_epoch;
    for (int s = 0; s < t->slot_capacity; s++)
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            if (control_window_live(&t->slots[s].surface[k]))
                t->slots[s].surface[k].theme = *theme;
    if (control_window_live(&t->measurer)) t->measurer.theme = *theme;
    /* The next prepare reapplies formatting with the changed face/color
       inputs. Selection-bearing surfaces still follow normal debt handling. */
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++)
        t->records[i].rendered_valid = false;
}

int transcript_measure_live(Transcript *t, RichTextControl *control,
    TranscriptMeasureQuality *quality) {
    if (!t || !control) return 0;
    return measure_control_q(t, control, t->view_width, quality);
}

/* Decision-time debt: pending deferred writes OR a live selection in any
    bound surface, OR an unrestored reader-state capture (a record whose
    captured selection or reasoning scroll is still waiting for its
    restoration still carries reader state). Every TranscriptPolicyItem
    built for a production decision (the capacity checkpoint and the
    dormant victim path) takes debt from here, so an unfocused turn with a
    live selection, deferred writes or an unrestored capture is
    class-protected (Tier-B) and can never be chosen by pre-eviction. */
bool transcript_record_debt(Transcript *t, int index) {
    if (!t || index < 0 || index >= CHAT_MAX_MESSAGES) return false;
    TranscriptRecord *rec = &t->records[index];
    if (rec->head_pending || rec->body_pending || rec->meta_pending ||
        rec->reason_pending) return true;
    if (rec->saved_reason_scroll >= 0) return true;
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
        if (rec->saved_sel_min[k] >= 0) return true;
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
        RichTextControl *control = transcript_surface(t, index,
            (TranscriptSurface)s);
        if (control && rich_text_has_selection(control)) return true;
    }
    return false;
}

/* ---- Shape, identity and stamp derivation (bounded engine) --------------- */

/* Bitmask of surface kinds with created windows on one slot. */
static uint32_t slot_created_kinds(const Transcript *t, int slot) {
    uint32_t kinds = 0;
    if (slot < 0 || slot >= t->slot_capacity) return 0;
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
        if (t->slots[slot].surface[k].window &&
            IsWindow(t->slots[slot].surface[k].window)) kinds |= 1u << k;
    return kinds;
}

/* Derived, allocation-free view of one message's rendering inputs: the same
    values prepare_turn reconciles against, shared with the off-screen
    measurement matrix so geometry can never diverge in shape from
    realization. */
typedef struct {
    const ChatMessage *m;
    const ChatConversation *c;
    bool assistant, running, terminal, has_row, content_started;
    wchar_t row[48];
    bool shape_head, shape_reason, shape_meta;   /* body is always wanted */
    /* Per-family displayed==desired currency (rendered-identity based;
        pending debt is checked separately because a deferred write leaves
        the identity current while the display lags). */
    bool head_current, body_current, meta_current, reason_current;
} TurnState;

static bool turn_state(Transcript *t, const TranscriptFeed *feed, int index,
    TurnState *s) {
    memset(s, 0, sizeof *s);
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count)
        return false;
    const ChatConversation *c = &chat->conversations[chat->active];
    if (index < 0 || index >= CHAT_MAX_MESSAGES ||
        (size_t)index >= c->message_count) return false;
    const ChatMessage *m = &c->messages[index];
    TranscriptRecord *rec = &t->records[index];
    s->c = c;
    s->m = m;
    s->assistant = m->role == CHAT_ROLE_ASSISTANT;
    s->running = feed->generating &&
        feed->request_conversation == chat->active && index == feed->request_message;
    s->content_started = feed->content_started;
    s->terminal = m->generation.state != CHAT_GENERATION_NONE &&
        m->generation.state != CHAT_GENERATION_RUNNING;
    s->has_row = row_for(feed, m, s->assistant, s->running, s->row, 48);
    s->shape_head = s->assistant;
    s->shape_meta = s->assistant && s->terminal;
    s->shape_reason = s->assistant && s->has_row && m->reasoning_open;
    bool same = rec->rendered_valid && rec->conversation == c->id &&
        rec->message == m->id;
    s->head_current = same && rec->revision == m->revision &&
        rec->role == m->role && rec->state == m->generation.state &&
        rec->running == s->running &&
        (!s->running || rec->content_started == feed->content_started) &&
        (s->has_row ? !wcscmp(rec->row, s->row) : !rec->row[0]);
    s->body_current = same && rec->body_revision == m->body_revision &&
        rec->role == m->role &&
        (m->role != CHAT_ROLE_ASSISTANT ||
         (rec->body_layout_width == t->view_width &&
          rec->body_layout_dpi == t->dpi &&
          rec->body_layout_theme == t->theme_epoch));
    s->meta_current = same && rec->revision == m->revision &&
        rec->state == m->generation.state;
    s->reason_current = same && rec->revision == m->revision &&
        rec->reasoning_open == m->reasoning_open;
    return true;
}

/* The kinds a record's current shape needs. Body is always wanted; the
    others follow the shape flags. */
static uint32_t turn_needed_kinds(const TurnState *s) {
    return (s->shape_head ? TRANSCRIPT_SURFACE_KIND(TRANSCRIPT_HEAD) : 0) |
        TRANSCRIPT_SURFACE_KIND(TRANSCRIPT_BODY) |
        (s->shape_reason ? TRANSCRIPT_SURFACE_KIND(TRANSCRIPT_REASON) : 0) |
        (s->shape_meta ? TRANSCRIPT_SURFACE_KIND(TRANSCRIPT_META) : 0);
}

/* True when the record's exact-measurement stamp matches the record's
    freshly derived shape and identity at the current width/dpi/theme. */
static bool stamp_current(const Transcript *t, const TranscriptRecord *rec,
    const TurnState *s) {
    if (!rec->measured_valid) return false;
    if (rec->measured_estimated) return false;
    if (rec->measured_width != t->view_width ||
        rec->measured_dpi != t->dpi ||
        rec->measured_theme != t->theme_epoch) return false;
    return rec->measured_conversation == s->c->id &&
        rec->measured_message == s->m->id &&
        rec->measured_revision == s->m->revision &&
        rec->measured_body_revision == s->m->body_revision &&
        rec->measured_role == s->m->role &&
        rec->measured_state == s->m->generation.state &&
        rec->measured_running == s->running &&
        (!s->running || rec->measured_content_started == s->content_started) &&
        rec->measured_reasoning_open == s->m->reasoning_open &&
        rec->measured_row == s->has_row;
}

/* Association certification: the record's cached identity certifies the
    content currently in its bound slot -- the association must be the exact
    one the identity was stamped from (same slot index AND same binding
    generation, because a slot number can be reused by another record after
    an unbind). This is the slot-incarnation predicate only; displayed-text
    currency is TurnState's per-family content_current checks. */
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
        if (!control_window_live(control)) continue;
        CHARRANGE none = { 0, 0 };
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&none);
    }
}

/* Hides one surface if it is realized; a focused surface first transfers
    focus to the container (the host's focus-transfer contract). */
static void hide_surface(Transcript *t, int index, TranscriptSurface surface) {
    RichTextControl *control = transcript_surface(t, index, surface);
    if (!control) return;
    focus_release_surface(t, control->window);
    ShowWindow(control->window, SW_HIDE);
}

/* Hides every surface of one record and clears its live flags; the slot
   binding is retained. */
static void hide_turn(Transcript *t, int index) {
    TranscriptRecord *rec = &t->records[index];
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++)
        hide_surface(t, index, (TranscriptSurface)s);
    rec->head_live = rec->body_live = rec->reason_live = rec->meta_live = false;
}

/* Reader state an eviction must preserve lives on the record: deferred debt
    and the rendered identity are record fields already; each surface's
    non-empty selection and the reasoning viewport's inner scroll are
    captured here so the next binding of the same message can restore them.
    New captures MERGE into the record's existing saved state: a surface
    that cannot be read (its creation failed after a rebind) keeps its
    earlier capture, so an eviction of a partially recreated record cannot
    erase reader state it never saw; only a replacement-class reset
    (reset_slot) discards captures with the stale identity. */
static void capture_reader_state(TranscriptRecord *rec,
    const TranscriptSlot *slot) {
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++) {
        const RichTextControl *control = &slot->surface[k];
        if (!control->window || !IsWindow(control->window)) continue;
        CHARRANGE sel;
        memset(&sel, 0, sizeof sel);
        SendMessageW(control->window, EM_EXGETSEL, 0, (LPARAM)&sel);
        if (sel.cpMax > sel.cpMin) {
            rec->saved_sel_min[k] = sel.cpMin;
            rec->saved_sel_max[k] = sel.cpMax;
        } else {
            rec->saved_sel_min[k] = -1;
            rec->saved_sel_max[k] = -1;
        }
        if (k == TRANSCRIPT_REASON) {
            POINT point = { 0, 0 };
            SendMessageW(control->window, EM_GETSCROLLPOS, 0, (LPARAM)&point);
            rec->saved_reason_scroll = point.y > 0 ? point.y : -1;
        }
    }
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
    capture_reader_state(rec, s);
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
        if (control_window_live(&s->surface[k])) {
            focus_release_surface(t, s->surface[k].window);
            ShowWindow(s->surface[k].window, SW_HIDE);
        }
    rec->head_live = rec->body_live = rec->reason_live = rec->meta_live = false;
    rec->slot = -1;
    s->record = -1;
}

/* Returns the record's bound slot, binding one if needed. Retain-all: the
    pool holds CHAT_MAX_MESSAGES slots and records never exceed that, so a
    free slot always exists and the policy victim path is unreachable this
    pass. Returns -1 (record stays unrealized) only if binding fails closed.
    Bounded: binds through transcript_policy_pick_slot — shape-aware reuse
    and pre-eviction under the overscan-widened must-keep set, with pristine
    slots consumed only as the final step — strictly within the raise-only
    governed limit [0, slot_limit): no selection ever considers a slot at or
    beyond it. Only when that selection returns -1 is a forced Tier-B
    eviction taken, and even that victim comes from within the limit. */
static int ensure_slot(Transcript *t, const TranscriptFeed *feed, int index) {
    TranscriptRecord *rec = &t->records[index];
    if (rec->slot >= 0) return rec->slot;
    int slot = -1;
    if (t->bounded) {
        TurnState s;
        if (!turn_state(t, feed, index, &s)) return -1;
        int limit = t->slot_limit;
        if (limit <= 0) {
            /* Never raised: no governed selection can run. */
            ++t->stat.exhaustion_refusals;
            return -1;   /* fail closed */
        }
        if (limit > t->slot_capacity) limit = t->slot_capacity;
        TranscriptPolicyItem bound[CHAT_MAX_MESSAGES];
        int h_min = px(t, 8);
        int pad = px(t, TRANSCRIPT_OVERSCAN_DIPS);
        fill_bound_items(t, feed, bound);
        slot = transcript_policy_pick_slot(bound, limit,
            turn_needed_kinds(&s), t->view_scroll - pad,
            t->view_page + 2 * pad, h_min);
        if (slot < 0) {
            /* The governed limit is genuinely exhausted: no free slot and
                no evictable (pre-evictable) binding inside [0, limit) —
                this is the ONLY trigger of forced eviction, never a bound
                count merely exceeding the Tier-B allowance. The forced
                victim is the LRU bound record that is neither visible in
                the window nor Tier-A protected (focused/streaming). Its
                reader state survives on the record (capture in
                unbind_slot) and returns at the next binding of the same
                message. */
            ++t->stat.limit_saturated;
            slot = transcript_policy_pick_forced_victim(bound, limit,
                t->view_scroll - pad, t->view_page + 2 * pad, h_min);
            if (slot < 0) {
                /* Every slot inside the limit is visible or Tier-A: the
                    caller must raise capacity (or fail closed); the record
                    stays unrealized and the refusal is counted. */
                ++t->stat.exhaustion_refusals;
                return -1;   /* fail closed */
            }
            unbind_slot(t, slot);
            ++t->stat.evictions;
            ++t->stat.forced_evictions;
            ++t->stat.rebinds;
        }
        if (t->slots[slot].record >= 0) {
            unbind_slot(t, slot);        /* pre-eviction: departed record */
            ++t->stat.evictions;
            ++t->stat.rebinds;
        }
    } else {
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
            HWND focus = decision_focus(t);
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
                bound[s].created_kinds = 0;
                if (focus)
                    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT &&
                        !bound[s].focused; k++)
                        bound[s].focused = other->slot >= 0 &&
                            s == other->slot &&
                            t->slots[s].surface[k].window == focus;
            }
            slot = transcript_policy_pick_victim(bound, t->slot_capacity,
                t->view_scroll, t->view_page, h_min);
            if (slot < 0) {
                ++t->stat.exhaustion_refusals;
                return -1;
            }
            unbind_slot(t, slot);
        }
    }
    t->slots[slot].record = index;
    /* Every new association receives a fresh, nonzero binding generation:
       slot-number reuse by another record can never be mistaken for the
       same surfaces. */
    t->slots[slot].generation = ++t->clock;
    rec->slot = slot;
    rec->last_used = t->slots[slot].generation;
    ++t->stat.binds;
    if (t->render_active) ++t->round_transitions;
    int bound_now = transcript_bound_slots(t);
    if (bound_now > t->stat.bound_peak) t->stat.bound_peak = bound_now;
    return slot;
}

/* Returns the record's bound surface, creating it on first use. Creation is
    per surface, exactly as before: an independent failure leaves the window
    NULL and is retried on the next prepare — except inside a realize loop,
    where attempt stamps allow exactly one creation attempt per surface per
    render; a failed attempt marks the record blocked_resource and later
    progress rides the external delta/timer/render paths. The control id is
    slot-based (100 + slot*4 + surface); with retain-all's permanent binding
    slot equals record index, so emitted ids are numerically unchanged. */
static RichTextControl *ensure_surface(Transcript *t,
    const TranscriptFeed *feed, int index, TranscriptSurface surface,
    bool viewport) {
    if (!t->view) return NULL;
    int slot = ensure_slot(t, feed, index);
    if (slot < 0) return NULL;
    RichTextControl *control = &t->slots[slot].surface[surface];
    if (!control_window_live(control)) {
        TranscriptRecord *rec = &t->records[index];
        uint32_t bit = 1u << (int)surface;
        if (t->render_active) {
            if (rec->attempted_epoch != t->render_epoch) {
                rec->attempted_epoch = t->render_epoch;
                rec->attempted_kinds = 0;
            }
            if (rec->attempted_kinds & bit) return NULL;   /* one per render */
            rec->attempted_kinds |= bit;
        }
        int id = 100 + slot * 4 + (int)surface;
        bool created = viewport
            ? rich_text_create_viewport(control, t->view, id, &t->theme, t->dpi)
            : rich_text_create_block(control, t->view, id, &t->theme, t->dpi);
        if (!created || !control->window) {
            if (t->render_active) rec->blocked_resource = true;
            return NULL;
        }
        ++t->stat.creations;
        if (!(t->slots[slot].arena_kinds & bit)) {
            t->slots[slot].arena_kinds |= bit;
            ++t->stat.created_hwnds;
        }
        update_created_peak(t);
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
   · $0.00165" followed by "OpenRouter · model" (or "Ollama · model"), with the
   backend named on the model line. A local Ollama turn shows "local" where a
   remote cost would appear. Normal "stop" is omitted; unusual finish reasons
   are surfaced. */
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
    if (g->backend == CHAT_BACKEND_OLLAMA) {
        /* Local generation has no billed cost; never imply one. */
        stats_append(info, cap, L"local");
    } else if (g->cost >= 0) {
        swprintf(piece, 192, L"$%.5f", g->cost);
        stats_append(info, cap, piece);
    }
    if (g->finish_reason[0] && wcscmp(g->finish_reason, L"stop") != 0) {
        swprintf(piece, 192, L"finish: %ls", g->finish_reason);
        stats_append(info, cap, piece);
    }
    const wchar_t *requested = g->requested_model[0] ? g->requested_model : NULL;
    const wchar_t *actual = g->actual_model[0] ? g->actual_model : NULL;
    wchar_t base[224];
    base[0] = 0;
    if (requested && actual) {
        if (!wcscmp(requested, actual)) wcsncpy(base, actual, 223);
        else swprintf(base, 224, L"%ls \u2192 %ls", requested, actual);
    } else if (actual) {
        wcsncpy(base, actual, 223);
    } else if (requested) {
        wcsncpy(base, requested, 223);
    }
    base[223] = 0;
    wchar_t model[256];
    model[0] = 0;
    if (base[0])
        swprintf(model, 256, L"%ls \u00b7 %ls", chat_backend_name(g->backend),
            base);
    model[255] = 0;
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
    the value in t->measured while t->measuring points at it. Quality is a
    property of the result: EXACT only when the notification arrived with a
    positive height; the EM_GETLINECOUNT fallback is an estimate. */
static int measure_control_q(Transcript *t, RichTextControl *control,
    int width, TranscriptMeasureQuality *quality) {
    if (quality) *quality = TRANSCRIPT_MEASURE_EXACT;
    if (!control || !control_window_live(control) || width <= 0) {
        if (quality) *quality = TRANSCRIPT_MEASURE_ESTIMATE;
        return 0;
    }
    RECT bounds;
    GetWindowRect(control->window, &bounds);
    /* Keep the current height while measuring. Shrinking a live block to a
        probe height on every token scrolls/clips its text before layout.

        A Rich Edit recomputes its line wrap only when a width transaction
        is immediately followed by EM_REQUESTRESIZE. A control resized
        earlier (e.g. by placement) and measured later answers from the
        stale, over-wrapped layout it was written at -- EM_REQUESTRESIZE
        neither re-wraps nor reports the post-resize height. So the width
        is re-asserted here on EVERY measurement: with a real resize when
        it differs, and -- because USER32 skips a same-size resize -- a
        one-pixel down-and-back cycle when it already matches. Spontaneous
        EN_REQUESTRESIZE notifications fired by these transactions are
        rejected below (t->measuring is NULL until the request). */
    int current = bounds.right - bounds.left;
    if (current != width) {
        SetWindowPos(control->window, NULL, 0, 0, width,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
    } else if (t->bounded && width > 1) {
        SetWindowPos(control->window, NULL, 0, 0, width - 1,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
        SetWindowPos(control->window, NULL, 0, 0, width,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
    }
    t->measuring = control;
    t->measured = 0;
    SendMessageW(control->window, EM_REQUESTRESIZE, 0, 0);
    t->measuring = NULL;
    int height = t->measured;
    if (height <= 0) {
        if (quality) *quality = TRANSCRIPT_MEASURE_ESTIMATE;
        int lines = (int)SendMessageW(control->window, EM_GETLINECOUNT, 0, 0);
        if (lines < 1) lines = 1;
        height = lines * px(t, t->theme.ui_size * 1.45f);
    }
    if (quality) {
        if (*quality == TRANSCRIPT_MEASURE_EXACT) ++t->stat.exact_measures;
        else ++t->stat.estimates;
    }
    int minimum = px(t, 8);
    return height < minimum ? minimum : height;
}

static int measure_control(Transcript *t, RichTextControl *control,
    int width) {
    return measure_control_q(t, control, width, NULL);
}

/* Applies the transcript's content width to the shared measurement surface
    BEFORE any content write, so every EM_REQUESTRESIZE answers a layout
    wrapped at the width the live surfaces render at. The parked x is
    preserved (SWP_NOMOVE): the surface never re-enters the client area. */
static void measurer_set_width(Transcript *t) {
    if (!t->measurer_valid || !t->measurer.window ||
        !IsWindow(t->measurer.window) || t->view_width <= 0) return;
    RECT bounds;
    GetWindowRect(t->measurer.window, &bounds);
    if (bounds.right - bounds.left != t->view_width)
        SetWindowPos(t->measurer.window, NULL, 0, 0, t->view_width,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOREDRAW);
}

/* The shared measurement surface: one read-only block, child of the
    container, kept WS_VISIBLE but parked fully beyond the client's right
    edge, where the container clips it away. A truly hidden Rich Edit stops
    re-laying-out its text: EM_REQUESTRESIZE then answers from the stale
    layout the control was created with (word-wrap at the 10px creation
    width), which is exactly the inflated-height failure the live surfaces
    never exhibit. Visible-but-clipped takes the identical layout path as
    the live surfaces while never painting. Created lazily and recreated
    after any loss, so a creation failure or a destroyed HWND degrades one
    pass to estimates and recovers on a later pass. The width is the
    transcript's content width from birth, so layout wraps at the live
    surfaces' width before any content is ever written (width before
    content). */
static bool ensure_measurer(Transcript *t) {
    if (t->measurer_valid && t->measurer.window &&
        IsWindow(t->measurer.window)) return true;
    memset(&t->measurer, 0, sizeof t->measurer);
    t->measurer_valid = false;
    if (!t->view) return false;
    if (!rich_text_create_block(&t->measurer, t->view, TRANSCRIPT_MEASURE_ID,
            &t->theme, t->dpi)) return false;
    /* Off-client from the first message cycle: no paint, no reader focus,
       and the parent clips the rect away (client widths never approach
       TRANSCRIPT_MEASURE_OFFX, so the control stays clipped for the
       transcript's lifetime without any show/hide cycling). */
    int width = t->view_width > 0 ? t->view_width : 10;
    SetWindowPos(t->measurer.window, NULL, TRANSCRIPT_MEASURE_OFFX, 0,
        width, 10, SWP_NOZORDER | SWP_NOACTIVATE);
    t->measurer_valid = true;
    /* The arena counts the measurer once per transcript: a recreation
       replaces a lost HWND rather than growing the arena. */
    ++t->stat.creations;
    if (!t->measurer_counted) {
        t->measurer_counted = true;
        ++t->stat.created_hwnds;
    }
    update_created_peak(t);
    return true;
}

bool transcript_pinned(const Transcript *t) {
    int maximum = t->view_content - t->view_page;
    if (maximum < 0) maximum = 0;
    return t->view_scroll >= maximum - 1;
}

/* ---- Reader scroll intent: follow mode, anchor, thumb drag ------------- */

bool transcript_following(const Transcript *t) {
    return t && t->follow == TRANSCRIPT_FOLLOW_BOTTOM;
}

void transcript_set_follow(Transcript *t, TranscriptFollowMode mode) {
    if (!t) return;
    t->follow = mode;
    /* The anchor exists only while the reader is free; bottom-following has
        no anchor to restore. */
    if (mode == TRANSCRIPT_FOLLOW_BOTTOM) t->anchor.valid = false;
}

void transcript_set_drag(Transcript *t, bool drag) {
    if (!t) return;
    t->thumb_drag = drag;
}

/* ---- Reader focus tracking through WM_COMMAND ---------------------------- */

void transcript_focus_notify(Transcript *t, RichTextControl *control,
    bool gained) {
    if (!t) return;
    if (!gained) {
        if (control && t->focus_window == control->window)
            t->focus_window = NULL;
        return;
    }
    if (!control || !control_window_live(control)) return;
    if (GetAncestor(control->window, GA_ROOT) != GetAncestor(t->view,
            GA_ROOT)) return;                     /* not ours */
    t->focus_window = control->window;
}

void transcript_focus_release(Transcript *t) {
    if (!t) return;
    HWND focused = t->focus_window;
    if (!focused || !IsWindow(focused)) { t->focus_window = NULL; return; }
    if (GetFocus() != focused) { t->focus_window = NULL; return; }
    /* The destination is host policy: the callback moves focus somewhere
        safe (the host focuses the composer) while the surface still
        exists. The transcript container never decides where focus lands,
        so it must not transfer to its own view here. */
    if (t->callbacks.focus_release)
        t->callbacks.focus_release(t->callbacks.user);
    ++t->stat.focus_transfers;
    t->focus_window = NULL;
}

/* Releases focus only if it sits on exactly this surface; called before the
    surface is hidden or its slot rebound, so a focused child never
    disappears under the reader. */
static void focus_release_surface(Transcript *t, HWND window) {
    if (!t->focus_window || t->focus_window != window) return;
    transcript_focus_release(t);
}

/* Decision-time reader focus: the tracked WM_COMMAND state when present,
    GetFocus() as the fallback before the first notification or when focus
    was set through a path that bypassed the Rich Edit notifications. */
static HWND decision_focus(const Transcript *t) {
    if (t->focus_window && IsWindow(t->focus_window)) return t->focus_window;
    return GetFocus();
}

void transcript_note_user_scroll(Transcript *t, int position) {
    if (!t) return;
    if (position < 0) position = 0;
    t->view_scroll = position;
    t->user_scroll_pending = true;
    /* The qualifying transition: only a user scroll that lands at the
        bottom re-enters follow. During a thumb drag the position is still
        recorded (the drag owns it) but the transition is suppressed until
        the release event arrives with the drag cleared. */
    int maximum = t->view_content - t->view_page;
    if (maximum < 0) maximum = 0;
    if (!t->thumb_drag)
        transcript_set_follow(t, position >= maximum - 1
            ? TRANSCRIPT_FOLLOW_BOTTOM : TRANSCRIPT_FOLLOW_FREE);
}

/* Surface geometry accessors over the record's laid-out fields. */
static int surface_y_of(const TranscriptRecord *rec, TranscriptSurface s) {
    switch (s) {
    case TRANSCRIPT_HEAD: return rec->head_y;
    case TRANSCRIPT_REASON: return rec->reason_y;
    case TRANSCRIPT_META: return rec->meta_y;
    default: return rec->body_y;
    }
}

static int surface_h_of(const TranscriptRecord *rec, TranscriptSurface s) {
    switch (s) {
    case TRANSCRIPT_HEAD: return rec->head_h;
    case TRANSCRIPT_REASON: return rec->reason_h;
    case TRANSCRIPT_META: return rec->meta_h;
    default: return rec->body_h;
    }
}

static bool surface_live_of(const TranscriptRecord *rec,
    TranscriptSurface s) {
    switch (s) {
    case TRANSCRIPT_HEAD: return rec->head_live;
    case TRANSCRIPT_REASON: return rec->reason_live;
    case TRANSCRIPT_META: return rec->meta_live;
    default: return rec->body_live;
    }
}

/* Captures the reader's position as an anchor, from the final placed
    scroll. FREE mode only: in BOTTOM mode the anchor is cleared (there is
    no position to hold). The anchor names the first record whose bottom
    edge is below the viewport top and, inside it, the live surface whose
    span contains the viewport top (offset in [0, height)); when the
    viewport top falls in a gap or above a turn, the nearest live surface
    below it is named with a negative offset, which restores exactly.
    Records whose rendered identity is not current cannot name an anchor,
    so capture refuses rather than pinning a stale identity. */
static void anchor_capture(Transcript *t) {
    if (t->follow != TRANSCRIPT_FOLLOW_FREE) { t->anchor.valid = false; return; }
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    if (count <= 0) { t->anchor.valid = false; return; }
    if (t->view_scroll == 0) {
        /* Top of the transcript: scroll 0 restores to scroll 0. A position
            inside the top margin above the first turn is NOT the top
            anchor: it falls through to the surface scan below and is
            named with a negative offset, which restores exactly. */
        TranscriptRecord *rec = &t->records[0];
        if (!rec->rendered_valid) { t->anchor.valid = false; return; }
        t->anchor.valid = true;
        t->anchor.top = true;
        t->anchor.conversation = rec->conversation;
        t->anchor.message = rec->message;
        t->anchor.surface = TRANSCRIPT_BODY;
        t->anchor.offset = 0;
        return;
    }
    for (int i = 0; i < count; i++) {
        TranscriptRecord *rec = &t->records[i];
        if (rec->y + rec->height <= t->view_scroll) continue;
        if (!rec->rendered_valid) { t->anchor.valid = false; return; }
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++) {
            TranscriptSurface s = (TranscriptSurface)k;
            if (!surface_live_of(rec, s)) continue;
            int y = surface_y_of(rec, s);
            int height = surface_h_of(rec, s);
            if (t->view_scroll >= y && t->view_scroll < y + height) {
                t->anchor.valid = true;
                t->anchor.top = false;
                t->anchor.conversation = rec->conversation;
                t->anchor.message = rec->message;
                t->anchor.surface = s;
                t->anchor.offset = t->view_scroll - y;
                return;
            }
        }
        /* No containing surface: name the nearest live surface below the
            viewport top (closest y at or above the scroll). */
        TranscriptSurface best = TRANSCRIPT_SURFACE_COUNT;
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++) {
            TranscriptSurface s = (TranscriptSurface)k;
            if (!surface_live_of(rec, s)) continue;
            if (surface_y_of(rec, s) < t->view_scroll) continue;
            if (best == TRANSCRIPT_SURFACE_COUNT ||
                surface_y_of(rec, s) < surface_y_of(rec, best)) best = s;
        }
        if (best == TRANSCRIPT_SURFACE_COUNT) {
            t->anchor.valid = false;
            return;
        }
        t->anchor.valid = true;
        t->anchor.top = false;
        t->anchor.conversation = rec->conversation;
        t->anchor.message = rec->message;
        t->anchor.surface = best;
        t->anchor.offset = t->view_scroll - surface_y_of(rec, best);
        return;
    }
    t->anchor.valid = false;
}

/* Resolves the anchor to a scroll position against the current geometry.
    The chat feed resolves first (records may all be invalidated, e.g.
    right after a conversation switch); record identity resolves second.
    A stale anchor — its message deleted or replaced — falls back to the
    nearest earlier surviving message, at its top, so a retry/regenerate
    that consumed the reader's position lands on the nearest surviving
    content instead of the bottom. Counts a restore when the resolution
    moves the scroll and a rejection when the anchor was stale. */
static bool anchor_resolve_scroll(Transcript *t, const TranscriptFeed *feed,
    int *scroll) {
    if (!t->anchor.valid) return false;
    if (t->anchor.top) {
        if (*scroll != 0) ++t->stat.anchor_restores;
        *scroll = 0;
        return true;
    }
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    int index = -1;
    const ChatConversation *active = NULL;
    if (feed && feed->chat && t->anchor.conversation &&
        feed->chat->active >= 0 &&
        feed->chat->active < feed->chat->conversation_count &&
        feed->chat->conversations[feed->chat->active].id ==
            t->anchor.conversation) {
        active = &feed->chat->conversations[feed->chat->active];
        index = chat_message_index_by_id(feed->chat, feed->chat->active,
            t->anchor.message);
    }
    if (index < 0)
        for (int i = 0; i < count; i++)
            if (t->records[i].rendered_valid &&
                t->records[i].conversation == t->anchor.conversation &&
                t->records[i].message == t->anchor.message) {
                index = i;
                break;
            }
    if (index < 0 || index >= count) {
        ++t->stat.anchor_rejected;
        if (active) {
            for (size_t i = active->message_count; i > 0; i--)
                if (active->messages[i - 1].id < t->anchor.message) {
                    index = (int)(i - 1);
                    break;
                }
        } else {
            for (int i = count; i > 0; i--)
                if (t->records[i - 1].rendered_valid &&
                    t->records[i - 1].conversation == t->anchor.conversation &&
                    t->records[i - 1].message < t->anchor.message) {
                    index = i - 1;
                    break;
                }
        }
        if (index < 0) {
            if (*scroll != 0) ++t->stat.anchor_restores;
            *scroll = 0;
            return true;
        }
        TranscriptRecord *rec = &t->records[index];
        int target = rec->body_live ? rec->body_y : rec->y;
        if (*scroll != target) ++t->stat.anchor_restores;
        *scroll = target;
        return true;
    }
    TranscriptRecord *rec = &t->records[index];
    int target;
    if (surface_live_of(rec, t->anchor.surface)) {
        TranscriptSurface s = t->anchor.surface;
        int y = surface_y_of(rec, s);
        int height = surface_h_of(rec, s);
        int offset = t->anchor.offset;
        /* A positive offset is clamped to the surface's last pixel so a
            surface that shrank keeps the anchor inside it -- height
            itself would name the next surface's top, outside
            [0, height); a negative offset (the viewport top above the
            surface, in a gap or above a turn) restores exactly and is
            never clamped. */
        if (offset >= 0 && height > 0 && offset > height - 1)
            offset = height - 1;
        target = y + offset;
    } else {
        /* The anchored surface left the layout (e.g. a reasoning viewport
            collapsed under the reader): the turn's body top is the honest
            fallback. */
        target = rec->body_live ? rec->body_y : rec->y;
    }
    if (*scroll != target) ++t->stat.anchor_restores;
    *scroll = target;
    return true;
}

/* Pins the anchor to the reader's current position after a reveal: the
    reveal scroll is the new reading position, so the mode becomes FREE and
    the anchor is captured from that exact position (whatever record or
    surface the viewport top lands on, including gaps and the space above
    the revealed turn, which restore exactly through negative offsets). */
static void anchor_pin_current(Transcript *t) {
    t->follow = TRANSCRIPT_FOLLOW_FREE;
    t->user_scroll_pending = false;
    anchor_capture(t);
}

/* ---- Per-conversation anchor table (linear, stable-ID keyed) -------------
   One entry per conversation id, searched linearly. Conversation ids are
   monotone store counters with gaps after deletion, so the table is never
   indexed by id. A switch saves the departing conversation's anchor into
   the table (transcript_invalidate) and loads the arriving conversation's
   entry at the next render's switch detection: a valid saved anchor sets
   FREE and restores it; a missing or stale entry sets BOTTOM and clears
   the anchor -- a fresh conversation is never blessed with FREE and no
   anchor. A render also prunes entries whose conversation no longer
   exists, since a session can mint far more distinct ids than the table
   holds. */

static TranscriptAnchorEntry *anchor_slot(Transcript *t,
    uint64_t conversation) {
    for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++) {
        TranscriptAnchorEntry *entry = &t->conversation_anchors[i];
        if (entry->used && entry->conversation == conversation) return entry;
    }
    return NULL;
}

/* Drops entries whose conversation id is no longer present. Ids are never
    reused, so a deleted conversation's entry can never be matched again;
    pruning them keeps the table live across arbitrarily many distinct ids
    instead of assuming only 128 exist per session. */
static void anchor_table_prune(Transcript *t, const Chat *chat) {
    if (!chat || chat->conversation_count < 0) return;
    for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++) {
        TranscriptAnchorEntry *entry = &t->conversation_anchors[i];
        if (!entry->used) continue;
        bool present = false;
        for (int j = 0; j < (int)chat->conversation_count; j++)
            if (chat->conversations[j].id == entry->conversation) {
                present = true;
                break;
            }
        if (!present) memset(entry, 0, sizeof *entry);
    }
}

/* Stores the active anchor for its own conversation. A reader who left the
    conversation without a position -- BOTTOM, or an anchor that no longer
    names this conversation -- has no anchor to store, and any older entry
    the conversation still holds must be cleared: leaving a stale FREE
    entry behind would restore the old position on a later return instead
    of landing BOTTOM. */
static void anchor_table_save(Transcript *t) {
    if (!t->active_conversation) return;
    TranscriptAnchorEntry *entry = anchor_slot(t, t->active_conversation);
    if (!t->anchor.valid || t->anchor.conversation != t->active_conversation) {
        if (entry) memset(entry, 0, sizeof *entry);
        return;
    }
    if (!entry) {
        for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++) {
            if (t->conversation_anchors[i].used) continue;
            entry = &t->conversation_anchors[i];
            entry->used = true;
            entry->conversation = t->anchor.conversation;
            break;
        }
    }
    if (entry) entry->anchor = t->anchor;
    else { /* unreachable after pruning: live conversations never exceed
              the table, and the active conversation's entry must exist */
        t->anchor.valid = false;
    }
}

/* True when the saved anchor still names a live message of the arriving
    conversation. Without the chat the check degrades to the identity
    fields; with it, an anchor whose message was deleted is stale. */
static bool anchor_entry_live(const Chat *chat, uint64_t conversation,
    const TranscriptAnchor *anchor) {
    if (anchor->conversation != conversation || !anchor->message) return false;
    if (!chat || chat->active < 0 ||
        chat->active >= chat->conversation_count ||
        chat->conversations[chat->active].id != conversation) return true;
    return chat_message_index_by_id(chat, chat->active,
        anchor->message) >= 0;
}

/* Loads the named conversation's anchor as the active one and restores the
    follow mode to match it: a valid saved anchor re-enters FREE (the
    reader's position survives the switch); a missing or stale entry --
    including one whose saved message no longer exists -- sets BOTTOM,
    clears the anchor and drops the stale entry, so a fresh conversation is
    never blessed with FREE with nothing to restore and a dead position is
    never restored. Returns true when a valid anchor was restored. */
static bool anchor_table_load(Transcript *t, const Chat *chat,
    uint64_t conversation) {
    if (!conversation) {
        transcript_set_follow(t, TRANSCRIPT_FOLLOW_BOTTOM);
        return false;
    }
    TranscriptAnchorEntry *entry = anchor_slot(t, conversation);
    if (entry && entry->anchor.valid &&
        anchor_entry_live(chat, conversation, &entry->anchor)) {
        t->anchor = entry->anchor;
        transcript_set_follow(t, TRANSCRIPT_FOLLOW_FREE);
        ++t->stat.conv_anchor_restores;
        return true;
    }
    if (entry) memset(entry, 0, sizeof *entry);   /* stale entry dropped */
    transcript_set_follow(t, TRANSCRIPT_FOLLOW_BOTTOM);
    return false;
}

/* Applies and clears the reader state captured at eviction (restore after
    rebind): each surface's saved selection and the reasoning viewport's
    inner scroll are applied to a re-certified bound record and cleared
    only after their successful application; a surface that is still
    missing retains its capture for the next render's retry. Called from
    place_and_scroll AFTER the surfaces are placed, because the placement
    transactions (SetWindowPos on a hidden-then-reshown viewport) reset a
    re-shown reasoning viewport's inner scroll -- the restoration must
    follow them to stick. Programmatic selection changes are suppressed by
    the applying guard. */
static void restore_saved_reader_state(Transcript *t, int index) {
    TranscriptRecord *rec = &t->records[index];
    if (rec->slot < 0 || !record_certified(t, rec)) return;
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++) {
        if (rec->saved_sel_min[k] < 0) continue;
        RichTextControl *control = transcript_surface(t, index,
            (TranscriptSurface)k);
        if (!control) continue;   /* retained for a later render's retry */
        CHARRANGE sel = { rec->saved_sel_min[k], rec->saved_sel_max[k] };
        t->applying = true;
        SendMessageW(control->window, EM_EXSETSEL, 0, (LPARAM)&sel);
        t->applying = false;
        rec->saved_sel_min[k] = -1;
        rec->saved_sel_max[k] = -1;
        ++t->stat.eviction_restores;
        ++t->stat.selection_restores;
    }
    if (rec->saved_reason_scroll >= 0) {
        RichTextControl *control = transcript_surface(t, index,
            TRANSCRIPT_REASON);
        if (control) {
            POINT point = { 0, rec->saved_reason_scroll };
            SendMessageW(control->window, EM_SETSCROLLPOS, 0, (LPARAM)&point);
            rec->saved_reason_scroll = -1;
            ++t->stat.eviction_restores;
            ++t->stat.reason_scroll_restores;
        }
    }
}

static void place_turn_control(Transcript *t, RichTextControl *control,
    bool live, int y, int height, int scroll, int page, int inset) {
    if (!control) return;
    if (!live) {
        focus_release_surface(t, control->window);
        ShowWindow(control->window, SW_HIDE);
        return;
    }
    int top = y - scroll;
    if (top >= page || top + height <= 0) {
        focus_release_surface(t, control->window);
        ShowWindow(control->window, SW_HIDE);
        return;
    }
    SetWindowPos(control->window, NULL, t->view_margin + inset, top,
        t->view_width - 2 * inset, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(control->window, SW_SHOWNOACTIVATE);
}

bool transcript_reveal_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    if (!t || !t->view || index < 0 || index >= t->record_count) return false;
    if (t->bounded && feed) {
        RealizeResult r;
        realize_loop(t, feed, false, index, true, &r);
        anchor_pin_current(t);
        place_and_scroll(t, feed, false);
    } else {
        TranscriptRecord *rec = &t->records[index];
        int top = rec->y;
        int bottom = rec->y + rec->height;
        if (top < t->view_scroll) t->view_scroll = top;
        else if (bottom > t->view_scroll + t->view_page)
            t->view_scroll = rec->height > t->view_page
                ? top : bottom - t->view_page;
        anchor_pin_current(t);
        transcript_position(t, feed, false);
    }
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
    transcript_position(t, NULL, follow);
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
    update (or a reasoning append) does not rewrite the body. Assistant
    Markdown bodies additionally depend on their layout currency (width, DPI
    and theme epoch), which only a successful assistant body write stamps; a
    verbatim non-assistant body is never made stale by a layout change. */
static bool body_current(const Transcript *t, const TranscriptRecord *rec,
    uint64_t conversation, uint64_t message, uint64_t body_revision,
    ChatRole role) {
    if (!(rec->rendered_valid && rec->conversation == conversation &&
        rec->message == message && rec->body_revision == body_revision &&
        rec->role == role)) return false;
    if (role != CHAT_ROLE_ASSISTANT) return true;
    return rec->body_layout_width == t->view_width &&
        rec->body_layout_dpi == t->dpi &&
        rec->body_layout_theme == t->theme_epoch;
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
    if (rich_text_has_selection(control)) {
        rec->head_pending = true;
        rec->blocked_debt = true;
        return false;
    }
    t->applying = true;
    rich_text_set_head(control, role, has_row ? row : NULL);
    t->applying = false;
    rec->head_pending = false;
    rec->measured_valid = false;
    rec->measured_estimated = false;
    if (t->render_active) ++t->round_transitions;
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
    if (rich_text_has_selection(control)) {
        rec->body_pending = true;
        rec->blocked_debt = true;
        return false;
    }
    t->applying = true;
    bool ok = true;
    if (role == CHAT_ROLE_ASSISTANT)
        ok = rich_text_set_markdown_width(control, role, text, t->view_width);
    else
        rich_text_set_block(control, role, text);
    t->applying = false;
    if (!ok) {
        /* The write did not land: keep the debt so a later pass retries, and
           certify nothing -- no revision, no layout currency, no measurement
           success. */
        rec->body_pending = true;
        return false;
    }
    rec->body_pending = false;
    rec->measured_valid = false;
    rec->measured_estimated = false;
    if (role == CHAT_ROLE_ASSISTANT) {
        rec->body_layout_width = t->view_width;
        rec->body_layout_dpi = t->dpi;
        rec->body_layout_theme = t->theme_epoch;
    }
    if (t->render_active) ++t->round_transitions;
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
    if (rich_text_has_selection(control)) {
        rec->meta_pending = true;
        rec->blocked_debt = true;
        return false;
    }
    wchar_t info[768];
    format_stats(g, info, 768);
    t->applying = true;
    rich_text_set_meta(control, info, g->error);
    t->applying = false;
    rec->meta_pending = false;
    rec->measured_valid = false;
    rec->measured_estimated = false;
    if (t->render_active) ++t->round_transitions;
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
    if (rich_text_has_selection(control)) {
        rec->reason_pending = true;
        rec->blocked_debt = true;
        return false;
    }
    t->applying = true;
    rich_text_set_reasoning(control, text);
    t->applying = false;
    rec->reason_pending = false;
    rec->measured_valid = false;
    rec->measured_estimated = false;
    if (t->render_active) ++t->round_transitions;
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

/* One destructive-write attempt per surface per render inside a realize
    loop (attempt stamps; write bits live at k+4, creation bits at k). The
    stamp is render-scoped: the first attempt in a new render epoch clears
    the previous render's bits, so nothing can ride across renders. Outside
    a loop — external deltas, flushes, selection changes — every call may
    attempt; those are the documented retry paths. */
static bool write_allowed(Transcript *t, TranscriptRecord *rec,
    TranscriptSurface surface) {
    if (!t->render_active) return true;
    if (rec->attempted_epoch != t->render_epoch) {
        rec->attempted_epoch = t->render_epoch;
        rec->attempted_kinds = 0;
    }
    uint32_t bit = 0x10u << (int)surface;
    if (rec->attempted_kinds & bit) return false;
    rec->attempted_kinds |= bit;
    return true;
}

/* Synchronizes one record's surfaces with its message. A message whose
   identity still matches what was rendered skips every destructive write;
   control realization, callback wiring and visibility reconciliation always
   run. Partial inlining would clone this past its bounds guard and trip the
   array-bounds analysis; the function is far too heavy to inline anyway. */
__attribute__((noinline))
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
    /* Use-touch: preparing an already-bound record refreshes its eviction
       recency, so LRU reflects actual use, not just the last bind. */
    if (rec->slot >= 0) rec->last_used = ++t->clock;
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
    bool body_fresh = certified && body_current(t, rec, c->id, m->id,
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
        if ((!fresh || rec->head_pending || created) &&
            write_allowed(t, rec, TRANSCRIPT_HEAD))
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
        write_allowed(t, rec, TRANSCRIPT_BODY) &&
        write_body(t, rec, index, m->role, chat_message_text(m)))
        rec->body_revision = m->body_revision;
    if (assistant && terminal) {
        bool created = transcript_surface(t, index, TRANSCRIPT_META) == NULL;
        ensure_surface(t, feed, index, TRANSCRIPT_META, false);
        if ((!fresh || rec->meta_pending || created) &&
            write_allowed(t, rec, TRANSCRIPT_META))
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
                if ((!fresh || rec->reason_pending || created || resumed ||
                    !same_message) &&
                    write_allowed(t, rec, TRANSCRIPT_REASON))
                    write_reasoning(t, rec, index, chat_message_reasoning(m));
            }
        }
    } else {
        hide_surface(t, index, TRANSCRIPT_REASON);
        rec->reason_live = false;
    }

    /* Reader state captured at eviction is restored after placement by
        place_and_scroll (restore_saved_reader_state): the placement
        transactions reset a re-shown reasoning viewport's inner scroll, so
        the restoration must follow them, and a surface that is still
        missing retains its capture for the next render's retry. */
}

/* Fills the decision-time policy view of every record: geometry, streaming
    flag, decision-time debt, expanded reasoning, reader focus and the LRU
    stamp. Shared by the capacity checkpoint and the capacity-governed
    enforcement so every production decision reads the same predicates. */
static void fill_policy_items(Transcript *t, const TranscriptFeed *feed,
    int count, TranscriptPolicyItem *items) {
    HWND focus = decision_focus(t);
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
        items[i].created_kinds = 0;
        if (focus)
            for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT && !items[i].focused;
                s++) {
                RichTextControl *control = transcript_surface(t, i,
                    (TranscriptSurface)s);
                items[i].focused = control && control->window == focus;
            }
    }
}

/* The dynamic required capacity of the governed pool, from pixel geometry
    alone: records that can intersect the strict viewport (at most
    ceil(page/h_min) + 1, every height read at the h_min floor), the
    overscan band above and below (ceil(2*overscan/h_min)), two Tier-A
    protected records anywhere (the streaming turn and the focused
    record), the Tier-B allowance and the spare headroom — clamped to the
    512-slot arena. Pure geometry: no per-record scan, so the value is a
    true upper bound of the padded window's membership and is monotone in
    the page. */
static int governed_required(const Transcript *t) {
    int h_min = px(t, 8);
    if (h_min < 1) h_min = 1;
    return transcript_policy_required_slots(t->view_page, h_min,
        2 * px(t, TRANSCRIPT_OVERSCAN_DIPS), TRANSCRIPT_TIER_B_ALLOWANCE,
        TRANSCRIPT_SPARE_SLOTS, CHAT_MAX_MESSAGES);
}

/* Raises the governed slot budget to the current required capacity.
    Raise-only: the limit never shrinks mid-shape, and every raise is
    counted. Called each realize-loop round before any selection, so
    binding always operates inside a limit that already covers the
    membership window the round is about to use. */
static void raise_slot_limit(Transcript *t) {
    int required = governed_required(t);
    if (required > t->slot_limit) {
        t->slot_limit = required;
        ++t->stat.capacity_raises;
    }
}

/* The single documented capacity checkpoint. The required slot count is
    computed every render and asserted against the pool: a P-CAP breach
    (needed > slot_capacity) is a contract error and fails fast -- it is
    never clamped away or discarded. Retain-all satisfies the precondition
    by construction (slot_capacity == CHAT_MAX_MESSAGES >= record count >=
    |V u P|), so the assertion cannot fire this pass. Bounded computes the
    same dynamic geometric required capacity that raises slot_limit. Debt
    is the full decision-time predicate (pending writes OR a live
    selection), so an unfocused selected turn is protected. Geometry comes
    from the current layout, so unmeasured records claim h_min of space --
    the same conservative bound the policy documents. */
static void capacity_checkpoint(Transcript *t, const TranscriptFeed *feed,
    int count, int pad) {
    (void)pad;               /* both evaluations pad internally */
    t->bound_count = transcript_bound_slots(t);
    if (count <= 0) { t->policy_needed = 0; return; }
    int needed;
    if (t->bounded) {
        /* Bounded: the dynamic geometric required capacity the governed
            limit already covers (raise-only; no trim). */
        needed = governed_required(t);
    } else {
        TranscriptPolicyItem items[CHAT_MAX_MESSAGES];
        fill_policy_items(t, feed, count, items);
        /* Retain-all keeps the strict-viewport |V u P| diagnostic. */
        needed = transcript_policy_needed_slots(items, count,
            t->view_scroll, t->view_page, px(t, 8),
            TRANSCRIPT_SPARE_SLOTS);
    }
    /* Fail-fast P-CAP (invariant I10): the pool always covers the bound
        set the engine may realize. */
    assert(needed <= t->slot_capacity);
    t->policy_needed = needed;
}

/* ---- Bounded realization engine -------------------------------------------
   Retain-all mode never runs any of this: transcript_render's retain-all
   branch prepares every record and measures every live surface through
   transcript_layout_from, exactly as before. */

/* One turn's geometry for the bounded layout walk: which surface families
    contribute and at what measured or cached heights. */
typedef struct {
    bool live[TRANSCRIPT_SURFACE_COUNT];
    int h[TRANSCRIPT_SURFACE_COUNT];
} TurnGeometry;

/* Writes the exact-measurement stamp. all_exact false records a flagged
    estimate: the heights remain usable for placement but the stamp never
    certifies them, and every later pass retries the exact measurement. */
static void write_stamp(Transcript *t, TranscriptRecord *rec,
    const TurnState *s, bool all_exact) {
    bool was_estimated = rec->measured_estimated;
    rec->measured_width = t->view_width;
    rec->measured_dpi = t->dpi;
    rec->measured_theme = t->theme_epoch;
    rec->measured_conversation = s->c->id;
    rec->measured_message = s->m->id;
    rec->measured_revision = s->m->revision;
    rec->measured_body_revision = s->m->body_revision;
    rec->measured_role = s->m->role;
    rec->measured_state = s->m->generation.state;
    rec->measured_running = s->running;
    rec->measured_content_started = s->content_started;
    rec->measured_reasoning_open = s->m->reasoning_open;
    rec->measured_row = s->has_row;
    rec->measured_valid = all_exact;
    rec->measured_estimated = !all_exact;
    if (all_exact && was_estimated) ++t->stat.measure_retries;
}

/* Arithmetic height estimate when no measurement surface is available:
    explicit newlines plus a wrapped-line approximation at the current
    width, in the same line-height units as the EM_GETLINECOUNT fallback.
    Always flagged ESTIMATE by the caller. */
static int estimate_text_height(Transcript *t, const wchar_t *text,
    float size_dips) {
    int lines = 1;
    int units = 0;
    for (const wchar_t *p = text ? text : L""; *p; ++p) {
        if (*p == L'\n') ++lines;
        else if (*p != L'\r') ++units;
    }
    int char_w = px(t, size_dips * 0.55f);
    if (char_w < 1) char_w = 1;
    int inset = px(t, 10);
    int usable = t->view_width > 2 * inset ? t->view_width - 2 * inset : t->view_width;
    if (usable < 1) usable = 1;
    int wrapped = (units * char_w + usable - 1) / usable;
    if (wrapped > lines) lines = wrapped;
    int height = lines * px(t, t->theme.ui_size * 1.45f);
    int minimum = px(t, 8);
    return height < minimum ? minimum : height;
}

/* One record's geometry per the bounded matrix. Never live-measures
    uncertified surfaces; the displayed-vs-desired distinction is carried by
    the TurnState currency checks and the pending/selection preflight.

    - bound + certified + (pending debt | live selection): the turn displays
      a lagging same-message revision or the reader is selecting: measure the
      DISPLAYED surfaces live, cache nothing (qualified blocked_debt
      geometry).
    - bound + certified + content current + stamp current: cached heights.
    - bound + certified + content current, stamp stale: live re-measure
      (displayed == desired), re-stamp. Surfaces that do not exist keep
      their reserved cached height.
    - otherwise (unbound, uncertified, or content stale): the shared hidden
      measurer over the current feed content — the content the next prepare
      will write — then stamp. A dead measurer degrades to flagged
      estimates; a resize storm defers this pass (cached heights, stamp left
      stale for the settle render). */
static void height_of_turn(Transcript *t, const TranscriptFeed *feed, int i,
    TurnGeometry *g) {
    if (i < 0 || i >= CHAT_MAX_MESSAGES) return;
    TranscriptRecord *rec = &t->records[i];
    memset(g, 0, sizeof *g);
    TurnState s;
    if (!turn_state(t, feed, i, &s)) return;
    bool certified = record_certified(t, rec);
    bool lagging = rec->head_pending || rec->body_pending ||
        rec->meta_pending || rec->reason_pending;
    bool selected = false;
    for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT && !selected; k++) {
        RichTextControl *control = transcript_surface(t, i,
            (TranscriptSurface)k);
        selected = control && rich_text_has_selection(control);
    }
    bool fully_current = s.head_current && s.body_current && s.meta_current &&
        s.reason_current;
    g->live[TRANSCRIPT_HEAD] = s.shape_head;
    g->live[TRANSCRIPT_BODY] = true;
    g->live[TRANSCRIPT_REASON] = s.shape_reason;
    g->live[TRANSCRIPT_META] = s.shape_meta;

    if (certified && (lagging || selected)) {
        /* Displayed-true geometry: measure what is actually shown, cache
           nothing until the debt applies. */
        g->live[TRANSCRIPT_HEAD] = rec->head_live;
        g->live[TRANSCRIPT_BODY] = rec->body_live;
        g->live[TRANSCRIPT_REASON] = rec->reason_live;
        g->live[TRANSCRIPT_META] = rec->meta_live;
        if (g->live[TRANSCRIPT_HEAD])
            g->h[TRANSCRIPT_HEAD] = measure_control_q(t,
                transcript_surface(t, i, TRANSCRIPT_HEAD), t->view_width, NULL);
        if (g->live[TRANSCRIPT_BODY])
            g->h[TRANSCRIPT_BODY] = measure_control_q(t,
                transcript_surface(t, i, TRANSCRIPT_BODY), t->view_width, NULL);
        if (g->live[TRANSCRIPT_META])
            g->h[TRANSCRIPT_META] = measure_control_q(t,
                transcript_surface(t, i, TRANSCRIPT_META), t->view_width, NULL);
        rec->measured_valid = false;
        rec->measured_estimated = false;
        return;
    }

    bool debt_free = !lagging && !selected;
    if (debt_free && stamp_current(t, rec, &s) &&
        (rec->slot < 0 || (certified && fully_current))) {
        g->h[TRANSCRIPT_HEAD] = rec->head_h;
        g->h[TRANSCRIPT_BODY] = rec->body_h;
        g->h[TRANSCRIPT_META] = rec->meta_h;
        return;                              /* cached, certified geometry */
    }

    if (t->resizing && !transcript_policy_visible(rec->y, rec->height,
            t->view_scroll, t->view_page, px(t, 8))) {
        /* Resize storm, off-screen record: re-measurement is deferred to the
           settle render; keep the last known heights and leave the stamp
           stale. This sits above the re-measure branches so a stale stamp
           does not route an off-screen record into them; strict-viewport
           visible records still live-measure each step (the storm
           contract). */
        g->h[TRANSCRIPT_HEAD] = g->live[TRANSCRIPT_HEAD] ? rec->head_h : 0;
        g->h[TRANSCRIPT_BODY] = rec->body_h;
        g->h[TRANSCRIPT_META] = g->live[TRANSCRIPT_META] ? rec->meta_h : 0;
        return;
    }

    bool missing_required = (g->live[TRANSCRIPT_HEAD] &&
        !transcript_surface(t, i, TRANSCRIPT_HEAD)) ||
        (g->live[TRANSCRIPT_BODY] &&
         !transcript_surface(t, i, TRANSCRIPT_BODY)) ||
        (g->live[TRANSCRIPT_META] &&
         !transcript_surface(t, i, TRANSCRIPT_META));
    if (certified && fully_current && !missing_required) {
        /* Displayed == desired: live re-measure at the current inputs. */
        bool all_exact = true;
        TranscriptMeasureQuality q;
        if (g->live[TRANSCRIPT_HEAD]) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_HEAD);
            if (control) {
                g->h[TRANSCRIPT_HEAD] = measure_control_q(t, control,
                    t->view_width, &q);
                if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
            } else g->h[TRANSCRIPT_HEAD] = rec->head_h;   /* reserved */
        }
        {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_BODY);
            if (control) {
                g->h[TRANSCRIPT_BODY] = measure_control_q(t, control,
                    t->view_width, &q);
                if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
            } else g->h[TRANSCRIPT_BODY] = rec->body_h;
        }
        if (g->live[TRANSCRIPT_META]) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_META);
            if (control) {
                g->h[TRANSCRIPT_META] = measure_control_q(t, control,
                    t->view_width, &q);
                if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
            } else g->h[TRANSCRIPT_META] = rec->meta_h;
        }
        write_stamp(t, rec, &s, all_exact);
        return;
    }

    if (!ensure_measurer(t)) {
        /* Flagged arithmetic estimates; retried on every later pass. */
        if (g->live[TRANSCRIPT_HEAD]) {
            wchar_t head[96];
            swprintf(head, 96, L"%ls%ls",
                s.m->role == CHAT_ROLE_USER ? L"You" :
                s.m->role == CHAT_ROLE_ERROR ? L"Error" :
                s.m->role == CHAT_ROLE_SYSTEM ? L"System" : L"Assistant",
                s.has_row ? L"\nrow" : L"");
            g->h[TRANSCRIPT_HEAD] = estimate_text_height(t, head,
                t->theme.ui_size);
        }
        g->h[TRANSCRIPT_BODY] = estimate_text_height(t, chat_message_text(s.m),
            t->theme.ui_size);
        if (g->live[TRANSCRIPT_META]) {
            wchar_t info[768];
            format_stats(&s.m->generation, info, 768);
            g->h[TRANSCRIPT_META] = estimate_text_height(t, info,
                t->theme.small_size);
        }
        ++t->stat.estimates;
        write_stamp(t, rec, &s, false);
        return;
    }

    /* Exact off-screen measurement of the desired content. Width is applied
        before the first content write so layout wraps at the live surfaces'
        width from the first request-resize; measure_control_q re-checks and
        never needs to resize afterwards. */
    measurer_set_width(t);
    bool all_exact = true;
    TranscriptMeasureQuality q;
    if (g->live[TRANSCRIPT_HEAD]) {
        rich_text_set_head(&t->measurer, s.m->role, s.has_row ? s.row : NULL);
        g->h[TRANSCRIPT_HEAD] = measure_control_q(t, &t->measurer,
            t->view_width, &q);
        if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
    }
    bool body_written = true;
    if (s.assistant)
        body_written = rich_text_set_markdown_width(&t->measurer, s.m->role,
            chat_message_text(s.m), t->view_width);
    else
        rich_text_set_block(&t->measurer, s.m->role, chat_message_text(s.m));
    if (body_written) {
        g->h[TRANSCRIPT_BODY] = measure_control_q(t, &t->measurer,
            t->view_width, &q);
        if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
    } else {
        /* The write did not land (width assertion failed or the verbatim
           fallback was incomplete): the measurer still holds stale or empty
           content, so never measure it and never stamp that as exact. Record
           an explicit estimate and leave the stamp retryable. */
        g->h[TRANSCRIPT_BODY] = estimate_text_height(t,
            chat_message_text(s.m), t->theme.ui_size);
        all_exact = false;
        ++t->stat.estimates;
    }
    if (g->live[TRANSCRIPT_META]) {
        wchar_t info[768];
        format_stats(&s.m->generation, info, 768);
        rich_text_set_meta(&t->measurer, info, s.m->generation.error);
        g->h[TRANSCRIPT_META] = measure_control_q(t, &t->measurer,
            t->view_width, &q);
        if (q != TRANSCRIPT_MEASURE_EXACT) all_exact = false;
    }
    write_stamp(t, rec, &s, all_exact);
}

/* The bounded layout walk: heights per the matrix, then the same stacking
    arithmetic as retain-all's layout (head, reasoning viewport with its
    fixed height between gaps, body, metadata footer between gaps). */
static bool geometry_pass(Transcript *t, const TranscriptFeed *feed) {
    bool changed = false;
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    int y = t->view_margin;
    for (int i = 0; i < count; i++) {
        TranscriptRecord *rec = &t->records[i];
        TurnGeometry g;
        height_of_turn(t, feed, i, &g);
        int old = rec->height;
        rec->y = y;
        int cursor = y;
        if (g.live[TRANSCRIPT_HEAD]) {
            rec->head_h = g.h[TRANSCRIPT_HEAD];
            rec->head_y = cursor;
            cursor += rec->head_h;
        }
        if (g.live[TRANSCRIPT_REASON]) {
            cursor += t->view_reason_gap;
            rec->reason_h = px(t, 150);
            rec->reason_y = cursor;
            cursor += rec->reason_h + t->view_reason_gap;
        }
        if (g.live[TRANSCRIPT_BODY]) {
            rec->body_h = g.h[TRANSCRIPT_BODY];
            rec->body_y = cursor;
            cursor += rec->body_h;
        }
        if (g.live[TRANSCRIPT_META]) {
            cursor += t->view_meta_gap;
            rec->meta_h = g.h[TRANSCRIPT_META];
            rec->meta_y = cursor;
            cursor += rec->meta_h;
        }
        rec->height = cursor - y;
        if (rec->height != old) changed = true;
        y = cursor + t->view_gap;
    }
    t->view_content = y;
    return changed;
}

/* Creation-attempted-this-render check for one surface kind (bit k is the
    creation stamp; the write stamp lives at bit k+4). */
static bool creation_attempted(const TranscriptRecord *rec, uint32_t epoch,
    TranscriptSurface surface) {
    return rec->attempted_epoch == epoch &&
        (rec->attempted_kinds & (1u << (int)surface));
}

/* Realization membership: the overscan-widened viewport union the class-
    protected records, with Tier-B protection bounded by the allowance
    (LRU-ranked). The caller passes the round's shared policy view; the
    scroll/page come from the transcript, whose pixel-space window is the
    same one the capacity checkpoint computes. */
static bool record_in_window(const Transcript *t, int i,
    const TranscriptPolicyItem *items, int count) {
    int h_min = px(t, 8);
    int pad = px(t, TRANSCRIPT_OVERSCAN_DIPS);
    return transcript_policy_in_window(items, count, i,
        t->view_scroll - pad, t->view_page + 2 * pad, h_min,
        TRANSCRIPT_TIER_B_ALLOWANCE);
}

/* Retain-all may have filled the pool before the bounded test seam activates.
    Release only records outside the current padded/protected set so activation
    immediately resumes viewport-shaped binding without touching reader state. */
static void release_outside_window(Transcript *t, int forced_index,
    const TranscriptPolicyItem *items, int count) {
    for (int s = 0; s < t->slot_capacity; s++) {
        int index = t->slots[s].record;
        if (index >= 0 && index != forced_index &&
            !record_in_window(t, index, items, count))
            unbind_slot(t, s);
    }
}

/* The per-slot decision view used by victim selection: bound[s].index is the
    bound record (or -1), with geometry, protection and LRU from the record. */
static void fill_bound_items(Transcript *t, const TranscriptFeed *feed,
    TranscriptPolicyItem *bound) {
    HWND focus = decision_focus(t);
    for (int p = 0; p < t->slot_capacity; p++) {
        TranscriptPolicyItem *item = &bound[p];
        int bound_record = t->slots[p].record;
        item->index = bound_record;
        item->created_kinds = slot_created_kinds(t, p);
        item->y = item->height = 0;
        item->streaming = item->focused = item->debt = item->expanded = false;
        item->last_used = 0;
        if (bound_record < 0) continue;
        TranscriptRecord *other = &t->records[bound_record];
        item->y = other->y;
        item->height = other->height;
        item->streaming = feed->generating &&
            feed->request_conversation == feed->chat->active &&
            bound_record == feed->request_message;
        item->debt = transcript_record_debt(t, bound_record);
        item->expanded = other->reason_live;
        item->last_used = other->last_used;
        if (focus)
            for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT && !item->focused;
                k++)
                item->focused = control_window_live(
                    &t->slots[p].surface[k]) &&
                    t->slots[p].surface[k].window == focus;
    }
}

/* Records one surface's would_defer debt at preflight time: the destructive
    write has not been attempted yet, so no *_pending flag exists — set it
    directly (the user's first-detection contract) so a later
    EN_SELCHANGE/apply sweep has recorded debt to apply. Later detections
    with the flag already set are plain blocked_debt. */
static void record_deferral(TranscriptRecord *rec, TranscriptSurface surface) {
    switch (surface) {
    case TRANSCRIPT_HEAD: rec->head_pending = true; break;
    case TRANSCRIPT_BODY: rec->body_pending = true; break;
    case TRANSCRIPT_META: rec->meta_pending = true; break;
    case TRANSCRIPT_REASON: rec->reason_pending = true; break;
    default: break;
    }
}

static bool classify_turn(Transcript *t, const TranscriptFeed *feed, int i,
    bool in_set) {
    TranscriptRecord *rec = &t->records[i];
    rec->blocked_debt = false;
    rec->blocked_resource = false;
    if (!in_set) return false;
    TurnState s;
    if (!turn_state(t, feed, i, &s)) return false;
    if (rec->slot < 0) return true;          /* unbound: needs a slot */
    bool actionable = false, debt = false, resource = false;
    /* A *_live bookkeeping bit whose window is gone (e.g. an external
        DestroyWindow behind the module's back) leaves the family claiming
        liveness it does not have: it must be treated exactly like the
        never-created case, regardless of shape or currency. */
    bool head_missing = s.shape_head && !transcript_surface(t, i, TRANSCRIPT_HEAD);
    bool body_missing = !transcript_surface(t, i, TRANSCRIPT_BODY);
    bool meta_missing = s.shape_meta && !transcript_surface(t, i, TRANSCRIPT_META);
    bool reason_missing = s.shape_reason && !transcript_surface(t, i, TRANSCRIPT_REASON);
    /* head/row family */
    if (s.shape_head || rec->head_live || rec->head_pending) {
        bool destructive = !s.head_current || rec->head_pending;
        bool reconcile = rec->head_live != s.shape_head;
        if (head_missing) reconcile = true;
        if (destructive || reconcile) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_HEAD);
            if (!control) {
                if (creation_attempted(rec, t->render_epoch, TRANSCRIPT_HEAD))
                    resource = true;
                else actionable = true;
            } else if (destructive && rich_text_has_selection(control)) {
                record_deferral(rec, TRANSCRIPT_HEAD);
                debt = true;
            }
            else actionable = true;
        }
    }
    /* body family: always wanted */
    {
        bool destructive = !s.body_current || rec->body_pending;
        if (destructive || !rec->body_live || body_missing) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_BODY);
            if (!control) {
                if (creation_attempted(rec, t->render_epoch, TRANSCRIPT_BODY))
                    resource = true;
                else actionable = true;
            } else if (destructive && rich_text_has_selection(control)) {
                record_deferral(rec, TRANSCRIPT_BODY);
                debt = true;
            }
            else actionable = true;
        }
    }
    /* meta family */
    if (s.shape_meta || rec->meta_live || rec->meta_pending) {
        bool destructive = !s.meta_current || rec->meta_pending;
        bool reconcile = rec->meta_live != s.shape_meta;
        if (meta_missing) reconcile = true;
        if (destructive || reconcile) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_META);
            if (!control) {
                if (creation_attempted(rec, t->render_epoch, TRANSCRIPT_META))
                    resource = true;
                else actionable = true;
            } else if (destructive && rich_text_has_selection(control)) {
                record_deferral(rec, TRANSCRIPT_META);
                debt = true;
            }
            else actionable = true;
        }
    }
    /* reasoning family */
    if (s.shape_reason || rec->reason_live || rec->reason_pending) {
        bool destructive = !s.reason_current || rec->reason_pending;
        bool reconcile = rec->reason_live != s.shape_reason;
        if (reason_missing) reconcile = true;
        if (destructive || reconcile) {
            RichTextControl *control = transcript_surface(t, i, TRANSCRIPT_REASON);
            if (!control) {
                if (creation_attempted(rec, t->render_epoch, TRANSCRIPT_REASON))
                    resource = true;
                else actionable = true;
            } else if (destructive && rich_text_has_selection(control)) {
                record_deferral(rec, TRANSCRIPT_REASON);
                debt = true;
            }
            else actionable = true;
        }
    }
    rec->blocked_debt = debt && !actionable;
    rec->blocked_resource = resource && !actionable;
    return actionable;
}

static bool scan_actionable(Transcript *t, const TranscriptFeed *feed,
    int forced_index, const TranscriptPolicyItem *items, int count,
    bool *actionable) {
    bool any = false;
    for (int i = 0; i < count; i++) {
        bool in_set = i == forced_index || record_in_window(t, i, items,
            count);
        actionable[i] = classify_turn(t, feed, i, in_set);
        if (actionable[i]) any = true;
    }
    return any;
}

/* Dead-record sweep: slots bound to records beyond the live message count
    are unbound (their surfaces are retained for reuse). */
static void unbind_dead_records(Transcript *t) {
    for (int s = 0; s < t->slot_capacity; s++)
        if (t->slots[s].record >= t->record_count) unbind_slot(t, s);
}

/* The bounded fixed-point realize/measure loop. Stable convergence:
    geometry unchanged (no height moved this pass) and no actionable work
    remains; blocked_debt/blocked_resource states are allowed and recorded.
    A round that leaves actionable work with no terminal transition exits
    immediately into the qualified degraded settle. The safety cap covers
    only unexpected geometry/membership propagation and fails closed into
    the documented fallback (prepare every strict-visible member, recompute
    geometry if a debt applied, place without a finality claim). `follow`
    resolves the target scroll inside the loop — bottom-following when the
    reader follows — so the realization window is computed where the reader
    will actually be after placement, not at the stale pre-render scroll.
    `forced_index` forces that record's realization membership; with
    `forced_reveal` it also drives the loop's scroll resolution (the search
    reveal), while without it the record only joins the window — a refresh
    (reasoning toggle, streaming recovery) must never move a FREE reader,
    whose scroll the anchor restore owns. */
static void realize_loop(Transcript *t, const TranscriptFeed *feed,
    bool follow, int forced_index, bool forced_reveal, RealizeResult *out) {
    int cap = t->diagnostic_round_cap >= 0 ? t->diagnostic_round_cap
        : 2 * t->record_count + 4;
    bool actionable[CHAT_MAX_MESSAGES];
    out->rounds = 0;
    out->stable = out->degraded = out->fallback = false;
    t->render_active = true;
    ++t->render_epoch;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        t->records[i].blocked_debt = false;
        t->records[i].blocked_resource = false;
    }
    int last_transitions = -1;
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    for (;;) {
        bool height_changed = geometry_pass(t, feed);
        /* Resolve the scroll against the just-computed content and the real
           viewport page, so the window and the placement agree. */
        RECT client;
        GetClientRect(t->view, &client);
        int page = client.bottom;
        int maximum = t->view_content - page;
        if (maximum < 0) maximum = 0;
        if (follow) t->view_scroll = maximum;
        else if (forced_reveal && forced_index >= 0 && forced_index < count) {
            TranscriptRecord *target = &t->records[forced_index];
            int bottom = target->y + target->height;
            if (target->y < t->view_scroll) t->view_scroll = target->y;
            else if (bottom > t->view_scroll + page)
                t->view_scroll = target->height > page
                    ? target->y : bottom - page;
        } else if (!t->user_scroll_pending && !t->thumb_drag &&
            t->follow == TRANSCRIPT_FOLLOW_FREE && t->anchor.valid) {
            /* Anchor restore against this round's fresh geometry, so the
                realization window forms around where the reader is being
                kept rather than around the stale pre-mutation scroll --
                the anchor record is realized by the very rounds this
                positions the window over. */
            int scroll = t->view_scroll;
            if (anchor_resolve_scroll(t, feed, &scroll)) t->view_scroll = scroll;
        }
        if (t->view_scroll > maximum) t->view_scroll = maximum;
        if (t->view_scroll < 0) t->view_scroll = 0;
        t->view_page = page;
        /* The round's shared policy view is built after the geometry pass,
            so visibility, protection and the Tier-B LRU ranking read the
            fresh heights. */
        TranscriptPolicyItem items[CHAT_MAX_MESSAGES];
        fill_policy_items(t, feed, count, items);
        if (t->bounded_prune_pending) {
            release_outside_window(t, forced_index, items, count);
            t->bounded_prune_pending = false;
        }
        /* Raise the governed slot budget before any selection this round:
            binding and forced eviction happen only inside [0, slot_limit),
            and the limit is raise-only (never trimmed to a target). */
        raise_slot_limit(t);
        bool any_actionable = scan_actionable(t, feed, forced_index, items,
            count, actionable);
        if (!height_changed && !any_actionable) { out->stable = true; break; }
        if (out->rounds >= cap) {
            out->fallback = true;
            ++t->stat.fallback_rounds;
            /* Defensive fallback: force every strict-visible turn through the
               same epoch, then recompute the geometry it may have changed.
               This deliberately does not claim stable finality. */
            for (int i = 0; i < count; i++)
                if (transcript_policy_visible(t->records[i].y,
                        t->records[i].height, t->view_scroll, t->view_page,
                        px(t, 8))) prepare_turn(t, feed, i);
            geometry_pass(t, feed);
            break;
        }
        if (out->rounds > 0 && any_actionable && last_transitions == 0) {
            out->degraded = true;
            ++t->stat.degraded_rounds;
            break;
        }
        t->round_transitions = 0;
        for (int i = 0; i < count; i++)
            if (actionable[i]) prepare_turn(t, feed, i);
        last_transitions = t->round_transitions;
        ++out->rounds;
    }
    t->render_active = false;
    t->stat.rounds = out->rounds;
}

/* Clamps the scroll, syncs the scrollbar, and places every bound record's
    surfaces — the placement half of the old transcript_position. In FREE
    mode, with no user position pending and no active drag, the anchor is
    restored against the just-computed geometry so mutations above the
    reader cannot shift content under them; a pending user position is the
    truth and is captured as the fresh anchor after placement. */
static void place_and_scroll(Transcript *t, const TranscriptFeed *feed,
    bool follow) {
    RECT client;
    GetClientRect(t->view, &client);
    int page = client.bottom;
    int maximum = t->view_content - page;
    if (maximum < 0) maximum = 0;
    if (follow) t->view_scroll = maximum;
    else if (t->user_scroll_pending || t->thumb_drag ||
        t->follow != TRANSCRIPT_FOLLOW_FREE || !t->anchor.valid) {
        if (t->view_scroll > maximum) t->view_scroll = maximum;
        else if (t->view_scroll < 0) t->view_scroll = 0;
    } else {
        int scroll = t->view_scroll;
        if (anchor_resolve_scroll(t, feed, &scroll)) {
            if (scroll > maximum) scroll = maximum;
            if (scroll < 0) scroll = 0;
            t->view_scroll = scroll;
        } else if (t->view_scroll > maximum) t->view_scroll = maximum;
        else if (t->view_scroll < 0) t->view_scroll = 0;
    }
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
    int count = t->record_count;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    for (int i = 0; i < count; i++) {
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
    /* Reader state captured at eviction is applied after the placement
        transactions, so the restoration survives them (see the helper). */
    if (t->bounded) {
        for (int i = 0; i < count; i++) restore_saved_reader_state(t, i);
    }
    /* Moving children does not repaint the background they uncover; the
        container paints every gap and margin itself. */
    InvalidateRect(t->view, NULL, FALSE);
    /* A completed user placement becomes the fresh anchor (FREE mode);
        during a drag the capture waits for the release event. */
    if (t->user_scroll_pending && !t->thumb_drag) {
        anchor_capture(t);
        t->user_scroll_pending = false;
    }
}

void transcript_position(Transcript *t, const TranscriptFeed *feed,
    bool follow) {
    if (!t->view) return;
    /* Funnel: every scroll/wheel/reveal path realizes the window (binds,
        prepares, debt application, blocked-state recording) before placing,
        so a visible record can never be unrealized or foreign. */
    if (t->bounded && feed) {
        RealizeResult r;
        realize_loop(t, feed, follow, -1, true, &r);
    }
    place_and_scroll(t, feed, follow);
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
    const ChatConversation *c = chat_active(chat);
    int count = c ? (int)c->message_count : 0;
    if (count < 0) count = 0;
    if (count > CHAT_MAX_MESSAGES) count = CHAT_MAX_MESSAGES;
    t->record_count = count;
    /* Conversation switch detection: the arriving conversation's anchor is
        loaded from the per-conversation table -- a valid saved anchor
        restores FREE, a missing or stale one sets BOTTOM (a fresh
        conversation is never blessed with FREE and no anchor). Dead
        entries for conversations that no longer exist are pruned first, so
        the table stays live across arbitrarily many distinct ids. */
    uint64_t active_id = c ? c->id : 0;
    if (active_id != t->active_conversation) {
        anchor_table_prune(t, chat);
        anchor_table_load(t, chat, active_id);
        t->active_conversation = active_id;
    }
    if (t->bounded) {
        /* Bounded: release dead bindings, run the fixed-point realize/
            measure loop, checkpoint the overscan-widened capacity, place. */
        unbind_dead_records(t);
        RealizeResult r;
        realize_loop(t, feed, transcript_following(t), -1, true, &r);
        capacity_checkpoint(t, feed, count, px(t, TRANSCRIPT_OVERSCAN_DIPS));
        place_and_scroll(t, feed, transcript_following(t));
        return;
    }
    for (int i = 0; i < count; i++) prepare_turn(t, feed, i);
    for (int i = count; i < CHAT_MAX_MESSAGES; i++) hide_turn(t, i);
    capacity_checkpoint(t, feed, count, 0);
    transcript_layout_from(t, 0, transcript_following(t));
}

/* Rebuilds one turn only, so toggling a row never disturbs another turn's
   viewport or the transcript scroll. */
void transcript_refresh_turn(Transcript *t, const TranscriptFeed *feed,
    int index) {
    const Chat *chat = feed->chat;
    if (chat->active < 0 || chat->active >= chat->conversation_count) return;
    if (index < 0 || index >= CHAT_MAX_MESSAGES ||
        (size_t)index >= chat->conversations[chat->active].message_count)
        return;
    bool pinned = transcript_following(t);
    if (t->bounded) {
        /* A refresh (reasoning toggle, streaming recovery) must never move
            a FREE reader: the loop's forced index joins the window without
            the reveal arithmetic, and the anchor restore owns the scroll. */
        RealizeResult r;
        realize_loop(t, feed, pinned, index, false, &r);
        place_and_scroll(t, feed, pinned);
    } else {
        prepare_turn(t, feed, index);
        transcript_layout_from(t, index, pinned);
    }
}

bool transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index) {
    if (index < 0 || index >= CHAT_MAX_MESSAGES) return true;
    const Chat *chat = feed->chat;
    if (feed->request_conversation < 0 ||
        feed->request_conversation >= chat->conversation_count) return true;
    const ChatConversation *c =
        &chat->conversations[feed->request_conversation];
    if ((size_t)index >= c->message_count) return true;
    const ChatMessage *m = &c->messages[index];
    TranscriptRecord *rec = &t->records[index];
    RichTextControl *body = transcript_surface(t, index, TRANSCRIPT_BODY);
    if (!body) {
        /* The body surface is missing (never created, or its creation
           failed this render). Bounded mode retries the prepare path once
           here — an external-event attempt, not a loop attempt — and
           reports failure so the caller keeps its flush armed for the next
           incoming delta, the 1 Hz sweep, or the next render. */
        if (t->bounded) {
            transcript_refresh_turn(t, feed, index);
            body = transcript_surface(t, index, TRANSCRIPT_BODY);
        }
        if (!body) return false;
    }
    if (rich_text_has_selection(body)) {
        /* The reader holds a selection in the live answer: the destructive
           Markdown rebuild is deferred until the selection clears. */
        rec->body_pending = true;
        rec->blocked_debt = true;
        return true;
    }
    bool pinned = transcript_following(t);
    t->applying = true;
    bool ok = true;
    if (m->role == CHAT_ROLE_ASSISTANT)
        ok = rich_text_set_markdown_width(body, m->role, chat_message_text(m),
            t->view_width);
    else
        rich_text_set_block(body, m->role, chat_message_text(m));
    t->applying = false;
    if (!ok) {
        /* The rebuild did not land: keep the debt and certify nothing so the
           caller's armed flush (or the next render) retries. */
        rec->body_pending = true;
        return false;
    }
    rec->body_pending = false;
    /* This path bypasses prepare_turn(); keep the recorded revision and the
       assistant layout currency in step. */
    rec->body_revision = m->body_revision;
    if (m->role == CHAT_ROLE_ASSISTANT) {
        rec->body_layout_width = t->view_width;
        rec->body_layout_dpi = t->dpi;
        rec->body_layout_theme = t->theme_epoch;
    }
    if (t->bounded) {
        RealizeResult r;
        realize_loop(t, feed, pinned, -1, true, &r);
        place_and_scroll(t, feed, pinned);
    } else {
        transcript_layout_from(t, index, pinned);
    }
    return true;
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
    /* Replacement class: the captured reader state belongs to the departed
        message instance and must never leak into the replacement. */
    for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++)
        rec->saved_sel_min[s] = rec->saved_sel_max[s] = -1;
    rec->saved_reason_scroll = -1;
}

void transcript_invalidate(Transcript *t) {
    bool guard = t->applying;
    t->applying = true;
    transcript_focus_release(t);
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) reset_slot(t, i);
    t->applying = guard;
    /* The reader is leaving the conversation: its anchor moves into the
        per-conversation table (stable-ID keyed) so returning restores the
        reading position. The active anchor is then cleared -- the follow
        mode survives and the host's switch sequence (or the next user
        scroll) decides the landing position. */
    anchor_table_save(t);
    t->anchor.valid = false;
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
                    reader's follow state first, then relayout from the
                    affected turn so following positions, view_content and
                    the scrollbar range follow the new content, keeping
                    bottom-following when the reader follows; a free reader
                    is anchor-restored instead. This is the external
                    selection-change event the blocked_debt retry contract
                    rides. */
                bool pinned = transcript_following(t);
                if (catch_up(t, feed, i)) {
                    if (t->bounded) {
                        RealizeResult r;
                        realize_loop(t, feed, pinned, -1, true, &r);
                        place_and_scroll(t, feed, pinned);
                    } else {
                        transcript_layout_from(t, i, pinned);
                    }
                }
            }
            return;
        }
    }
}

void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed) {
    if (t->applying) return;
    bool pinned = transcript_following(t);
    int first = -1;
    for (int i = 0; i < t->record_count; i++) {
        TranscriptRecord *rec = &t->records[i];
        if (rec->head_pending || rec->body_pending || rec->meta_pending ||
            rec->reason_pending) {
            if (catch_up(t, feed, i) && first < 0) first = i;
        }
    }
    /* One relayout from the earliest changed turn covers every applied write. */
    if (first >= 0) {
        if (t->bounded) {
            RealizeResult r;
            realize_loop(t, feed, pinned, -1, true, &r);
            place_and_scroll(t, feed, pinned);
        } else {
            transcript_layout_from(t, first, pinned);
        }
    }
}

bool transcript_create(Transcript *t, HWND view, const RichTextTheme *theme,
    float dpi) {
    memset(t, 0, sizeof *t);
    t->view = view;
    t->dpi = dpi;
    t->theme = *theme;
    t->theme_epoch = 1;
    t->diagnostic_round_cap = -1;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        t->records[i].slot = -1;
        t->records[i].rendered_slot = -1;
        t->records[i].attempted_epoch = 0;
        for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
            t->records[i].saved_sel_min[k] = t->records[i].saved_sel_max[k] = -1;
        t->records[i].saved_reason_scroll = -1;
    }
    /* Exactly one allocation for the transcript's lifetime: the realized-slot
       pool. Failure leaves the transcript safe but unrealized and fails host
       startup (fail closed, like the other host subsystems). The shared
       measurement surface is NOT created here: it is created lazily on the
       first bounded geometry pass and recreated after any loss. */
    t->slots = calloc(CHAT_MAX_MESSAGES, sizeof *t->slots);
    if (!t->slots) { t->slot_capacity = 0; return false; }
    t->slot_capacity = CHAT_MAX_MESSAGES;
    for (int i = 0; i < t->slot_capacity; i++) t->slots[i].record = -1;
    return true;
}

void transcript_dispose(Transcript *t) {
    /* Bookkeeping only: the child surfaces (and the measurer) are destroyed
       with the container window, which must already be gone when this runs
       (see the header). */
    if (!t || !t->slots) return;
    free(t->slots);
    t->slots = NULL;
    t->slot_capacity = 0;
    t->measurer_valid = false;
}

void transcript_set_dpi(Transcript *t, float dpi) {
    t->dpi = dpi;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++)
        for (int s = 0; s < TRANSCRIPT_SURFACE_COUNT; s++) {
            RichTextControl *control = transcript_surface(t, i,
                (TranscriptSurface)s);
            if (control) rich_text_set_dpi(control, dpi);
        }
    /* The measurement surface re-derives at the new DPI too; every record's
       stamp self-invalidates through measured_dpi on the next pass. */
    if (t->measurer_valid && control_window_live(&t->measurer))
        rich_text_set_dpi(&t->measurer, dpi);
}

void transcript_measure_notify(Transcript *t, RichTextControl *control,
    const RECT *required) {
    if (t->diagnostic_drop_measure_notify) return;
    if (control && control == t->measuring)
        t->measured = required->bottom - required->top;
}
