/* Deterministic lifecycle of the transcript realized-slot pool: creation,
   disposal ordering guarantees, and pool-allocation failure under a wrapped
   calloc. No windows and no Rich Edit surfaces are created -- surface
   realization is lazy, so the pool can be exercised headlessly. */
#include "chat/transcript/transcript_win32.h"
#include "chat/transcript/transcript_policy.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Allocation seam: the pool must be created with exactly one calloc, so the
   failure point can be pinned deterministically (1-based call index within
   one create; 0 disables the failure). */
static int calloc_count, calloc_fail;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size) {
    ++calloc_count;
    if (calloc_fail && calloc_count == calloc_fail) return NULL;
    return __real_calloc(n, size);
}

int main(void) {
    RichTextTheme theme;
    memset(&theme, 0, sizeof theme);

    /* Dispose on a zeroed (never-created) transcript: a no-op. */
    {
        Transcript t;
        memset(&t, 0, sizeof t);
        transcript_dispose(&t);
        CHECK(!t.slots && t.slot_capacity == 0);
    }

    /* Pool-allocation failure: create fails closed, leaves the transcript
       safe (no pool, zero capacity, no record bound), and a dispose of the
       failed transcript is still safe and idempotent. */
    {
        Transcript t;
        memset(&t, 0, sizeof t);
        calloc_count = 0;
        calloc_fail = 1;                       /* the pool calloc itself */
        CHECK(!transcript_create(&t, NULL, &theme, 96.0f));
        CHECK(!t.slots && t.slot_capacity == 0);
        CHECK(t.records[0].slot == -1);
        for (int i = 1; i < CHAT_MAX_MESSAGES; i++)
            CHECK(t.records[i].slot == -1);
        CHECK(transcript_surface(&t, 0, TRANSCRIPT_HEAD) == NULL);
        CHECK(transcript_surface(&t, 0, TRANSCRIPT_BODY) == NULL);
        transcript_dispose(&t);
        transcript_dispose(&t);                /* idempotent */
        CHECK(!t.slots && t.slot_capacity == 0);
        calloc_fail = 0;
    }

    /* Success: exactly one calloc, every slot unbound, every record
        unassociated, and the accessor answers NULL for an unbound record.
        Bounded mode defaults OFF and the shared measurement surface is not
        attempted at create (lazy, best-effort), so a headless create with a
        NULL view stays clean. */
    {
        Transcript t;
        memset(&t, 0, sizeof t);
        calloc_count = 0;
        CHECK(transcript_create(&t, NULL, &theme, 96.0f));
        CHECK(calloc_count == 1);              /* exactly one pool allocation */
        CHECK(t.slots && t.slot_capacity == CHAT_MAX_MESSAGES);
        CHECK(!t.bounded);                     /* seam defaults off */
        CHECK(!t.measurer_valid);              /* nothing attempted headless */
        CHECK(!t.resizing && !t.render_active && t.render_epoch == 0);
        CHECK(t.diagnostic_round_cap == -1 && !t.diagnostic_drop_measure_notify);
        CHECK(t.policy_needed == 0 && t.slot_limit == 0 && t.bound_count == 0);
        CHECK(transcript_bound_slots(&t) == 0);
        CHECK(transcript_created_windows(&t) == 0);
        CHECK(t.stat.created_hwnds == 0 && t.stat.created_peak == 0);
        for (int s = 0; s < t.slot_capacity; s++) {
            CHECK(t.slots[s].record == -1);
            CHECK(t.slots[s].generation == 0);     /* never bound */
        }
        for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
            CHECK(t.records[i].slot == -1);
            CHECK(t.records[i].rendered_slot == -1);
            CHECK(!t.records[i].measured_valid && !t.records[i].measured_estimated);
            CHECK(t.records[i].body_layout_width == 0 &&
                t.records[i].body_layout_dpi == 0.0f &&
                t.records[i].body_layout_theme == 0);
            CHECK(!t.records[i].blocked_debt && !t.records[i].blocked_resource);
            CHECK(t.records[i].saved_sel_min[TRANSCRIPT_HEAD] == -1 &&
                t.records[i].saved_sel_min[TRANSCRIPT_BODY] == -1 &&
                t.records[i].saved_sel_min[TRANSCRIPT_META] == -1 &&
                t.records[i].saved_sel_min[TRANSCRIPT_REASON] == -1 &&
                t.records[i].saved_reason_scroll == -1);
            for (int k = 0; k < TRANSCRIPT_SURFACE_COUNT; k++)
                CHECK(transcript_surface(&t, i, (TranscriptSurface)k) == NULL);
        }
        /* Disposal frees exactly the pool and re-zeroes it; double dispose
            is safe. Records are inline and intentionally untouched. */
        transcript_dispose(&t);
        CHECK(!t.slots && t.slot_capacity == 0);
        CHECK(t.theme_epoch == 1);             /* stamp bookkeeping survives */
        transcript_dispose(&t);
        CHECK(!t.slots && t.slot_capacity == 0);
    }

    /* Disposal order contract: the pool is freed only after the window
       hierarchy is gone, which is the host's ownership-layer job. Here that
       reduces to dispose-then-done, but the accessor must refuse everything
       once the pool is gone. */
    {
        Transcript t;
        memset(&t, 0, sizeof t);
        CHECK(transcript_create(&t, NULL, &theme, 96.0f));
        for (int i = 0; i < CHAT_MAX_MESSAGES; i++)
            CHECK(t.records[i].slot == -1);
        transcript_dispose(&t);
        CHECK(transcript_surface(&t, 0, TRANSCRIPT_HEAD) == NULL);
        CHECK(transcript_surface(&t, CHAT_MAX_MESSAGES - 1, TRANSCRIPT_META)
            == NULL);
    }

    /* Capacity-governed policy (pure): the hard slot capacity is computed
       in pixel space (window ∪ Tier-A ∪ Tier-B allowance ∪ spare, never a
       hardcoded slot count), the forced victim never touches a visible or
       Tier-A record, and Tier-B window membership is LRU-ranked under the
       allowance. */
    {
        /* Window-only conversation: |window| + clamped spare. */
        TranscriptPolicyItem items[6];
        memset(items, 0, sizeof items);
        for (int i = 0; i < 6; i++) {
            items[i].index = i;
            items[i].y = i * 100;
            items[i].height = 90;
        }
        /* Viewport [0,500): records 0..4 touch it, record 5 does not. */
        CHECK(transcript_policy_visible(0, 90, 0, 500, 8));
        CHECK(transcript_policy_bounded_capacity(items, 6, 0, 500, 8, 24, 2)
            == 6);   /* 5 window + 1 clamped spare */
        CHECK(transcript_policy_bounded_capacity(items, 6, 0, 500, 8, 24, 0)
            == 5);
        /* Tier-A off-screen protection raises the capacity past the window. */
        items[5].streaming = true;
        CHECK(transcript_policy_bounded_capacity(items, 6, 0, 500, 8, 24, 2)
            == 6);   /* 5 window + 1 Tier-A, spare clamped to 0 */
        items[5].streaming = false;
        /* Tier-B is capped by the allowance, not by its population. */
        TranscriptPolicyItem wide[40];
        memset(wide, 0, sizeof wide);
        for (int i = 0; i < 40; i++) {
            wide[i].index = i;
            wide[i].y = i < 10 ? i * 100 : 100000 + i * 100;   /* 10 in window */
            wide[i].height = 90;
            if (i >= 10) wide[i].debt = true;                  /* 30 Tier-B */
            wide[i].last_used = (uint64_t)(i + 1);
        }
        CHECK(transcript_policy_bounded_capacity(wide, 40, 0, 1000, 8, 24, 2)
            == 10 + 2 + 24);
        CHECK(transcript_policy_bounded_capacity(wide, 40, 0, 1000, 8, 4, 2)
            == 10 + 2 + 4);
        CHECK(transcript_policy_bounded_capacity(wide, 40, 0, 1000, 8, 0, 0)
            == 10);   /* no spare, no allowance: window only */
        CHECK(transcript_policy_bounded_capacity(NULL, 40, 0, 1000, 8, 24, 2)
            == 0);
        CHECK(transcript_policy_bounded_capacity(wide, 0, 0, 1000, 8, 24, 2)
            == 0);

        /* Forced victim: eligible = bound, off-window, not Tier-A; the
            oldest last_used wins, then the lowest position. */
        TranscriptPolicyItem bound[4];
        memset(bound, 0, sizeof bound);
        for (int s = 0; s < 4; s++) bound[s].index = s;
        bound[0].y = 0;      bound[0].height = 90;    /* visible */
        bound[1].y = 100000; bound[1].height = 90;
        bound[1].streaming = true;                    /* Tier-A off-screen */
        bound[2].y = 100000; bound[2].height = 90;
        bound[2].debt = true; bound[2].last_used = 10;
        bound[3].y = 100000; bound[3].height = 90;
        bound[3].last_used = 5;
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 3);   /* plain evictable, oldest */
        bound[3].debt = true;                         /* now also Tier-B */
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 3);   /* Tier-B is evictable; still the oldest */
        bound[3].last_used = 10;                      /* tie with slot 2 */
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 2);   /* tie: lowest position */
        bound[2].last_used = 0;
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 2);
        bound[3].last_used = 0;
        bound[2].focused = true;                      /* Tier-A: protected */
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 3);
        bound[3].debt = false;
        bound[3].expanded = true;
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == 3);   /* expanded Tier-B stays eligible */
        bound[3].expanded = false;
        bound[3].y = 0;                               /* now visible */
        CHECK(transcript_policy_pick_forced_victim(bound, 4, 0, 1000, 8)
            == -1);  /* only visible and Tier-A remain */
        CHECK(transcript_policy_pick_forced_victim(NULL, 4, 0, 1000, 8)
            == -1);
        CHECK(transcript_policy_pick_forced_victim(bound, 0, 0, 1000, 8)
            == -1);

        /* Tier-B window membership ranks by last_used under the allowance. */
        TranscriptPolicyItem tier[5];
        memset(tier, 0, sizeof tier);
        for (int i = 0; i < 5; i++) {
            tier[i].index = i;
            tier[i].y = 100000;    /* all off-window */
            tier[i].height = 90;
            tier[i].debt = true;
            tier[i].last_used = (uint64_t)(i + 1);
        }
        CHECK(transcript_policy_in_window(tier, 5, 4, 0, 1000, 8, 3));
        CHECK(transcript_policy_in_window(tier, 5, 3, 0, 1000, 8, 3));
        CHECK(transcript_policy_in_window(tier, 5, 2, 0, 1000, 8, 3));
        CHECK(!transcript_policy_in_window(tier, 5, 1, 0, 1000, 8, 3));
        CHECK(!transcript_policy_in_window(tier, 5, 0, 0, 1000, 8, 3));
        CHECK(!transcript_policy_in_window(tier, 5, 2, 0, 1000, 8, 0));
        tier[2].streaming = true;    /* Tier-A outranks the allowance */
        CHECK(transcript_policy_in_window(tier, 5, 2, 0, 1000, 8, 0));
        CHECK(transcript_policy_in_window(tier, 5, 2, 0, 1000, 8, 3));
        tier[2].streaming = false;
        CHECK(!transcript_policy_in_window(NULL, 5, 2, 0, 1000, 8, 3));
        CHECK(!transcript_policy_in_window(tier, 5, -1, 0, 1000, 8, 3));
        CHECK(!transcript_policy_in_window(tier, 5, 5, 0, 1000, 8, 3));

        /* The dynamic required capacity is pure geometry: the strict
            viewport's worst case plus the overscan band, two Tier-A
            records, the Tier-B allowance and the spares -- clamped to the
            arena, floors clamped, monotone in the page. */
        /* page 500, h_min 8, overscan 600: ceil(500/8)+1 + ceil(600/8)
           + 2 + 24 + 2 */
        CHECK(transcript_policy_required_slots(500, 8, 600, 24, 2, 512)
            == 63 + 1 + 75 + 2 + 24 + 2);
        /* A taller page requires more window slots (monotone). */
        CHECK(transcript_policy_required_slots(900, 8, 600, 24, 2, 512)
            > transcript_policy_required_slots(500, 8, 600, 24, 2, 512));
        /* The arena clamps; degenerate inputs clamp to sane floors. */
        CHECK(transcript_policy_required_slots(500, 8, 600, 24, 2, 100)
            == 100);
        CHECK(transcript_policy_required_slots(-1, 0, -1, -1, -1, -1)
            == 1 + 0 + 2 + 0 + 0);   /* page 0 band 0: ceil(0/1)+1 + 2 */
        CHECK(transcript_policy_required_slots(0, 0, 0, 0, 0, 512) == 3);
    }

    puts("Transcript slots: dispose on zeroed state, deterministic pool-calloc "
        "failure leaving the transcript safe with all records unbound, "
        "successful create with exactly one allocation and a fully unbound "
        "pool (bounded seam off, measurer untouched, arena diagnostics zero, "
        "no stale stamps, blocked flags or saved reader state), idempotent "
        "double dispose, accessor refusals after disposal, and the "
        "capacity-governed policy (pixel-space bounded capacity with Tier-A "
        "protection and the Tier-B allowance, the dynamic geometric required "
        "slot capacity with arena clamping and page monotonicity, forced "
        "victims never visible or Tier-A with oldest-LRU ties, and LRU-ranked "
        "Tier-B window membership) passed");
    return 0;
}
