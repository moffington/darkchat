#include "../chat/chat.h"
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
void *__wrap_realloc(void *pointer, size_t size) {
    if (fail_next_reallocs > 0) { --fail_next_reallocs; return NULL; }
    void *grown = __real_realloc(pointer, size);
    /* Poison only fresh allocations. Growing an existing buffer must keep
       the bytes realloc already copied, or live message text and overflow
       pointers would be destroyed by the seam itself. */
    if (grown && !pointer) memset(grown, 0x5C, size);
    return grown;
}
void *__wrap_malloc(size_t size) {
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

    wchar_t reply[CHAT_MESSAGE_TEXT];
    chat_fake_reply(chat, L"Explain native text controls", reply, CHAT_MESSAGE_TEXT);
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
    wchar_t *big = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_MESSAGE_TEXT * 2));
    if (!big) return 2;
    for (size_t i = 0; i < CHAT_MESSAGE_TEXT * 2 - 1; i++) big[i] = L'x';
    big[CHAT_MESSAGE_TEXT * 2 - 1] = 0;
    chat_append(spare, CHAT_ROLE_USER, big);
    const ChatMessage *last =
        &chat_active(spare)->messages[chat_active(spare)->message_count - 1];
    check(wcslen(chat_message_text(last)) == CHAT_MESSAGE_TEXT * 2 - 1,
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
        "a 12,000-unit response is preserved past the old 4,096 limit");
    free(mid);
    chat_dispose(long_chat); free(long_chat);

    /* Streamed growth across the inline boundary must copy exactly the
       inline characters onto the heap; a wrong copy count still leaves
       the string NUL-terminated, so only memory checks can see the damage. */
    Chat *grow = (Chat *)calloc(1, sizeof *grow);
    if (!grow) return 2;
    chat_init(grow);
    wchar_t *fill = (wchar_t *)malloc(sizeof(wchar_t) * (CHAT_MESSAGE_TEXT + 1));
    if (!fill) return 2;
    for (size_t i = 0; i < CHAT_MESSAGE_TEXT - 1; i++) fill[i] = L'x';
    fill[CHAT_MESSAGE_TEXT - 1] = 0;
    int stream = chat_append(grow, CHAT_ROLE_ASSISTANT, L"");
    check(stream >= 0, "empty assistant message appended");
    ChatMessage *m = &grow->conversations[grow->active].messages[stream];
    check(chat_message_append_text(m, fill) && !m->text_overflow,
        "a boundary-minus-one answer stays inline");
    check(chat_message_append_text(m, L"!"),
        "appending past the boundary succeeds");
    check(m->text_overflow != NULL, "the crossed answer moved to heap storage");
    check(wcslen(chat_message_text(m)) == CHAT_MESSAGE_TEXT,
        "the crossed answer keeps every character");
    check(chat_message_text(m)[0] == L'x' &&
        chat_message_text(m)[CHAT_MESSAGE_TEXT - 2] == L'x' &&
        chat_message_text(m)[CHAT_MESSAGE_TEXT - 1] == L'!',
        "the crossed answer preserves head, seam and tail");
    /* Further growth reuses and doubles the heap buffer without touching the
       inline source again; content must survive every hop, including a
       subsequent heap-doubling reallocation. */
    for (int i = 0; i < 3; i++) {
        check(chat_message_append_text(m, fill) &&
            wcslen(chat_message_text(m)) ==
                CHAT_MESSAGE_TEXT + (size_t)(i + 1) * (CHAT_MESSAGE_TEXT - 1),
            "repeated heap growth preserves the streamed answer");
    }

    /* The reasoning buffer has the same inline size and grows identically,
       without disturbing the answer stored beside it. */
    for (size_t i = 0; i < CHAT_MESSAGE_TEXT - 1; i++) fill[i] = L'r';
    fill[CHAT_MESSAGE_TEXT - 1] = 0;
    int think = chat_append(grow, CHAT_ROLE_ASSISTANT, L"");
    check(think >= 0, "second assistant message appended");
    ChatMessage *t = &grow->conversations[grow->active].messages[think];
    check(chat_message_append_reasoning(t, fill) && !t->reasoning_overflow,
        "a boundary-minus-one reasoning stays inline");
    check(chat_message_append_reasoning(t, L"!"),
        "reasoning appending past the boundary succeeds");
    check(t->reasoning_overflow != NULL &&
        wcslen(chat_message_reasoning(t)) == CHAT_MESSAGE_TEXT &&
        chat_message_reasoning(t)[0] == L'r' &&
        chat_message_reasoning(t)[CHAT_MESSAGE_TEXT - 2] == L'r' &&
        chat_message_reasoning(t)[CHAT_MESSAGE_TEXT - 1] == L'!',
        "the crossed reasoning preserves head, seam and tail");
    check(!t->text[0] && !t->text_overflow,
        "answer storage is untouched by reasoning growth");
    free(fill);
    chat_dispose(grow); free(grow);

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
            wchar_t *fill = (wchar_t *)malloc(sizeof(wchar_t) * CHAT_MESSAGE_TEXT);
            if (!fill) return 2;
            for (size_t i = 0; i < CHAT_MESSAGE_TEXT - 1; i++) fill[i] = L'w';
            fill[CHAT_MESSAGE_TEXT - 1] = 0;
            check(chat_message_append_text(guarded, fill),
                "the guarded message fills its inline buffer");
            check(chat_message_append_text(guarded, L"!"),
                "the guarded boundary crossing succeeds");
            check(guarded->text_overflow != NULL &&
                wcslen(chat_message_text(guarded)) == CHAT_MESSAGE_TEXT &&
                chat_message_text(guarded)[0] == L'w' &&
                chat_message_text(guarded)[CHAT_MESSAGE_TEXT - 1] == L'!',
                "the guarded crossing preserves the message");
            int intact = 1;
            /* Spare capacity begins one past the new terminator. */
            for (size_t i = CHAT_MESSAGE_TEXT + 1;
                i < CHAT_MESSAGE_TEXT + 257; i++)
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
        check(c->message_capacity == 64, "growth doubles to the hard cap");
        while (c->message_count < 64)
            chat_append(growth, CHAT_ROLE_ASSISTANT, L"filler");
        check(c->message_count == 64 && c->message_capacity == 64,
            "the conversation holds exactly 64 messages at full capacity");
        check(chat_append(growth, CHAT_ROLE_USER, L"overflow") < 0,
            "no message is accepted beyond the cap");
        check(c->message_count == 64 && c->message_capacity == 64,
            "capacity never rises above 64");
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
            sizeof(wchar_t) * (CHAT_MESSAGE_TEXT + 700));
        if (!oversized) return 2;
        for (size_t i = 0; i < CHAT_MESSAGE_TEXT + 699; i++) oversized[i] = L'x';
        oversized[CHAT_MESSAGE_TEXT + 699] = 0;
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

    /* Replacement at the 64-message cap: the final live count is within the
       cap, so no growth is needed and the operation succeeds. */
    {
        Chat *cap = (Chat *)calloc(1, sizeof *cap);
        if (!cap) return 2;
        chat_init(cap); chat_clear(cap);
        const ChatConversation *c = chat_active(cap);
        fill_conversation(cap, 32, 0);     /* 64 messages, capacity 64 */
        check(c->message_count == 64 && c->message_capacity == 64,
            "boundary conversation is at the hard cap");
        /* Retry replaces the last response, so the old one must be in a
           failed terminal state first. */
        c->messages[63].generation.state = CHAT_GENERATION_FAILED;
        check(chat_begin_response(cap, CHAT_RETRY, NULL) == 63,
            "retry succeeds at the 64-message cap");
        check(c->message_count == 64 && c->message_capacity == 64,
            "retry at the cap needs no growth");
        check(!wcscmp(c->messages[62].text, L"question") &&
            !c->messages[63].text[0],
            "retry at the cap replaced the last response");
        c->messages[63].generation.state = CHAT_GENERATION_COMPLETE;
        check(chat_begin_response(cap, CHAT_REGENERATE, NULL) == 63,
            "regenerate succeeds at the 64-message cap");
        c->messages[63].generation.state = CHAT_GENERATION_COMPLETE;
        check(chat_begin_response(cap, CHAT_EDIT_RESEND, L"edited at the cap")
            == 63, "edit-resend succeeds at the 64-message cap");
        check(!wcscmp(c->messages[62].text, L"edited at the cap") &&
            c->message_count == 64,
            "edit-resend at the cap replaced the user text");
        check_invariants(cap);

        /* A final shape of 65 is rejected without mutation. */
        chat_clear(cap);
        fill_conversation(cap, 31, 1);     /* 63 messages, last is user */
        uint64_t next_id = cap->next_id;
        int64_t modified_at = c->modified_at;
        check(chat_begin_response(cap, CHAT_SEND, L"one too many") < 0,
            "a send whose final shape needs 65 live messages is rejected");
        check(c->message_count == 63 && cap->next_id == next_id &&
            c->modified_at == modified_at,
            "a rejected send mutates nothing");
        chat_append(cap, CHAT_ROLE_USER, L"sixty-fourth");  /* user at 63 */
        next_id = cap->next_id; modified_at = c->modified_at;
        check(chat_begin_response(cap, CHAT_REGENERATE, NULL) < 0,
            "a regenerate whose final shape needs 65 live messages is rejected");
        check(chat_begin_response(cap, CHAT_RETRY, NULL) < 0,
            "a retry whose final shape needs 65 live messages is rejected");
        check(chat_begin_response(cap, CHAT_EDIT_RESEND, L"no") < 0,
            "an edit-resend whose final shape needs 65 live messages is rejected");
        check(c->message_count == 64 && cap->next_id == next_id &&
            c->modified_at == modified_at,
            "rejected 65-message replacements mutate nothing");
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

    /* Ownership: deleting a mid-list conversation transfers the surviving
       conversations' allocations without double frees, and the moved
       conversation keeps accepting messages. */
    {
        Chat *own = (Chat *)calloc(1, sizeof *own);
        if (!own) return 2;
        chat_init(own);
        wchar_t *big = (wchar_t *)malloc(
            sizeof(wchar_t) * (CHAT_MESSAGE_TEXT * 2));
        if (!big) return 2;
        for (size_t i = 0; i < CHAT_MESSAGE_TEXT * 2 - 1; i++) big[i] = L'x';
        big[CHAT_MESSAGE_TEXT * 2 - 1] = 0;
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
            CHAT_MESSAGE_TEXT * 2 - 1,
            "the moved overflow allocation survived the ownership transfer");
        check(chat_append(own, CHAT_ROLE_ASSISTANT, L"after the move") == 2,
            "the moved conversation still appends after a deletion elsewhere");
        check(wcslen(chat_message_text(&own->conversations[0].messages[1])) ==
            CHAT_MESSAGE_TEXT * 2 - 1,
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

    chat_dispose(chat);
    free(chat);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat checks passed\n");
    return 0;
}
