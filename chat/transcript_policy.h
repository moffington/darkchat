#ifndef DARKCHAT_TRANSCRIPT_POLICY_H
#define DARKCHAT_TRANSCRIPT_POLICY_H

/* Transcript realization policy. Pure C: no Win32 and no allocation. This
   module decides *which* layout records must be realized, how many realized
   slots the transcript must own, and which slot a new binding may take; the
   transcript module (chat/transcript_win32.c) owns the records, the slots and
   the native surfaces and executes these decisions.

   The transcript keeps one layout record per message and realizes native
   surfaces through a bounded pool of slots. A record that intersects the
   viewport must always be realized (an unrealized visible record is a blank
   gap), and a record carrying live reader state must never lose its slot
   (that would destroy a selection, a deferred write, an inner scroll
   position or the stream target). Everything here is deterministic and
   read-only over its inputs; no function allocates, fails or mutates state.

   Capacity precondition (P-CAP): slot_capacity >= |V u P|, where V is the
   set of visible records and P the set of class-protected records. Under
   P-CAP, realizing all of V u P before any spare record and taking eviction
   victims only from the complement leaves every visible record realized --
   that is the whole no-blank-gap argument. A pick_victim result of -1 does
   NOT avoid a gap for a visible candidate; it only refuses to violate the
   victim rule. Under P-CAP it is unreachable; when reached, the correct
   response is raising capacity to transcript_policy_needed_slots, never
   evicting a must-keep record. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Generic spare (unused, evictable) slot capacity the transcript keeps above
   the must-keep set |V u P|. It selects nothing and anchors to nothing: it
   is plain headroom so that small scrolls do not immediately force new
   bindings. The transcript clamps it to the records that remain. */
#define TRANSCRIPT_SPARE_SLOTS 2

/* A borrowed description of one layout record (or of one slot's bound
    record) at decision time. `index` is always the represented record index,
    or -1 for a free slot; the slot position in a victim search is the array
    position of the item, never item.index. `height <= 0` means "not yet
    measured" and every geometry-aware seam reads it as h_min. Flags are
    computed by the caller from record state at decision time; nothing here
    caches them. `last_used` is the record-owned LRU stamp: it ranks eviction
    candidates only and never participates in realization ordering.

    `created_kinds` describes the native surface kinds the slot already owns
    (bitmask, bit k = kind k has a created window; 0 = pristine slot). It
    feeds only transcript_policy_pick_slot's shape-aware reuse tiers; the
    retain-all victim search ignores it. The mapping from bit to surface kind
    belongs to the caller (the transcript's TranscriptSurface order). */
typedef struct {
    int index;
    int y, height;
    bool streaming, focused, debt, expanded;
    uint64_t last_used;
    uint32_t created_kinds;
} TranscriptPolicyItem;

/* Visibility is geometry with one source of truth, mirroring the transcript
   container's placement rule exactly: a record occupies [y, y + height) in
   content coordinates and is visible iff it intersects the open interval
   (scroll, scroll + page); touching an edge is not visible. A non-positive
   height is read as h_min (h_min <= 0 degenerates to 1), so unmeasured
   records still claim their worst-case space. page < 0 is treated as 0. */
bool transcript_policy_visible(int y, int height, int scroll, int page,
    int h_min);

/* Class protection: reader state an unbind would destroy. True for a record
   that is currently streaming, owns the focused surface, carries a deferred
   write or a live selection (debt), or holds an expanded reasoning viewport
   (whose inner scroll position is destroyed by unbinding). Geometry-
   independent; a visible-but-unprotected record is still must-keep (rank 0),
   it just is not state-protected. A NULL item is not protected. */
bool transcript_policy_protected(const TranscriptPolicyItem *item);

/* Realization priority classes: 0 = visible (must realize first), 1 =
   protected but off-screen, 2 = spare (evictable). Ordering is (rank, index)
   by transcript_policy_before; the LRU stamp is never consulted here --
   realization priority and eviction recency are fully separated. */
int transcript_policy_rank(const TranscriptPolicyItem *item, int scroll,
    int page, int h_min);

/* Total order for realization: rank first, then record index (a stable,
   content-independent tiebreak). Neither item may be NULL. */
bool transcript_policy_before(const TranscriptPolicyItem *a,
    const TranscriptPolicyItem *b, int scroll, int page, int h_min);

/* Required slot capacity: |V u P| plus min(spare_slots, count - |V u P|),
   clamped to [0, count]. V and P are computed internally from the items by
   transcript_policy_visible and transcript_policy_protected, so the union is
   counted exactly once (a visible record that is also protected contributes
   one slot, not two). Monotone in page and count; 0 when count <= 0 or
   items is NULL; spare_slots <= 0 contributes nothing. This result is the
   exposed required-capacity output: a capacity-governed transcript compares
   it against its pool size and grows (or evicts under P-CAP) rather than
   ever leaving a visible record unrealized. */
int transcript_policy_needed_slots(const TranscriptPolicyItem *items,
    int count, int scroll, int page, int h_min, int spare_slots);

/* Slot selection for a new binding. `bound` describes every slot by array
    position: bound[s].index is the record slot s currently renders, or -1
    when s is free; the returned value is the slot position, never item.index.
    Preference: a free slot first (the lowest-positioned one); otherwise the
    oldest-last_used bound slot that is neither visible nor class-protected
    (ties keep the lowest position); -1 when every slot is must-keep, which
    under P-CAP is unreachable and must be answered by raising capacity.

    This is the retain-all-era contract: it still backs the dormant victim
    path when the pool fills. Bounded realization binds through
    transcript_policy_pick_slot instead. */
int transcript_policy_pick_victim(const TranscriptPolicyItem *bound,
    int slot_count, int scroll, int page, int h_min);

/* Shape-aware bounded-mode slot selection. The caller passes the padded
    viewport (scroll and page already widened by the realization overscan) so
    the must-keep set is padded-window ∪ protected; `needed_kinds` is the
    bitmask of surface kinds the new binding requires. Tiers, in order:

    1. a free slot whose created_kinds equals needed_kinds (pure HWND reuse,
       no eviction) — free-slot ties take the lowest position;
    2. an evictable bound slot whose created_kinds equals needed_kinds
       (pre-eviction: the departed record is outside the must-keep set, so
       unbinding it destroys no reader state);
    3. a free slot with created HWNDs and the most kind overlap (free-slot
       ties take the lowest position);
    4. an evictable bound slot with the most kind overlap (any overlap,
       including zero) — evictable ties take the oldest last_used, then the
       lowest position;
    5. a pristine free slot (created_kinds == 0) — lowest position — the
       final step: a pristine slot is never consumed while any free slot
       with HWNDs or any evictable bound slot exists, so the HWND arena stays
       viewport-shaped under non-accumulating use.

    -1 only when every slot is a bound must-keep record and none is free,
    which cannot happen while the pool has at least as many slots as records;
    it is preserved fail-closed for a future hard-capped arena. */
int transcript_policy_pick_slot(const TranscriptPolicyItem *bound,
    int slot_count, uint32_t needed_kinds, int scroll, int page, int h_min);

/* Bounded realization membership, the capacity-governed pass: true iff record
    `index` belongs in the realization window. The window is the overscan-
    widened pixel viewport (caller pads scroll/page; h_min = px(t,8) is the
    unmeasured worst case) union the Tier-A protected records (streaming or
    focused, wherever they sit) union at most `tier_b_allowance` Tier-B
    protected records (deferred debt or expanded reasoning) -- ranked LRU by
    last_used among themselves, ties by index, so the newest reader state
    stays realized and the stalest falls out. Pure and read-only. */
bool transcript_policy_in_window(const TranscriptPolicyItem *items, int count,
    int index, int scroll, int page, int h_min, int tier_b_allowance);

/* Exact-set required bound-slot capacity for the capacity-governed pool:
    records visible in the padded pixel window, plus Tier-A protected records
    anywhere, plus at most `tier_b_allowance` Tier-B protected records, plus
    min(spare_slots, room), clamped to [0, count]. Window, Tier-A and Tier-B
    sets are disjoint by construction. Monotone in page and count. This is
    the per-record exact-set form of the capacity computation -- the
    transcript raises its governed limit from the geometric
    transcript_policy_required_slots instead -- and the pure test suite
    exercises it as the exact-set counterpart (window ∪ Tier-A ∪ Tier-B
    allowance ∪ spare), with Tier-B deliberately capped by the allowance. */
int transcript_policy_bounded_capacity(const TranscriptPolicyItem *items,
    int count, int scroll, int page, int h_min, int tier_b_allowance,
    int spare_slots);

/* Forced-eviction victim selection, the hard-cap counterpart to
    pick_victim. The caller passes bound slots (same convention:
    bound[s].index is the record, -1 free; returned value is the slot
    position). A victim is a bound record that is neither visible in the
    padded viewport nor Tier-A protected (streaming or focused): Tier-B
    records (debt, expanded reasoning) and plain evictables are eligible,
    because their reader state survives eviction on the record. The oldest
    last_used wins, then the lowest position. -1 when every bound record is
    visible or Tier-A -- under P-CAP unreachable; the caller must raise
    capacity (or fail closed), never evict a must-keep record. */
int transcript_policy_pick_forced_victim(const TranscriptPolicyItem *bound,
    int slot_count, int scroll, int page, int h_min);

/* Raise-only governed slot budget, the dynamic required capacity of the
    capacity-governed pool. Computed from pixel geometry alone -- no
    per-record scan -- so it is a true upper bound of the padded window's
    membership and is monotone in the page:

    - ceil(page/h_min) + 1 records can intersect the strict viewport
      (every height read at the h_min floor; +1 for boundary touches);
    - ceil(2*overscan/h_min) records can sit in the overscan band above
      and below it;
    - two Tier-A protected records anywhere (the streaming turn and the
      focused record);
    - the Tier-B allowance records (deferred debt or expanded reasoning);
    - the spare headroom.

    All inputs are clamped to sane floors (page, overscan, allowance and
    spares are non-negative; h_min >= 1) and the result is clamped to
    [0, arena]. The transcript raises its governed slot limit to this
    value only when the value exceeds the limit -- the limit never
    shrinks, and binding never operates outside [0, slot_limit). */
int transcript_policy_required_slots(int page, int h_min, int overscan,
    int tier_b_allowance, int spare_slots, int arena);

/* Identity validation: true iff the message instance a slot's surfaces were
   built from is exactly the message instance about to be rendered. A false
   result is replacement-class staleness: the transcript must clear the
   selection, drop deferred debt and replace content immediately, without
   deferral. Revision-level staleness within the same instance is decided by
   the transcript's rendered-identity bookkeeping, not here. */
bool transcript_policy_same_message(uint64_t slot_conversation,
    uint64_t slot_message, uint64_t record_conversation,
    uint64_t record_message);

#endif
