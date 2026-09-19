/* Pure transcript realization policy suite: no Win32, no allocation. */
#include "chat/transcript/transcript_policy.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Compound literal: an addressable item with the given fields. */
#define ITEM(index, y, height, streaming, focused, debt, expanded, used) \
    &(TranscriptPolicyItem) { index, y, height, streaming, focused, debt, \
        expanded, used, 0 }

/* Same, with an explicit created-kinds bitmask for pick_slot. */
#define KIND_ITEM(index, y, height, streaming, focused, debt, expanded, used, \
    kinds) \
    &(TranscriptPolicyItem) { index, y, height, streaming, focused, debt, \
        expanded, used, kinds }

static int test_visibility(void) {
    /* Interior intersection is visible. */
    CHECK(transcript_policy_visible(10, 20, 0, 100, 8));
    /* Touching the top edge (record ends exactly at scroll) is not visible,
       matching the container's `top + height <= 0` hide rule. */
    CHECK(!transcript_policy_visible(0, 10, 10, 100, 8));
    /* Touching the bottom edge (record starts exactly at scroll + page) is
       not visible, matching the container's `top >= page` hide rule. */
    CHECK(!transcript_policy_visible(100, 10, 0, 100, 8));
    /* Partly visible at either edge is visible. */
    CHECK(transcript_policy_visible(-5, 10, 0, 100, 8));
    CHECK(transcript_policy_visible(95, 10, 0, 100, 8));
    /* An unmeasured height is read as h_min. */
    CHECK(transcript_policy_visible(0, 0, 0, 5, 8));
    CHECK(!transcript_policy_visible(8, 0, 0, 5, 8));
    /* h_min <= 0 degenerates to one unit so unmeasured records still claim
       their worst-case space. */
    CHECK(transcript_policy_visible(0, 0, 0, 5, 0));
    /* A negative page is an empty viewport. */
    CHECK(!transcript_policy_visible(0, 20, 0, -1, 8));
    /* Scrolled far past the record. */
    CHECK(!transcript_policy_visible(0, 20, 500, 100, 8));
    return 0;
}

static int test_protection(void) {
    CHECK(!transcript_policy_protected(ITEM(0, 0, 10, false, false, false, false, 1)));
    CHECK(transcript_policy_protected(ITEM(0, 0, 10, true, false, false, false, 1)));
    CHECK(transcript_policy_protected(ITEM(0, 0, 10, false, true, false, false, 1)));
    CHECK(transcript_policy_protected(ITEM(0, 0, 10, false, false, true, false, 1)));
    CHECK(transcript_policy_protected(ITEM(0, 0, 10, false, false, false, true, 1)));
    CHECK(!transcript_policy_protected(NULL));
    return 0;
}

static int test_rank_and_order(void) {
    /* Rank classes: visible beats protected beats spare, regardless of LRU. */
    CHECK(transcript_policy_rank(ITEM(0, 0, 10, false, false, false, false, 1),
        0, 100, 8) == 0);
    CHECK(transcript_policy_rank(ITEM(0, 0, 10, false, false, true, false, 1),
        500, 100, 8) == 1);
    CHECK(transcript_policy_rank(ITEM(0, 0, 10, false, false, false, false, 1),
        500, 100, 8) == 2);
    /* Visible but unprotected is still rank 0 (must-keep, not state-safe). */
    CHECK(transcript_policy_rank(ITEM(0, 0, 10, false, false, false, false, 1),
        0, 100, 8) == 0);
    /* Ordering: rank first, then index. */
    TranscriptPolicyItem visible = *ITEM(1, 0, 10, false, false, false, false, 50);
    TranscriptPolicyItem protected_off = *ITEM(2, 500, 10, false, false, true, false, 1);
    CHECK(transcript_policy_before(&visible, &protected_off, 0, 100, 8));
    CHECK(!transcript_policy_before(&protected_off, &visible, 0, 100, 8));
    /* Same rank: index order, and never the LRU stamp. Two items identical
       except for last_used order exactly like their index order reversed. */
    TranscriptPolicyItem a = *ITEM(1, 500, 10, false, false, false, false, 100);
    TranscriptPolicyItem b = *ITEM(2, 500, 10, false, false, false, false, 1);
    CHECK(transcript_policy_before(&a, &b, 0, 100, 8));
    CHECK(!transcript_policy_before(&b, &a, 0, 100, 8));
    /* A difference in last_used alone must not reorder same-rank items:
       identical rank and index order identically regardless of stamps. */
    TranscriptPolicyItem c = *ITEM(1, 500, 10, false, false, false, false, 1);
    CHECK(!transcript_policy_before(&c, &a, 0, 100, 8));
    CHECK(!transcript_policy_before(&a, &c, 0, 100, 8));
    /* Rank 0 has no proximity tiebreak: distance from the viewport center
       plays no part, only index. The nearer item has the larger index. */
    TranscriptPolicyItem near = *ITEM(2, 40, 10, false, false, false, false, 0);
    TranscriptPolicyItem far = *ITEM(1, 90, 10, false, false, false, false, 0);
    CHECK(transcript_policy_before(&far, &near, 0, 100, 8));
    CHECK(!transcript_policy_before(&near, &far, 0, 100, 8));
    return 0;
}

static int test_needed_slots(void) {
    /* Empty and degenerate inputs. */
    CHECK(transcript_policy_needed_slots(NULL, 0, 0, 100, 8, 2) == 0);
    CHECK(transcript_policy_needed_slots(NULL, 5, 0, 100, 8, 2) == 0);
    /* Exact union counting: one visible, one protected off-screen, one
       spare; must = 2, spare capped to the one remaining record. */
    {
        TranscriptPolicyItem items[3] = {
            *ITEM(0, 0, 20, false, false, false, false, 1),
            *ITEM(1, 500, 20, false, false, true, false, 2),
            *ITEM(2, 900, 20, false, false, false, false, 3),
        };
        CHECK(transcript_policy_needed_slots(items, 3, 0, 100, 8, 2) == 3);
        /* No spare capacity: exactly the must-keep set. */
        CHECK(transcript_policy_needed_slots(items, 3, 0, 100, 8, 0) == 2);
        /* A visible record that is also protected contributes one slot. */
        items[0].streaming = true;
        CHECK(transcript_policy_needed_slots(items, 3, 0, 100, 8, 2) == 3);
        CHECK(transcript_policy_needed_slots(items, 3, 0, 100, 8, 0) == 2);
    }
    /* page smaller than h_min: at most one record can intersect, so with
       three records the must set is exactly the visible one. */
    {
        TranscriptPolicyItem items[3] = {
            *ITEM(0, 0, 20, false, false, false, false, 1),
            *ITEM(1, 20, 20, false, false, false, false, 2),
            *ITEM(2, 40, 20, false, false, false, false, 3),
        };
        CHECK(transcript_policy_needed_slots(items, 3, 0, 1, 8, 2) == 3);
        CHECK(transcript_policy_needed_slots(items, 3, 0, 1, 8, 0) == 1);
        /* Page exactly covering one record selects that record only. */
        CHECK(transcript_policy_needed_slots(items, 3, 20, 20, 8, 0) == 1);
    }
    /* Unmeasured heights claim h_min of space: the first record (y 0,
       unmeasured) intersects a 50px page; the second (y 100) does not. */
    {
        TranscriptPolicyItem items[2] = {
            *ITEM(0, 0, 0, false, false, false, false, 1),
            *ITEM(1, 100, 0, false, false, false, false, 2),
        };
        CHECK(transcript_policy_needed_slots(items, 2, 0, 50, 8, 0) == 1);
        /* A page tall enough to reach both unmeasured records covers them
           both: 0..8 and 100..108 both intersect (0, 110). */
        CHECK(transcript_policy_needed_slots(items, 2, 0, 110, 8, 0) == 2);
        /* Far enough apart that a short page reaches neither. */
        CHECK(transcript_policy_needed_slots(items, 2, 500, 20, 8, 0) == 0);
    }
    /* Monotone in page. */
    {
        TranscriptPolicyItem items[16];
        for (int i = 0; i < 16; i++)
            items[i] = *ITEM(i, i * 10, 10, false, false, false, false, 1);
        int previous = -1;
        for (int page = 0; page <= 160; page += 8) {
            int needed =
                transcript_policy_needed_slots(items, 16, 0, page, 8, 0);
            CHECK(needed >= previous);
            previous = needed;
        }
    }
    /* Every record protected and visible: needed is exactly count, spare
       contributes nothing, and the result never exceeds count. */
    {
        TranscriptPolicyItem items[5];
        for (int i = 0; i < 5; i++)
            items[i] = *ITEM(i, 0, 10, true, true, true, true, 1);
        CHECK(transcript_policy_needed_slots(items, 5, 0, 100, 8, 2) == 5);
        CHECK(transcript_policy_needed_slots(items, 5, 0, 100, 8, 100) == 5);
    }
    /* Spare capacity is clamped to the records that remain. */
    {
        TranscriptPolicyItem items[1] = {
            *ITEM(0, 500, 10, false, false, false, false, 1),
        };
        CHECK(transcript_policy_needed_slots(items, 1, 0, 100, 8, 2) == 1);
    }
    /* Monotone in count: appending a spare record grows or holds. */
    {
        TranscriptPolicyItem items[4] = {
            *ITEM(0, 0, 10, false, false, false, false, 1),
            *ITEM(1, 500, 10, false, false, false, false, 2),
            *ITEM(2, 900, 10, false, false, false, false, 3),
            *ITEM(3, 1300, 10, false, false, false, false, 4),
        };
        int two = transcript_policy_needed_slots(items, 2, 0, 100, 8, 2);
        int three = transcript_policy_needed_slots(items, 3, 0, 100, 8, 2);
        int four = transcript_policy_needed_slots(items, 4, 0, 100, 8, 2);
        CHECK(two <= three && three <= four);
        CHECK(two == 2 && four == 3);   /* 1 must + spare, capped */
    }
    return 0;
}

static int test_pick_victim(void) {
    /* A free slot is always taken, lowest position first, regardless of the
       LRU stamps or state of bound slots. */
    {
        TranscriptPolicyItem bound[3] = {
            *ITEM(7, 500, 10, false, false, false, false, 1),
            *ITEM(-1, 0, 0, false, false, false, false, 0),
            *ITEM(9, 0, 10, false, false, false, false, 2),
        };
        CHECK(transcript_policy_pick_victim(bound, 3, 0, 100, 8) == 1);
    }
    /* Among evictable bound slots the oldest last_used wins. */
    {
        TranscriptPolicyItem bound[3] = {
            *ITEM(7, 500, 10, false, false, false, false, 30),
            *ITEM(8, 600, 10, false, false, false, false, 10),
            *ITEM(9, 700, 10, false, false, false, false, 20),
        };
        CHECK(transcript_policy_pick_victim(bound, 3, 0, 100, 8) == 1);
    }
    /* Visible and protected slots are never returned. */
    {
        TranscriptPolicyItem bound[4] = {
            *ITEM(7, 0, 10, false, false, false, false, 1),    /* visible */
            *ITEM(8, 500, 10, false, false, true, false, 2),   /* debt */
            *ITEM(9, 600, 10, true, false, false, false, 3),   /* streaming */
            *ITEM(10, 700, 10, false, false, false, false, 4),
        };
        CHECK(transcript_policy_pick_victim(bound, 4, 0, 100, 8) == 3);
    }
    /* Every slot must-keep: fail closed. */
    {
        TranscriptPolicyItem bound[2] = {
            *ITEM(7, 0, 10, false, false, false, false, 1),
            *ITEM(8, 500, 10, false, false, true, false, 2),
        };
        CHECK(transcript_policy_pick_victim(bound, 2, 0, 100, 8) == -1);
    }
    /* A tie on last_used keeps the lowest slot position. */
    {
        TranscriptPolicyItem bound[3] = {
            *ITEM(7, 500, 10, false, false, false, false, 10),
            *ITEM(8, 600, 10, false, false, false, false, 10),
            *ITEM(9, 700, 10, false, false, false, false, 10),
        };
        CHECK(transcript_policy_pick_victim(bound, 3, 0, 100, 8) == 0);
    }
    /* The result is the slot position (array index), never item.index: the
       victim here is record 77 bound at position 1. */
    {
        TranscriptPolicyItem bound[3] = {
            *ITEM(5, 500, 10, false, false, true, false, 1),
            *ITEM(77, 600, 10, false, false, false, false, 1),
            *ITEM(9, 0, 10, false, false, false, false, 9),    /* visible */
        };
        CHECK(transcript_policy_pick_victim(bound, 3, 0, 100, 8) == 1);
    }
    /* Degenerate inputs fail closed. */
    CHECK(transcript_policy_pick_victim(NULL, 3, 0, 100, 8) == -1);
    {
        TranscriptPolicyItem bound[1] = { *ITEM(-1, 0, 0, false, false, false, false, 0) };
        CHECK(transcript_policy_pick_victim(bound, 0, 0, 100, 8) == -1);
    }
    /* Visibility inside the victim search is computed from the passed
       geometry, not caller-supplied flags. */
    {
        TranscriptPolicyItem bound[2] = {
            *ITEM(7, 40, 10, false, false, false, false, 1),
            *ITEM(8, 900, 10, false, false, false, false, 2),
        };
        /* Scroll 0 page 100: slot 0 is visible, slot 1 must be the victim. */
        CHECK(transcript_policy_pick_victim(bound, 2, 0, 100, 8) == 1);
        /* Scroll 850 page 100: slot 1 is visible, slot 0 must be the
           victim. */
        CHECK(transcript_policy_pick_victim(bound, 2, 850, 100, 8) == 0);
    }
    return 0;
}

static int test_identity(void) {
    CHECK(transcript_policy_same_message(11, 22, 11, 22));
    CHECK(!transcript_policy_same_message(11, 22, 12, 22));
    CHECK(!transcript_policy_same_message(11, 22, 11, 23));
    CHECK(!transcript_policy_same_message(12, 22, 11, 23));
    /* Zero is a valid (if never-allocated) identity value; equality is
        exact, nothing is wildcarded. */
    CHECK(transcript_policy_same_message(0, 0, 0, 0));
    CHECK(!transcript_policy_same_message(0, 0, 0, 1));
    return 0;
}

static int test_pick_slot(void) {
    /* Tier 1: a free slot with an exact kind match wins over everything;
       ties take the lowest position. */
    {
        TranscriptPolicyItem bound[3] = {
            *KIND_ITEM(7, 500, 10, false, false, false, false, 1, 0xF),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0xF),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0xF),
        };
        CHECK(transcript_policy_pick_slot(bound, 3, 0xF, 0, 100, 8) == 1);
    }
    /* Tier 1 only considers slots with created HWNDs: a pristine slot is
       never taken while a kind-matching free slot exists. */
    {
        TranscriptPolicyItem bound[2] = {
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x3),
        };
        CHECK(transcript_policy_pick_slot(bound, 2, 0x3, 0, 100, 8) == 1);
    }
    /* Tier 2: evictable bound slot with an exact kind match; ties take the
       oldest last_used, then the lowest position. Visible and protected
       records are never evicted. */
    {
        TranscriptPolicyItem bound[4] = {
            *KIND_ITEM(7, 0, 10, false, false, false, false, 5, 0xF),   /* visible */
            *KIND_ITEM(8, 500, 10, false, false, true, false, 9, 0xF),  /* debt */
            *KIND_ITEM(9, 500, 10, false, false, false, false, 4, 0xF),
            *KIND_ITEM(10, 500, 10, false, false, false, false, 2, 0xF),
        };
        CHECK(transcript_policy_pick_slot(bound, 4, 0xF, 0, 100, 8) == 3);
        /* LRU tie: equal stamps keep the lowest position. */
        bound[2].last_used = 2;
        bound[3].last_used = 2;
        CHECK(transcript_policy_pick_slot(bound, 4, 0xF, 0, 100, 8) == 2);
    }
    /* Tier 3: free slot with the most kind overlap, including zero overlap;
       any free slot with HWNDs precedes bound-slot eviction. */
    {
        TranscriptPolicyItem bound[3] = {
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x1),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x2),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x6),
        };
        CHECK(transcript_policy_pick_slot(bound, 3, 0x6, 0, 100, 8) == 2);
        /* Zero overlap remains a tier-3 candidate. */
        CHECK(transcript_policy_pick_slot(bound, 3, 0x8, 0, 100, 8) == 0);
    }
    /* Tier 4: evictable bound slot with the most overlap, any overlap
       including zero; ties take the oldest last_used, then the lowest
       position. */
    {
        TranscriptPolicyItem bound[3] = {
            *KIND_ITEM(7, 0, 10, false, false, false, false, 1, 0x1),   /* visible */
            *KIND_ITEM(8, 500, 10, false, false, false, false, 3, 0x4),
            *KIND_ITEM(9, 500, 10, false, false, false, false, 2, 0x2),
        };
        /* Overlap of slot 1 (0x4&0x6=1 bit) vs slot 2 (0x2&0x6=1 bit): tie,
           slot 2 has the older stamp. */
        CHECK(transcript_policy_pick_slot(bound, 3, 0x6, 0, 100, 8) == 2);
        /* Overlap dominates recency: slot 1 overlaps 2 bits. */
        CHECK(transcript_policy_pick_slot(bound, 3, 0x5, 0, 100, 8) == 1);
        /* Zero overlap still reaches tier 4: both slots tie on overlap,
           the older stamp (slot 2, used=2) wins. */
        CHECK(transcript_policy_pick_slot(bound, 3, 0x8, 0, 100, 8) == 2);
    }
    /* Tier 5: pristine consumption only after free-with-HWNDs and evictable
       slots are exhausted — the creep-hazard regression. */
    {
        TranscriptPolicyItem bound[4] = {
            *KIND_ITEM(7, 0, 10, false, false, false, false, 1, 0x1),   /* visible */
            *KIND_ITEM(8, 500, 10, false, false, false, false, 2, 0x2), /* evictable */
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0),     /* pristine */
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0),     /* pristine */
        };
        /* Zero overlap on the free HWND slot would still precede eviction;
           there is none here, so tier 4 wins before pristine tier 5. */
        CHECK(transcript_policy_pick_slot(bound, 4, 0x2, 0, 100, 8) == 1);
        CHECK(transcript_policy_pick_slot(bound, 4, 0x4, 0, 100, 8) == 1);
        /* Pristine slots are consumed only when nothing else qualifies:
           make the evictable slot visible. */
        bound[1].y = 0;
        CHECK(transcript_policy_pick_slot(bound, 4, 0x4, 0, 100, 8) == 2);
    }
    /* Tier ordering against a kind-matching free slot: tier 1 wins over
       the evictable slot; a kind no free slot carries exactly falls to the
       evictable tier-2 match. */
    {
        TranscriptPolicyItem bound[3] = {
            *KIND_ITEM(7, 500, 10, false, false, false, false, 1, 0x6),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x2),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0),
        };
        CHECK(transcript_policy_pick_slot(bound, 3, 0x2, 0, 100, 8) == 1);
        /* The evictable slot's exact kind set (0x6) is matched by tier 2
           before any partial-overlap tier: the free 0x2 slot only
           overlaps 0x6 partially. */
        CHECK(transcript_policy_pick_slot(bound, 3, 0x6, 0, 100, 8) == 0);
    }
    /* Tier 3 beats tier 4: a free slot with partial overlap is preferred
       over evicting any bound slot. */
    {
        TranscriptPolicyItem bound[2] = {
            *KIND_ITEM(7, 500, 10, false, false, false, false, 1, 0x8),
            *KIND_ITEM(-1, 0, 0, false, false, false, false, 0, 0x2),
        };
        CHECK(transcript_policy_pick_slot(bound, 2, 0x2, 0, 100, 8) == 1);
    }
    /* Fail closed: every slot is a bound must-keep record. */
    {
        TranscriptPolicyItem bound[2] = {
            *KIND_ITEM(7, 0, 10, false, false, false, false, 1, 0x2),
            *KIND_ITEM(8, 500, 10, false, false, true, false, 2, 0x2),
        };
        CHECK(transcript_policy_pick_slot(bound, 2, 0x2, 0, 100, 8) == -1);
    }
    /* Degenerate inputs fail closed. */
    CHECK(transcript_policy_pick_slot(NULL, 3, 0x1, 0, 100, 8) == -1);
    {
        TranscriptPolicyItem bound[1] = { *KIND_ITEM(-1, 0, 0, false, false,
            false, false, 0, 0) };
        CHECK(transcript_policy_pick_slot(bound, 0, 0x1, 0, 100, 8) == -1);
    }
    return 0;
}

static int test_required_slots(void) {
    /* Pure geometry: ceil(page/h_min)+1 window records, ceil(overscan/h_min)
       overscan records, two Tier-A slots, the Tier-B allowance and the
       spares. page 500/h_min 8 -> 63+1, overscan 600 -> 75. */
    CHECK(transcript_policy_required_slots(500, 8, 600, 24, 2, 512)
        == 63 + 1 + 75 + 2 + 24 + 2);
    /* Monotone in the page: a taller viewport can hold more records. */
    int previous = -1;
    for (int page = 0; page <= 800; page += 8) {
        int required =
            transcript_policy_required_slots(page, 8, 600, 24, 2, 512);
        CHECK(required >= previous);
        previous = required;
    }
    /* The arena clamps the required value. */
    CHECK(transcript_policy_required_slots(5000, 8, 600, 24, 2, 512)
        == 512);
    CHECK(transcript_policy_required_slots(5000, 8, 600, 24, 2, 0) == 0);
    /* Negative page/overscan/allowance/spares clamp to zero; h_min to 1. */
    CHECK(transcript_policy_required_slots(-1, 0, -1, -1, -1, 512) == 3);
    CHECK(transcript_policy_required_slots(0, 0, 0, 0, 0, 512) == 3);
    /* A negative arena reads as unbounded (no clamp). */
    CHECK(transcript_policy_required_slots(500, 8, 600, 24, 2, -1)
        == 63 + 1 + 75 + 2 + 24 + 2);
    /* Exact boundary: page 16/h_min 8 needs ceil(16/8)+1 = 3 window slots. */
    CHECK(transcript_policy_required_slots(16, 8, 0, 0, 0, 512) == 5);
    /* Overscan band: 2*overscan passed in as 600px wide and h_min 8 -> 75. */
    CHECK(transcript_policy_required_slots(0, 8, 601, 0, 0, 512)
        == 1 + 76 + 2);
    return 0;
}

int main(void) {
    if (test_visibility()) return 1;
    if (test_protection()) return 1;
    if (test_rank_and_order()) return 1;
    if (test_needed_slots()) return 1;
    if (test_pick_victim()) return 1;
    if (test_identity()) return 1;
    if (test_pick_slot()) return 1;
    if (test_required_slots()) return 1;
    puts("Transcript policy: geometry-derived visibility (edge-touch excluded, "
        "unmeasured heights as h_min), class protection, rank/index ordering "
        "with LRU excluded from realization priority, exact |V u P| capacity "
        "with clamped spare slots and page/count monotonicity, the dynamic "
        "geometric required slot capacity with arena clamping and page "
        "monotonicity, fail-closed victim selection by slot position with LRU "
        "among evictable slots only, exact message-instance identity, and "
        "shape-aware bounded pick_slot (kind reuse before pre-eviction, "
        "evictable before pristine, free ties by lowest position, evictable "
        "ties by oldest LRU then position, fail closed) passed");
    return 0;
}
