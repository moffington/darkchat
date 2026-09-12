#include "chat.h"
#include <string.h>
#include <time.h>

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
    chat->next_id = (uint64_t)chat_now();
    chat->sidebar_width = 232;
    chat->window_width = 1100;
    chat->window_height = 720;
    chat->window_x = chat->window_y = -32000;
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
    conversation->id = ++chat->next_id;
    conversation->created_at = conversation->modified_at = chat_now();
    chat->active = index;
    wcsncpy(chat->status, L"New conversation", CHAT_STATUS_TEXT - 1);
    chat->status[CHAT_STATUS_TEXT - 1] = 0;
    return index;
}

bool chat_select_conversation(Chat *chat, int index) {
    if (index < 0 || index >= chat->conversation_count) return false;
    chat->active = index;
    swprintf(chat->status, CHAT_STATUS_TEXT, L"Opened %ls",
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
    memset(message, 0, sizeof *message);
    chat_generation_init(&message->generation);
    message->created_at = message->modified_at = chat_now();
    conversation->modified_at = message->modified_at;
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
        if (first && !conversation->renamed && message->text[0]) {
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


int64_t chat_now(void) {
    return (int64_t)time(NULL) * 1000;
}

void chat_generation_init(ChatGeneration *g) {
    memset(g, 0, sizeof *g);
    g->ttft_ms = g->latency_ms = g->prompt_tokens = g->completion_tokens =
        g->total_tokens = g->cost = g->reasoning_ms = -1;
}

const wchar_t *chat_generation_name(ChatGenerationState state) {
    static const wchar_t *names[] = {L"Local", L"Generating", L"Complete",
        L"Cancelled", L"Interrupted", L"Failed"};
    return state >= 0 && state <= CHAT_GENERATION_FAILED ? names[state] : L"Unknown";
}

void chat_remember_model(Chat *chat) {
    int found = chat->model_history_count;
    for (int i = 0; i < chat->model_history_count; i++)
        if (!wcscmp(chat->model_history[i], chat->model)) { found = i; break; }
    if (found == CHAT_MODEL_HISTORY) --found;
    for (int i = found; i > 0; --i)
        wcscpy(chat->model_history[i], chat->model_history[i - 1]);
    wcscpy(chat->model_history[0], chat->model);
    if (chat->model_history_count < CHAT_MODEL_HISTORY && found == chat->model_history_count)
        ++chat->model_history_count;
}

bool chat_rename(Chat *chat, const wchar_t *title) {
    if (!chat_active(chat) || !title || !title[0] || wcslen(title) >= CHAT_TITLE_TEXT) return false;
    ChatConversation *c = &chat->conversations[chat->active];
    wcscpy(c->title, title);
    c->renamed = true;
    c->modified_at = chat_now();
    return true;
}

bool chat_delete(Chat *chat) {
    if (!chat_active(chat)) return false;
    int index = chat->active;
    --chat->conversation_count;
    memmove(&chat->conversations[index], &chat->conversations[index + 1],
        (chat->conversation_count - index) * sizeof(ChatConversation));
    memset(&chat->conversations[chat->conversation_count], 0, sizeof(ChatConversation));
    if (!chat->conversation_count) chat_new_conversation(chat);
    else if (index >= chat->conversation_count) chat->active = chat->conversation_count - 1;
    return true;
}

bool chat_delete_all(Chat *chat) {
    if (!chat_active(chat)) return false;
    memset(chat->conversations, 0, sizeof chat->conversations);
    chat->conversation_count = 0;
    chat->active = -1;
    return chat_new_conversation(chat) >= 0;
}

void chat_clear(Chat *chat) {
    if (!chat_active(chat)) return;
    ChatConversation *c = &chat->conversations[chat->active];
    memset(c->messages, 0, sizeof c->messages);
    c->message_count = 0;
    c->draft[0] = 0;
    c->modified_at = chat_now();
}

int chat_latest_user(const ChatConversation *c) {
    if (c) for (int i = c->message_count - 1; i >= 0; --i)
        if (c->messages[i].role == CHAT_ROLE_USER) return i;
    return -1;
}

bool chat_history_message(const ChatMessage *m) {
    return m->role != CHAT_ROLE_ERROR &&
        (m->role != CHAT_ROLE_ASSISTANT || m->generation.state == CHAT_GENERATION_COMPLETE);
}

int chat_begin_response(Chat *chat, ChatSendMode mode, const wchar_t *prompt) {
    if (!chat_active(chat)) return -1;
    ChatConversation *c = &chat->conversations[chat->active];
    for (int i = 0; i < c->message_count; i++)
        if (c->messages[i].generation.state == CHAT_GENERATION_RUNNING) return -1;
    int user = chat_latest_user(c);
    if (mode == CHAT_SEND) {
        if (!prompt || !prompt[0] || wcslen(prompt) >= CHAT_MESSAGE_TEXT || chat_remaining(chat) < 2) return -1;
        user = chat_append(chat, CHAT_ROLE_USER, prompt);
    } else {
        if (user < 0 || user + 1 >= CHAT_MAX_MESSAGES) return -1;
        if (mode == CHAT_RETRY && c->message_count > user + 1 &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_FAILED &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_CANCELLED &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_INTERRUPTED) return -1;
        if (mode == CHAT_EDIT_RESEND) {
            if (!prompt || !prompt[0] || wcslen(prompt) >= CHAT_MESSAGE_TEXT) return -1;
            wcscpy(c->messages[user].text, prompt);
            c->messages[user].modified_at = chat_now();
        }
        memset(&c->messages[user + 1], 0, (c->message_count - user - 1) * sizeof(ChatMessage));
        c->message_count = user + 1;
    }
    int index = chat_append(chat, CHAT_ROLE_ASSISTANT, L"");
    ChatGeneration *g = &c->messages[index].generation;
    g->state = CHAT_GENERATION_RUNNING;
    g->started_at = chat_now();
    wcscpy(g->requested_model, chat->model);
    chat_remember_model(chat);
    return index;
}

