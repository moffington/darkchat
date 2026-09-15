#include "transcript_policy.h"

bool transcript_policy_visible(int y, int height, int scroll, int page,
    int h_min) {
    if (page < 0) page = 0;
    int h = height > 0 ? height : (h_min > 0 ? h_min : 1);
    return y < scroll + page && y + h > scroll;
}

bool transcript_policy_protected(const TranscriptPolicyItem *item) {
    if (!item) return false;
    return item->streaming || item->focused || item->debt || item->expanded;
}

int transcript_policy_rank(const TranscriptPolicyItem *item, int scroll,
    int page, int h_min) {
    if (!item) return 2;
    if (transcript_policy_visible(item->y, item->height, scroll, page,
            h_min)) return 0;
    if (transcript_policy_protected(item)) return 1;
    return 2;
}

bool transcript_policy_before(const TranscriptPolicyItem *a,
    const TranscriptPolicyItem *b, int scroll, int page, int h_min) {
    int rank_a = transcript_policy_rank(a, scroll, page, h_min);
    int rank_b = transcript_policy_rank(b, scroll, page, h_min);
    if (rank_a != rank_b) return rank_a < rank_b;
    return a->index < b->index;
}

int transcript_policy_needed_slots(const TranscriptPolicyItem *items,
    int count, int scroll, int page, int h_min, int spare_slots) {
    if (!items || count <= 0) return 0;
    int must = 0;
    for (int i = 0; i < count; i++) {
        if (transcript_policy_visible(items[i].y, items[i].height, scroll,
                page, h_min) || transcript_policy_protected(&items[i]))
            ++must;
    }
    if (must > count) must = count;
    int spare = spare_slots > 0 ? spare_slots : 0;
    if (spare > count - must) spare = count - must;
    return must + spare;
}

int transcript_policy_pick_victim(const TranscriptPolicyItem *bound,
    int slot_count, int scroll, int page, int h_min) {
    if (!bound || slot_count <= 0) return -1;
    for (int s = 0; s < slot_count; s++)
        if (bound[s].index < 0) return s;   /* a free slot is always taken */
    int victim = -1;
    for (int s = 0; s < slot_count; s++) {
        const TranscriptPolicyItem *item = &bound[s];
        if (transcript_policy_visible(item->y, item->height, scroll, page,
                h_min) || transcript_policy_protected(item)) continue;
        if (victim < 0 || item->last_used < bound[victim].last_used)
            victim = s;
    }
    return victim;
}

bool transcript_policy_same_message(uint64_t slot_conversation,
    uint64_t slot_message, uint64_t record_conversation,
    uint64_t record_message) {
    return slot_conversation == record_conversation &&
        slot_message == record_message;
}
