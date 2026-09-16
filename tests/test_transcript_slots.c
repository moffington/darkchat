/* Deterministic lifecycle of the transcript realized-slot pool: creation,
   disposal ordering guarantees, and pool-allocation failure under a wrapped
   calloc. No windows and no Rich Edit surfaces are created -- surface
   realization is lazy, so the pool can be exercised headlessly. */
#include "../chat/transcript_win32.h"
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
        CHECK(t.policy_needed == 0 && t.bound_limit == 0 && t.bound_count == 0);
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
            CHECK(!t.records[i].blocked_debt && !t.records[i].blocked_resource);
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

    puts("Transcript slots: dispose on zeroed state, deterministic pool-calloc "
        "failure leaving the transcript safe with all records unbound, "
        "successful create with exactly one allocation and a fully unbound "
        "pool (bounded seam off, measurer untouched, arena diagnostics zero, "
        "no stale stamps or blocked flags), idempotent double dispose, and "
        "accessor refusals after disposal passed");
    return 0;
}
