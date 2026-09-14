#include "context.h"
#include <string.h>

/* Framing costs of the body the client encodes (build_request in
   chat/openrouter_winhttp.c):

     {"model":<model>,"messages":[<message>,...],"stream":true,"reasoning":{"enabled":true}}

   The three raw literals total 67 bytes; the model and every message text are
   each one quoted JSON string, measured by json_encoded_string_size() so the
   escape rules stay in the encoder. A message is {"role":"<role>","content":
   <text>}: 22 raw bytes plus the role name. Every message after the first is
   preceded by one comma, so a body with `count` messages carries `count - 1`
   separators. tests/test_openrouter.c proves that the size this module reports
   equals the length of the body the real encoder produces. */
#define ENVELOPE_RAW_BYTES 67u
#define MESSAGE_RAW_BYTES 22u
#define SEPARATOR_BYTES 1u

/* Encoded bytes of a role name. Mirrors role_name() in
   chat/openrouter_winhttp.c; the encoder cross-check test catches drift. */
static size_t role_bytes(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_ASSISTANT: return 9;   /* "assistant" */
    case CHAT_ROLE_SYSTEM: return 6;      /* "system" */
    default: return 4;                    /* "user" */
    }
}

/* Saturating add: a size that cannot be represented stays at SIZE_MAX instead
   of wrapping to a small, misleading number, so a budget comparison and a
   diagnostic can only be conservative. */
static size_t add_bytes(size_t total, size_t part) {
    return total > SIZE_MAX - part ? SIZE_MAX : total + part;
}

/* Body bytes one message adds, excluding the separator before it. */
static size_t message_bytes(ChatRole role, const wchar_t *text) {
    return add_bytes(add_bytes(MESSAGE_RAW_BYTES, role_bytes(role)),
        json_encoded_string_size(text));
}

ChatContextResult chat_context_build(const Chat *chat, const ChatConversation *c,
    int user_index, size_t budget, ChatRequestContext *out) {
    if (!out) return CHAT_CONTEXT_INVALID;
    /* Zeroed before every INVALID return, so a caller can never read a stale or
       poisoned field out of a rejected build. */
    memset(out, 0, sizeof *out);
    out->first_kept_index = -1;
    if (!chat || !c) return CHAT_CONTEXT_INVALID;
    if (budget == 0) return CHAT_CONTEXT_INVALID;
    if (user_index < 0 || (size_t)user_index >= c->message_count)
        return CHAT_CONTEXT_INVALID;
    const ChatMessage *trigger = &c->messages[user_index];
    if (trigger->role != CHAT_ROLE_USER) return CHAT_CONTEXT_INVALID;

    bool has_system = chat->system_prompt[0] != 0;
    size_t system_bytes = has_system
        ? message_bytes(CHAT_ROLE_SYSTEM, chat->system_prompt) : 0;
    size_t trigger_bytes = message_bytes(CHAT_ROLE_USER, chat_message_text(trigger));
    size_t comma_for_trigger = has_system ? SEPARATOR_BYTES : 0;
    size_t envelope_bytes = add_bytes(ENVELOPE_RAW_BYTES,
        json_encoded_string_size(chat->model));
    /* The complete body the indispensable content would require: the envelope,
       the system prompt when set, and the triggering message with the
       separator that precedes it. Reported by every OVERSIZE result, and
       saturated rather than wrapped, so it can only overstate, never
       understate, what the request needs. */
    out->required_bytes = add_bytes(add_bytes(add_bytes(envelope_bytes,
        system_bytes), comma_for_trigger), trigger_bytes);

    /* Compare by subtracting from the budget so no sum can overflow. `alone`
       is the room one message has when nothing precedes it. */
    size_t alone = envelope_bytes < budget ? budget - envelope_bytes : 0;
    if (has_system && system_bytes > alone) return CHAT_CONTEXT_OVERSIZE_SYSTEM;
    if (trigger_bytes > alone) return CHAT_CONTEXT_OVERSIZE_USER;
    size_t remaining = alone - system_bytes;
    if (comma_for_trigger > remaining ||
        trigger_bytes > remaining - comma_for_trigger)
        return CHAT_CONTEXT_OVERSIZE_COMBINED;
    remaining -= comma_for_trigger + trigger_bytes;

    /* History is scanned newest first, so exactly the oldest eligible messages
       are dropped and the kept ones stay contiguous. A kept message also needs
       its separator. Once one message cannot fit, the messages older than it
       are only counted: their text is never measured, so an arbitrarily large
       dropped history costs a walk of roles and states, not of its text. */
    int first_kept = user_index;
    int kept = 0, dropped = 0;
    bool measuring = true;
    for (int i = user_index; i-- > 0;) {
        const ChatMessage *message = &c->messages[i];
        if (!chat_history_message(message)) continue;
        if (measuring) {
            /* cost < remaining implies cost + one separator <= remaining. */
            size_t cost = message_bytes(message->role, chat_message_text(message));
            if (cost < remaining) {
                remaining -= cost + SEPARATOR_BYTES;
                first_kept = i;
                ++kept;
                continue;
            }
            measuring = false;
        }
        ++dropped;
    }

    int count = 0;
    if (has_system) {
        out->messages[count].role = CHAT_ROLE_SYSTEM;
        out->messages[count].text = chat->system_prompt;
        ++count;
    }
    for (int i = first_kept; i < user_index; i++) {
        const ChatMessage *message = &c->messages[i];
        if (!chat_history_message(message)) continue;
        out->messages[count].role = message->role;
        out->messages[count].text = chat_message_text(message);
        ++count;
    }
    out->messages[count].role = CHAT_ROLE_USER;
    out->messages[count].text = chat_message_text(trigger);
    ++count;

    out->count = count;
    /* The body size is the budgeted remainder rather than a second sum of the
       same parts: it is exactly what the selection consumed, and it cannot
       disagree with it or overflow. */
    out->bytes = budget - remaining;
    out->first_kept_index = kept ? first_kept : -1;
    out->dropped_messages = dropped;
    return CHAT_CONTEXT_OK;
}
