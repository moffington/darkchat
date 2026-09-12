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

    /* Oversized message is truncated safely. */
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
    check(wcslen(last->text) == CHAT_MESSAGE_TEXT - 1, "oversized message truncated");
    free(big);

    /* A trailing high surrogate is not left dangling. */
    chat_append(spare, CHAT_ROLE_USER, L"tail\xd83d");
    last = &chat_active(spare)->messages[chat_active(spare)->message_count - 1];
    check(wcslen(last->text) == 4, "dangling high surrogate trimmed");
    free(spare);

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
        wcslen(chat_active(long_chat)->messages[mid_index].text) == 12000,
        "a 12,000-unit response is preserved past the old 4,096 limit");
    free(mid);
    free(long_chat);

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
