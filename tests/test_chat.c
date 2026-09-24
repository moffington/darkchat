#include "chat/core/chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DarkChat application-state tests. No Windows APIs, so they run anywhere.
   Built and run by `chat.bat test`. */

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

/* Allocation seam for growth-boundary and allocation-failure tests: the chat
   test links with -Wl,--wrap=realloc and -Wl,--wrap=malloc (chat.bat test
   does), so every allocation the message-growth path makes starts poisoned —
   the spare capacity beyond a copied string is then known memory, which makes
   the crossing checks deterministic instead of reading whatever malloc left
   there — and the countdowns below force deterministic allocation failures.
   Arm a countdown immediately before the operation under test and reset it
   right after, so only that operation observes the failure. */
void *__real_realloc(void *pointer, size_t size);
void *__real_malloc(size_t size);
static long fail_next_reallocs, fail_next_mallocs;
/* One combined allocation sequence across BOTH wrappers: when armed, the
   allocation in position N (counting mallocs and reallocs together) fails
   exactly once. This is what models "the Nth allocation of an operation". */
static long alloc_number, fail_allocation;
static bool allocation_position(void) {
    long position=++alloc_number;
    if (position==fail_allocation) {
        fail_allocation=0; alloc_number=0;
        return true;
    }
    return false;
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (fail_allocation > 0 && allocation_position()) return NULL;
    if (fail_next_reallocs > 0) { --fail_next_reallocs; return NULL; }
    void *grown = __real_realloc(pointer, size);
    /* Poison only fresh allocations. Growing an existing buffer must keep
       the bytes realloc already copied, or live message text and overflow
       pointers would be destroyed by the seam itself. */
    if (grown && !pointer) memset(grown, 0x5C, size);
    return grown;
}
void *__wrap_malloc(size_t size) {
    if (fail_allocation > 0 && allocation_position()) return NULL;
    if (fail_next_mallocs > 0) { --fail_next_mallocs; return NULL; }
    return __real_malloc(size);
}

/* Checks the documented allocation invariants (chat.h) for every
   conversation of a chat, plus the content-part representation invariants
   (projection authority, bounded arrays, v1 shape). */
static int parts_consistent(const ChatMessage *m) {
    if (!m->parts.items)
        return m->parts.count == 0 && m->parts.capacity == 0;
    if (!m->parts.count || m->parts.count > m->parts.capacity ||
            m->parts.capacity > CHAT_MAX_PARTS ||
            m->parts.count > CHAT_MAX_PARTS)
        return 0;
    int text_parts = 0;
    for (size_t i = 0; i < m->parts.count; i++) {
        const ChatPart *p = &m->parts.items[i];
        if (p->reserved != 0) return 0;
        if (p->kind == CHAT_PART_TEXT) {
            if (i != 0 || p->flags != 0 || ++text_parts > 1) return 0;
        } else if (p->kind == CHAT_PART_IMAGE) {
            if ((p->flags & (uint8_t)~CHAT_PART_FLAG_MASK) != 0) return 0;
        } else {
            return 0;
        }
    }
    /* The plain-text projection equals the concatenation of TEXT parts. */
    const wchar_t *actual = chat_message_text(m);
    size_t pos = 0;
    for (size_t i = 0; i < m->parts.count; i++) {
        const ChatPart *p = &m->parts.items[i];
        if (p->kind != CHAT_PART_TEXT) continue;
        size_t n = p->u.text.length;
        if (wcsnlen(actual + pos, n) != n) return 0;
        if (n && wmemcmp(actual + pos, p->u.text.data, n) != 0) return 0;
        pos += n;
    }
    return actual[pos] == 0;
}

static void check_invariants(Chat *chat) {
    int parts_bad = 0;
    for (int i = 0; i < chat->conversation_count; i++) {
        const ChatConversation *c = &chat->conversations[i];
        check(c->message_capacity <= CHAT_MAX_MESSAGES,
            "capacity never exceeds CHAT_MAX_MESSAGES");
        check(c->message_count <= c->message_capacity,
            "message_count never exceeds message_capacity");
        check((c->messages == NULL) == (c->message_capacity == 0),
            "messages is NULL exactly when capacity is zero");
        check(c->message_count == 0 || c->messages != NULL,
            "live messages imply a live allocation");
        for (size_t j = 0; j < c->message_count; j++)
            if (!parts_consistent(&c->messages[j])) ++parts_bad;
    }
    check(parts_bad == 0,
        "content parts stay within the representation invariants");
}

/* Builds the active conversation from `pairs` user/assistant turns (the
   assistant COMPLETE, so retries are legal) plus `extra_users` trailing user
   messages, via plain appends. */
static void fill_conversation(Chat *chat, int pairs, int extra_users) {
    for (int i = 0; i < pairs; i++) {
        chat_append(chat, CHAT_ROLE_USER, L"question");
        int answer = chat_append(chat, CHAT_ROLE_ASSISTANT, L"answer");
        chat->conversations[chat->active].messages[answer].generation.state =
            CHAT_GENERATION_COMPLETE;
    }
    for (int i = 0; i < extra_users; i++)
        chat_append(chat, CHAT_ROLE_USER, L"trailing question");
}

int main(void) {
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!chat) return 2;
    chat_init(chat);
    check(chat->conversation_count == 1, "init creates one conversation");
    check(chat->active == 0, "init selects the first conversation");
    check(wcscmp(chat->model, L"openai/gpt-4o-mini") == 0, "init sets default model");
    const ChatConversation *active = chat_active(chat);
    check(active && active->message_count == 1, "welcome message present");
    check(chat_remaining(chat) == CHAT_MAX_MESSAGES - 1, "remaining after welcome");

    /* Fixed-residue amplification gate. This bounds the fixed struct/slack
        cost that every structural copy carries: live state plus the saver's
        pending and in-flight snapshots plus the snapshot under construction,
        which is built before the displaced pending copy is disposed (4
        simultaneous structural copies). It deliberately excludes heap-backed
        live text — message overflow and, since format 4, the owned
        customization text (per-conversation prompt overrides and profile
        prompts), which is proportional to actual content — so it is not a
        complete worst-case memory bound. The customization structs add only
        the inline profile name slots and the per-conversation model override
        arrays to the fixed residue. CHAT_MAX_MESSAGES is the shipped
        bound (512, raised by format 3), and the gate must hold at it. */
    {
        const size_t structural_copies = 4;
        size_t amplified = structural_copies *
            (sizeof(Chat) + (size_t)CHAT_MAX_CONVERSATIONS * CHAT_MAX_MESSAGES *
                sizeof(ChatMessage));
        printf("amplification gate: sizeof(Chat)=%zu sizeof(ChatMessage)=%zu "
            "peak=%zu bytes\n", sizeof(Chat), sizeof(ChatMessage), amplified);
        check(amplified <= ((size_t)1 << 30),
            "fixed structural amplification stays within 1 GiB at the shipped limits");
    }

    ChatGeneration generation;
    chat_generation_init(&generation);
    check(generation.reasoning_ms == -1,
        "generation init marks reasoning duration unavailable");
    check(!active->messages[0].reasoning[0], "new messages carry no reasoning");
    check(!active->messages[0].reasoning_open, "new messages start collapsed");

    chat_append(chat, CHAT_ROLE_USER, L"Explain native text controls");
    active = chat_active(chat);
    check(active->message_count == 2, "user message appended");
    check(wcscmp(active->title, L"Explain native text controls") == 0,
        "first user message names the conversation");

    /* The generated reply is far beyond the message inline residue, so it
       needs a buffer sized to the fake reply itself, not to the residue. */
    wchar_t reply[2048];
    chat_fake_reply(chat, L"Explain native text controls", reply, 2048);
    check(wcslen(reply) > 40, "fake reply has content");
    check(wcsstr(reply, L"```") != NULL, "fake reply contains a code fence");
    check(wcsstr(reply, L"http") != NULL, "fake reply contains a URL");
    check(wcsstr(reply, L"Explain native text controls") != NULL,
        "fake reply echoes the prompt");
    check(wcslen(chat->status) > 0, "fake reply sets status");

    int before = chat->conversation_count;
    check(chat_new_conversation(chat) == before, "new conversation appends");
    check(chat->active == before, "new conversation becomes active");
    check(chat_active(chat)->message_count == 0, "new conversation is empty");
    check(chat_select_conversation(chat, 0), "select original conversation");
    check(chat->active == 0, "selection sticks");
    check(!chat_select_conversation(chat, 99), "invalid selection rejected");

    /* Capacity contract the host's send guard relies on: a two-message send
       succeeds only while two slots remain, and appends never overflow. */
    while (chat_remaining(chat) > 2)
        chat_append(chat, CHAT_ROLE_ASSISTANT, L"filler");
    check(chat_remaining(chat) == 2, "two slots left before the last send");
    check(chat_append(chat, CHAT_ROLE_USER, L"last user") >= 0, "user fits");
    check(chat_append(chat, CHAT_ROLE_ASSISTANT, L"last reply") >= 0, "reply fits");
    check(chat_remaining(chat) == 0, "conversation is now full");
    check(chat_append(chat, CHAT_ROLE_USER, L"overflow") < 0, "overflow rejected");
    check(chat_active(chat)->message_count == CHAT_MAX_MESSAGES, "count capped");

    /* Oversized generated text grows beyond the inline buffer. */
    Chat *spare = (Chat *)calloc(1, sizeof *spare);
    if (!spare) return 2;
    chat_init(spare);
    wchar_t *big = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_MESSAGE_INLINE * 2));
    if (!big) return 2;
    for (size_t i = 0; i < CHAT_MESSAGE_INLINE * 2 - 1; i++) big[i] = L'x';
    big[CHAT_MESSAGE_INLINE * 2 - 1] = 0;
    chat_append(spare, CHAT_ROLE_USER, big);
    const ChatMessage *last =
        &chat_active(spare)->messages[chat_active(spare)->message_count - 1];
    check(wcslen(chat_message_text(last)) == CHAT_MESSAGE_INLINE * 2 - 1,
        "oversized model text grows beyond inline storage");
    free(big);

    /* A trailing high surrogate is not left dangling. */
    chat_append(spare, CHAT_ROLE_USER, L"tail\xd83d");
    last = &chat_active(spare)->messages[chat_active(spare)->message_count - 1];
    check(wcslen(chat_message_text(last)) == 4,
        "dangling high surrogate trimmed");
    chat_dispose(spare); free(spare);

    /* A normal long response now fits: the old 4,096 bound cancelled it. */
    Chat *long_chat = (Chat *)calloc(1, sizeof *long_chat);
    if (!long_chat) return 2;
    chat_init(long_chat);
    wchar_t *mid = (wchar_t *)malloc(sizeof(wchar_t) * 12001);
    if (!mid) return 2;
    for (size_t i = 0; i < 12000; i++) mid[i] = L'y';
    mid[12000] = 0;
    chat_append(long_chat, CHAT_ROLE_USER, L"question");
    int mid_index = chat_append(long_chat, CHAT_ROLE_ASSISTANT, mid);
    check(mid_index >= 0 &&
        wcslen(chat_message_text(
            &chat_active(long_chat)->messages[mid_index])) == 12000,
        "a 12,000-unit response is preserved in overflow storage");
    free(mid);
    chat_dispose(long_chat); free(long_chat);

    /* Streamed growth across the inline boundary must copy exactly the
       inline characters onto the heap; a wrong copy count still leaves
       the string NUL-terminated, so only memory checks can see the damage. */
    Chat *grow = (Chat *)calloc(1, sizeof *grow);
    if (!grow) return 2;
    chat_init(grow);
    wchar_t *fill = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_MESSAGE_INLINE + 1));
    if (!fill) return 2;
    for (size_t i = 0; i < CHAT_MESSAGE_INLINE - 1; i++) fill[i] = L'x';
    fill[CHAT_MESSAGE_INLINE - 1] = 0;
    int stream = chat_append(grow, CHAT_ROLE_ASSISTANT, L"");
    check(stream >= 0, "empty assistant message appended");
    ChatMessage *m = &grow->conversations[grow->active].messages[stream];
    check(chat_message_append_text(m, fill) && !m->text_overflow,
        "a boundary-minus-one answer stays inline");
    check(chat_message_append_text(m, L"!"),
        "appending past the boundary succeeds");
    check(m->text_overflow != NULL, "the crossed answer moved to heap storage");
    check(wcslen(chat_message_text(m)) == CHAT_MESSAGE_INLINE,
        "the crossed answer keeps every character");
    check(chat_message_text(m)[0] == L'x' &&
        chat_message_text(m)[CHAT_MESSAGE_INLINE - 2] == L'x' &&
        chat_message_text(m)[CHAT_MESSAGE_INLINE - 1] == L'!',
        "the crossed answer preserves head, seam and tail");
    /* Further growth reuses and doubles the heap buffer without touching the
       inline source again; content must survive every hop, including a
       subsequent heap-doubling reallocation. */
    for (int i = 0; i < 3; i++) {
        check(chat_message_append_text(m, fill) &&
            wcslen(chat_message_text(m)) ==
                CHAT_MESSAGE_INLINE + (size_t)(i + 1) * (CHAT_MESSAGE_INLINE - 1),
            "repeated heap growth preserves the streamed answer");
    }

    /* The reasoning buffer has the same inline size and grows identically,
       without disturbing the answer stored beside it. */
    for (size_t i = 0; i < CHAT_MESSAGE_INLINE - 1; i++) fill[i] = L'r';
    fill[CHAT_MESSAGE_INLINE - 1] = 0;
    int think = chat_append(grow, CHAT_ROLE_ASSISTANT, L"");
    check(think >= 0, "second assistant message appended");
    ChatMessage *t = &grow->conversations[grow->active].messages[think];
    check(chat_message_append_reasoning(t, fill) && !t->reasoning_overflow,
        "a boundary-minus-one reasoning stays inline");
    check(chat_message_append_reasoning(t, L"!"),
        "reasoning appending past the boundary succeeds");
    check(t->reasoning_overflow != NULL &&
        wcslen(chat_message_reasoning(t)) == CHAT_MESSAGE_INLINE &&
        chat_message_reasoning(t)[0] == L'r' &&
        chat_message_reasoning(t)[CHAT_MESSAGE_INLINE - 2] == L'r' &&
        chat_message_reasoning(t)[CHAT_MESSAGE_INLINE - 1] == L'!',
        "the crossed reasoning preserves head, seam and tail");
    check(!t->text[0] && !t->text_overflow,
        "answer storage is untouched by reasoning growth");
    free(fill);
    chat_dispose(grow); free(grow);

    /* Inline-residue contract: the exact set-path boundaries at
       capacity-1/capacity/capacity+1, shrinking back to inline, and
       re-promoting afterwards. The set path draws the boundary one unit
       tighter than the append path (needed < inline_capacity fits), so a
       capacity-unit message is already on overflow storage. */
    {
        ChatMessage *probe = (ChatMessage *)calloc(1, sizeof *probe);
        check(probe != NULL, "inline-residue probe allocated");
        if (probe) {
            wchar_t text[CHAT_MESSAGE_INLINE + 2];
            for (size_t i = 0; i < CHAT_MESSAGE_INLINE - 1; i++) text[i] = L'a';
            text[CHAT_MESSAGE_INLINE - 1] = 0;
            check(chat_message_set_text(probe, text) && !probe->text_overflow &&
                probe->text_length == CHAT_MESSAGE_INLINE - 1,
                "a capacity-minus-one message set stays inline");
            text[CHAT_MESSAGE_INLINE - 1] = L'a';
            text[CHAT_MESSAGE_INLINE] = 0;
            check(chat_message_set_text(probe, text) && probe->text_overflow &&
                probe->text_length == CHAT_MESSAGE_INLINE &&
                probe->text_capacity == CHAT_MESSAGE_INLINE + 1,
                "a capacity-unit message set promotes to overflow");
            text[CHAT_MESSAGE_INLINE] = L'b';
            text[CHAT_MESSAGE_INLINE + 1] = 0;
            check(chat_message_set_text(probe, text) && probe->text_overflow &&
                probe->text_length == CHAT_MESSAGE_INLINE + 1,
                "a capacity-plus-one message set promotes to overflow");
            check(chat_message_set_text(probe, L"back") && !probe->text_overflow &&
                probe->text_capacity == 0 && probe->text_length == 4 &&
                !wcscmp(probe->text, L"back"),
                "setting a short message shrinks back to inline storage");
            text[CHAT_MESSAGE_INLINE + 1] = 0;
            check(chat_message_set_text(probe, text) && probe->text_overflow &&
                probe->text_length == CHAT_MESSAGE_INLINE + 1,
                "the message promotes to overflow again after shrinking");
            chat_message_dispose(probe);
            free(probe);
        }
    }

    /* The newly fallible short-to-overflow transitions are transactional:
       a failed promotion leaves the message exactly as it was, inline
       content included, and the same operation succeeds once allocation is
       allowed again. */
    {
        ChatMessage *cross = (ChatMessage *)calloc(1, sizeof *cross);
        ChatMessage *setfail = (ChatMessage *)calloc(1, sizeof *setfail);
        check(cross != NULL && setfail != NULL, "transition probes allocated");
        if (cross && setfail) {
            wchar_t fill[CHAT_MESSAGE_INLINE + 1];
            for (size_t i = 0; i < CHAT_MESSAGE_INLINE - 1; i++) fill[i] = L'x';
            fill[CHAT_MESSAGE_INLINE - 1] = 0;
            check(chat_message_append_text(cross, fill) && !cross->text_overflow,
                "the crossing probe fills the inline residue");
            uint64_t before_revision = cross->revision;
            fail_next_reallocs = 1;
            bool crossed = chat_message_append_text(cross, L"!");
            fail_next_reallocs = 0;
            check(!crossed, "the inline-to-overflow transition can fail");
            check(!cross->text_overflow && cross->text_capacity == 0 &&
                cross->text_length == CHAT_MESSAGE_INLINE - 1 &&
                !wcsncmp(cross->text, fill, CHAT_MESSAGE_INLINE - 1) &&
                cross->revision == before_revision,
                "a failed transition leaves the message inline and unchanged");
            check(chat_message_append_text(cross, L"!") && cross->text_overflow &&
                cross->text_length == CHAT_MESSAGE_INLINE,
                "the same transition succeeds once allocation is allowed");
            chat_message_dispose(cross);

            check(chat_message_set_text(setfail, L"seed") &&
                !setfail->text_overflow, "the promotion probe seeds inline");
            wchar_t long_text[CHAT_MESSAGE_INLINE + 8];
            for (size_t i = 0; i < CHAT_MESSAGE_INLINE + 7; i++) long_text[i] = L'y';
            long_text[CHAT_MESSAGE_INLINE + 7] = 0;
            fail_next_mallocs = 1;
            bool stored = chat_message_set_text(setfail, long_text);
            fail_next_mallocs = 0;
            check(!stored && !setfail->text_overflow &&
                setfail->text_length == 4 && !wcscmp(setfail->text, L"seed"),
                "a failed promotion leaves the previous inline content intact");
            check(chat_message_set_text(setfail, long_text) &&
                setfail->text_overflow != NULL,
                "the promotion succeeds once allocation is allowed");
            chat_message_dispose(setfail);
        }
        free(cross); free(setfail);
    }

    /* The reasoning promotion has the same transactional failure behavior:
       a failed set leaves the inline content, length and revision exactly as
       they were, and the same operation succeeds once allocation is allowed. */
    {
        ChatMessage *reasonfail = (ChatMessage *)calloc(1, sizeof *reasonfail);
        check(reasonfail != NULL, "reasoning promotion probe allocated");
        if (reasonfail) {
            check(chat_message_set_reasoning(reasonfail, L"seed") &&
                !reasonfail->reasoning_overflow,
                "the reasoning promotion probe seeds inline");
            uint64_t before_revision = reasonfail->revision;
            wchar_t long_text[CHAT_REASONING_INLINE + 8];
            for (size_t i = 0; i < CHAT_REASONING_INLINE + 7; i++)
                long_text[i] = L'y';
            long_text[CHAT_REASONING_INLINE + 7] = 0;
            fail_next_mallocs = 1;
            bool stored = chat_message_set_reasoning(reasonfail, long_text);
            fail_next_mallocs = 0;
            check(!stored && !reasonfail->reasoning_overflow &&
                reasonfail->reasoning_length == 4 &&
                !wcscmp(reasonfail->reasoning, L"seed") &&
                reasonfail->revision == before_revision,
                "a failed reasoning promotion leaves inline content and revision intact");
            check(chat_message_set_reasoning(reasonfail, long_text) &&
                reasonfail->reasoning_overflow != NULL &&
                reasonfail->reasoning_length == CHAT_REASONING_INLINE + 7,
                "the reasoning promotion succeeds once allocation is allowed");
            chat_message_dispose(reasonfail);
            free(reasonfail);
        }
    }

    /* A protected boundary catches overreads that content checks cannot: a
       too-large copy count still produces a correct, NUL-terminated string
       while dragging bytes of the sibling buffer past the terminator. The
       reasoning storage carries a sentinel, and the overflow storage starts
       poisoned by the realloc seam, so any overread shows up as sentinel
       values displacing the poison in the spare capacity. */
    {
        ChatMessage *guarded = (ChatMessage *)calloc(1, sizeof *guarded);
        check(guarded != NULL, "guarded message allocated");
        if (guarded) {
            memset(guarded->reasoning, 0xA5, sizeof guarded->reasoning);
            guarded->role = CHAT_ROLE_ASSISTANT;
            wchar_t *fill = (wchar_t *)malloc(sizeof(wchar_t) * CHAT_MESSAGE_INLINE);
            if (!fill) return 2;
            for (size_t i = 0; i < CHAT_MESSAGE_INLINE - 1; i++) fill[i] = L'w';
            fill[CHAT_MESSAGE_INLINE - 1] = 0;
            check(chat_message_append_text(guarded, fill),
                "the guarded message fills its inline buffer");
            check(chat_message_append_text(guarded, L"!"),
                "the guarded boundary crossing succeeds");
            check(guarded->text_overflow != NULL &&
                wcslen(chat_message_text(guarded)) == CHAT_MESSAGE_INLINE &&
                chat_message_text(guarded)[0] == L'w' &&
                chat_message_text(guarded)[CHAT_MESSAGE_INLINE - 1] == L'!',
                "the guarded crossing preserves the message");
            int intact = 1;
            /* Spare capacity begins one past the new terminator and ends at
               the promoted allocation's own capacity. */
            for (size_t i = CHAT_MESSAGE_INLINE + 1; i < guarded->text_capacity; i++)
                if (guarded->text_overflow[i] != (wchar_t)0x5C5C) intact = 0;
            check(intact,
                "the crossing copied nothing beyond the inline terminator");
            chat_message_dispose(guarded);
            free(guarded);
            free(fill);
        }
    }

    /* Revision bookkeeping: bumped only when observable content changes,
       never speculatively. View state only; never persisted. */
    {
        Chat *rev = (Chat *)calloc(1, sizeof *rev);
        if (!rev) return 2;
        chat_init(rev);
        chat_clear(rev);
        int rev_index = chat_append(rev, CHAT_ROLE_USER, L"revision probe");
        check(rev_index == 0, "revision probe appended");
        ChatMessage *rm = &rev->conversations[rev->active].messages[rev_index];
        uint64_t before = rm->revision;
        check(chat_message_set_text(rm, L"revision probe") &&
            rm->revision == before,
            "setting identical text leaves the revision alone");
        check(chat_message_set_text(rm, L"changed") &&
            rm->revision == before + 1,
            "changed text bumps the revision");
        before = rm->revision;
        check(chat_message_append_text(rm, L" more") &&
            rm->revision == before + 1, "appended text bumps the revision");
        check(chat_message_append_text(rm, L"") &&
            rm->revision == before + 1,
            "an empty append is not a change");
        before = rm->revision;
        check(chat_message_set_reasoning(rm, L"thinking") &&
            rm->revision == before + 1,
            "reasoning set bumps the revision");
        check(chat_message_set_reasoning(rm, L"thinking") &&
            rm->revision == before + 1,
            "identical reasoning leaves the revision alone");
        before = rm->revision;
        check(chat_message_append_reasoning(rm, L"!") &&
            rm->revision == before + 1,
            "appended reasoning bumps the revision");
        before = rm->revision;
        chat_message_touch(rm);
        check(rm->revision == before + 1,
            "chat_message_touch marks a direct mutation");
        check(rm->id != 0, "appended messages carry an instance id");
        chat_dispose(rev);
        free(rev);
    }

    /* chat_remaining guards invalid state. */
    chat->active = 99;
    check(chat_remaining(chat) == 0, "invalid active has no remaining");

    /* Late replies land in the conversation they were asked for, even after
       the user switched away. */
    Chat *late = (Chat *)calloc(1, sizeof *late);
    if (!late) return 2;
    chat_init(late);
    chat_append(late, CHAT_ROLE_USER, L"question");
    int origin = late->active;
    check(chat_new_conversation(late) >= 0, "switch to a second conversation");
    check(chat_append_at(late, origin, CHAT_ROLE_ASSISTANT, L"late reply") >= 0,
        "append targets the given conversation");
    check(chat_active(late)->message_count == 0, "active conversation untouched");
    check(late->conversations[origin].message_count == 3,
        "origin conversation received the reply");
    check(chat_append_at(late, -1, CHAT_ROLE_USER, L"no") < 0,
        "invalid conversation rejected");
    check(chat_append_at(late, late->conversation_count, CHAT_ROLE_USER, L"no")
        < 0, "out-of-range conversation rejected");
    chat_dispose(late);
    free(late);

    /* Growth policy: transactional doubling to the hard cap, and the
       never-used conversation shape. */
    {
        Chat *growth = (Chat *)calloc(1, sizeof *growth);
        if (!growth) return 2;
        chat_init(growth);
        check(chat_new_conversation(growth) == 1, "growth conversation created");
        const ChatConversation *c = &growth->conversations[1];
        check(c->messages == NULL && c->message_count == 0 &&
            c->message_capacity == 0,
            "a never-used conversation has zero capacity and a NULL pointer");
        check(chat_append(growth, CHAT_ROLE_USER, L"first") == 0,
            "first message appended");
        check(c->message_capacity == 8,
            "the first allocation reserves the initial turn budget");
        while (c->message_count < 8)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        check(c->message_capacity == 8,
            "capacity holds while messages fit");
        chat_append(growth, CHAT_ROLE_USER, L"ninth");
        check(c->message_capacity == 16,
            "growth doubles past the initial budget");
        while (c->message_count < 16)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        chat_append(growth, CHAT_ROLE_USER, L"seventeenth");
        check(c->message_capacity == 32, "growth doubles again past 16");
        while (c->message_count < 32)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        chat_append(growth, CHAT_ROLE_USER, L"thirty-third");
        check(c->message_capacity == 64, "growth doubles to 64");
        while (c->message_count < 64)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        chat_append(growth, CHAT_ROLE_USER, L"sixty-fifth");
        check(c->message_capacity == 128, "growth doubles past 64");
        while (c->message_count < 128)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        chat_append(growth, CHAT_ROLE_USER, L"129th");
        check(c->message_capacity == 256, "growth doubles past 128");
        while (c->message_count < 256)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        chat_append(growth, CHAT_ROLE_USER, L"257th");
        check(c->message_capacity == 512, "growth doubles to the hard cap");
        while (c->message_count < 512)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        check(c->message_count == 512 && c->message_capacity == 512,
            "the conversation holds exactly 512 messages at full capacity");
        check(chat_append(growth, CHAT_ROLE_USER, L"overflow") < 0,
            "no message is accepted beyond the cap");
        check(c->message_count == 512 && c->message_capacity == 512,
            "capacity never rises above 512");
        check_invariants(growth);
        chat_dispose(growth); free(growth);
    }

    /* chat_append_at failure atomicity: allocation failure is invisible to
       the conversation. */
    {
        Chat *atom = (Chat *)calloc(1, sizeof *atom);
        if (!atom) return 2;
        chat_init(atom); chat_clear(atom);
        const ChatConversation *c = chat_active(atom);
        uint64_t seed_next_id = atom->next_id;
        int64_t seed_modified_at = c->modified_at;
        wchar_t seed_title[CHAT_TITLE_TEXT];
        wcscpy(seed_title, c->title);

        /* A failed first growth allocates nothing and changes nothing. */
        fail_next_reallocs = 1;
        int rejected = chat_append(atom, CHAT_ROLE_USER, L"never stored");
        fail_next_reallocs = 0;
        check(rejected < 0, "a failed array growth rejects the append");
        check(c->message_count == 0 && atom->next_id == seed_next_id &&
            c->modified_at == seed_modified_at &&
            !wcscmp(c->title, seed_title),
            "a failed append consumes no id, timestamp or title");
        check(c->messages == NULL && c->message_capacity == 0,
            "a failed first growth leaves no allocation");
        check_invariants(atom);

        check(chat_append(atom, CHAT_ROLE_USER, L"kept") == 0,
            "atomicity seed appended");
        /* Recapture after the seed: a first user message names the
           conversation and stamps it. */
        seed_next_id = atom->next_id;
        seed_modified_at = c->modified_at;
        wcscpy(seed_title, c->title);

        /* A construction failure without growth keeps capacity exactly as it
           was and commits nothing. */
        wchar_t *oversized = (wchar_t *)malloc(
            sizeof(wchar_t) * (CHAT_MESSAGE_INLINE + 700));
        if (!oversized) return 2;
        for (size_t i = 0; i < CHAT_MESSAGE_INLINE + 699; i++) oversized[i] = L'x';
        oversized[CHAT_MESSAGE_INLINE + 699] = 0;
        fail_next_mallocs = 1;
        rejected = chat_append(atom, CHAT_ROLE_USER, oversized);
        fail_next_mallocs = 0;
        check(rejected < 0,
            "a failed message construction rejects the append");
        check(c->message_count == 1 && atom->next_id == seed_next_id &&
            c->modified_at == seed_modified_at &&
            !wcscmp(c->title, seed_title) &&
            !wcscmp(chat_message_text(&c->messages[0]), L"kept"),
            "construction failure commits nothing");
        check(c->messages != NULL && c->message_capacity == 8,
            "construction failure needs no growth and keeps capacity");
        check_invariants(atom);

        /* Growth succeeded, construction failed: the retained capacity is
           harmless and every invariant holds on the empty conversation. */
        Chat *fresh = (Chat *)calloc(1, sizeof *fresh);
        if (!fresh) return 2;
        chat_init(fresh); chat_clear(fresh);
        const ChatConversation *empty = chat_active(fresh);
        uint64_t fresh_next_id = fresh->next_id;
        wchar_t fresh_title[CHAT_TITLE_TEXT];
        wcscpy(fresh_title, empty->title);
        fail_next_mallocs = 1;
        rejected = chat_append(fresh, CHAT_ROLE_USER, oversized);
        fail_next_mallocs = 0;
        check(rejected < 0,
            "a construction failure after a successful growth rejects the append");
        check(empty->messages != NULL && empty->message_count == 0 &&
            empty->message_capacity == 8,
            "an empty conversation may retain capacity from a grown-then-failed append");
        check(fresh->next_id == fresh_next_id &&
            !wcscmp(empty->title, fresh_title),
            "the grown-then-failed append consumed no id and no title");
        check_invariants(fresh);
        free(oversized);
        chat_dispose(atom); free(atom);
        chat_dispose(fresh); free(fresh);
    }

    /* chat_begin_response transactionality: every mode preflights its final
       post-operation shape, so allocation failure never partially mutates. */
    {
        Chat *br = (Chat *)calloc(1, sizeof *br);
        if (!br) return 2;
        chat_init(br); chat_clear(br);
        const ChatConversation *c = chat_active(br);
        uint64_t next_id;
        int64_t modified_at;
        int rc;

        /* CHAT_SEND failing on the growth it needs. */
        fill_conversation(br, 4, 0);       /* 8 messages, capacity 8 */
        next_id = br->next_id; modified_at = c->modified_at;
        fail_next_reallocs = 1;
        rc = chat_begin_response(br, CHAT_SEND, L"new question");
        fail_next_reallocs = 0;
        check(rc < 0, "a send that must grow the array fails on allocation");
        check(c->message_count == 8 && br->next_id == next_id &&
            c->modified_at == modified_at,
            "a failed send leaves the conversation unchanged");
        check(!wcscmp(c->messages[0].text, L"question") &&
            !wcscmp(c->messages[7].text, L"answer"),
            "existing messages survive a failed send");
        rc = chat_begin_response(br, CHAT_SEND, L"new question");
        check(rc == 9, "the same send succeeds once growth is allowed");
        check(c->message_count == 10 && c->message_capacity == 16,
            "the send grew the array exactly once");
        check(!wcscmp(c->messages[8].text, L"new question") &&
            c->messages[9].generation.state == CHAT_GENERATION_RUNNING,
            "the successful send appended user and running assistant turns");
        check_invariants(br);

        /* Replacement modes failing on growth before any trim or edit. */
        chat_clear(br);
        fill_conversation(br, 3, 2);       /* trailing users at 6 and 7 */
        next_id = br->next_id; modified_at = c->modified_at;
        fail_next_reallocs = 1;
        rc = chat_begin_response(br, CHAT_RETRY, NULL);
        fail_next_reallocs = 0;
        check(rc < 0, "a retry that must grow the array fails on allocation");
        check(c->message_count == 8 && br->next_id == next_id &&
            c->modified_at == modified_at,
            "a failed retry leaves the conversation unchanged");
        check(!wcscmp(c->messages[7].text, L"trailing question"),
            "a failed retry trims nothing");
        fail_next_reallocs = 1;
        rc = chat_begin_response(br, CHAT_EDIT_RESEND, L"edited while failing");
        fail_next_reallocs = 0;
        check(rc < 0, "an edit-resend that must grow fails on allocation");
        check(!wcscmp(c->messages[7].text, L"trailing question"),
            "a failed edit-resend leaves the user text unedited");
        check(c->message_count == 8 && br->next_id == next_id &&
            c->modified_at == modified_at,
            "a failed edit-resend mutates nothing");
        fail_next_reallocs = 1;
        rc = chat_begin_response(br, CHAT_REGENERATE, NULL);
        fail_next_reallocs = 0;
        check(rc < 0, "a regenerate that must grow fails on allocation");
        check(c->message_count == 8 && br->next_id == next_id,
            "a failed regenerate mutates nothing");

        /* The same replacements succeed once growth is allowed, retaining
           capacity across the trim. */
        rc = chat_begin_response(br, CHAT_RETRY, NULL);
        check(rc == 8, "retry succeeds after the failed attempts");
        check(c->message_count == 9 && c->message_capacity == 16 &&
            c->messages != NULL,
            "retry trimmed into retained capacity");
        check(!wcscmp(c->messages[7].text, L"trailing question") &&
            c->messages[8].generation.state == CHAT_GENERATION_RUNNING,
            "retry kept the trailing user turn and started a response");
        c->messages[8].generation.state = CHAT_GENERATION_COMPLETE;
        rc = chat_begin_response(br, CHAT_EDIT_RESEND, L"edited for real");
        check(rc == 8, "edit-resend succeeds after the failed attempt");
        check(!wcscmp(c->messages[7].text, L"edited for real") &&
            c->message_count == 9,
            "edit-resend replaced the user text and response");
        check_invariants(br);
        chat_dispose(br); free(br);
    }

    /* Product input limit: chat_begin_response itself enforces the composer's
       16,383-unit bound, independently of the much smaller message inline
       residue, so the residue can never drift into becoming the input limit.
       Prompts past the residue are accepted and stored through overflow. */
    {
        wchar_t *prompt = (wchar_t *)malloc(
            sizeof(wchar_t) * (CHAT_COMPOSER_TEXT + 1));
        Chat *limit = (Chat *)calloc(1, sizeof *limit);
        check(prompt != NULL && limit != NULL, "input-limit fixture allocated");
        if (prompt && limit) {
            for (size_t i = 0; i < CHAT_COMPOSER_TEXT; i++) prompt[i] = L'x';
            prompt[CHAT_COMPOSER_TEXT] = 0;
            chat_init(limit); chat_clear(limit);
            const ChatConversation *c = chat_active(limit);

            prompt[300] = 0;
            check(chat_begin_response(limit, CHAT_SEND, prompt) >= 0 &&
                c->message_count == 2 &&
                c->messages[0].text_overflow != NULL &&
                c->messages[0].text_length == 300,
                "a prompt past the inline residue is accepted through overflow");
            chat_clear(limit);

            prompt[300] = L'x';
            prompt[CHAT_COMPOSER_TEXT - 1] = 0;
            check(chat_begin_response(limit, CHAT_SEND, prompt) >= 0 &&
                c->messages[0].text_overflow != NULL &&
                c->messages[0].text_length == CHAT_COMPOSER_TEXT - 1,
                "a CHAT_COMPOSER_TEXT - 1 unit prompt is accepted whole");
            chat_clear(limit);

            prompt[CHAT_COMPOSER_TEXT - 1] = L'x';
            uint64_t before_id = limit->next_id;
            int64_t before_modified = c->modified_at;
            size_t before_count = c->message_count;
            check(chat_begin_response(limit, CHAT_SEND, prompt) < 0 &&
                c->message_count == before_count &&
                limit->next_id == before_id &&
                c->modified_at == before_modified,
                "a CHAT_COMPOSER_TEXT unit prompt is rejected transactionally");
            check_invariants(limit);
            chat_dispose(limit);
        }
        free(prompt); free(limit);
    }

    /* Replacement at the 512-message cap: the final live count is within the
        cap, so no growth is needed and the operation succeeds. */
    {
        Chat *cap = (Chat *)calloc(1, sizeof *cap);
        if (!cap) return 2;
        chat_init(cap); chat_clear(cap);
        const ChatConversation *c = chat_active(cap);
        fill_conversation(cap, 256, 0);    /* 512 messages, capacity 512 */
        check(c->message_count == 512 && c->message_capacity == 512,
            "boundary conversation is at the hard cap");
        /* Retry replaces the last response, so the old one must be in a
            failed terminal state first. */
        c->messages[511].generation.state = CHAT_GENERATION_FAILED;
        check(chat_begin_response(cap, CHAT_RETRY, NULL) == 511,
            "retry succeeds at the 512-message cap");
        check(c->message_count == 512 && c->message_capacity == 512,
            "retry at the cap needs no growth");
        check(!wcscmp(c->messages[510].text, L"question") &&
            !c->messages[511].text[0],
            "retry at the cap replaced the last response");
        c->messages[511].generation.state = CHAT_GENERATION_COMPLETE;
        check(chat_begin_response(cap, CHAT_REGENERATE, NULL) == 511,
            "regenerate succeeds at the 512-message cap");
        c->messages[511].generation.state = CHAT_GENERATION_COMPLETE;
        check(chat_begin_response(cap, CHAT_EDIT_RESEND, L"edited at the cap")
            == 511, "edit-resend succeeds at the 512-message cap");
        check(!wcscmp(c->messages[510].text, L"edited at the cap") &&
            c->message_count == 512,
            "edit-resend at the cap replaced the user text");
        check_invariants(cap);

        /* A final shape of 513 is rejected without mutation. */
        chat_clear(cap);
        fill_conversation(cap, 255, 1);    /* 511 messages, last is user */
        uint64_t next_id = cap->next_id;
        int64_t modified_at = c->modified_at;
        check(chat_begin_response(cap, CHAT_SEND, L"one too many") < 0,
            "a send whose final shape needs 513 live messages is rejected");
        check(c->message_count == 511 && cap->next_id == next_id &&
            c->modified_at == modified_at,
            "a rejected send mutates nothing");
        chat_append(cap, CHAT_ROLE_USER, L"512th");  /* user at 511 */
        next_id = cap->next_id; modified_at = c->modified_at;
        check(chat_begin_response(cap, CHAT_REGENERATE, NULL) < 0,
            "a regenerate whose final shape needs 513 live messages is rejected");
        check(chat_begin_response(cap, CHAT_RETRY, NULL) < 0,
            "a retry whose final shape needs 513 live messages is rejected");
        check(chat_begin_response(cap, CHAT_EDIT_RESEND, L"no") < 0,
            "an edit-resend whose final shape needs 513 live messages is rejected");
        check(c->message_count == 512 && cap->next_id == next_id &&
            c->modified_at == modified_at,
            "rejected 513-message replacements mutate nothing");
        check_invariants(cap);
        chat_dispose(cap); free(cap);
    }

    /* Element pointers are invalidated by growth: identity must be
       re-derived by index after any mutating call, and trims retain the
       allocation for reuse. */
    {
        Chat *stable = (Chat *)calloc(1, sizeof *stable);
        if (!stable) return 2;
        chat_init(stable); chat_clear(stable);
        int first = chat_append(stable, CHAT_ROLE_USER, L"stable content");
        ChatMessage *cached = &chat_active(stable)->messages[first];
        check(cached != NULL, "the element pointer is valid before growth");
        /* These appends grow (and realloc) the message array; the cached
           element pointer must not be dereferenced past this point. */
        for (int i = 0; i < 8; i++)
            chat_append(stable, CHAT_ROLE_ASSISTANT, L"padding");
        check(wcslen(chat_message_text(
            &chat_active(stable)->messages[first])) ==
                wcslen(L"stable content"),
            "identity re-derived by index after growth stays intact");
        /* A trim reduces the live range but retains capacity for reuse. */
        chat_active(stable)->messages[first + 1].generation.state =
            CHAT_GENERATION_FAILED;
        check(chat_begin_response(stable, CHAT_RETRY, NULL) == 1,
            "retry after growth succeeds");
        check(chat_active(stable)->message_count == 2 &&
            chat_active(stable)->message_capacity == 16 &&
            chat_active(stable)->messages != NULL,
            "a trim reduces the live range and retains the allocation");
        check_invariants(stable);
        chat_dispose(stable); free(stable);
    }

    /* Stage 6: the 128-conversation bound, stable-identity lookup, and
       deletion invariants at scale. */
    {
        Chat *many = (Chat *)calloc(1, sizeof *many);
        if (!many) return 2;
        chat_init(many);
        for (int i = 1; i < CHAT_MAX_CONVERSATIONS; i++)
            check(chat_new_conversation(many) == i,
                "a conversation is created below the cap");
        check(chat_new_conversation(many) == -1,
            "creation fails closed at the conversation cap");
        check(many->conversation_count == CHAT_MAX_CONVERSATIONS,
            "the conversation cap is reached exactly");
        uint64_t seen[CHAT_MAX_CONVERSATIONS];
        int duplicates = 0;
        for (int i = 0; i < many->conversation_count; i++) {
            seen[i] = many->conversations[i].id;
            for (int j = 0; j < i; j++)
                if (seen[i] == seen[j]) duplicates++;
        }
        check(duplicates == 0, "conversation ids are unique across the range");
        check(chat_index_of_id(many, many->conversations[7].id) == 7,
            "stable-identity lookup resolves a middle conversation");
        check(chat_index_of_id(many, many->conversations[CHAT_MAX_CONVERSATIONS - 1].id)
            == CHAT_MAX_CONVERSATIONS - 1,
            "stable-identity lookup resolves the last conversation");
        check(chat_index_of_id(many, 0) == -1 &&
            chat_index_of_id(many,
                many->conversations[CHAT_MAX_CONVERSATIONS - 1].id + 1) == -1,
            "unknown or zero ids resolve to -1");
        /* Deleting a middle conversation shifts indices but never ids. */
        uint64_t victim = many->conversations[5].id;
        uint64_t survivor = many->conversations[6].id;
        many->active = 5;
        check(chat_delete(many), "a middle conversation deletes");
        check(many->conversation_count == CHAT_MAX_CONVERSATIONS - 1,
            "the count shrinks by one");
        check(many->conversations[5].id == survivor,
            "the survivor shifts into the vacated index");
        check(chat_index_of_id(many, victim) == -1 &&
            chat_index_of_id(many, survivor) == 5,
            "identity lookup follows the deletion");
        check(chat_new_conversation(many) == CHAT_MAX_CONVERSATIONS - 1,
            "room below the cap is restored by deletion");
        /* Deleting the active last conversation clamps the selection. */
        many->active = many->conversation_count - 1;
        check(chat_delete(many) && chat_active(many) != NULL &&
            many->active == many->conversation_count - 1,
            "deleting the active last conversation clamps the selection");
        check_invariants(many);
        chat_dispose(many); free(many);
    }

    /* Ownership: deleting a mid-list conversation transfers the surviving
       conversations' allocations without double frees, and the moved
       conversation keeps accepting messages. */
    {
        Chat *own = (Chat *)calloc(1, sizeof *own);
        if (!own) return 2;
        chat_init(own);
        wchar_t *big = (wchar_t *)malloc(
            sizeof(wchar_t) * (CHAT_MESSAGE_INLINE * 2));
        if (!big) return 2;
        for (size_t i = 0; i < CHAT_MESSAGE_INLINE * 2 - 1; i++) big[i] = L'x';
        big[CHAT_MESSAGE_INLINE * 2 - 1] = 0;
        check(chat_new_conversation(own) == 1, "second conversation created");
        check(chat_append(own, CHAT_ROLE_USER, L"question one") >= 0,
            "user turn in the conversation to move");
        check(chat_append(own, CHAT_ROLE_ASSISTANT, big) >= 0,
            "overflow answer appended");
        check(chat_new_conversation(own) == 2, "third conversation created");
        check(chat_append(own, CHAT_ROLE_USER, L"other conversation") >= 0,
            "user turn in the last conversation");
        check(chat_select_conversation(own, 0),
            "select the first conversation");
        check(chat_delete(own), "delete the first conversation");
        check(own->conversation_count == 2 && own->active == 0,
            "the list compacted around the deleted slot");
        const ChatConversation *moved = &own->conversations[0];
        check(moved->message_count == 2 && moved->messages != NULL &&
            moved->message_capacity >= 2,
            "the moved conversation kept its storage");
        check(wcslen(chat_message_text(&moved->messages[1])) ==
            CHAT_MESSAGE_INLINE * 2 - 1,
            "the moved overflow allocation survived the ownership transfer");
        check(chat_append(own, CHAT_ROLE_ASSISTANT, L"after the move") == 2,
            "the moved conversation still appends after a deletion elsewhere");
        check(wcslen(chat_message_text(&own->conversations[0].messages[1])) ==
            CHAT_MESSAGE_INLINE * 2 - 1,
            "appending after the move did not disturb earlier content");
        check_invariants(own);
        check(chat_delete_all(own), "delete-all releases every conversation");
        check(own->conversation_count == 1 &&
            chat_active(own)->messages == NULL &&
            chat_active(own)->message_capacity == 0,
            "delete-all leaves one fresh, unallocated conversation");
        check(chat_append(own, CHAT_ROLE_USER, L"fresh again") == 0,
            "the fresh conversation works after delete-all");
        check_invariants(own);
        free(big);
        chat_dispose(own); free(own);
    }

    /* An unknown send mode is rejected before anything else happens. */
    {
        Chat *bad_mode = (Chat *)calloc(1, sizeof *bad_mode);
        if (!bad_mode) return 2;
        chat_init(bad_mode); chat_clear(bad_mode);
        check(chat_append(bad_mode, CHAT_ROLE_USER, L"question") == 0,
            "invalid-mode probe seeded");
        uint64_t before_id = bad_mode->next_id;
        int64_t before_modified = chat_active(bad_mode)->modified_at;
        check(chat_begin_response(bad_mode, (ChatSendMode)99, L"x") < 0,
            "an out-of-range send mode is rejected");
        check(chat_begin_response(bad_mode, (ChatSendMode)-1, L"x") < 0,
            "a negative send mode is rejected");
        check(chat_active(bad_mode)->message_count == 1 &&
            !wcscmp(chat_message_text(&chat_active(bad_mode)->messages[0]),
                L"question") &&
            bad_mode->next_id == before_id &&
            chat_active(bad_mode)->modified_at == before_modified,
            "a rejected mode mutates nothing");
        check_invariants(bad_mode);
        chat_dispose(bad_mode); free(bad_mode);
    }

    /* Identity-counter exhaustion: allocation fails without mutating
       anything, exactly like the message-cap and allocation-failure paths. */
    {
        Chat *exhausted = (Chat *)calloc(1, sizeof *exhausted);
        if (!exhausted) return 2;
        chat_init(exhausted); chat_clear(exhausted);
        exhausted->next_id = CHAT_MAX_ID;
        int before_count = exhausted->conversation_count;
        int before_active = exhausted->active;
        uint64_t before_conversation_id = exhausted->conversations[0].id;
        int64_t before_modified = chat_active(exhausted)->modified_at;
        check(chat_new_conversation(exhausted) == -1 &&
            exhausted->conversation_count == before_count &&
            exhausted->active == before_active &&
            exhausted->next_id == CHAT_MAX_ID,
            "a conversation cannot be created at the id ceiling");
        check(chat_append(exhausted, CHAT_ROLE_USER, L"hi") == -1 &&
            chat_active(exhausted)->message_count == 0 &&
            exhausted->next_id == CHAT_MAX_ID &&
            exhausted->conversations[0].id == before_conversation_id &&
            chat_active(exhausted)->modified_at == before_modified,
            "an append at the id ceiling mutates nothing");
        check_invariants(exhausted);
        /* Just below the ceiling both allocations work again. */
        exhausted->next_id = CHAT_MAX_ID - 2;
        check(chat_new_conversation(exhausted) == 1 &&
            exhausted->conversations[1].id == CHAT_MAX_ID - 1,
            "conversation allocation resumes below the ceiling");
        check(chat_append_at(exhausted, 0, CHAT_ROLE_USER, L"hi") == 0 &&
            exhausted->conversations[0].messages[0].id == CHAT_MAX_ID,
            "the last id below the ceiling is still allocatable");
        check_invariants(exhausted);
        chat_dispose(exhausted); free(exhausted);
    }

    /* Deep snapshot for asynchronous persistence: the copy is fully owned by
       the caller, aliases no source allocation, is transactional under
       allocation failure, and never follows later source mutations. */
    {
        Chat *own = (Chat *)calloc(1, sizeof *own);
        if (!own) return 2;
        chat_init(own); chat_clear(own);
        wchar_t *big = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_MESSAGE_INLINE * 2));
        wchar_t *thought = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_REASONING_INLINE + 8));
        if (!big || !thought) return 2;
        for (size_t i = 0; i < CHAT_MESSAGE_INLINE * 2 - 1; i++) big[i] = L'x';
        big[CHAT_MESSAGE_INLINE * 2 - 1] = 0;
        for (size_t i = 0; i < CHAT_REASONING_INLINE + 7; i++) thought[i] = L'y';
        thought[CHAT_REASONING_INLINE + 7] = 0;
        check(chat_append(own, CHAT_ROLE_USER, L"question one") == 0,
            "snapshot fixture user turn appended");
        int answer = chat_append(own, CHAT_ROLE_ASSISTANT, big);
        check(answer == 1, "snapshot fixture overflow answer appended");
        check(own->conversations[0].messages[1].text_overflow != NULL,
            "snapshot fixture answer really is on overflow storage");
        check(chat_message_set_reasoning(&own->conversations[0].messages[1], thought),
            "snapshot fixture reasoning appended");
        check(own->conversations[0].messages[1].reasoning_overflow != NULL,
            "snapshot fixture reasoning really is on overflow storage");
        uint64_t answer_id = own->conversations[0].messages[1].id;
        check_invariants(own);

        Chat *copy = chat_snapshot(own);
        check(copy != NULL, "snapshot of a populated chat succeeds");
        if (copy) {
            check(copy != own &&
                copy->conversations[0].messages[1].text_overflow !=
                    own->conversations[0].messages[1].text_overflow &&
                copy->conversations[0].messages[1].reasoning_overflow !=
                    own->conversations[0].messages[1].reasoning_overflow,
                "snapshot overflow storage is freshly allocated, never aliased");
            check(!wcscmp(chat_message_text(&copy->conversations[0].messages[0]),
                    L"question one") &&
                !wcscmp(chat_message_text(&copy->conversations[0].messages[1]), big) &&
                !wcscmp(chat_message_reasoning(&copy->conversations[0].messages[1]),
                    thought),
                "snapshot carries inline and overflow content verbatim");
            check(copy->conversations[0].messages[1].text_capacity ==
                    wcslen(big) + 1 &&
                copy->conversations[0].messages[1].reasoning_capacity ==
                    wcslen(thought) + 1,
                "snapshot overflow capacities match the promoted lengths");
            check(copy->conversations[0].messages[1].id == answer_id,
                "snapshot keeps stable message identity");
            check(copy->conversations[0].message_capacity ==
                copy->conversations[0].message_count,
                "snapshot arrays carry exactly the live message count");
            check_invariants(copy);

            /* Snapshot isolation: the source keeps mutating; the copy never
               follows, and disposing the copy cannot disturb the source. */
            check(chat_message_append_text(
                    &own->conversations[0].messages[1], L" more"),
                "source mutates after the snapshot was taken");
            check(!wcscmp(chat_message_text(&copy->conversations[0].messages[1]), big),
                "an earlier snapshot does not follow later source mutations");
            chat_dispose(copy);
            free(copy);
            check(own->conversations[0].messages[1].text_overflow != NULL &&
                wcslen(chat_message_text(&own->conversations[0].messages[1])) ==
                    CHAT_MESSAGE_INLINE * 2 - 1 + 5,
                "disposing the snapshot leaves the source's storage intact");
        }
        size_t answer_len = wcslen(chat_message_text(&own->conversations[0].messages[1]));
        size_t reasoning_len = wcslen(chat_message_reasoning(&own->conversations[0].messages[1]));
        /* Transactional failure sweep: failing each position of the one
           combined allocation sequence in turn (the snapshot makes exactly
           four allocations: the Chat struct, one exact live-count message
           array per conversation, and one overflow copy per heap-backed
           text) either fails the whole snapshot with the source untouched
           and nothing leaked, or completes. */
        {
            bool saw_failure = false, saw_success = false;
            bool source_damaged = false, snapshot_bad = false;
            for (long i = 1; i <= 5; i++) {
                alloc_number = 0; fail_allocation = i;
                Chat *snap = chat_snapshot(own);
                alloc_number = 0; fail_allocation = 0;
                if (!snap) {
                    saw_failure = true;
                    if (wcslen(chat_message_text(&own->conversations[0].messages[1]))
                            != answer_len ||
                        wcslen(chat_message_reasoning(
                            &own->conversations[0].messages[1])) != reasoning_len ||
                        wcscmp(chat_message_text(&own->conversations[0].messages[0]),
                            L"question one"))
                        source_damaged = true;
                    continue;
                }
                saw_success = true;
                if (wcslen(chat_message_text(&snap->conversations[0].messages[1]))
                        != answer_len ||
                    wcslen(chat_message_reasoning(&snap->conversations[0].messages[1]))
                        != reasoning_len ||
                    wcscmp(chat_message_text(&snap->conversations[0].messages[0]),
                        L"question one") ||
                    snap->conversations[0].messages[1].text_overflow ==
                        own->conversations[0].messages[1].text_overflow ||
                    snap->conversations[0].messages[1].reasoning_overflow ==
                        own->conversations[0].messages[1].reasoning_overflow)
                    snapshot_bad = true;
                check_invariants(snap);
                chat_dispose(snap); free(snap);
            }
            check(saw_failure, "snapshot allocation-failure path is reachable");
            check(saw_success, "snapshot succeeds once failures are exhausted");
            check(!source_damaged, "a failed snapshot leaves the source untouched");
            check(!snapshot_bad, "every completed snapshot is complete and unaliased");
        }
        check_invariants(own);
        free(big); free(thought);
        chat_dispose(own); free(own);
    }

    /* Format 4 customization data layer: the owned-text primitive, the
        prompt-profile library and the per-conversation overrides. Nothing
        here is user-visible yet; the storage round-trips live in
        test_storage. */
    {
        /* ChatText primitive: set/clone/dispose, unset-vs-empty, surrogate
            trimming and transactional failure. */
        ChatText t = {0};
        check(!t.data && t.length == 0 && t.capacity == 0 &&
            !wcscmp(chat_text_value(&t), L""),
            "unset text reads as the empty string");
        check(chat_text_set(&t, L"override") && t.data != NULL &&
            t.length == 8 && t.capacity == 9 &&
            !wcscmp(chat_text_value(&t), L"override"),
            "setting text stores length+1 owned units");
        check(chat_text_set(&t, L"") && !t.data && t.length == 0 &&
            t.capacity == 0,
            "an empty set disposes back to unset");
        check(chat_text_set(&t, L"kept") &&
            chat_text_set(&t, L"tail\xd83d") && t.length == 4 &&
            !wcsncmp(t.data, L"tail", 4),
            "a dangling high surrogate is trimmed, not stored");
        ChatText src = {0}, dst = {0};
        check(chat_text_clone(&dst, &src) && dst.data == NULL,
            "cloning an unset source leaves the destination unset");
        check(chat_text_set(&src, L"clone me") &&
            chat_text_clone(&dst, &src) && dst.data != src.data &&
            !wcscmp(chat_text_value(&dst), L"clone me"),
            "a clone owns a fresh copy of the source");
        check(chat_text_set(&src, L"changed") &&
            !wcscmp(chat_text_value(&dst), L"clone me"),
            "a clone never follows later source mutations");
        fail_next_mallocs = 1;
        bool stored = chat_text_set(&t, L"a replacement far too long to fail silently");
        fail_next_mallocs = 0;
        check(!stored && t.length == 4 && !wcscmp(chat_text_value(&t), L"tail"),
            "a failed set leaves the previous content untouched");
        fail_next_mallocs = 1;
        bool cloned = chat_text_clone(&dst, &src);
        fail_next_mallocs = 0;
        check(!cloned && !wcscmp(chat_text_value(&dst), L"clone me"),
            "a failed clone leaves the previous content untouched");
        chat_text_dispose(&t); chat_text_dispose(&src); chat_text_dispose(&dst);

        /* Prompt-profile library: bounds, uniqueness, transactional set,
            removal ownership. */
        Chat *lib = (Chat *)calloc(1, sizeof *lib);
        if (!lib) return 2;
        chat_init(lib);
        check(chat_profile_count(lib) == 0 && chat_profile(lib, 0) == NULL,
            "a fresh chat has an empty profile library");
        check(chat_profile_add(lib, L"", L"p") == -1 &&
            chat_profile_add(lib, NULL, L"p") == -1,
            "an empty or missing name is rejected");
        wchar_t longname[CHAT_PROFILE_NAME_TEXT + 1];
        for (size_t i = 0; i < CHAT_PROFILE_NAME_TEXT; i++) longname[i] = L'n';
        longname[CHAT_PROFILE_NAME_TEXT] = 0;
        check(chat_profile_add(lib, longname, L"p") == -1,
            "a 64-unit name is rejected (63 stored units plus NUL is the bound)");
        longname[CHAT_PROFILE_NAME_TEXT - 1] = 0;
        check(chat_profile_add(lib, longname, L"p") == 0,
            "a 63-unit name is accepted");
        check(chat_profile_add(lib, L"Concise", L"Be concise.") == 1,
            "a second profile is appended");
        check(chat_profile_count(lib) == 2, "the count tracks adds");
        check(chat_profile(lib, 1) != NULL &&
            !wcscmp(chat_profile(lib, 1)->name, L"Concise") &&
            !wcscmp(chat_text_value(&chat_profile(lib, 1)->prompt),
                L"Be concise."),
            "profile content round-trips in memory");
        check(chat_profile(lib, 2) == NULL && chat_profile(NULL, 0) == NULL,
            "out-of-range and null-chat profile lookups are rejected");
        check(chat_profile_add(lib, L"CONCISE", L"x") == -1,
            "a name differing only by ordinal case is a duplicate");
        check(chat_profile_count(lib) == 2,
            "a rejected add grows nothing");
        fail_next_mallocs = 1;
        check(chat_profile_add(lib, L"Fresh", L"prompt") == -1,
            "an add that fails to allocate its prompt is rejected");
        fail_next_mallocs = 0;
        check(chat_profile_count(lib) == 2,
            "a failed add consumes no slot");
        check(chat_profile_add(lib, L"Fresh", L"") == 2,
            "an empty prompt is a valid profile (means apply-no-prompt)");
        check(chat_profile(lib, 2)->prompt.data == NULL,
            "an empty profile prompt is stored unset");
        check(chat_profile_set(lib, 0, L"Renamed", L"new prompt"),
            "set replaces name and prompt");
        check(!wcscmp(chat_profile(lib, 0)->name, L"Renamed") &&
            !wcscmp(chat_text_value(&chat_profile(lib, 0)->prompt),
                L"new prompt"),
            "set wrote both fields");
        fail_next_mallocs = 1;
        check(!chat_profile_set(lib, 0, L"Changed", L"allocating prompt"),
            "a set that fails to allocate its new prompt changes nothing");
        fail_next_mallocs = 0;
        check(!wcscmp(chat_profile(lib, 0)->name, L"Renamed") &&
            !wcscmp(chat_text_value(&chat_profile(lib, 0)->prompt),
                L"new prompt"),
            "a failed set leaves the existing name and prompt intact");
        check(!chat_profile_set(lib, 0, L"concise", L"x"),
            "a rename colliding with another profile's name fails");
        check(!wcscmp(chat_profile(lib, 0)->name, L"Renamed"),
            "the colliding rename changes nothing");
        check(!chat_profile_set(lib, 5, L"x", L"") &&
            !chat_profile_set(NULL, 0, L"x", L""),
            "out-of-range and null-chat sets are rejected");
        check(chat_profile_remove(lib, 0), "mid-list removal succeeds");
        check(chat_profile_count(lib) == 2 &&
            !wcscmp(chat_profile(lib, 0)->name, L"Concise") &&
            !wcscmp(chat_text_value(&chat_profile(lib, 0)->prompt),
                L"Be concise.") &&
            !wcscmp(chat_profile(lib, 1)->name, L"Fresh"),
            "survivors shifted down with their heap prompts intact");
        check(!chat_profile_remove(lib, 2) && !chat_profile_remove(NULL, 0),
            "out-of-range and null-chat removals are rejected");
        check(!lib->profiles[2].name[0] && lib->profiles[2].prompt.data == NULL,
            "the vacated tail slot is zeroed so nothing can be freed twice");
        check_invariants(lib);
        chat_dispose(lib); free(lib);

        /* Per-conversation overrides: validation, empty-clears-to-inherit,
            Clear survival. */
        Chat *ov = (Chat *)calloc(1, sizeof *ov);
        if (!ov) return 2;
        chat_init(ov);
        check(!chat_conversation_set_system_prompt(ov, 1, L"x") &&
            !chat_conversation_set_system_prompt(NULL, 0, L"x"),
            "an out-of-range prompt override is rejected");
        check(!chat_conversation_set_model(ov, 0, (ChatBackend)9, L"x"),
            "an unknown backend is rejected");
        check(chat_conversation_set_system_prompt(ov, 0, L"You are terse.") &&
            !wcscmp(chat_text_value(&ov->conversations[0].system_prompt),
                L"You are terse."),
            "the prompt override is stored");
        wchar_t longmodel[CHAT_MODEL_TEXT + 1];
        for (size_t i = 0; i < CHAT_MODEL_TEXT; i++) longmodel[i] = L'm';
        longmodel[CHAT_MODEL_TEXT] = 0;
        check(!chat_conversation_set_model(ov, 0, CHAT_BACKEND_OPENROUTER,
                longmodel),
            "a 96-unit model override is rejected");
        check(ov->conversations[0].model[0] == 0,
            "a rejected model override leaves the slot empty");
        longmodel[CHAT_MODEL_TEXT - 1] = 0;
        check(chat_conversation_set_model(ov, 0, CHAT_BACKEND_OPENROUTER,
                longmodel) && !wcscmp(ov->conversations[0].model, longmodel),
            "a 95-unit model override is stored");
        check(chat_conversation_set_model(ov, 0, CHAT_BACKEND_OLLAMA,
                L"llama3") &&
            !wcscmp(ov->conversations[0].ollama_model, L"llama3"),
            "the Ollama override is stored");
        check(chat_conversation_set_model(ov, 0, CHAT_BACKEND_OPENROUTER,
            L"") && !ov->conversations[0].model[0],
            "an empty model override clears back to inherit");
        check(chat_conversation_set_system_prompt(ov, 0, L"") &&
            ov->conversations[0].system_prompt.data == NULL,
            "an empty prompt override clears to unset");
        check(chat_conversation_apply_system_prompt(ov, 0, L"") &&
            ov->conversations[0].system_prompt.data == NULL &&
            ov->conversations[0].system_prompt_present,
            "an applied empty prompt is a real override, not inherit");
        check(chat_conversation_apply_system_prompt(ov, 0, L"explicit") &&
            !wcscmp(chat_text_value(&ov->conversations[0].system_prompt),
                L"explicit") &&
            !ov->conversations[0].system_prompt_present,
            "applying a non-empty prompt behaves like the setter");
        check(chat_conversation_set_system_prompt(ov, 0, L"") &&
            ov->conversations[0].system_prompt.data == NULL &&
            !ov->conversations[0].system_prompt_present,
            "the setter's empty clears the text and the empty-override flag");
        check(!chat_conversation_apply_system_prompt(ov, 5, L"x") &&
            !chat_conversation_apply_system_prompt(NULL, 0, L"x"),
            "out-of-range and null-chat applies are rejected");
        /* A failed non-empty set/apply must not silently convert the
            deliberate empty override into inheritance: the flag survives
            the failed allocation. */
        check(chat_conversation_apply_system_prompt(ov, 0, L""),
            "fixture: the empty override is applied");
        fail_next_mallocs = 1;
        check(!chat_conversation_set_system_prompt(ov, 0, L"alloc fails"),
            "a set that fails to allocate reports failure");
        check(ov->conversations[0].system_prompt_present &&
            ov->conversations[0].system_prompt.data == NULL,
            "the failed set keeps the explicit-empty override");
        fail_next_mallocs = 1;
        check(!chat_conversation_apply_system_prompt(ov, 0, L"alloc fails"),
            "an apply that fails to allocate reports failure");
        check(ov->conversations[0].system_prompt_present &&
            ov->conversations[0].system_prompt.data == NULL,
            "the failed apply keeps the explicit-empty override");
        fail_next_mallocs = 0;
        check(chat_conversation_set_system_prompt(ov, 0, L"recover") &&
            !wcscmp(chat_text_value(&ov->conversations[0].system_prompt),
                L"recover"),
            "the set succeeds once allocation is allowed");
        check(chat_append(ov, CHAT_ROLE_USER, L"probe") == 1,
            "override conversation has a message");
        check(chat_conversation_set_system_prompt(ov, 0, L"survives clear"),
            "the override is set before Clear");
        chat_clear(ov);
        check(!wcscmp(chat_text_value(&ov->conversations[0].system_prompt),
                L"survives clear"),
            "the prompt override survives Clear, like the title");
        check_invariants(ov);
        chat_dispose(ov); free(ov);

        /* Effective resolution: per-backend overrides, empty = inherit, the
            deliberately-empty prompt override, and the request bookkeeping
            that records the effective model. */
        Chat *eff = (Chat *)calloc(1, sizeof *eff);
        if (!eff) return 2;
        chat_init(eff);
        check(chat_conversation_set_model(eff, 0, CHAT_BACKEND_OPENROUTER,
                L"conv-or") &&
            chat_conversation_set_model(eff, 0, CHAT_BACKEND_OLLAMA,
                L"conv-ollama"),
            "fixture model overrides are set");
        check(!wcscmp(chat_effective_model(eff, chat_active(eff)), L"conv-or"),
            "the override for the active backend wins");
        check(!wcscmp(chat_effective_model_for_backend(eff,
                chat_active(eff), CHAT_BACKEND_OLLAMA), L"conv-ollama"),
            "the explicit-backend resolver is backend-specific");
        check(chat_conversation_set_model(eff, 0, CHAT_BACKEND_OPENROUTER,
                L"") &&
            !wcscmp(chat_effective_model(eff, chat_active(eff)), eff->model),
            "an empty override inherits the global slot");
        check(!wcscmp(chat_effective_model(eff, NULL), eff->model),
            "a NULL conversation inherits the global slot");
        eff->backend = CHAT_BACKEND_OLLAMA;
        check(!wcscmp(chat_effective_model(eff, chat_active(eff)),
                L"conv-ollama"),
            "the active-backend resolver follows the backend");
        eff->backend = CHAT_BACKEND_OPENROUTER;
        wcscpy(eff->system_prompt, L"global prompt");
        check(!wcscmp(chat_effective_system_prompt(eff, chat_active(eff)),
                L"global prompt"),
            "no prompt override inherits the global prompt");
        check(chat_conversation_apply_system_prompt(eff, 0, L"") &&
            chat_effective_system_prompt(eff, chat_active(eff))[0] == 0,
            "the deliberately-empty override suppresses the global prompt");
        check(chat_conversation_apply_system_prompt(eff, 0, L"here") &&
            !wcscmp(chat_effective_system_prompt(eff, chat_active(eff)),
                L"here"),
            "a text override wins over the global prompt");
        check(chat_conversation_set_system_prompt(eff, 0, L"") &&
            !wcscmp(chat_effective_system_prompt(eff, chat_active(eff)),
                L"global prompt"),
            "clearing the override inherits the global prompt again");
        /* Session-only reasoning: defaults on, flips per conversation, is
            bounds-checked, and reads enabled for a NULL conversation. */
        check(chat_effective_reasoning(eff, chat_active(eff)),
            "reasoning defaults on for a conversation");
        check(chat_effective_reasoning(eff, NULL),
            "a NULL conversation reads as reasoning on");
        check(!chat_conversation_set_reasoning(eff, 99, false),
            "an out-of-range reasoning set is rejected");
        check(!chat_conversation_set_reasoning(NULL, 0, false),
            "a NULL chat reasoning set is rejected");
        check(chat_conversation_set_reasoning(eff, 0, false),
            "the reasoning preference flips off");
        check(!chat_effective_reasoning(eff, &eff->conversations[0]),
            "the off preference is reflected");
        check(chat_conversation_set_reasoning(eff, 0, true),
            "the reasoning preference flips back on");
        check(chat_effective_reasoning(eff, &eff->conversations[0]),
            "the on preference is reflected");
        check(chat_conversation_set_model(eff, 0, CHAT_BACKEND_OPENROUTER,
                L"req-model"),
            "the request fixture override is set");
        check(chat_append(eff, CHAT_ROLE_USER, L"question") == 1,
            "the request fixture has a user turn");
        int resp = chat_begin_response(eff, CHAT_SEND, L"question");
        check(resp >= 0, "the effective-model response begins");
        if (resp >= 0) {
            const ChatGeneration *g =
                &chat_active(eff)->messages[resp].generation;
            check(!wcscmp(g->requested_model, L"req-model"),
                "requested_model records the effective model");
            bool remembered = false;
            for (int i = 0; i < eff->model_history_count; i++)
                if (eff->model_history_backend[i] ==
                        CHAT_BACKEND_OPENROUTER &&
                    !wcscmp(eff->model_history[i], L"req-model"))
                    remembered = true;
            check(remembered,
                "the effective model is remembered for its backend");
        }
        check_invariants(eff);
        chat_dispose(eff); free(eff);

        /* Snapshot ownership: customization is detached then cloned, the
            snapshot never aliases the source, and failing any allocation in
            the sequence is transactional. Allocation order for this
            fixture: 1 = Chat struct, 2 = profile prompt clone, 3 = prompt
            override clone, 4 = the welcome message's exact live-count
            array and 5 = its promoted overflow copy (chat_init seeds one
            long welcome turn). */
        Chat *cust = (Chat *)calloc(1, sizeof *cust);
        if (!cust) return 2;
        chat_init(cust);
        check(chat_profile_add(cust, L"Terse", L"You are terse.") == 0,
            "snapshot fixture profile added");
        check(chat_conversation_set_system_prompt(cust, 0,
                L"Override prompt"),
            "snapshot fixture override set");
        Chat *snap = chat_snapshot(cust);
        check(snap != NULL, "a snapshot of a customized chat succeeds");
        if (snap) {
            check(snap->profiles[0].prompt.data !=
                    cust->profiles[0].prompt.data &&
                !wcscmp(chat_text_value(&snap->profiles[0].prompt),
                    L"You are terse."),
                "the profile prompt is cloned, never aliased");
            check(snap->conversations[0].system_prompt.data !=
                    cust->conversations[0].system_prompt.data &&
                !wcscmp(chat_text_value(&snap->conversations[0].system_prompt),
                    L"Override prompt"),
                "the override prompt is cloned, never aliased");
            check(chat_profile_set(cust, 0, L"Terse", L"Mutated") &&
                chat_conversation_set_system_prompt(cust, 0, L"Mutated"),
                "the source mutates after the snapshot");
            check(!wcscmp(chat_text_value(&snap->profiles[0].prompt),
                    L"You are terse.") &&
                !wcscmp(chat_text_value(&snap->conversations[0].system_prompt),
                    L"Override prompt"),
                "the snapshot does not follow source customization mutation");
            chat_dispose(snap); free(snap);
            check(!wcscmp(chat_text_value(&cust->profiles[0].prompt),
                    L"Mutated") &&
                !wcscmp(chat_text_value(&cust->conversations[0].system_prompt),
                    L"Mutated") &&
                cust->profiles[0].prompt.data != NULL &&
                cust->conversations[0].system_prompt.data != NULL,
                "disposing the snapshot leaves the source's owned text intact");
        }
        {
            bool saw_failure = false, saw_success = false, damaged = false;
            for (long i = 1; i <= 6; i++) {
                alloc_number = 0; fail_allocation = i;
                Chat *s = chat_snapshot(cust);
                alloc_number = 0; fail_allocation = 0;
                if (!s) {
                    saw_failure = true;
                    if (wcscmp(chat_text_value(&cust->profiles[0].prompt),
                            L"Mutated") ||
                        wcscmp(chat_text_value(
                            &cust->conversations[0].system_prompt),
                            L"Mutated") ||
                        cust->profiles[0].prompt.data == NULL ||
                        cust->conversations[0].system_prompt.data == NULL)
                        damaged = true;
                    continue;
                }
                saw_success = true;
                if (wcscmp(chat_text_value(&s->profiles[0].prompt),
                        L"Mutated") ||
                    wcscmp(chat_text_value(&s->conversations[0].system_prompt),
                        L"Mutated") ||
                    s->profiles[0].prompt.data ==
                        cust->profiles[0].prompt.data ||
                    s->conversations[0].system_prompt.data ==
                        cust->conversations[0].system_prompt.data)
                    damaged = true;
                check_invariants(s);
                chat_dispose(s); free(s);
            }
            check(saw_failure && saw_success && !damaged,
                "failing any snapshot allocation is transactional for owned text");
        }

        /* Deletion ownership: a moved conversation keeps its override; the
            removed one's override is released exactly once; delete-all
            preserves the profile library. */
        Chat *own3 = (Chat *)calloc(1, sizeof *own3);
        if (!own3) return 2;
        chat_init(own3);
        check(chat_new_conversation(own3) == 1 &&
            chat_new_conversation(own3) == 2, "three conversations created");
        check(chat_conversation_set_system_prompt(own3, 0,
                L"first override") &&
            chat_conversation_set_system_prompt(own3, 1,
                L"second override"),
            "overrides set on the first two conversations");
        own3->active = 0;
        check(chat_delete(own3), "the first conversation deletes");
        check(own3->conversations[0].system_prompt.data != NULL &&
            !wcscmp(chat_text_value(&own3->conversations[0].system_prompt),
                L"second override"),
            "the moved conversation transferred its owned override");
        check(own3->conversations[1].system_prompt.data == NULL,
            "the vacated tail slot is zeroed");
        check(chat_append(own3, CHAT_ROLE_USER, L"after the move") == 0,
            "the moved conversation still appends after deletion");
        check(chat_profile_add(own3, L"Kept", L"profile prompt") == 0,
            "a profile exists before delete-all");
        check(chat_delete_all(own3), "delete-all succeeds");
        check(chat_profile_count(own3) == 1 &&
            !wcscmp(chat_text_value(&own3->profiles[0].prompt),
                L"profile prompt"),
            "delete-all preserves the profile library");
        check(own3->conversation_count == 1 &&
            own3->conversations[0].system_prompt.data == NULL &&
            !own3->conversations[0].model[0] &&
            !own3->conversations[0].ollama_model[0],
            "delete-all leaves fresh, uncustomized conversations");
        check(chat_append(own3, CHAT_ROLE_USER, L"fresh again") == 0,
            "the fresh conversation works after delete-all");
        check_invariants(own3);
        chat_dispose(own3); free(own3);
        chat_dispose(cust); free(cust);
    }

    /* Content parts: fast-path view, promotion, projection, revisions,
       ownership, OOM staging, and the borrowed-input contract. */
    {
        Chat *pc = (Chat *)calloc(1, sizeof *pc);
        if (!pc) return 2;
        chat_init(pc);
        chat_clear(pc);

        /* Fast path: no ChatPart object exists; part_at fills by value. */
        int fi = chat_append(pc, CHAT_ROLE_USER, L"fast path text");
        ChatMessage *fm = &pc->conversations[pc->active].messages[fi];
        check(fm->parts.items == NULL && fm->parts.count == 0 &&
                fm->parts.capacity == 0,
            "text-only messages own no parts array");
        check(chat_message_part_count(fm) == 1,
            "fast path presents one text part");
        ChatPartView view;
        memset(&view, 0xA5, sizeof view);
        check(chat_message_part_at(fm, 0, &view) &&
                view.kind == CHAT_PART_TEXT && view.flags == 0 &&
                view.u.text.data == chat_message_text(fm) &&
                view.u.text.length == wcslen(L"fast path text"),
            "fast-path part_at borrows the message text by value view");
        memset(&view, 0xA5, sizeof view);
        check(!chat_message_part_at(fm, 1, &view) && view.kind == 0 &&
                view.u.text.data == NULL && view.u.image.attachment_id == 0,
            "part_at past count fails and zeroes the out view");
        check(!chat_message_has_images(fm),
            "fast path reports no images");
        int ei = chat_append(pc, CHAT_ROLE_ASSISTANT, L"");
        ChatMessage *em = &pc->conversations[pc->active].messages[ei];
        check(chat_message_part_count(em) == 0,
            "empty fast-path text presents zero parts");
        check(!chat_message_part_at(em, 0, &view),
            "empty fast path has no part zero");

        /* Promotion with text: [TEXT, IMAGE], projection preserved. */
        ChatImagePart img;
        memset(&img, 0, sizeof img);
        img.attachment_id = 42;
        img.pixel_width = 3;
        img.pixel_height = 4;
        memcpy(img.mime, "image/png", 10);
        wcscpy(img.display_name, L"shot.png");
        uint64_t rev_before = fm->revision;
        uint64_t body_before = fm->body_revision;
        check(chat_message_add_image(fm, &img, 0),
            "add_image promotes a text message");
        check(fm->parts.items != NULL && fm->parts.count == 2 &&
                fm->parts.items[0].kind == CHAT_PART_TEXT &&
                fm->parts.items[1].kind == CHAT_PART_IMAGE &&
                fm->parts.items[1].u.image.attachment_id == 42 &&
                fm->parts.items[1].flags == 0,
            "promotion builds [TEXT, IMAGE] and copies image metadata");
        check(fm->revision == rev_before + 1 &&
                fm->body_revision == body_before + 1,
            "promotion bumps revision and body_revision");
        check(!wcscmp(chat_message_text(fm), L"fast path text"),
            "promotion preserves the plain-text projection");
        check(chat_message_has_images(fm),
            "promoted message reports images");
        check(chat_message_part_count(fm) == 2,
            "promoted part_count is the array length");
        memset(&view, 0, sizeof view);
        check(chat_message_part_at(fm, 1, &view) &&
                view.kind == CHAT_PART_IMAGE &&
                view.u.image.attachment_id == 42 &&
                !wcscmp(view.u.image.display_name, L"shot.png"),
            "part_at copies IMAGE fields by value after promotion");
        check(parts_consistent(fm),
            "projection matches TEXT parts after promotion");

        /* Flags: unknown bits rejected; FIRST_FRAME accepted. */
        check(!chat_message_add_image(fm, &img, 0x02),
            "add_image rejects flags outside the image mask");
        check(fm->parts.count == 2 && fm->revision == rev_before + 1,
            "rejected flags leave the message untouched");
        check(chat_message_add_image(fm, &img, CHAT_PART_FLAG_FIRST_FRAME),
            "add_image accepts CHAT_PART_FLAG_FIRST_FRAME");
        check(fm->parts.items[2].flags == CHAT_PART_FLAG_FIRST_FRAME,
            "the first-frame flag lands on the new IMAGE part");
        check(chat_message_remove_part(fm, 2),
            "the flagged image removes");

        /* Borrowed input: set/append with text owned by the same message. */
        check(chat_message_set_text(fm, chat_message_text(fm)),
            "promoted set_text accepts the borrowed projection");
        check(!wcscmp(chat_message_text(fm), L"fast path text") &&
                parts_consistent(fm),
            "aliased set_text keeps content and projection");
        {
            ChatPartView text_view;
            check(chat_message_part_at(fm, 0, &text_view) &&
                    text_view.u.text.data != NULL,
                "TEXT part view available for the alias append");
            check(chat_message_append_text(fm, text_view.u.text.data),
                "promoted append_text accepts borrowed TEXT part data");
            check(!wcscmp(chat_message_text(fm),
                    L"fast path textfast path text") &&
                    parts_consistent(fm),
                "aliased append_text doubles the projection consistently");
        }
        check(chat_message_append_text(fm, chat_message_text(fm)),
            "promoted append_text accepts the borrowed projection");
        check(!wcscmp(chat_message_text(fm),
                L"fast path textfast path textfast path textfast path text"),
            "projection tracks the two aliased appends");
        check(parts_consistent(fm),
            "projection consistent after aliased appends");

        /* set_text rewrites the leading TEXT part in place. */
        check(chat_message_set_text(fm, L"rewritten") &&
                fm->parts.count == 2 &&
                fm->parts.items[0].kind == CHAT_PART_TEXT &&
                !wcscmp(chat_message_text(fm), L"rewritten") &&
                parts_consistent(fm),
            "promoted set_text rewrites TEXT and projection");

        /* Reorder / remove: image slots only; move(i,i) is a no-op.
           The message still carries the original promotion image (id 42). */
        ChatImagePart a, b;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        a.attachment_id = 1;
        b.attachment_id = 2;
        memcpy(a.mime, "image/png", 10);
        memcpy(b.mime, "image/jpeg", 11);
        check(chat_message_add_image(fm, &a, 0) &&
                chat_message_add_image(fm, &b, 0),
            "two images append for reorder tests");
        check(fm->parts.count == 4 &&
                fm->parts.items[1].u.image.attachment_id == 42 &&
                fm->parts.items[2].u.image.attachment_id == 1 &&
                fm->parts.items[3].u.image.attachment_id == 2,
            "images land in display order after the original promotion image");
        rev_before = fm->revision;
        body_before = fm->body_revision;
        check(chat_message_move_part(fm, 2, 2),
            "move_part(i, i) succeeds");
        check(fm->revision == rev_before &&
                fm->body_revision == body_before &&
                fm->parts.items[2].u.image.attachment_id == 1 &&
                fm->parts.items[3].u.image.attachment_id == 2,
            "move_part(i, i) is a no-op with no revision bump");
        check(chat_message_move_part(fm, 2, 3),
            "move_part reorders two images");
        check(fm->revision == rev_before + 1 &&
                fm->parts.items[2].u.image.attachment_id == 2 &&
                fm->parts.items[3].u.image.attachment_id == 1,
            "image order swapped across the TEXT prefix");
        check(!chat_message_move_part(fm, 0, 1),
            "move_part cannot displace the leading TEXT part");
        check(!chat_message_move_part(fm, 0, 0),
            "move_part(i, i) still rejects the leading TEXT slot");
        check(!chat_message_remove_part(fm, 0),
            "remove_part cannot drop the leading TEXT part");
        check(chat_message_remove_part(fm, 2) &&
                fm->parts.items[2].u.image.attachment_id == 1,
            "remove_part drops an image and shifts survivors");
        check(!chat_message_remove_part(fm, 99),
            "remove_part past count fails");

        /* Image-only / empty text. */
        ChatMessage *io = &pc->conversations[pc->active].messages[ei];
        check(chat_message_set_text(io, L""),
            "clear assistant text for the image-only case");
        ChatImagePart only;
        memset(&only, 0, sizeof only);
        only.attachment_id = 7;
        memcpy(only.mime, "image/webp", 11);
        check(chat_message_add_image(io, &only, 0),
            "empty message accepts an image");
        check(chat_message_part_count(io) == 1 &&
                chat_message_has_images(io) &&
                chat_message_text(io)[0] == 0 &&
                parts_consistent(io),
            "image-only message: one IMAGE part, empty projection");

        /* Lone high surrogate: staged empty must not NULL-deref. */
        {
            check(chat_message_set_text(fm, L"\xd800"),
                "promoted set_text accepts a lone high surrogate");
            check(chat_message_text(fm)[0] == 0 && parts_consistent(fm),
                "lone high surrogate stages as empty text");
            check(chat_message_append_text(fm, L"\xd800"),
                "promoted append_text accepts a lone high surrogate");
            check(chat_message_text(fm)[0] == 0 && parts_consistent(fm),
                "lone high surrogate append stages as empty");
            check(chat_message_set_text(fm, L"rewritten"),
                "restore projection after surrogate probes");
        }
        {
            check(chat_message_set_text(io, L"\xd800"),
                "image-only set_text accepts a lone high surrogate");
            check(chat_message_text(io)[0] == 0 && parts_consistent(io),
                "image-only lone surrogate stages as empty");
            check(chat_message_append_text(io, L"\xd800"),
                "image-only append_text accepts a lone high surrogate");
            check(chat_message_text(io)[0] == 0 && parts_consistent(io),
                "image-only lone surrogate append stages as empty");
        }

        /* CHAT_MAX_PARTS: fill an image-only message, then reject. */
        {
            int mj = chat_append(pc, CHAT_ROLE_USER, L"");
            ChatMessage *mm = &pc->conversations[pc->active].messages[mj];
            bool filled = true;
            for (size_t k = 0; k < CHAT_MAX_PARTS; k++) {
                ChatImagePart p;
                memset(&p, 0, sizeof p);
                p.attachment_id = 1000 + k;
                memcpy(p.mime, "image/png", 10);
                if (!chat_message_add_image(mm, &p, 0)) { filled = false; break; }
            }
            check(filled && mm->parts.count == CHAT_MAX_PARTS,
                "image-only message accepts exactly CHAT_MAX_PARTS parts");
            ChatImagePart extra;
            memset(&extra, 0, sizeof extra);
            extra.attachment_id = 9999;
            memcpy(extra.mime, "image/png", 10);
            size_t at_cap = mm->parts.count;
            uint64_t rev_cap = mm->revision;
            check(!chat_message_add_image(mm, &extra, 0) &&
                    mm->parts.count == at_cap &&
                    mm->revision == rev_cap,
                "add_image past CHAT_MAX_PARTS fails without mutation");
        }

        /* Demotion: clear_parts returns to the fast path with text intact. */
        rev_before = fm->revision;
        body_before = fm->body_revision;
        chat_message_clear_parts(fm);
        check(fm->parts.items == NULL && fm->parts.count == 0 &&
                fm->revision == rev_before + 1 &&
                fm->body_revision == body_before + 1,
            "clear_parts demotes and bumps");
        check(!wcscmp(chat_message_text(fm), L"rewritten"),
            "clear_parts keeps the plain-text projection");
        check(chat_message_part_count(fm) == 1 &&
                !chat_message_has_images(fm),
            "demoted message reads as one text part again");
        chat_message_clear_parts(fm);
        check(fm->revision == rev_before + 1,
            "clear_parts on the fast path is a no-op");

        /* Removing the last image demotes a TEXT+IMAGE message. */
        int dj = chat_append(pc, CHAT_ROLE_USER, L"demote me");
        ChatMessage *dm = &pc->conversations[pc->active].messages[dj];
        check(chat_message_add_image(dm, &only, 0) && dm->parts.items != NULL,
            "demote fixture promoted");
        check(chat_message_remove_part(dm, 1) && dm->parts.items == NULL &&
                !wcscmp(chat_message_text(dm), L"demote me"),
            "removing the last image demotes and keeps text");

        /* Revision matrix for part mutations (§3.5). */
        int rj = chat_append(pc, CHAT_ROLE_USER, L"revision parts");
        ChatMessage *rm = &pc->conversations[pc->active].messages[rj];
        uint64_t r0 = rm->revision, b0 = rm->body_revision;
        check(chat_message_add_image(rm, &only, 0) &&
                rm->revision == r0 + 1 && rm->body_revision == b0 + 1,
            "add_image bumps both counters");
        r0 = rm->revision; b0 = rm->body_revision;
        check(chat_message_set_reasoning(rm, L"why") &&
                rm->revision == r0 + 1 && rm->body_revision == b0,
            "reasoning after promotion still skips body_revision");
        r0 = rm->revision; b0 = rm->body_revision;
        chat_message_touch(rm);
        check(rm->revision == r0 + 1 && rm->body_revision == b0,
            "touch still skips body_revision");

        /* Staged OOM: every allocation position either fails cleanly or
           completes; a failure leaves the ChatMessage byte-identical. */
        {
            int oj = chat_append(pc, CHAT_ROLE_USER, L"oom fixture");
            ChatMessage *om = &pc->conversations[pc->active].messages[oj];
            /* Fast-path promote with text: stage TEXT payload + items array
               = two allocations. */
            {
                bool saw_fail = false, saw_ok = false, damaged = false;
                for (long pos = 1; pos <= 4; pos++) {
                    ChatMessage before = *om;
                    alloc_number = 0;
                    fail_allocation = pos;
                    bool ok = chat_message_add_image(om, &only, 0);
                    alloc_number = 0;
                    fail_allocation = 0;
                    if (!ok) {
                        saw_fail = true;
                        if (memcmp(om, &before, sizeof *om) != 0)
                            damaged = true;
                        continue;
                    }
                    saw_ok = true;
                    chat_message_clear_parts(om);
                    if (memcmp(om, &before, sizeof *om) != 0 &&
                            wcscmp(chat_message_text(om), L"oom fixture"))
                        damaged = true;
                }
                check(saw_fail && saw_ok && !damaged,
                    "add_image promotion is byte-unchanged on each failed alloc");
            }
            /* Promoted set_text staging: input copy, TEXT payload, optional
               items insert, optional projection overflow. */
            check(chat_message_add_image(om, &only, 0) &&
                    chat_message_set_text(om, L"seed"),
                "oom set_text fixture is promoted");
            {
                bool saw_fail = false, saw_ok = false, damaged = false;
                for (long pos = 1; pos <= 6; pos++) {
                    ChatMessage before = *om;
                    alloc_number = 0;
                    fail_allocation = pos;
                    bool ok = chat_message_set_text(om,
                        L"a promoted set_text payload long enough to force "
                        L"overflow allocation on the projection path");
                    alloc_number = 0;
                    fail_allocation = 0;
                    if (!ok) {
                        saw_fail = true;
                        if (memcmp(om, &before, sizeof *om) != 0)
                            damaged = true;
                        continue;
                    }
                    saw_ok = true;
                    if (!parts_consistent(om)) damaged = true;
                    chat_message_set_text(om, L"seed");
                }
                check(saw_fail && saw_ok && !damaged,
                    "promoted set_text is byte-unchanged on each failed alloc");
            }
            /* append_text: combined buffer then the set path. */
            {
                bool saw_fail = false, saw_ok = false, damaged = false;
                for (long pos = 1; pos <= 6; pos++) {
                    ChatMessage before = *om;
                    alloc_number = 0;
                    fail_allocation = pos;
                    bool ok = chat_message_append_text(om, L" plus suffix");
                    alloc_number = 0;
                    fail_allocation = 0;
                    if (!ok) {
                        saw_fail = true;
                        if (memcmp(om, &before, sizeof *om) != 0)
                            damaged = true;
                        continue;
                    }
                    saw_ok = true;
                    if (!parts_consistent(om)) damaged = true;
                    chat_message_set_text(om, L"seed");
                }
                check(saw_fail && saw_ok && !damaged,
                    "promoted append_text is byte-unchanged on each failed alloc");
            }
            /* Image-array growth: promote, fill to capacity, then fail the
               staged items malloc on the next add. */
            {
                chat_message_clear_parts(om);
                check(chat_message_set_text(om, L"grow"),
                    "growth fixture text set");
                check(chat_message_add_image(om, &only, 0) &&
                        om->parts.items != NULL,
                    "growth fixture promoted");
                bool filled = true;
                while (om->parts.count < om->parts.capacity) {
                    ChatImagePart p;
                    memset(&p, 0, sizeof p);
                    p.attachment_id = 5000 + om->parts.count;
                    memcpy(p.mime, "image/png", 10);
                    if (!chat_message_add_image(om, &p, 0)) {
                        filled = false;
                        break;
                    }
                }
                check(filled && om->parts.count == om->parts.capacity,
                    "growth fixture filled to parts capacity");
                {
                    ChatMessage before = *om;
                    alloc_number = 0;
                    fail_allocation = 1;
                    bool ok = chat_message_add_image(om, &only, 0);
                    alloc_number = 0;
                    fail_allocation = 0;
                    check(!ok && memcmp(om, &before, sizeof *om) == 0,
                        "failed image-array growth is byte-unchanged");
                }
                size_t cap_before = om->parts.capacity;
                size_t count_before = om->parts.count;
                check(chat_message_add_image(om, &only, 0) &&
                        om->parts.count == count_before + 1 &&
                        om->parts.capacity > cap_before,
                    "image-array growth succeeds once allocation is allowed");
            }
            chat_message_clear_parts(om);
        }

        /* Snapshot deep-copies parts and TEXT payloads; isolation both ways. */
        {
            Chat *src = (Chat *)calloc(1, sizeof *src);
            if (!src) return 2;
            chat_init(src);
            chat_clear(src);
            int si = chat_append(src, CHAT_ROLE_USER, L"snapshot parts");
            ChatMessage *sm = &src->conversations[0].messages[si];
            check(chat_message_add_image(sm, &img, 0),
                "snapshot fixture promoted");
            Chat *snap = chat_snapshot(src);
            check(snap != NULL, "snapshot of a parts message succeeds");
            if (snap) {
                ChatMessage *cm = &snap->conversations[0].messages[si];
                check(cm->parts.items != NULL &&
                        cm->parts.items != sm->parts.items &&
                        cm->parts.count == sm->parts.count,
                    "snapshot clones the parts array, never aliases it");
                check(cm->parts.items[0].kind == CHAT_PART_TEXT &&
                        cm->parts.items[0].u.text.data !=
                            sm->parts.items[0].u.text.data &&
                        !wcscmp(cm->parts.items[0].u.text.data,
                            sm->parts.items[0].u.text.data),
                    "snapshot clones TEXT part payloads");
                check(cm->parts.items[1].u.image.attachment_id ==
                        sm->parts.items[1].u.image.attachment_id,
                    "snapshot copies IMAGE metadata by value");
                check(chat_message_set_text(sm, L"source changed") &&
                        !wcscmp(chat_message_text(cm), L"snapshot parts") &&
                        !wcscmp(cm->parts.items[0].u.text.data,
                            L"snapshot parts"),
                    "mutating the source after snapshot leaves the copy alone");
                size_t source_count = sm->parts.count;
                check(chat_message_remove_part(cm, 1) &&
                        sm->parts.items != NULL &&
                        sm->parts.count == source_count,
                    "mutating the copy leaves the source alone");
                check_invariants(snap);
                chat_dispose(snap);
                free(snap);
                check(sm->parts.items != NULL &&
                        !wcscmp(chat_message_text(sm), L"source changed"),
                    "disposing the snapshot leaves the source's parts intact");
            }

            /* OOM sweep across the snapshot's parts clone positions. */
            {
                bool saw_fail = false, saw_ok = false, damaged = false;
                for (long pos = 1; pos <= 8; pos++) {
                    alloc_number = 0;
                    fail_allocation = pos;
                    Chat *s = chat_snapshot(src);
                    alloc_number = 0;
                    fail_allocation = 0;
                    if (!s) {
                        saw_fail = true;
                        if (sm->parts.items == NULL ||
                                sm->parts.count != 2 ||
                                !parts_consistent(sm))
                            damaged = true;
                        continue;
                    }
                    saw_ok = true;
                    if (!parts_consistent(&s->conversations[0].messages[si]))
                        damaged = true;
                    chat_dispose(s);
                    free(s);
                }
                check(saw_fail && saw_ok && !damaged,
                    "failing any snapshot parts allocation is transactional");
            }
            chat_dispose(src);
            free(src);
        }

        /* Conversation deletion / clear dispose parts without double-free. */
        {
            Chat *del = (Chat *)calloc(1, sizeof *del);
            if (!del) return 2;
            chat_init(del);
            chat_clear(del);
            chat_append(del, CHAT_ROLE_USER, L"first");
            chat_message_add_image(
                &del->conversations[0].messages[0], &img, 0);
            chat_append(del, CHAT_ROLE_USER, L"second");
            chat_message_add_image(
                &del->conversations[0].messages[1], &img, 0);
            check(chat_new_conversation(del) >= 1,
                "second conversation for delete transfer");
            del->active = 0;
            check(chat_delete(del),
                "conversation with part messages deletes");
            check(del->conversations[0].messages == NULL &&
                    del->conversations[0].message_count == 0,
                "deleted conversation releases message storage");
            chat_append(del, CHAT_ROLE_USER, L"after delete");
            chat_clear(del);
            check(del->conversations[0].messages == NULL &&
                    del->conversations[0].message_count == 0,
                "clear releases message storage");
            check_invariants(del);
            chat_dispose(del);
            free(del);
        }

        check_invariants(pc);
        chat_dispose(pc);
        free(pc);
    }

    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat checks passed\n");
    return 0;
}
