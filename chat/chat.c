#include "chat.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const wchar_t *const welcome =
    L"Welcome to DarkChat.\n\n"
    L"Type a message below and press Enter; the conversation goes to the model "
    L"selected in the toolbar, sent to OpenRouter over HTTPS or to a local "
    L"Ollama server over its OpenAI-compatible endpoint. Settings > Backend "
    L"chooses which. OpenRouter needs OPENROUTER_API_KEY in the environment; "
    L"Ollama needs only a running local server. DarkChat explains what is "
    L"missing instead of answering.";

static const wchar_t *message_value(const wchar_t *inline_text,
    const wchar_t *overflow) {
    return overflow ? overflow : inline_text;
}

const wchar_t *chat_message_text(const ChatMessage *m) {
    return m ? message_value(m->text,m->text_overflow) : L"";
}

const wchar_t *chat_message_reasoning(const ChatMessage *m) {
    return m ? message_value(m->reasoning,m->reasoning_overflow) : L"";
}

static bool set_message_value(wchar_t *inline_text, size_t inline_capacity,
    wchar_t **overflow, size_t *length, size_t *capacity,
    const wchar_t *text) {
    if (!text) text=L"";
    size_t needed=wcslen(text);
    if (needed && text[needed-1]>=0xd800 && text[needed-1]<=0xdbff) --needed;
    if (needed<inline_capacity) {
        free(*overflow); *overflow=NULL; *capacity=0;
        if (needed) wmemcpy(inline_text,text,needed);
        inline_text[needed]=0; *length=needed;
        return true;
    }
    if (needed==SIZE_MAX/sizeof(wchar_t)) return false;
    wchar_t *next=(wchar_t *)malloc((needed+1)*sizeof(wchar_t));
    if (!next) return false;
    if (needed) wmemcpy(next,text,needed);
    next[needed]=0;
    free(*overflow);
    *overflow=next; *length=needed; *capacity=needed+1;
    inline_text[0]=0;
    return true;
}

static bool append_message_value(wchar_t *inline_text, size_t inline_capacity,
    wchar_t **overflow, size_t *length, size_t *capacity,
    const wchar_t *text) {
    if (!text || !text[0]) return true;
    const wchar_t *current=message_value(inline_text,*overflow);
    if (!*length && current[0]) *length=wcslen(current);
    size_t added=wcslen(text);
    if (added>SIZE_MAX-1-*length) return false;
    size_t needed=*length+added+1;
    if (!*overflow && needed<=inline_capacity) {
        wmemcpy(inline_text+*length,text,added+1);
        *length+=added;
        return true;
    }
    if (needed>*capacity) {
        size_t next=*capacity ? *capacity : inline_capacity*2;
        while (next<needed) {
            if (next>SIZE_MAX/2) { next=needed; break; }
            next*=2;
        }
        if (next>SIZE_MAX/sizeof(wchar_t)) return false;
        wchar_t *grown=(wchar_t *)realloc(*overflow,
            next*sizeof(wchar_t));
        if (!grown) return false;
        /* Byte-oriented copy: the count is explicitly in bytes, so the unit
           of the length cannot be confused with elements again. */
        if (!*overflow) memcpy(grown,inline_text,(*length+1)*sizeof *grown);
        *overflow=grown; *capacity=next; inline_text[0]=0;
    }
    wmemcpy(*overflow+*length,text,added+1);
    *length+=added;
    return true;
}

bool chat_message_set_text(ChatMessage *m,const wchar_t *text) {
    if (!m) return false;
    if (!text) text=L"";
    const wchar_t *previous=message_value(m->text,m->text_overflow);
    bool unchanged=!wcscmp(previous,text);
    if (!set_message_value(m->text,CHAT_MESSAGE_INLINE,&m->text_overflow,
        &m->text_length,&m->text_capacity,text)) return false;
    /* Bump only when the stored value actually changes. The text-only
       revision lets the transcript keep the rendered body when only metadata
       or reasoning changed. */
    if (!unchanged) { ++m->revision; ++m->body_revision; }
    return true;
}
bool chat_message_set_reasoning(ChatMessage *m,const wchar_t *text) {
    if (!m) return false;
    if (!text) text=L"";
    const wchar_t *previous=message_value(m->reasoning,m->reasoning_overflow);
    bool unchanged=!wcscmp(previous,text);
    if (!set_message_value(m->reasoning,CHAT_REASONING_INLINE,
        &m->reasoning_overflow,&m->reasoning_length,&m->reasoning_capacity,
        text)) return false;
    if (!unchanged) ++m->revision;
    return true;
}
bool chat_message_append_text(ChatMessage *m,const wchar_t *text) {
    if (!m) return false;
    if (!append_message_value(m->text,CHAT_MESSAGE_INLINE,&m->text_overflow,
        &m->text_length,&m->text_capacity,text)) return false;
    if (text && text[0]) { ++m->revision; ++m->body_revision; }
    return true;
}
bool chat_message_append_reasoning(ChatMessage *m,const wchar_t *text) {
    if (!m) return false;
    if (!append_message_value(m->reasoning,CHAT_REASONING_INLINE,
        &m->reasoning_overflow,&m->reasoning_length,&m->reasoning_capacity,
        text)) return false;
    if (text && text[0]) ++m->revision;
    return true;
}
/* Direct generation mutations that the setters cannot see go through here. */
void chat_message_touch(ChatMessage *message) {
    if (message) ++message->revision;
}

void chat_message_dispose(ChatMessage *m) {
    if (!m) return;
    free(m->text_overflow); free(m->reasoning_overflow);
    m->text_overflow=m->reasoning_overflow=NULL;
    m->text_capacity=m->reasoning_capacity=0;
}

/* Initial reservation for a conversation that starts receiving messages:
   small enough to reserve little memory, large enough that ordinary
   prompt/answer pairs rarely regrow. Growth doubles up to the hard cap. */
#define CHAT_MESSAGES_INITIAL 8

/* Grows the message array to hold at least `needed` live messages.
   Transactional: on failure the original allocation stays valid and the
   conversation is untouched. Never shrinks and never exceeds the cap. */
static bool chat_ensure_capacity(ChatConversation *c, size_t needed) {
    if (needed <= c->message_capacity) return true;
    if (needed > CHAT_MAX_MESSAGES) return false;
    size_t capacity = c->message_capacity ? c->message_capacity
        : CHAT_MESSAGES_INITIAL;
    while (capacity < needed) {
        if (capacity > CHAT_MAX_MESSAGES / 2) { capacity = CHAT_MAX_MESSAGES; break; }
        capacity *= 2;
    }
    ChatMessage *grown = realloc(c->messages, capacity * sizeof *grown);
    if (!grown) return false;
    c->messages = grown;
    c->message_capacity = capacity;
    return true;
}

bool chat_reserve_messages(ChatConversation *c, size_t count) {
    return c && chat_ensure_capacity(c, count);
}

/* Releases everything a Chat owns: every live message's overflow allocations,
   then each conversation's message array itself, leaving all three dynamic
   fields reset so a later overwrite cannot double-free. */
void chat_dispose(Chat *chat) {
    if (!chat) return;
    for (int i=0;i<chat->conversation_count;i++) {
        ChatConversation *c=&chat->conversations[i];
        for (size_t j=0;j<c->message_count;j++)
            chat_message_dispose(&c->messages[j]);
        free(c->messages);
        c->messages=NULL;
        c->message_count=0;
        c->message_capacity=0;
    }
}

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
    if (chat->next_id >= CHAT_MAX_ID) return -1;
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

/* Stable-identity lookup for the sidebar and search navigation: array
   positions shift on deletion, ids never do. */
int chat_index_of_id(const Chat *chat, uint64_t id) {
    if (!chat || !id) return -1;
    for (int i = 0; i < chat->conversation_count; i++)
        if (chat->conversations[i].id == id) return i;
    return -1;
}

/* Stable message identity inside one conversation: the transcript anchor
   resolves its saved (conversation, message) pair through this after records
   were invalidated, before any record is prepared again. */
int chat_message_index_by_id(const Chat *chat, int conversation,
    uint64_t id) {
    if (!chat || !id || conversation < 0 ||
        conversation >= chat->conversation_count) return -1;
    const ChatConversation *c = &chat->conversations[conversation];
    for (size_t i = 0; i < c->message_count; i++)
        if (c->messages[i].id == id) return (int)i;
    return -1;
}

const ChatConversation *chat_active(const Chat *chat) {
    if (chat->active < 0 || chat->active >= chat->conversation_count) return NULL;
    return &chat->conversations[chat->active];
}

int chat_remaining(const Chat *chat) {
    const ChatConversation *conversation = chat_active(chat);
    if (!conversation || conversation->message_count >= CHAT_MAX_MESSAGES)
        return 0;
    return CHAT_MAX_MESSAGES - (int)conversation->message_count;
}

/* Transactional append. Phase 1 reserves capacity for one more live message;
   Phase 2 builds the message entirely inside the still-scratch slot at
   message_count; Phase 3 commits id, timestamps, count, title. A failure at
   any point returns -1 with the conversation semantically unchanged: no live
   message, message_count, next_id, modified_at or title is touched. If a
   capacity growth succeeded but construction then failed, the retained
   (larger) capacity is harmless: the conversation simply holds unused backing
   storage, and every allocation invariant still holds. */
int chat_append_at(Chat *chat, int conversation_index, ChatRole role,
    const wchar_t *text) {
    if (conversation_index < 0 ||
        conversation_index >= chat->conversation_count) return -1;
    /* Every appended message consumes a stable id; the persisted counter
       ceiling is a hard limit like the message cap, and exhaustion must fail
       before any observable mutation. */
    if (chat->next_id >= CHAT_MAX_ID) return -1;
    ChatConversation *conversation = &chat->conversations[conversation_index];
    if (conversation->message_count >= CHAT_MAX_MESSAGES) return -1;
    /* Phase 1: ensure capacity before any observable mutation. */
    if (!chat_ensure_capacity(conversation, conversation->message_count + 1))
        return -1;
    /* Phase 2: build in the scratch slot; no observable state changes yet. */
    ChatMessage *message = &conversation->messages[conversation->message_count];
    memset(message, 0, sizeof *message);
    chat_generation_init(&message->generation);
    message->role = role;
    if (!chat_message_set_text(message, text)) {
        chat_message_dispose(message);
        memset(message, 0, sizeof *message);
        return -1;
    }
    /* Phase 3: commit — only non-failing operations from here on. */
    message->created_at = message->modified_at = chat_now();
    /* Stable message id, drawn from the same persisted counter as
       conversation ids and saved with the message. */
    message->id = ++chat->next_id;
    int index = (int)conversation->message_count++;
    conversation->modified_at = message->modified_at;
    if (role == CHAT_ROLE_USER) {
        /* Name the conversation after its first user message. */
        bool first = true;
        for (int i = 0; i < index; i++)
            if (conversation->messages[i].role == CHAT_ROLE_USER) first = false;
        if (first && !conversation->renamed && chat_message_text(message)[0]) {
            wchar_t derived[CHAT_TITLE_TEXT];
            first_line(derived, CHAT_TITLE_TEXT, chat_message_text(message), 40);
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


Chat *chat_snapshot(const Chat *chat) {
    if (!chat) return NULL;
    Chat *copy=(Chat *)malloc(sizeof *copy);
    if (!copy) return NULL;
    *copy=*chat;
    /* Detach every conversation's dynamic fields before any fallible step:
       from here the copy owns nothing, so a failure disposes only what this
       function allocated and can never reach the source's pointers. */
    for (int i=0;i<chat->conversation_count;i++) {
        copy->conversations[i].messages=NULL;
        copy->conversations[i].message_count=0;
        copy->conversations[i].message_capacity=0;
    }
    for (int i=0;i<chat->conversation_count;i++) {
        const ChatConversation *source=&chat->conversations[i];
        ChatConversation *destination=&copy->conversations[i];
        /* The snapshot carries exactly the live messages: allocate the exact
           count rather than reusing the growth policy, so a snapshot never
           reserves spare slots for a state that is already frozen. */
        if (source->message_count) {
            destination->messages=(ChatMessage *)malloc(
                source->message_count*sizeof *destination->messages);
            if (!destination->messages) {
                chat_dispose(copy); free(copy); return NULL;
            }
            destination->message_capacity=source->message_count;
        }
        for (size_t j=0;j<source->message_count;j++) {
            const ChatMessage *from=&source->messages[j];
            ChatMessage *to=&destination->messages[j];
            /* The struct copy would alias the source's overflow pointers;
               detach them before the slot becomes live, so a failed overflow
               copy disposes exactly the state this copy built. */
            *to=*from;
            to->text_overflow=to->reasoning_overflow=NULL;
            to->text_capacity=to->reasoning_capacity=0;
            destination->message_count=j+1;
            if (from->text_overflow) {
                size_t bytes=(from->text_length+1)*sizeof(wchar_t);
                to->text_overflow=(wchar_t *)malloc(bytes);
                if (!to->text_overflow) {
                    chat_dispose(copy); free(copy); return NULL;
                }
                memcpy(to->text_overflow,from->text_overflow,bytes);
                to->text_capacity=from->text_length+1;
            }
            if (from->reasoning_overflow) {
                size_t bytes=(from->reasoning_length+1)*sizeof(wchar_t);
                to->reasoning_overflow=(wchar_t *)malloc(bytes);
                if (!to->reasoning_overflow) {
                    chat_dispose(copy); free(copy); return NULL;
                }
                memcpy(to->reasoning_overflow,from->reasoning_overflow,bytes);
                to->reasoning_capacity=from->reasoning_length+1;
            }
        }
    }
    return copy;
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

const wchar_t *chat_backend_name(ChatBackend backend) {
    return backend == CHAT_BACKEND_OLLAMA ? L"Ollama" : L"OpenRouter";
}

const wchar_t *chat_active_model(const Chat *chat) {
    if (!chat) return L"";
    return chat->backend == CHAT_BACKEND_OLLAMA ? chat->ollama_model : chat->model;
}

void chat_remember_model(Chat *chat) {
    const wchar_t *model = chat_active_model(chat);
    ChatBackend backend = chat->backend;
    int found = chat->model_history_count;
    for (int i = 0; i < chat->model_history_count; i++)
        if (chat->model_history_backend[i] == backend &&
            !wcscmp(chat->model_history[i], model)) { found = i; break; }
    if (found == CHAT_MODEL_HISTORY) --found;
    for (int i = found; i > 0; --i) {
        wcscpy(chat->model_history[i], chat->model_history[i - 1]);
        chat->model_history_backend[i] = chat->model_history_backend[i - 1];
    }
    wcsncpy(chat->model_history[0], model, CHAT_MODEL_TEXT - 1);
    chat->model_history[0][CHAT_MODEL_TEXT - 1] = 0;
    chat->model_history_backend[0] = backend;
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
    ChatConversation *removed=&chat->conversations[index];
    /* Release the removed conversation's storage before the move: the
       memmove transfers ownership of the surviving conversations' allocations
       over this slot, and the vacated tail slot is zeroed so the moved
       pointers can never be freed twice. */
    for (size_t i=0;i<removed->message_count;i++)
        chat_message_dispose(&removed->messages[i]);
    free(removed->messages);
    removed->messages=NULL;
    removed->message_count=0;
    removed->message_capacity=0;
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
    chat_dispose(chat);
    memset(chat->conversations, 0, sizeof chat->conversations);
    chat->conversation_count = 0;
    chat->active = -1;
    return chat_new_conversation(chat) >= 0;
}

/* Clear wipes the conversation's message content, so it releases the dynamic
   message storage entirely, restoring the never-allocated NULL/0/0 shape
   rather than opportunistically shrinking mid-use allocations. */
void chat_clear(Chat *chat) {
    if (!chat_active(chat)) return;
    ChatConversation *c = &chat->conversations[chat->active];
    for (size_t i=0;i<c->message_count;i++) chat_message_dispose(&c->messages[i]);
    free(c->messages);
    c->messages = NULL;
    c->message_count = 0;
    c->message_capacity = 0;
    c->draft[0] = 0;
    c->modified_at = chat_now();
}

int chat_latest_user(const ChatConversation *c) {
    if (c) for (size_t i = c->message_count; i-- > 0;)
        if (c->messages[i].role == CHAT_ROLE_USER) return (int)i;
    return -1;
}

bool chat_history_message(const ChatMessage *m) {
    return m->role != CHAT_ROLE_ERROR &&
        (m->role != CHAT_ROLE_ASSISTANT || m->generation.state == CHAT_GENERATION_COMPLETE);
}

/* Replacement/send preflight: validates the requested mode, then computes the
   final post-operation live message count — counting only messages the
   completed operation keeps or adds, never messages it will trim or replace —
    and reserves capacity for that shape before any destructive mutation. A
    conversation at the message cap can still replace its tail (final count
    unchanged), and an operation that would genuinely need more live messages
    than the cap allows fails before anything is modified. After the
    reservation succeeds, no
   remaining step can fail on array growth, so a -1 return from this function
   always leaves the conversation exactly as it was. */
int chat_begin_response(Chat *chat, ChatSendMode mode, const wchar_t *prompt) {
    if (!chat_active(chat)) return -1;
    if (mode != CHAT_SEND && mode != CHAT_RETRY &&
        mode != CHAT_REGENERATE && mode != CHAT_EDIT_RESEND) return -1;
    ChatConversation *c = &chat->conversations[chat->active];
    for (size_t i = 0; i < c->message_count; i++)
        if (c->messages[i].generation.state == CHAT_GENERATION_RUNNING) return -1;
    int user = chat_latest_user(c);
    size_t final_count;
    if (mode == CHAT_SEND) {
        if (!prompt || !prompt[0] || wcslen(prompt) >= CHAT_COMPOSER_TEXT ||
            c->message_count + 2 > CHAT_MAX_MESSAGES) return -1;
        final_count = c->message_count + 2; /* user turn + fresh response */
    } else {
        if (user < 0 || (size_t)user + 2 > CHAT_MAX_MESSAGES) return -1;
        if (mode == CHAT_RETRY && c->message_count > (size_t)user + 1 &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_FAILED &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_CANCELLED &&
            c->messages[user + 1].generation.state != CHAT_GENERATION_INTERRUPTED) return -1;
        if (mode == CHAT_EDIT_RESEND) {
            if (!prompt || !prompt[0] || wcslen(prompt) >= CHAT_COMPOSER_TEXT) return -1;
        }
        /* Kept messages [0..user] plus the fresh response. */
        final_count = (size_t)user + 2;
    }
    /* Capacity reservation happens before the edit, trim or append: from here
       the appends cannot fail on growth. */
    if (!chat_ensure_capacity(c, final_count)) return -1;
    if (mode != CHAT_SEND) {
        if (mode == CHAT_EDIT_RESEND) {
            if (!chat_message_set_text(&c->messages[user],prompt)) return -1;
            c->messages[user].modified_at = chat_now();
        }
        for (size_t i=(size_t)user+1;i<c->message_count;i++)
            chat_message_dispose(&c->messages[i]);
        c->message_count = (size_t)user + 1;
    }
    int user_index = mode == CHAT_SEND
        ? chat_append(chat, CHAT_ROLE_USER, prompt) : user;
    if (user_index < 0) return -1;
    int index = chat_append(chat, CHAT_ROLE_ASSISTANT, L"");
    if (index<0) return -1;
    ChatGeneration *g = &c->messages[index].generation;
    g->state = CHAT_GENERATION_RUNNING;
    g->backend = chat->backend;
    g->started_at = chat_now();
    wcsncpy(g->requested_model, chat_active_model(chat), CHAT_MODEL_TEXT - 1);
    g->requested_model[CHAT_MODEL_TEXT - 1] = 0;
    chat_remember_model(chat);
    return index;
}

