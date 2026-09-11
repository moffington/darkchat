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

    /* chat_remaining guards invalid state. */
    chat->active = 99;
    check(chat_remaining(chat) == 0, "invalid active has no remaining");

    free(chat);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat checks passed\n");
    return 0;
}
