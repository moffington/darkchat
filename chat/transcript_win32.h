#ifndef DARKCHAT_TRANSCRIPT_WIN32_H
#define DARKCHAT_TRANSCRIPT_WIN32_H

/* Transcript update bookkeeping for the chat host, isolated from the rest of
   the host. The transcript is a two-layer structure:

   - One lightweight layout record per message (TranscriptRecord), indexed by
     message index. Records carry the rendered identity, the deferred-write
     debt, the visibility state, the geometry and the measurement stamp -- and
     no HWNDs. Records never store a ChatMessage pointer: identity is stored
     by value and message state is re-derived from the feed at every use, so
     growth reallocs and conversation changes cannot dangle anything.

   - A bounded pool of realized slots (TranscriptSlot) that own the native
     Rich Edit surfaces. A slot binds at most one record and a record binds at
     most one slot (a bijection: records[i].slot == s iff slots[s].record ==
     i, -1 meaning unbound on both sides). Control ids are slot-based
     (100 + slot*4 + surface); with this pass's permanent binding slot index
     equals record index, so the emitted ids are numerically unchanged, while
     a future rebind moves the id with the slot. Notify routing never depends
     on ids.

   Which records must be realized, how many slots are required, and which
   slot a new binding takes are pure policy decisions (chat/transcript_policy.h);
   this module executes them. Realization runs in one of two modes, selected
   per transcript by transcript_set_bounded (legal at any time, effective on
   the next render):

   - Retain-all (the default, and the shipped production mode): the pool
     holds CHAT_MAX_MESSAGES slots, every record binds one slot on first use
     and keeps it for the process lifetime, layout measures every live
     surface unconditionally, and the capacity checkpoint records (never
     enforces) the policy's required count. Behaviorally equivalent to the
     pre-bounded passes.

    - Bounded (test infrastructure for the activation pass): the prepare set
      is the overscan-widened viewport union the class-protected records; a
      fixed-point realize/measure loop binds and prepares only actionable
      records through the shape-aware pick_slot policy; heights are stamped
      per record and consumed only while exact, certified, and debt-free;
      off-screen geometry is measured exactly through one shared clipped
      Rich Edit measurement surface (visible but beyond the client edge; a
      truly hidden surface stops laying out and answers inflated heights).
      Selection operates inside a raise-only governed slot budget
      (Transcript's slot_limit): the limit starts at 0, is raised to the
      dynamic required capacity (geometric worst case of the padded window
      plus Tier-A, the Tier-B allowance and spares, clamped to the 512-slot
      arena) whenever that exceeds it, and never shrinks. All slot
      selection -- free reuse, pre-eviction and forced Tier-B eviction --
      happens only within [0, slot_limit); the bound set is never trimmed
      toward a target.

   Every record stores the message identity its surfaces were built from.
   When that identity still matches, destructive content writes are skipped,
   so unchanged historical controls survive completion and layout changes
   with their reader state (selection, scroll, caret) intact. Destructive
   reformatting of a surface that currently holds a selection is deferred
   until the selection clears; the debt lives on the record, so it survives
   any future unbind/rebind cycle. Signature matches skip destructive writes
   only: visibility reconciliation, callback wiring, width-dependent
   measurement, positioning and DPI work always run. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"
#include "rich_text_win32.h"

/* Markdown streaming rebuilds the visible body at most this often; deltas
   accumulate in the message between rebuilds. */
#define CHAT_BODY_RENDER_MS 100

/* How far beyond the strict viewport a bounded realization window reaches,
   in DIPs, before records are realized: overscan keeps small scrolls from
   forcing binds at the window edge. */
#define TRANSCRIPT_OVERSCAN_DIPS 300

/* Tier-B allowance of the hard slot capacity (see TranscriptStats and
   transcript_policy_bounded_capacity): at most this many off-screen records
   carrying reader state (deferred debt or an expanded reasoning viewport)
   stay realized; population beyond it is answered by forced eviction, which
   preserves the state on the record, never by unbounded growth. Tier-A
   records (focused surface, streaming turn) are protected without a cap. */
#define TRANSCRIPT_TIER_B_ALLOWANCE 24

/* Control id of the shared measurement surface. Slot-based turn ids live at
   100 + slot*4 + surface, so id 1 is unambiguous. */
#define TRANSCRIPT_MEASURE_ID 1

/* Client x of the shared measurement surface: fully beyond any transcript
   client's right edge, so the container clips it away (see ensure_measurer).
   A truly hidden Rich Edit stops re-laying-out its text, so EM_REQUESTRESIZE
   answers from a stale, over-wrapped layout; visible-but-clipped lays out
   exactly like the live surfaces. */
#define TRANSCRIPT_MEASURE_OFFX 32000

/* The four native surfaces one turn can own, in stacking order. The value
   of each enumerator is also its kind bit in the policy's created_kinds
   bitmask. */
typedef enum {
    TRANSCRIPT_HEAD, TRANSCRIPT_BODY, TRANSCRIPT_REASON, TRANSCRIPT_META,
    TRANSCRIPT_SURFACE_COUNT
} TranscriptSurface;

#define TRANSCRIPT_SURFACE_KIND(surface) (1u << (surface))

/* Quality of one height measurement: EXACT only when the control answered
   EN_REQUESTRESIZE with a positive height; the EM_GETLINECOUNT and
   arithmetic fallbacks are estimates and stay retryable. */
typedef enum {
    TRANSCRIPT_MEASURE_EXACT,
    TRANSCRIPT_MEASURE_ESTIMATE
} TranscriptMeasureQuality;

/* Lightweight per-message layout record: identity, debt/state, geometry,
   measurement stamp and realized-slot association. Contains no HWNDs. */
typedef struct {
    /* Rendered identity: the message state these surfaces were built from.
       `revision` is stamped eagerly (the state the render attempted);
       `body_revision` is stamped only when the write actually lands, so it
       always names the answer text present in the bound control. */
    bool rendered_valid;
    uint64_t conversation, message, revision, body_revision;
    ChatRole role;
    ChatGenerationState state;
    bool running, content_started, reasoning_open;
    wchar_t row[48];                    /* rendered reasoning-row text */
    /* Destructive writes deferred while a selection is held in a surface. */
    bool head_pending, body_pending, meta_pending, reason_pending;
    /* Visibility reconciliation: which surfaces are currently live. */
    bool head_live, body_live, reason_live, meta_live;
    /* Geometry in content coordinates. Surface heights are valid only under
       the full measurement stamp below AND while the rendered identity is
       current AND no surface debt is outstanding. Retain-all layout records
       the stamp and re-measures unconditionally; bounded layout consumes
       the stamp through height_of's matrix. */
    int y, height, head_y, reason_y, body_y, meta_y;
    int head_h, reason_h, body_h, meta_h;
    int measured_width;
    float measured_dpi;
    uint32_t measured_theme;
    /* Exact-measurement stamp (bounded mode). Heights are certified only
       while every stamp input matches the record's freshly derived shape
       and identity, the measurement was EXACT (never flagged estimated),
       the binding is certified (or the record is unbound), and no surface
       debt is outstanding. Stamped from feed state at measurement time,
       never conflated with the rendered identity (which certifies surface
       content, not geometry). */
    bool measured_valid, measured_estimated;
    uint64_t measured_conversation, measured_message, measured_revision,
        measured_body_revision;
    ChatRole measured_role;
    ChatGenerationState measured_state;
    bool measured_running, measured_content_started, measured_reasoning_open,
        measured_row;
    /* Realized-slot association: -1 = unrealized. */
    int slot;
    /* Binding generation certified by the cached identity: the association
       the certified content belongs to. A slot number can be reused by
       another record (A uses S, unbinds, B rewrites S, unbinds, A returns
       to S), so certification requires BOTH the slot index and the binding
       generation to match; otherwise the cached identity must never
       certify the surfaces and a complete rewrite is forced. */
    int rendered_slot;
    uint64_t rendered_generation;
    /* Record-owned LRU stamp, bumped when the record binds a slot and when
       an already-bound record is prepared. Eviction input only; never
       consumed by realization ordering. */
    uint64_t last_used;
    /* Qualified blocked states, recorded by the last render's prepare
        passes (cleared at each render start): a same-message destructive
        write deferred by a live selection (blocked_debt), or a surface
        creation that failed this render (blocked_resource). Neither prevents
        convergence; displayed geometry stays valid under qualified I-GAP. */
    bool blocked_debt, blocked_resource;
    /* Reader state captured when a bounded eviction (pre-eviction or
        forced) took the record's slot, restored at the next binding of the
        same message instance. The deferred-write debt and rendered identity
        already live on the record and survive unbinding; each surface's
        selection is saved independently (only when non-empty) and the
        reasoning viewport's inner scroll only when it was live. (-1 =
        nothing saved for that surface.) Captures are cleared only after
        their successful restoration; a failed binding (surface creation
        failure) retains them for the next render's retry. */
    int saved_sel_min[TRANSCRIPT_SURFACE_COUNT],
        saved_sel_max[TRANSCRIPT_SURFACE_COUNT];
    int saved_reason_scroll;
    /* Per-render attempt stamps (one destructive-write attempt and one
       creation attempt per surface per render): attempted_epoch names the
       render that stamped the bits below. */
    uint32_t attempted_epoch;
    uint32_t attempted_kinds;
} TranscriptRecord;

/* A realized slot owns the native surfaces plus the identity of the current
   association: the bound record and the association's binding generation. It
   carries no scheduling state of its own: last-use recency lives on the
   bound record and protection is computed at decision time from record
   state and geometry. */
typedef struct {
    RichTextControl surface[TRANSCRIPT_SURFACE_COUNT];
    /* Arena cells are slot/surface positions that have ever owned an HWND.
       Recreating a lost HWND reuses the same cell rather than growing it. */
    uint32_t arena_kinds;
    int record;                         /* bound record index, -1 = unbound */
    /* Generation of the current association, assigned fresh (nonzero, from
       the transcript's monotonic clock) on every new binding, so a reused
       slot number is never mistaken for the same surfaces. Zero = never
       bound. */
    uint64_t generation;
} TranscriptSlot;

/* Host streaming context, refreshed by the host before every call. */
typedef struct {
    const Chat *chat;
    bool generating, content_started, reasoning_streaming;
    int request_conversation, request_message;
} TranscriptFeed;

/* Host-owned behavior the transcript surfaces need; wired once at creation.
    `focus_release` moves keyboard focus somewhere safe when a surface is
    about to be hidden, invalidated, rebound, switched away, or torn down;
    the host implements it by focusing the composer. */
typedef struct {
    bool (*surface_key)(void *user, WPARAM key, bool shift, bool control,
        bool down);
    bool (*row_click)(void *user, RichTextControl *control, int line,
        bool down);
    void (*focus_release)(void *user);
    void *user;
} TranscriptCallbacks;

/* Reader scroll-intent state. Bottom-follow is entered only through the
   explicit qualifying transitions in transcript_note_user_scroll (a user
   scroll that lands at the bottom) or by construction (a fresh transcript);
   every mutation pass — streaming deltas, completion, deferred-write
   application, height corrections, resource-retry renders, resize/DPI
   reflow, reasoning toggles, eviction — preserves the mode, so nothing can
   force-follow a reader who has left the bottom. The old geometric
   transcript_pinned query survives only as that qualifying detector's
   comparison and as a diagnostic; it no longer decides follow behavior. */
typedef enum { TRANSCRIPT_FOLLOW_BOTTOM, TRANSCRIPT_FOLLOW_FREE }
    TranscriptFollowMode;

/* Scroll anchor: the reader's position as a stable (conversation, message)
   identity plus a surface family and a pixel offset below that surface's
   top. The offset is in [0, height) when the viewport top lies inside the
   surface, and negative when it lies above that surface's top (in a gap or
   above a turn), which restores exactly. Resolution always recomputes from
   fresh geometry, so any change above the anchor surface — a turn growing,
   a deferred write applied, a reasoning viewport expanding, an estimate
   correcting to exact, a resize or DPI reflow — is absorbed by the
   recompute instead of shifting content under the reader; the anchor
   surface's own top only moves when content above it changed, which is
   exactly the motion the reader asked to keep. `top` marks the
   top-of-transcript anchor (view_scroll at the first record's top or
   above), which restores to scroll 0. */
typedef struct {
    bool valid, top;
    uint64_t conversation, message;
    TranscriptSurface surface;
    int offset;
} TranscriptAnchor;

/* Per-conversation anchor entry. The table is a fixed linear array searched
    by stable conversation id (never indexed by it: ids are monotone store
    counters, not array positions, and can be arbitrarily sparse). At most
    one entry per conversation id; a render prunes entries whose
    conversation no longer exists -- a session can mint far more than the
    table's 128 distinct ids through delete/create churn, so the table is
    reused rather than assumed exhaustible. */
typedef struct {
    bool used;
    uint64_t conversation;
    TranscriptAnchor anchor;
} TranscriptAnchorEntry;

/* Bounded-realization instrumentation: counters advanced as work happens,
   never asserted as wall-clock thresholds. Units: binds/rebinds/evictions
   count bind events (rebinds: the chosen slot was bound by another record
   and was pre-evicted); creations counts raw successful HWND creates;
   created_hwnds counts monotone arena cells (the measurer counts once even
   if recreated), while created_peak is the peak current live-HWND count;
   rounds records
   the last realize loop's round count; degraded/fallback record the
   qualified-degraded and safety-cap exits. */
typedef struct {
    int binds, rebinds, evictions, creations;
    int exact_measures, estimates, measure_retries;
    int rounds, degraded_rounds, fallback_rounds;
    int created_hwnds, created_peak, bound_peak;
    /* Reader-anchor diagnostics: restores counts anchor resolutions that
        moved the scroll; rejected counts stale anchors (message deleted or
        replaced) that fell back to the nearest earlier surviving turn. */
    int anchor_restores, anchor_rejected;
    /* Capacity-governance diagnostics (bounded mode): capacity_raises
        counts raises of the governed slot limit (raise-only, never a
        shrink); forced_evictions counts evictions a full governed limit
        forced (selection within [0, slot_limit) returned -1 and the LRU
        non-window/non-Tier-A binding was taken), as opposed to
        pre-evictions a binding chose itself; limit_saturated counts the
        selection events that found the governed limit full (the -1
        pick_slot results a forced eviction or a refusal answers), and
        exhaustion_refusals counts bind requests the engine gave up on
        (returned -1 with the record left unrealized) -- fail-closed
        insurance that must stay zero while the limit covers the window.
        eviction_restores counts captured reader-state pieces (selection,
        reasoning scroll) reapplied at a later binding of the same
        message, with selection_restores and reason_scroll_restores
        splitting them per kind. focus_transfers counts focus moved off a
        surface that was about to be hidden, invalidated, rebound,
        switched away, or torn down. conv_anchor_restores counts
        conversation switches whose saved anchor was valid and restored
        the reader's FREE position. */
    int capacity_raises, forced_evictions, eviction_restores, focus_transfers;
    int exhaustion_refusals, limit_saturated;
    int selection_restores, reason_scroll_restores, conv_anchor_restores;
} TranscriptStats;

typedef struct {
    HWND view;                          /* transcript container child window */
    float dpi;
    RichTextTheme theme;
    uint32_t theme_epoch;               /* part of the measurement stamp */
    int view_scroll, view_content, view_page, view_width, view_margin, view_gap;
    int view_reason_gap, view_meta_gap, view_reason_inset;
    RichTextControl *measuring;         /* control awaiting EN_REQUESTRESIZE */
    int measured;
    TranscriptRecord records[CHAT_MAX_MESSAGES];
    int record_count;
    TranscriptSlot *slots;              /* realized-slot pool (heap) */
    int slot_capacity;
    /* Last capacity-checkpoint result (diagnostic): the required slot count
        computed for the most recent full render -- the geometric dynamic
        formula (window worst case + Tier-A + Tier-B allowance + spares) in
        bounded mode, the strict-viewport |V u P| in retain-all. Recorded,
        never a trim target. */
    int policy_needed;
    /* The raise-only governed slot budget of bounded mode: all selection
        (free reuse, pre-eviction, forced Tier-B eviction) operates only
        within [0, slot_limit), and the limit is raised to the dynamic
        required capacity whenever that exceeds it (capacity_raises counts
        each raise). It never shrinks -- the bound set is never trimmed
        toward a target -- and retain-all leaves it at 0 (never consulted). */
    int slot_limit;
    int bound_count;                    /* diagnostic, recomputed at checkpoint */
    uint64_t clock;                     /* monotonic clock: LRU stamps and
                                           nonzero binding generations */
    /* Realization mode and per-render loop state. bounded defaults to
       false (retain-all); render_active is true only inside a realize
       loop (attempt stamps and round transitions are loop-scoped). */
    bool bounded, render_active;
    bool bounded_prune_pending;          /* one-shot off -> on cleanup */
    uint32_t render_epoch;
    int round_transitions;
    bool resizing;                      /* WM_ENTERSIZEMOVE storm: off-screen
                                           re-measurement deferred to settle */
    /* Opt-in deterministic test seams; production leaves both zero/false. */
    int diagnostic_round_cap;
    bool diagnostic_drop_measure_notify;
    /* Shared measurement surface for exact off-screen heights: one read-only
        block, child of the container, kept WS_VISIBLE but parked at
        TRANSCRIPT_MEASURE_OFFX so the container clips it away entirely. A
        truly hidden Rich Edit stops re-laying-out its text and answers
        EM_REQUESTRESIZE from a stale, over-wrapped layout; visible-but-
        clipped lays out exactly like the live surfaces. Created lazily and
        recreated after any loss (creation failure or a lost HWND); a child
        of the container, destroyed with the window hierarchy. Created only
        in bounded mode. */
    RichTextControl measurer;
    bool measurer_valid;
    bool measurer_counted;              /* arena accounting: counted once */
    /* Last time the streaming body was rebuilt as Markdown. */
    ULONGLONG body_render_tick;
    /* Reader scroll-intent state (see TranscriptFollowMode). */
    TranscriptFollowMode follow;
    TranscriptAnchor anchor;
    /* The active conversation's working anchor; per-conversation storage
        lives in the linear stable-ID table below. `active_conversation` is
        the conversation id of the last render (0 = none yet), so a switch
        is detected at the next render and the table entry is loaded. */
    uint64_t active_conversation;
    TranscriptAnchorEntry conversation_anchors[CHAT_MAX_CONVERSATIONS];
    /* Reader focus tracked through WM_COMMAND (EN_SETFOCUS/EN_KILLFOCUS
        from the transcript surfaces): the last surface the reader focused.
        Protection, decision-time debt and focus-transfer sites read this
        instead of GetFocus(), so hiding, invalidating, rebinding,
        switching and teardown can move focus deliberately before they act. */
    HWND focus_window;
    /* Active scrollbar thumb drag: the drag owns the position (nTrackPos);
        anchor restore and capture are suspended and follow transitions are
        suppressed until the release event re-qualifies. */
    bool thumb_drag;
    /* Set by transcript_note_user_scroll and cleared at the next placement:
        while set, the user's position is the truth (no anchor restore) and
        the placement captures a fresh anchor from the final position. */
    bool user_scroll_pending;
    /* Reentrancy guard: programmatic selection changes fired while this module
        writes must not recursively trigger deferred-update application. */
    bool applying;
    TranscriptCallbacks callbacks;
    TranscriptStats stat;
} Transcript;

/* Creates the container bookkeeping and the realized-slot pool. Performs
   exactly one allocation (the pool); a failure leaves the transcript
   unusable-but-safe (every record unrealized, surfaces NULL) and returns
   false, which fails host startup. */
bool transcript_create(Transcript *t, HWND view, const RichTextTheme *theme,
    float dpi);
/* Frees the slot pool. Safe on a zeroed (never-created) transcript, safe
   after a failed create, and idempotent. Frees bookkeeping memory only: it
   never touches the child HWNDs (they are destroyed with the container) and
   never touches the records. Call exactly once per host, from the ownership
   layer AFTER the window hierarchy is fully destroyed -- never from the
   parent window procedure, whose WM_DESTROY runs while child surfaces still
   exist and whose GWLP_USERDATA points into the pool. */
void transcript_dispose(Transcript *t);
/* Mutable access to one bound surface, or NULL when the record is unbound,
   the pool is absent, or surface creation failed. */
RichTextControl *transcript_surface(Transcript *t, int index,
    TranscriptSurface surface);
/* Decision-time debt for one record: a pending deferred write OR a live
    selection in any bound surface. The capacity checkpoint and the dormant
    victim path build TranscriptPolicyItem debt from this, so an unfocused
    selected turn is class-protected exactly like a debt-bearing one. */
bool transcript_record_debt(Transcript *t, int index);
/* Realization mode: false (the default) keeps the retain-all behavior;
    true activates the bounded realize/measure engine on the next render.
    An off -> on transition schedules one pruning pass for retain-all bindings;
    subsequent convergence rounds recycle only through normal slot policy. */
void transcript_set_bounded(Transcript *t, bool bounded);
/* Replaces formatting inputs and advances the stamp epoch. This is the theme
   mutation path used by the bounded measurer as well as newly formatted live
   surfaces. */
void transcript_set_theme(Transcript *t, const RichTextTheme *theme);
/* Diagnostics over the fixed arena: native child windows created (pool
    surfaces plus the measurer) and slots currently bound to records. */
int transcript_created_windows(const Transcript *t);
int transcript_bound_slots(const Transcript *t);
/* Diagnostic measurement of one realized surface at the transcript's
    current width, with its quality — the seam the equality property is
    asserted through (off-screen EXACT == live EXACT). */
int transcript_measure_live(Transcript *t, RichTextControl *control,
    TranscriptMeasureQuality *quality);
/* DPI change: re-derives every turn surface; the next layout re-measures
    (stamps self-invalidate through measured_dpi). */
void transcript_set_dpi(Transcript *t, float dpi);
void transcript_measure_notify(Transcript *t, RichTextControl *control,
    const RECT *required);
/* Drops every turn's rendered identity, pending updates and selections, so
    the next render replaces content without deferral. */
void transcript_invalidate(Transcript *t);
/* Same, from one turn slot onward: retry/regenerate/edit slot reuse. */
void transcript_invalidate_from(Transcript *t, int from);
void transcript_render(Transcript *t, const TranscriptFeed *feed);
void transcript_refresh_turn(Transcript *t, const TranscriptFeed *feed,
    int index);
/* Throttled Markdown rebuild of one streaming turn's body; defers while the
    body holds a selection. Returns false only when the body surface is
    missing and a retry (prepare attempt) could not create it either: the
    caller keeps its flush armed and retries on the next external event
    (incoming delta, 1 Hz sweep, next render). A deferred selection write
    is not a failure. */
bool transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index);
/* Clamps and applies the scroll, realizes the funnel (bounded mode: the
    realize loop runs before placing), and repositions every placed turn. */
void transcript_position(Transcript *t, const TranscriptFeed *feed,
    bool follow);
/* Scrolls only enough to reveal one already-rendered turn. A turn taller than
    the viewport is aligned at its top. No content or selection is rewritten;
    the position funnel realizes the revealed turn before placing. */
bool transcript_reveal_turn(Transcript *t, const TranscriptFeed *feed,
    int index);
void transcript_layout_from(Transcript *t, int start, bool follow);
bool transcript_pinned(const Transcript *t);
/* Follow mode: BOTTOM follows the newest content on every pass; FREE holds
    the reader's position (restoring the anchor when geometry changes).
    Entering BOTTOM clears the anchor. */
void transcript_set_follow(Transcript *t, TranscriptFollowMode mode);
bool transcript_following(const Transcript *t);
/* The one user-scroll entry point. Records the position, marks it as the
    reader's truth (captured as the new anchor at the next placement), and
    runs the qualifying transition: a release-position scroll that lands
    within the pinned tolerance of the bottom re-enters BOTTOM follow;
    anything else enters FREE. While a thumb drag is active the transition
    is suppressed and runs on the release event instead. */
void transcript_note_user_scroll(Transcript *t, int position);
/* Active thumb drag state: suspends anchor capture/restore and follow
    transitions while held. */
void transcript_set_drag(Transcript *t, bool drag);
/* WM_COMMAND focus notifications from the transcript surfaces
    (EN_SETFOCUS/EN_KILLFOCUS through WM_COMMAND; no special event mask is
    required). Updates the tracked reader focus the decision sites read.
    Control must be a transcript surface (the host routes only its own
    children). */
void transcript_focus_notify(Transcript *t, RichTextControl *control,
    bool gained);
/* Moves keyboard focus off any tracked transcript surface when one is
    about to be hidden, invalidated, rebound, switched away, or torn down.
    The destination is host policy, invoked through
    TranscriptCallbacks::focus_release (the host focuses the composer), so
    the transcript container never decides where focus lands. Counts
    focus_transfers only when a real focused window was moved. */
void transcript_focus_release(Transcript *t);
/* EN_SELCHANGE from any turn surface: applies that turn's deferred writes when
   the selection has cleared and relayouts from the affected turn. */
void transcript_selection_changed(Transcript *t, const TranscriptFeed *feed,
    RichTextControl *control);
/* Fallback sweep (idle timer): applies every deferred write whose selection
   is gone and relayouts once from the earliest changed turn. */
void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed);

#endif
