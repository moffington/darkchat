#include "chat.h"
#include <string.h>

static const wchar_t *const welcome =
    L"Welcome to DarkChat.\n\n"
    L"Type a message below and press Enter; the whole conversation goes to "
    L"the model selected in the toolbar over the OpenRouter chat-completions "
    L"endpoint. Set OPENROUTER_API_KEY in the environment before launching; "
    L"without it DarkChat explains what is missing instead of answering.";

/* Bounded append that never overruns the destination. */
static void append3(wchar_t *dst, size_t capacity, size_t *used,
    const wchar_t *part) {
    if (!part || *used + 1 >= capacity) return;
    size_t room = capacity - 1 - *used;
    size_t len = wcslen(part);
    if (len > room) len = room;
    wmemcpy(dst + *used, part, len);
    *used += len;
    dst[*used] = 0;
}

/* Copies the first line of text, trimmed to at most max code units. */
static void first_line(wchar_t *dst, size_t capacity, const wchar_t *text,
    size_t max) {
    if (capacity == 0) return;
    dst[0] = 0;
    if (!text) return;
    size_t length = 0;
    while (text[length] && text[length] != L'\n' && text[length] != L'\r' &&
        length < max && length + 1 < capacity) {
        dst[length] = text[length];
        ++length;
    }
    /* Never leave a dangling high surrogate. */
    if (length && dst[length - 1] >= 0xd800 && dst[length - 1] <= 0xdbff) --length;
    dst[length] = 0;
}

void chat_init(Chat *chat) {
    memset(chat, 0, sizeof *chat);
    wcsncpy(chat->model, L"openai/gpt-4o-mini", CHAT_MODEL_TEXT - 1);
    chat->model[CHAT_MODEL_TEXT - 1] = 0;
    chat->active = 0;
    chat_new_conversation(chat);
    chat_append(chat, CHAT_ROLE_ASSISTANT, welcome);
}

int chat_new_conversation(Chat *chat) {
    if (chat->conversation_count >= CHAT_MAX_CONVERSATIONS) return -1;
    int index = chat->conversation_count++;
    ChatConversation *conversation = &chat->conversations[index];
    memset(conversation, 0, sizeof *conversation);
    swprintf(conversation->title, CHAT_TITLE_TEXT, L"Conversation %d", index + 1);
    chat->active = index;
    wcsncpy(chat->status, L"New conversation", CHAT_STATUS_TEXT - 1);
    chat->status[CHAT_STATUS_TEXT - 1] = 0;
    return index;
}

bool chat_select_conversation(Chat *chat, int index) {
    if (index < 0 || index >= chat->conversation_count) return false;
    chat->active = index;
    swprintf(chat->status, CHAT_STATUS_TEXT, L"Opened %s",
        chat->conversations[index].title);
    return true;
}

const ChatConversation *chat_active(const Chat *chat) {
    if (chat->active < 0 || chat->active >= chat->conversation_count) return NULL;
    return &chat->conversations[chat->active];
}

int chat_remaining(const Chat *chat) {
    const ChatConversation *conversation = chat_active(chat);
    if (!conversation) return 0;
    int remaining = CHAT_MAX_MESSAGES - conversation->message_count;
    return remaining > 0 ? remaining : 0;
}

int chat_append_at(Chat *chat, int conversation_index, ChatRole role,
    const wchar_t *text) {
    if (conversation_index < 0 ||
        conversation_index >= chat->conversation_count) return -1;
    ChatConversation *conversation = &chat->conversations[conversation_index];
    if (conversation->message_count >= CHAT_MAX_MESSAGES) return -1;
    ChatMessage *message = &conversation->messages[conversation->message_count];
    message->role = role;
    size_t length = text ? wcslen(text) : 0;
    if (length >= CHAT_MESSAGE_TEXT) length = CHAT_MESSAGE_TEXT - 1;
    if (length && text[length - 1] >= 0xd800 && text[length - 1] <= 0xdbff) --length;
    if (length) wmemcpy(message->text, text, length);
    message->text[length] = 0;
    int index = conversation->message_count++;
    if (role == CHAT_ROLE_USER) {
        /* Name the conversation after its first user message. */
        bool first = true;
        for (int i = 0; i < index; i++)
            if (conversation->messages[i].role == CHAT_ROLE_USER) first = false;
        if (first && message->text[0]) {
            wchar_t derived[CHAT_TITLE_TEXT];
            first_line(derived, CHAT_TITLE_TEXT, message->text, 40);
            if (derived[0]) {
                wcsncpy(conversation->title, derived, CHAT_TITLE_TEXT - 1);
                conversation->title[CHAT_TITLE_TEXT - 1] = 0;
            }
        }
    }
    return index;
}

int chat_append(Chat *chat, ChatRole role, const wchar_t *text) {
    return chat_append_at(chat, chat->active, role, text);
}

void chat_fake_reply(Chat *chat, const wchar_t *prompt, wchar_t *out,
    size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = 0;
    unsigned reply = ++chat->replies;
    wchar_t echo[160];
    first_line(echo, 160, prompt, 120);
    size_t used = 0;
    switch ((reply - 1) % 3) {
    case 0:
        append3(out, capacity, &used, L"You wrote: \u201c");
        append3(out, capacity, &used, echo);
        append3(out, capacity, &used,
            L"\u201d\n\n"
            L"Here is an offline sample reply. The transcript you are reading is "
            L"a native Rich Edit control, so you can select, copy and scroll it "
            L"just like any Windows text.\n\n"
            L"```c\n"
            L"int darkchat_ready(void) {\n"
            L"    return 1;\n"
            L"}\n"
            L"```\n\n"
            L"In the next pass this text arrives from OpenRouter over HTTPS. "
            L"See https://openrouter.ai/docs/quickstart for the endpoint details.");
        break;
    case 1:
        append3(out, capacity, &used, L"Echo: \u201c");
        append3(out, capacity, &used, echo);
        append3(out, capacity, &used,
            L"\u201d\n\n"
            L"This build renders role headers, monospace code fences and "
            L"automatic links without any network access.\n\n"
            L"```bat\n"
            L"chat.bat\n"
            L"build\\darkchat.exe\n"
            L"```\n\n"
            L"Try Shift+Enter for a newline, or Ctrl+A to select the whole "
            L"transcript and Ctrl+C to copy it.");
        break;
    default:
        append3(out, capacity, &used, L"Received \u201c");
        append3(out, capacity, &used, echo);
        append3(out, capacity, &used,
            L"\u201d.\n\n"
            L"Everything here is generated locally. The composer keeps native "
            L"undo, clipboard and selection, so Ctrl+Z, Ctrl+C and Ctrl+V all "
            L"behave exactly as you expect.\n\n"
            L"See https://learn.microsoft.com/windows/win32/controls/rich-edit-controls "
            L"for the control this app relies on.");
        break;
    }
    swprintf(chat->status, CHAT_STATUS_TEXT, L"Offline reply %u generated", reply);
}

