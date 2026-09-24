#include "chat/core/chat.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* The one Win32 use in this module: CompareStringOrdinal provides the
   locale-independent ordinal case-insensitive comparison that profile-name
   uniqueness and the future profile picker share. No window, graphics or
   UI dependency comes with it. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
        /* Copy into the inline residue before releasing overflow: `text` may
           borrow the message's own overflow (aliased set/append), and freeing
           first would make the copy read freed storage. */
        if (needed) wmemcpy(inline_text,text,needed);
        inline_text[needed]=0;
        free(*overflow); *overflow=NULL; *capacity=0;
        *length=needed;
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

/* ---- content parts -----------------------------------------------------
   Transactional rule for every mutator: stage every new allocation (input
   copy, TEXT payload, replacement items array) before any owned pointer is
   replaced. A failure at any stage frees only the staged locals and leaves
   the message byte-identical to its prior state. */

static void dispose_parts(ChatContent *c) {
    if (!c || !c->items) return;
    for (size_t i = 0; i < c->count; i++)
        if (c->items[i].kind == CHAT_PART_TEXT)
            free(c->items[i].u.text.data);
    free(c->items);
    c->items = NULL;
    c->count = 0;
    c->capacity = 0;
}

/* Stages an owned NUL-terminated copy of `text` (same surrogate trim as
   set_message_value). Empty input stages nothing (all NULL/0). */
static bool stage_text_value(const wchar_t *text, wchar_t **out_data,
    size_t *out_length, size_t *out_capacity) {
    *out_data = NULL;
    *out_length = 0;
    *out_capacity = 0;
    if (!text) text = L"";
    size_t needed = wcslen(text);
    if (needed && text[needed-1] >= 0xd800 && text[needed-1] <= 0xdbff)
        --needed;
    if (!needed) return true;
    if (needed == SIZE_MAX / sizeof(wchar_t)) return false;
    wchar_t *data = (wchar_t *)malloc((needed + 1) * sizeof(wchar_t));
    if (!data) return false;
    wmemcpy(data, text, needed);
    data[needed] = 0;
    *out_data = data;
    *out_length = needed;
    *out_capacity = needed + 1;
    return true;
}

static void stage_text_dispose(wchar_t **data, size_t *length,
    size_t *capacity) {
    free(*data);
    *data = NULL;
    *length = 0;
    *capacity = 0;
}

static bool parts_next_capacity(size_t needed, size_t *out) {
    if (needed > CHAT_MAX_PARTS) return false;
    size_t cap = 4;
    while (cap < needed) cap *= 2;
    if (cap > CHAT_MAX_PARTS) cap = CHAT_MAX_PARTS;
    *out = cap;
    return true;
}

static bool message_has_text_part(const ChatMessage *m) {
    return m->parts.items && m->parts.count &&
        m->parts.items[0].kind == CHAT_PART_TEXT;
}

static bool set_text_fast_path(ChatMessage *m, const wchar_t *text) {
    const wchar_t *previous = message_value(m->text, m->text_overflow);
    bool unchanged = !wcscmp(previous, text);
    if (!set_message_value(m->text, CHAT_MESSAGE_INLINE, &m->text_overflow,
        &m->text_length, &m->text_capacity, text)) return false;
    if (!unchanged) { ++m->revision; ++m->body_revision; }
    return true;
}

/* Promoted set_text: stages an input copy first so `text` may borrow the
   projection, overflow, or a TEXT part payload. Then stages the new TEXT
   payload and, when inserting a leading TEXT into an image-only message, a
   replacement items array — all before set_message_value or any free of
   owned pointers. */
static bool set_text_promoted(ChatMessage *m, const wchar_t *text) {
    const wchar_t *previous = message_value(m->text, m->text_overflow);
    bool unchanged = !wcscmp(previous, text);

    wchar_t *input = NULL;
    size_t input_length = 0, input_capacity = 0;
    if (text[0]) {
        if (!stage_text_value(text, &input, &input_length, &input_capacity))
            return false;
        /* A lone high surrogate stages as empty (input stays NULL). Read the
           staged empty as L"" rather than dereferencing NULL. */
        text = input ? input : L"";
    }

    bool has_text_part = message_has_text_part(m);

    if (!text[0]) {
        stage_text_dispose(&input, &input_length, &input_capacity);
        if (!set_message_value(m->text, CHAT_MESSAGE_INLINE,
                &m->text_overflow, &m->text_length, &m->text_capacity, L""))
            return false;
        if (has_text_part) {
            free(m->parts.items[0].u.text.data);
            memmove(&m->parts.items[0], &m->parts.items[1],
                (m->parts.count - 1) * sizeof(ChatPart));
            m->parts.count--;
            if (m->parts.count == 0) {
                free(m->parts.items);
                m->parts.items = NULL;
                m->parts.capacity = 0;
            }
        }
        if (!unchanged) { ++m->revision; ++m->body_revision; }
        return true;
    }

    wchar_t *text_data = NULL;
    size_t text_length = 0, text_capacity = 0;
    if (!stage_text_value(text, &text_data, &text_length, &text_capacity)) {
        stage_text_dispose(&input, &input_length, &input_capacity);
        return false;
    }

    ChatPart *new_items = NULL;
    size_t new_capacity = 0;
    if (!has_text_part) {
        if (m->parts.count >= CHAT_MAX_PARTS ||
                !parts_next_capacity(m->parts.count + 1, &new_capacity)) {
            stage_text_dispose(&text_data, &text_length, &text_capacity);
            stage_text_dispose(&input, &input_length, &input_capacity);
            return false;
        }
        new_items = (ChatPart *)malloc(new_capacity * sizeof *new_items);
        if (!new_items) {
            stage_text_dispose(&text_data, &text_length, &text_capacity);
            stage_text_dispose(&input, &input_length, &input_capacity);
            return false;
        }
        if (m->parts.count)
            memcpy(new_items + 1, m->parts.items,
                m->parts.count * sizeof *new_items);
        new_items[0].kind = CHAT_PART_TEXT;
        new_items[0].flags = 0;
        new_items[0].reserved = 0;
        new_items[0].u.text.data = text_data;
        new_items[0].u.text.length = text_length;
        new_items[0].u.text.capacity = text_capacity;
    }

    if (!set_message_value(m->text, CHAT_MESSAGE_INLINE, &m->text_overflow,
            &m->text_length, &m->text_capacity, text)) {
        stage_text_dispose(&text_data, &text_length, &text_capacity);
        free(new_items);
        stage_text_dispose(&input, &input_length, &input_capacity);
        return false;
    }

    if (has_text_part) {
        free(m->parts.items[0].u.text.data);
        m->parts.items[0].u.text.data = text_data;
        m->parts.items[0].u.text.length = text_length;
        m->parts.items[0].u.text.capacity = text_capacity;
    } else {
        free(m->parts.items);
        m->parts.items = new_items;
        m->parts.count += 1;
        m->parts.capacity = new_capacity;
    }
    stage_text_dispose(&input, &input_length, &input_capacity);
    if (!unchanged) { ++m->revision; ++m->body_revision; }
    return true;
}

static bool append_text_fast_path(ChatMessage *m, const wchar_t *text) {
    if (!append_message_value(m->text, CHAT_MESSAGE_INLINE, &m->text_overflow,
        &m->text_length, &m->text_capacity, text)) return false;
    if (text && text[0]) { ++m->revision; ++m->body_revision; }
    return true;
}

/* Promoted append: stage the combined string first (so `text` may borrow the
   projection or a TEXT part), then run the promoted set path. */
static bool append_text_promoted(ChatMessage *m, const wchar_t *text) {
    const wchar_t *current = message_value(m->text, m->text_overflow);
    size_t current_length = wcslen(current);
    size_t added = wcslen(text);
    if (added > SIZE_MAX - 1 - current_length) return false;
    wchar_t *combined = (wchar_t *)malloc(
        (current_length + added + 1) * sizeof(wchar_t));
    if (!combined) return false;
    if (current_length) wmemcpy(combined, current, current_length);
    wmemcpy(combined + current_length, text, added);
    combined[current_length + added] = 0;
    bool ok = set_text_promoted(m, combined);
    free(combined);
    return ok;
}

size_t chat_message_part_count(const ChatMessage *m) {
    if (!m) return 0;
    if (m->parts.items) return m->parts.count;
    return message_value(m->text, m->text_overflow)[0] ? 1u : 0u;
}

bool chat_message_part_at(const ChatMessage *m, size_t index,
    ChatPartView *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!m) return false;
    if (m->parts.items) {
        if (index >= m->parts.count) return false;
        const ChatPart *part = &m->parts.items[index];
        out->kind = part->kind;
        out->flags = part->flags;
        if (part->kind == CHAT_PART_TEXT) {
            out->u.text.data = part->u.text.data;
            out->u.text.length = part->u.text.length;
        } else {
            out->u.image = part->u.image;
        }
        return true;
    }
    if (index > 0) return false;
    const wchar_t *text = message_value(m->text, m->text_overflow);
    if (!text[0]) return false;
    out->kind = CHAT_PART_TEXT;
    out->flags = 0;
    out->u.text.data = text;
    out->u.text.length = m->text_length ? m->text_length : wcslen(text);
    return true;
}

bool chat_message_has_images(const ChatMessage *m) {
    if (!m || !m->parts.items) return false;
    for (size_t i = 0; i < m->parts.count; i++)
        if (m->parts.items[i].kind == CHAT_PART_IMAGE) return true;
    return false;
}

bool chat_message_add_image(ChatMessage *m, const ChatImagePart *image,
    uint8_t flags) {
    if (!m || !image) return false;
    if ((flags & CHAT_PART_FLAG_MASK) != flags) return false;

    ChatPart part;
    memset(&part, 0, sizeof part);
    part.kind = (uint8_t)CHAT_PART_IMAGE;
    part.flags = flags;
    part.u.image = *image;

    if (!m->parts.items) {
        const wchar_t *text = message_value(m->text, m->text_overflow);
        bool has_text = text[0] != 0;
        size_t new_count = has_text ? 2u : 1u;
        size_t capacity = 0;
        if (!parts_next_capacity(new_count, &capacity)) return false;

        wchar_t *text_data = NULL;
        size_t text_length = 0, text_capacity = 0;
        if (has_text && !stage_text_value(text, &text_data, &text_length,
                &text_capacity))
            return false;
        ChatPart *items = (ChatPart *)malloc(capacity * sizeof *items);
        if (!items) {
            stage_text_dispose(&text_data, &text_length, &text_capacity);
            return false;
        }
        size_t at = 0;
        if (has_text) {
            items[0].kind = (uint8_t)CHAT_PART_TEXT;
            items[0].flags = 0;
            items[0].reserved = 0;
            items[0].u.text.data = text_data;
            items[0].u.text.length = text_length;
            items[0].u.text.capacity = text_capacity;
            at = 1;
        }
        items[at] = part;
        m->parts.items = items;
        m->parts.count = new_count;
        m->parts.capacity = capacity;
        ++m->revision;
        ++m->body_revision;
        return true;
    }

    if (m->parts.count >= CHAT_MAX_PARTS) return false;
    if (m->parts.count == m->parts.capacity) {
        size_t capacity = m->parts.capacity * 2;
        if (capacity > CHAT_MAX_PARTS) capacity = CHAT_MAX_PARTS;
        ChatPart *items = (ChatPart *)malloc(capacity * sizeof *items);
        if (!items) return false;
        memcpy(items, m->parts.items, m->parts.count * sizeof *items);
        free(m->parts.items);
        m->parts.items = items;
        m->parts.capacity = capacity;
    }
    m->parts.items[m->parts.count++] = part;
    ++m->revision;
    ++m->body_revision;
    return true;
}

bool chat_message_remove_part(ChatMessage *m, size_t index) {
    if (!m || !m->parts.items || index >= m->parts.count) return false;
    if (m->parts.items[index].kind == CHAT_PART_TEXT) return false;
    memmove(&m->parts.items[index], &m->parts.items[index + 1],
        (m->parts.count - index - 1) * sizeof(ChatPart));
    m->parts.count--;
    if (m->parts.count == 0) {
        free(m->parts.items);
        m->parts.items = NULL;
        m->parts.capacity = 0;
    } else if (m->parts.count == 1 &&
            m->parts.items[0].kind == CHAT_PART_TEXT) {
        free(m->parts.items[0].u.text.data);
        free(m->parts.items);
        m->parts.items = NULL;
        m->parts.count = 0;
        m->parts.capacity = 0;
    }
    ++m->revision;
    ++m->body_revision;
    return true;
}

bool chat_message_move_part(ChatMessage *m, size_t from, size_t to) {
    if (!m || !m->parts.items) return false;
    if (from >= m->parts.count || to >= m->parts.count) return false;
    /* Image slots only — reject TEXT (index 0 when present) before the
       from==to no-op so move_part(0,0) cannot succeed on a TEXT slot. */
    size_t first_image = message_has_text_part(m) ? 1u : 0u;
    if (from < first_image || to < first_image) return false;
    if (from == to) return true;
    ChatPart moving = m->parts.items[from];
    if (from < to) {
        memmove(&m->parts.items[from], &m->parts.items[from + 1],
            (to - from) * sizeof(ChatPart));
    } else {
        memmove(&m->parts.items[to + 1], &m->parts.items[to],
            (from - to) * sizeof(ChatPart));
    }
    m->parts.items[to] = moving;
    ++m->revision;
    ++m->body_revision;
    return true;
}

void chat_message_clear_parts(ChatMessage *m) {
    if (!m || !m->parts.items) return;
    dispose_parts(&m->parts);
    ++m->revision;
    ++m->body_revision;
}

bool chat_message_set_text(ChatMessage *m,const wchar_t *text) {
    if (!m) return false;
    if (!text) text=L"";
    if (m->parts.items) return set_text_promoted(m, text);
    return set_text_fast_path(m, text);
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
    if (m->parts.items) {
        if (!text || !text[0]) return true;
        return append_text_promoted(m, text);
    }
    return append_text_fast_path(m, text);
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
    dispose_parts(&m->parts);
}

/* Owned customization text. The set/clone pair is transactional exactly like
   the message setters: the new allocation is built and filled before the old
   one is released, so a failure never destroys the previous content. */
bool chat_text_set(ChatText *t, const wchar_t *value) {
    if (!t) return false;
    if (!value) value=L"";
    size_t needed=wcslen(value);
    /* Never store a dangling high surrogate. */
    if (needed && value[needed-1]>=0xd800 && value[needed-1]<=0xdbff) --needed;
    if (!needed) { chat_text_dispose(t); return true; }  /* empty == unset */
    if (needed==SIZE_MAX/sizeof(wchar_t)) return false;
    wchar_t *next=(wchar_t *)malloc((needed+1)*sizeof *next);
    if (!next) return false;
    wmemcpy(next,value,needed);
    next[needed]=0;
    free(t->data);
    t->data=next; t->length=needed; t->capacity=needed+1;
    return true;
}

bool chat_text_clone(ChatText *destination, const ChatText *source) {
    if (!destination || !source) return false;
    wchar_t *copy=NULL;
    if (source->data) {
        copy=(wchar_t *)malloc((source->length+1)*sizeof *copy);
        if (!copy) return false;
        wmemcpy(copy,source->data,source->length+1);
    }
    free(destination->data);
    destination->data=copy;
    destination->length=source->length;
    destination->capacity=copy ? source->length+1 : 0;
    return true;
}

void chat_text_dispose(ChatText *text) {
    if (!text) return;
    free(text->data);
    text->data=NULL; text->length=0; text->capacity=0;
}

const wchar_t *chat_text_value(const ChatText *text) {
    return text && text->data ? text->data : L"";
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

/* Releases one conversation's dynamic storage and owned override text,
   leaving the fields reset so a later overwrite cannot double-free. Shared
   by chat_dispose (everything goes) and chat_delete_all (conversations go,
   the profile library survives). */
static void dispose_conversation(ChatConversation *c) {
    chat_text_dispose(&c->system_prompt);
    c->system_prompt_present = false;
    for (size_t j=0;j<c->message_count;j++)
        chat_message_dispose(&c->messages[j]);
    free(c->messages);
    c->messages=NULL;
    c->message_count=0;
    c->message_capacity=0;
}

/* Releases everything a Chat owns: every conversation's message overflow
   allocations and owned prompt override, then each conversation's message
   array itself, then the profile library's owned prompts, leaving every
   dynamic field reset. */
void chat_dispose(Chat *chat) {
    if (!chat) return;
    for (int i=0;i<chat->conversation_count;i++)
        dispose_conversation(&chat->conversations[i]);
    for (int i=0;i<chat->profile_count;i++)
        chat_text_dispose(&chat->profiles[i].prompt);
    chat->profile_count=0;
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
        function allocated and can never reach the source's pointers. This
        includes every owned ChatText: the struct copy aliased the profile
        prompts and the per-conversation prompt overrides. */
    for (int i=0;i<chat->profile_count;i++) {
        copy->profiles[i].prompt.data=NULL;
        copy->profiles[i].prompt.length=0;
        copy->profiles[i].prompt.capacity=0;
    }
    for (int i=0;i<chat->conversation_count;i++) {
        copy->conversations[i].messages=NULL;
        copy->conversations[i].message_count=0;
        copy->conversations[i].message_capacity=0;
        copy->conversations[i].system_prompt.data=NULL;
        copy->conversations[i].system_prompt.length=0;
        copy->conversations[i].system_prompt.capacity=0;
    }
    for (int i=0;i<chat->profile_count;i++) {
        if (!chat_text_clone(&copy->profiles[i].prompt,
                &chat->profiles[i].prompt)) {
            chat_dispose(copy); free(copy); return NULL;
        }
    }
    for (int i=0;i<chat->conversation_count;i++) {
        const ChatConversation *source=&chat->conversations[i];
        ChatConversation *destination=&copy->conversations[i];
        if (!chat_text_clone(&destination->system_prompt,
                &source->system_prompt)) {
            chat_dispose(copy); free(copy); return NULL;
        }
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
            /* The struct copy would alias the source's overflow pointers and
               parts array; detach them before the slot becomes live, so a
               failed overflow or parts copy disposes exactly the state this
               copy built. Parts count/capacity stay 0 until each entry is
               individually disposable — never expose uninitialized slots to
               the failure-path dispose. */
            *to=*from;
            to->text_overflow=to->reasoning_overflow=NULL;
            to->text_capacity=to->reasoning_capacity=0;
            to->parts.items=NULL;
            to->parts.count=0;
            to->parts.capacity=0;
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
            if (from->parts.items) {
                to->parts.items=(ChatPart *)malloc(
                    from->parts.count*sizeof *to->parts.items);
                if (!to->parts.items) {
                    chat_dispose(copy); free(copy); return NULL;
                }
                to->parts.capacity=from->parts.count;
                for (size_t pi=0;pi<from->parts.count;pi++) {
                    ChatPart slot=from->parts.items[pi];
                    if (slot.kind==CHAT_PART_TEXT) {
                        slot.u.text.data=NULL;
                        slot.u.text.length=0;
                        slot.u.text.capacity=0;
                    }
                    to->parts.items[pi]=slot;
                    to->parts.count=pi+1;
                    if (from->parts.items[pi].kind==CHAT_PART_TEXT &&
                            from->parts.items[pi].u.text.data) {
                        size_t units=from->parts.items[pi].u.text.length+1;
                        to->parts.items[pi].u.text.data=
                            (wchar_t *)malloc(units*sizeof(wchar_t));
                        if (!to->parts.items[pi].u.text.data) {
                            chat_dispose(copy); free(copy); return NULL;
                        }
                        memcpy(to->parts.items[pi].u.text.data,
                            from->parts.items[pi].u.text.data,
                            units*sizeof(wchar_t));
                        to->parts.items[pi].u.text.length=
                            from->parts.items[pi].u.text.length;
                        to->parts.items[pi].u.text.capacity=units;
                    }
                }
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

/* The global remembered slot of one backend. */
static const wchar_t *global_model_slot(const Chat *chat, ChatBackend backend) {
    return backend == CHAT_BACKEND_OLLAMA ? chat->ollama_model : chat->model;
}

const wchar_t *chat_effective_model_for_backend(const Chat *chat,
    const ChatConversation *c, ChatBackend backend) {
    if (!chat) return L"";
    const wchar_t *global = global_model_slot(chat, backend);
    if (!c) return global;
    const wchar_t *override = backend == CHAT_BACKEND_OLLAMA
        ? c->ollama_model : c->model;
    return override[0] ? override : global;
}

const wchar_t *chat_effective_model(const Chat *chat,
    const ChatConversation *c) {
    return chat ? chat_effective_model_for_backend(chat, c, chat->backend)
                : L"";
}

const wchar_t *chat_effective_system_prompt(const Chat *chat,
    const ChatConversation *c) {
    if (!chat) return L"";
    if (c) {
        if (c->system_prompt.data) return c->system_prompt.data;
        if (c->system_prompt_present) return L"";
    }
    return chat->system_prompt;
}

bool chat_effective_reasoning(const Chat *chat, const ChatConversation *c) {
    (void)chat;
    return c ? !c->reasoning_disabled : true;
}

void chat_remember_model(Chat *chat) {
    /* The remembered history records the model a request actually uses,
        which is the active conversation's effective model (an override or
        the global slot), tagged with the backend that sent it. */
    const wchar_t *model = chat_effective_model(chat, chat_active(chat));
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

/* Profile-name uniqueness uses locale-independent ordinal
   case-insensitive comparison, the same folding the model catalog filter
   and the future profile picker use. */
static bool profile_name_taken(const Chat *chat, const wchar_t *name,
    int except_index) {
    for (int i = 0; i < chat->profile_count; i++) {
        if (i == except_index) continue;
        if (CompareStringOrdinal(chat->profiles[i].name, -1, name, -1,
                TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

static bool profile_arguments_valid(const wchar_t *name,
    const wchar_t *prompt) {
    if (!name || !name[0] || wcslen(name) >= CHAT_PROFILE_NAME_TEXT)
        return false;
    if (!prompt) prompt = L"";
    return wcslen(prompt) < CHAT_COMPOSER_TEXT;
}

int chat_profile_count(const Chat *chat) {
    return chat ? chat->profile_count : 0;
}

const ChatPromptProfile *chat_profile(const Chat *chat, int index) {
    if (!chat || index < 0 || index >= chat->profile_count) return NULL;
    return &chat->profiles[index];
}

int chat_profile_add(Chat *chat, const wchar_t *name, const wchar_t *prompt) {
    if (!chat || chat->profile_count >= CHAT_MAX_PROMPT_PROFILES) return -1;
    if (!profile_arguments_valid(name, prompt)) return -1;
    if (profile_name_taken(chat, name, -1)) return -1;
    ChatText next = {0};
    if (!chat_text_set(&next, prompt)) return -1;   /* allocation checked first */
    ChatPromptProfile *profile = &chat->profiles[chat->profile_count];
    memset(profile, 0, sizeof *profile);
    wcscpy(profile->name, name);   /* length validated above */
    profile->prompt = next;
    return chat->profile_count++;
}

bool chat_profile_set(Chat *chat, int index, const wchar_t *name,
    const wchar_t *prompt) {
    if (!chat || index < 0 || index >= chat->profile_count) return false;
    if (!profile_arguments_valid(name, prompt)) return false;
    if (profile_name_taken(chat, name, index)) return false;
    /* Both new values validate and the new prompt is allocated before
       either the existing name or the existing prompt changes. */
    ChatText next = {0};
    if (!chat_text_set(&next, prompt)) return false;
    ChatPromptProfile *profile = &chat->profiles[index];
    wcscpy(profile->name, name);
    chat_text_dispose(&profile->prompt);
    profile->prompt = next;
    return true;
}

bool chat_profile_remove(Chat *chat, int index) {
    if (!chat || index < 0 || index >= chat->profile_count) return false;
    /* The same ownership pattern as conversation deletion: dispose the
       removed profile's owned prompt, memmove the survivors (their prompt
       pointers transfer bytewise), decrement, and zero the vacated tail
       slot so nothing can be freed twice. */
    chat_text_dispose(&chat->profiles[index].prompt);
    --chat->profile_count;
    memmove(&chat->profiles[index], &chat->profiles[index + 1],
        (size_t)(chat->profile_count - index) * sizeof(ChatPromptProfile));
    memset(&chat->profiles[chat->profile_count], 0, sizeof(ChatPromptProfile));
    return true;
}

bool chat_conversation_set_system_prompt(Chat *chat, int conversation,
    const wchar_t *text) {
    if (!chat || conversation < 0 ||
        conversation >= chat->conversation_count) return false;
    if (!text) text = L"";
    if (wcslen(text) >= CHAT_COMPOSER_TEXT) return false;
    ChatConversation *c = &chat->conversations[conversation];
    /* The flag is dropped only after the text set succeeds: a fallible
        non-empty set must never turn a deliberate empty override into
        inheritance by clearing the flag before its allocation failed. The
        empty path disposes (infallible) and clears the flag together. */
    if (!chat_text_set(&c->system_prompt, text)) return false;
    c->system_prompt_present = false;
    return true;
}

bool chat_conversation_apply_system_prompt(Chat *chat, int conversation,
    const wchar_t *text) {
    if (!chat || conversation < 0 ||
        conversation >= chat->conversation_count) return false;
    if (!text) text = L"";
    if (wcslen(text) >= CHAT_COMPOSER_TEXT) return false;
    ChatConversation *c = &chat->conversations[conversation];
    if (!text[0]) {
        /* An explicitly applied empty prompt is a real override, not
            inherit: dispose the text and mark the override present-empty.
            Dispose cannot fail, so this is transactional. */
        chat_text_dispose(&c->system_prompt);
        c->system_prompt_present = true;
        return true;
    }
    /* Same ordering as the setter: the flag survives a failed allocation,
        so the explicit-empty override is never silently widened into
        inheritance. */
    if (!chat_text_set(&c->system_prompt, text)) return false;
    c->system_prompt_present = false;
    return true;
}

bool chat_conversation_set_model(Chat *chat, int conversation,
    ChatBackend backend, const wchar_t *text) {
    if (!chat || conversation < 0 ||
        conversation >= chat->conversation_count) return false;
    if (backend != CHAT_BACKEND_OPENROUTER && backend != CHAT_BACKEND_OLLAMA)
        return false;
    if (!text) text = L"";
    /* Validate before touching the slot: a too-long model leaves the
       previous override in place. */
    if (wcslen(text) >= CHAT_MODEL_TEXT) return false;
    wchar_t *slot = backend == CHAT_BACKEND_OLLAMA
        ? chat->conversations[conversation].ollama_model
        : chat->conversations[conversation].model;
    wcscpy(slot, text);   /* empty clears back to inherit */
    return true;
}

bool chat_conversation_set_reasoning(Chat *chat, int conversation,
    bool enabled) {
    if (!chat || conversation < 0 ||
        conversation >= chat->conversation_count) return false;
    chat->conversations[conversation].reasoning_disabled = !enabled;
    return true;
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
        pointers can never be freed twice. The owned prompt override is
        released here too; the surviving conversations' overrides move
        bytewise with their structs. */
    dispose_conversation(removed);
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
    /* Delete-all wipes the conversations, not the profile library: profiles
       are app-level data, so they survive and stay owned until chat_dispose. */
    for (int i=0;i<chat->conversation_count;i++)
        dispose_conversation(&chat->conversations[i]);
    memset(chat->conversations, 0, sizeof chat->conversations);
    chat->conversation_count = 0;
    chat->active = -1;
    return chat_new_conversation(chat) >= 0;
}

/* Clear wipes the conversation's message content, so it releases the dynamic
    message storage entirely, restoring the never-allocated NULL/0/0 shape
    rather than opportunistically shrinking mid-use allocations. The
    conversation's customization (title, rename flag, model and prompt
    overrides) deliberately survives: Clear empties history, it does not
    delete the conversation. */
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
    /* The audit metadata records the model this request actually resolves
        to: the conversation's override for the active backend, or the
        global slot. The host sends this same recorded value, so request
        body and metadata can never disagree. */
    wcsncpy(g->requested_model,
        chat_effective_model_for_backend(chat, c, chat->backend),
        CHAT_MODEL_TEXT - 1);
    g->requested_model[CHAT_MODEL_TEXT - 1] = 0;
    chat_remember_model(chat);
    return index;
}

