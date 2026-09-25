#include "chat/generation/context.h"
#include "chat/generation/completion_request.h"
#include <string.h>

/* The framing costs come from chat/generation/completion_request.c, the same pure module
   the transport encodes with, so the measured envelope, per-message bytes and
   separators cannot drift from the body actually sent for either backend. The
   backend and active model are resolved here exactly as the host sends them:
   OpenRouter's envelope carries its optional provider object; Ollama's carries
   stream_options.include_usage and never a provider object.
   Every message is charged as a text side (framing, array syntax, text terms)
   plus an encoded image-payload side, against the two budget remainders.
   tests/test_openrouter.c proves that the size this module reports equals the
   length of the body the real encoder produces for both backends. */

/* Saturating add: a size that cannot be represented stays at SIZE_MAX instead
   of wrapping to a small, misleading number, so a budget comparison and a
   diagnostic can only be conservative. */
static size_t add_bytes(size_t total, size_t part) {
    return total > SIZE_MAX - part ? SIZE_MAX : total + part;
}

/* The text side of one message: the string framing on the fast path, else the
   message frame plus one text-part term per TEXT part. Walks the part list
   but never consults the attachment table -- this is what lets a history
   candidate the text budget rules out be dropped with no lookup at all. */
static size_t message_text_cost(const ChatMessage *m, ChatRole role) {
    if (!m->parts.items)
        return chat_completion_text_message_bytes(role, chat_message_text(m));
    size_t total = chat_completion_message_frame_bytes(role,
        (int)m->parts.count);
    for (size_t i = 0; i < m->parts.count; i++) {
        const ChatPart *part = &m->parts.items[i];
        if (part->kind == CHAT_PART_TEXT)
            total = add_bytes(total,
                chat_completion_text_part_bytes(part->u.text.data));
    }
    return total;
}

/* The encoded image-payload side of one message: one image part term per
   IMAGE part, costed from the attachment record's stored length alone.
   Metadata-only -- chat_attachment() reads the table, never a blob. False
   when any referenced record is missing (the caller decides whether that is
   fatal). */
static bool message_attachment_cost(const Chat *chat, const ChatMessage *m,
    size_t *out) {
    size_t total = 0;
    if (m->parts.items) {
        for (size_t i = 0; i < m->parts.count; i++) {
            const ChatPart *part = &m->parts.items[i];
            if (part->kind != CHAT_PART_IMAGE) continue;
            const ChatAttachmentMeta *rec = chat_attachment(chat,
                part->u.image.attachment_id);
            if (!rec) return false;
            total = add_bytes(total, chat_completion_image_part_bytes(
                chat->backend, rec->mime, rec->bytes));
        }
    }
    *out = total;
    return true;
}

/* Carves one placed message's borrowed part run from the context's scratch
   pool. Only placed messages are resolved (JEV-A: a dangling image in dropped
   history must not block sending), and resolution is metadata-only --
   chat_attachment() reads the table, never a blob. A missing attachment
   record or a scratch bound breach fails the build; the caller resets the
   output. Fast-path messages (no parts array) get parts == NULL and consume
   no slots. */
static bool fill_parts(const Chat *chat, const ChatMessage *m,
    ChatRequestMessage *entry, ChatRequestContext *out) {
    entry->parts = NULL;
    entry->part_count = 0;
    if (!m->parts.items) return true;
    size_t count = m->parts.count;
    if (count > (size_t)(CHAT_CONTEXT_MAX_ENTRIES * CHAT_MAX_PARTS) -
            (size_t)out->part_slots_used)
        return false;
    ChatRequestPart *run = &out->part_scratch[out->part_slots_used];
    for (size_t i = 0; i < count; i++) {
        const ChatPart *part = &m->parts.items[i];
        run[i].kind = part->kind;
        run[i].flags = part->flags;
        if (part->kind == CHAT_PART_TEXT) {
            run[i].u.text = part->u.text.data;
        } else {
            const ChatAttachmentMeta *rec = chat_attachment(chat,
                part->u.image.attachment_id);
            if (!rec) return false;
            run[i].u.image.meta = &part->u.image;
            run[i].u.image.rec = rec;
            run[i].u.image.bytes = NULL;
            run[i].u.image.byte_length = rec->bytes;
        }
    }
    entry->parts = run;
    entry->part_count = (int)count;
    out->part_slots_used += (int)count;
    return true;
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

    const wchar_t *system_prompt = chat_effective_system_prompt(chat, c);
    bool has_system = system_prompt[0] != 0;
    size_t system_bytes = has_system
        ? chat_completion_text_message_bytes(CHAT_ROLE_SYSTEM, system_prompt)
        : 0;
    size_t trigger_text = message_text_cost(trigger, CHAT_ROLE_USER);
    /* The trigger's images are resolved unconditionally -- even when a text
       check below fails, because required_bytes must include them to mean
       anything. A missing trigger record is therefore fatal before any
       OVERSIZE result: reporting an oversize number built from untrustworthy
       metadata would lie about what the request needs. */
    size_t trigger_attach = 0;
    if (!message_attachment_cost(chat, trigger, &trigger_attach))
        return CHAT_CONTEXT_INVALID;
    size_t comma_for_trigger = has_system
        ? CHAT_COMPLETION_SEPARATOR_BYTES : 0;
    size_t envelope_bytes = chat_completion_envelope_bytes(chat->backend,
        chat_effective_model_for_backend(chat, c, chat->backend),
        &chat->provider_routing, chat_effective_reasoning(chat, c));
    /* The complete body the indispensable content would require: the envelope,
       the system prompt when set, and the triggering message with its images
       and the separator that precedes it. Reported by every OVERSIZE result,
       and saturated rather than wrapped, so it can only overstate, never
       understate, what the request needs. */
    out->required_attachment_bytes = trigger_attach;
    out->required_bytes = add_bytes(add_bytes(add_bytes(add_bytes(
        envelope_bytes, system_bytes), comma_for_trigger), trigger_text),
        trigger_attach);

    /* Compare by subtracting from the budget so no sum can overflow. `alone`
       is the room one message has when nothing precedes it. The text side and
       the image side draw on independent remainders: a message is kept only
       when both fit. */
    size_t alone = envelope_bytes < budget ? budget - envelope_bytes : 0;
    if (has_system && system_bytes > alone) return CHAT_CONTEXT_OVERSIZE_SYSTEM;
    if (trigger_text > alone) return CHAT_CONTEXT_OVERSIZE_USER;
    size_t remaining = alone - system_bytes;
    if (comma_for_trigger > remaining ||
        trigger_text > remaining - comma_for_trigger)
        return CHAT_CONTEXT_OVERSIZE_COMBINED;
    remaining -= comma_for_trigger + trigger_text;
    if (trigger_attach > CHAT_ATTACHMENT_BUDGET_BYTES)
        return CHAT_CONTEXT_OVERSIZE_ATTACHMENTS;
    size_t attach_remaining = CHAT_ATTACHMENT_BUDGET_BYTES - trigger_attach;

    /* History is scanned newest first, so exactly the oldest eligible messages
       are dropped and the kept ones stay contiguous. A kept message also needs
       its separator. Once one message cannot fit, the messages older than it
       are only counted: their text is never measured, so an arbitrarily large
       dropped history costs a walk of roles and states, not of its text. The
       text side is tried first and rules a message out with no attachment
       lookup at all; only a candidate the text side admits has its images
       resolved (a missing record there is fatal -- the run could not be built
       with a trustworthy byte_length). */
    int first_kept = user_index;
    int kept = 0, dropped = 0;
    bool measuring = true;
    for (int i = user_index; i-- > 0;) {
        const ChatMessage *message = &c->messages[i];
        if (!chat_history_message(message)) continue;
        if (measuring) {
            /* cost < remaining implies cost + one separator <= remaining. */
            size_t text = message_text_cost(message, message->role);
            if (text < remaining) {
                size_t attach = 0;
                if (!message_attachment_cost(chat, message, &attach)) {
                    memset(out, 0, sizeof *out);
                    out->first_kept_index = -1;
                    return CHAT_CONTEXT_INVALID;
                }
                if (attach <= attach_remaining) {
                    remaining -= text + CHAT_COMPLETION_SEPARATOR_BYTES;
                    attach_remaining -= attach;
                    first_kept = i;
                    ++kept;
                    continue;
                }
                /* The images alone overflow the image budget: the whole
                   message is dropped, never stripped to its text. */
            }
            measuring = false;
        }
        ++dropped;
    }

    int count = 0;
    if (has_system) {
        out->messages[count].role = CHAT_ROLE_SYSTEM;
        out->messages[count].text = system_prompt;
        out->messages[count].parts = NULL;
        out->messages[count].part_count = 0;
        ++count;
    }
    /* Runs are carved only for placed messages: the budget above resolved
       nothing, so a dangling attachment reference in dropped history never
       blocks the send (JEV-A). */
    bool placed = true;
    for (int i = first_kept; i < user_index && placed; i++) {
        const ChatMessage *message = &c->messages[i];
        if (!chat_history_message(message)) continue;
        out->messages[count].role = message->role;
        out->messages[count].text = chat_message_text(message);
        placed = fill_parts(chat, message, &out->messages[count], out);
        if (placed) ++count;
    }
    if (placed) {
        out->messages[count].role = CHAT_ROLE_USER;
        out->messages[count].text = chat_message_text(trigger);
        placed = fill_parts(chat, trigger, &out->messages[count], out);
        if (placed) ++count;
    }
    if (!placed) {
        /* A placed message referenced an attachment record the table does not
           hold (or the scratch bound was breached): fail closed rather than
           emit a run with an untrustworthy rec/byte_length. The output has
           been partially filled, so reset it explicitly to the same state
           every INVALID return promises. */
        memset(out, 0, sizeof *out);
        out->first_kept_index = -1;
        return CHAT_CONTEXT_INVALID;
    }

    out->count = count;
    /* The two sides are budget-remainder derivations rather than a second sum
       of the same parts: they are exactly what the selection consumed, and
       they cannot disagree with it or overflow. Their sum is the body size. */
    out->text_bytes = budget - remaining;
    out->attachment_bytes = CHAT_ATTACHMENT_BUDGET_BYTES - attach_remaining;
    out->bytes = add_bytes(out->text_bytes, out->attachment_bytes);
    out->first_kept_index = kept ? first_kept : -1;
    out->dropped_messages = dropped;
    /* required_bytes and required_attachment_bytes are OVERSIZE diagnostics
       only (the header contract): a successful build reports no requirement
       beyond what it actually built. */
    out->required_bytes = 0;
    out->required_attachment_bytes = 0;
    return CHAT_CONTEXT_OK;
}
