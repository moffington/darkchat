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
   this module executes them. In THIS pass the pool is retained-everything:
   slot_capacity == CHAT_MAX_MESSAGES, every record binds one slot on first
   use and keeps it for the process lifetime, so realization behavior is
   unchanged; the capacity checkpoint in transcript_render marks the single
   place a future pass activates policy-governed capacity.

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

/* The four native surfaces one turn can own, in stacking order. */
typedef enum {
    TRANSCRIPT_HEAD, TRANSCRIPT_BODY, TRANSCRIPT_REASON, TRANSCRIPT_META,
    TRANSCRIPT_SURFACE_COUNT
} TranscriptSurface;

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
       current AND no surface debt is outstanding. This pass records the
       stamp and re-measures unconditionally; nothing consumes it yet. */
    int y, height, head_y, reason_y, body_y, meta_y;
    int head_h, reason_h, body_h, meta_h;
    int measured_width;
    float measured_dpi;
    uint32_t measured_theme;
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
    /* Record-owned LRU stamp, bumped when the record binds a slot. Eviction
       input only; never consumed by realization ordering. */
    uint64_t last_used;
} TranscriptRecord;

/* A realized slot owns the native surfaces plus the identity of the current
   association: the bound record and the association's binding generation. It
   carries no scheduling state of its own: last-use recency lives on the
   bound record and protection is computed at decision time from record
   state and geometry. */
typedef struct {
    RichTextControl surface[TRANSCRIPT_SURFACE_COUNT];
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

/* Host-owned behavior the transcript surfaces need; wired once at creation. */
typedef struct {
    bool (*surface_key)(void *user, WPARAM key, bool shift, bool control,
        bool down);
    bool (*row_click)(void *user, RichTextControl *control, int line,
        bool down);
    void *user;
} TranscriptCallbacks;

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
       the policy computed for the most recent full render. Recorded for
       observability; it never governs in this pass (retain-all). */
    int policy_needed;
    uint64_t clock;                     /* monotonic clock: LRU stamps and
                                           nonzero binding generations */
    /* Last time the streaming body was rebuilt as Markdown. */
    ULONGLONG body_render_tick;
    /* Reentrancy guard: programmatic selection changes fired while this module
       writes must not recursively trigger deferred-update application. */
    bool applying;
    TranscriptCallbacks callbacks;
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
/* DPI change: re-derives every turn surface; the next layout re-measures. */
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
   body holds a selection. */
void transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index);
void transcript_position(Transcript *t, bool follow);
/* Scrolls only enough to reveal one already-rendered turn. A turn taller than
   the viewport is aligned at its top. No content or selection is rewritten. */
bool transcript_reveal_turn(Transcript *t, int index);
void transcript_layout_from(Transcript *t, int start, bool follow);
bool transcript_pinned(const Transcript *t);
/* EN_SELCHANGE from any turn surface: applies that turn's deferred writes when
   the selection has cleared and relayouts from the affected turn. */
void transcript_selection_changed(Transcript *t, const TranscriptFeed *feed,
    RichTextControl *control);
/* Fallback sweep (idle timer): applies every deferred write whose selection
   is gone and relayouts once from the earliest changed turn. */
void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed);

#endif
