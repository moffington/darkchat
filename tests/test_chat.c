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
   conversation of a chat. */
static void check_invariants(Chat *chat) {
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
    }
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
        live text, which is proportional to actual content, so it is not a
        complete worst-case memory bound. CHAT_MAX_MESSAGES is the shipped
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

    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat checks passed\n");
    return 0;
}
