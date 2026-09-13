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

/* Allocation seam for the boundary-crossing test below: the chat test links
   with -Wl,--wrap=realloc (chat.bat test does), so every allocation the
   message growth path makes starts poisoned. The spare capacity beyond a
   copied string is then known memory, which makes the crossing checks
   deterministic instead of reading whatever malloc left there. */
void *__real_realloc(void *pointer, size_t size);
void *__wrap_realloc(void *pointer, size_t size) {
    void *grown = __real_realloc(pointer, size);
    if (grown) memset(grown, 0x5C, size);
    return grown;
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
    free(late);

    free(chat);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat checks passed\n");
    return 0;
}
