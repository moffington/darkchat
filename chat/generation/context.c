#include "chat/generation/context.h"
#include "chat/generation/completion_request.h"
#include <string.h>

/* The framing costs come from chat/generation/completion_request.c, the same pure module
   the transport encodes with, so the measured envelope, per-message bytes and
   separators cannot drift from the body actually sent for either backend. The
   backend and active model are resolved here exactly as the host sends them:
   OpenRouter's envelope carries its optional provider object; Ollama's carries
   stream_options.include_usage and never a provider object.
   tests/test_openrouter.c proves that the size this module reports equals the
   length of the body the real encoder produces for both backends. */

/* Saturating add: a size that cannot be represented stays at SIZE_MAX instead
   of wrapping to a small, misleading number, so a budget comparison and a
   diagnostic can only be conservative. */
static size_t add_bytes(size_t total, size_t part) {
    return total > SIZE_MAX - part ? SIZE_MAX : total + part;
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
        ? chat_completion_message_bytes(CHAT_ROLE_SYSTEM, chat->system_prompt)
        : 0;
    size_t trigger_bytes = chat_completion_message_bytes(CHAT_ROLE_USER,
        chat_message_text(trigger));
    size_t comma_for_trigger = has_system
        ? CHAT_COMPLETION_SEPARATOR_BYTES : 0;
    size_t envelope_bytes = chat_completion_envelope_bytes(chat->backend,
        chat_active_model(chat), &chat->provider_routing);
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
            size_t cost = chat_completion_message_bytes(message->role,
                chat_message_text(message));
            if (cost < remaining) {
                remaining -= cost + CHAT_COMPLETION_SEPARATOR_BYTES;
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
