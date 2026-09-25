#include "chat/generation/completion_request.h"
#include <string.h>

/* Framing literals. The OpenRouter suffix is byte-for-byte the historical
   encoder's literal; the Ollama suffix requests streamed usage via
   stream_options.include_usage and nothing else. Both end with the object's
   closing brace. */
#define REQUEST_PREFIX_MODEL "{\"model\":"
#define REQUEST_PREFIX_MODEL_BYTES (sizeof REQUEST_PREFIX_MODEL - 1)
#define REQUEST_PREFIX_MESSAGES ",\"messages\":["
#define REQUEST_PREFIX_MESSAGES_BYTES (sizeof REQUEST_PREFIX_MESSAGES - 1)
#define REQUEST_OPENROUTER_SUFFIX \
    "],\"stream\":true,\"reasoning\":{\"enabled\":true}"
#define REQUEST_OPENROUTER_SUFFIX_BYTES (sizeof REQUEST_OPENROUTER_SUFFIX - 1)
/* The same envelope with reasoning suppressed: the reasoning object is
   omitted entirely, so the provider applies its own default. */
#define REQUEST_OPENROUTER_PLAIN_SUFFIX "],\"stream\":true"
#define REQUEST_OPENROUTER_PLAIN_SUFFIX_BYTES \
    (sizeof REQUEST_OPENROUTER_PLAIN_SUFFIX - 1)
#define REQUEST_OLLAMA_SUFFIX \
    "],\"stream\":true,\"stream_options\":{\"include_usage\":true}"
#define REQUEST_OLLAMA_SUFFIX_BYTES (sizeof REQUEST_OLLAMA_SUFFIX - 1)
#define REQUEST_FINAL "}"
#define REQUEST_FINAL_BYTES (sizeof REQUEST_FINAL - 1)
/* {"role":"<role>","content":<text>} minus the role name and text. */
#define REQUEST_MESSAGE_RAW_BYTES 22u

/* Content-array terms. TEXT: {"type":"text","text":<string>}. Image parts
   differ per backend -- OpenRouter nests the URL under an object, Ollama's
   compatibility layer takes a bare string (verified A2). Each image literal
   carries the URL string's own quotes, so the base64 payload inside is
   written and charged unquoted; using the quoted size here would count the
   URL quotes twice. */
#define REQ_TEXT_PART_PREFIX "{\"type\":\"text\",\"text\":"
#define REQ_TEXT_PART_PREFIX_BYTES (sizeof REQ_TEXT_PART_PREFIX - 1)
#define REQ_TEXT_PART_SUFFIX "}"
#define REQ_TEXT_PART_SUFFIX_BYTES (sizeof REQ_TEXT_PART_SUFFIX - 1)
#define REQ_OR_IMG_PREFIX "{\"type\":\"image_url\",\"image_url\":{\"url\":\""
#define REQ_OR_IMG_PREFIX_BYTES (sizeof REQ_OR_IMG_PREFIX - 1)
#define REQ_OR_IMG_SUFFIX "\"}}"
#define REQ_OR_IMG_SUFFIX_BYTES (sizeof REQ_OR_IMG_SUFFIX - 1)
#define REQ_OL_IMG_PREFIX "{\"type\":\"image_url\",\"image_url\":\""
#define REQ_OL_IMG_PREFIX_BYTES (sizeof REQ_OL_IMG_PREFIX - 1)
#define REQ_OL_IMG_SUFFIX "\"}"
#define REQ_OL_IMG_SUFFIX_BYTES (sizeof REQ_OL_IMG_SUFFIX - 1)
/* The data-URL head written between the opening quote and the payload. */
#define REQ_DATA_URL_HEAD "data:"
#define REQ_DATA_URL_HEAD_BYTES (sizeof REQ_DATA_URL_HEAD - 1)
#define REQ_DATA_URL_MID ";base64,"
#define REQ_DATA_URL_MID_BYTES (sizeof REQ_DATA_URL_MID - 1)

/* Saturating add: a size that cannot be represented stays at SIZE_MAX instead
   of wrapping to a small, misleading number (the budget compares these). */
static size_t add_bytes(size_t total, size_t part) {
    return total > SIZE_MAX - part ? SIZE_MAX : total + part;
}

static const char *role_name(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_ASSISTANT: return "assistant";
    case CHAT_ROLE_SYSTEM: return "system";
    default: return "user";
    }
}

static size_t role_name_bytes(ChatRole role) {
    return strlen(role_name(role));
}

size_t chat_completion_envelope_bytes(ChatBackend backend, const wchar_t *model,
    const ChatProviderRouting *routing, bool reasoning) {
    size_t total = REQUEST_PREFIX_MODEL_BYTES +
        json_encoded_string_size(model) + REQUEST_PREFIX_MESSAGES_BYTES +
        REQUEST_FINAL_BYTES;
    if (backend == CHAT_BACKEND_OLLAMA)
        total += REQUEST_OLLAMA_SUFFIX_BYTES;
    else
        total += (reasoning ? REQUEST_OPENROUTER_SUFFIX_BYTES
            : REQUEST_OPENROUTER_PLAIN_SUFFIX_BYTES) +
            chat_provider_envelope_bytes(routing);
    return total;
}

size_t chat_completion_text_message_bytes(ChatRole role, const wchar_t *text) {
    return REQUEST_MESSAGE_RAW_BYTES + role_name_bytes(role) +
        json_encoded_string_size(text);
}

size_t chat_completion_message_frame_bytes(ChatRole role, int part_count) {
    size_t total = REQUEST_MESSAGE_RAW_BYTES + role_name_bytes(role);
    /* part_count == 0 is the string fast path (the content is one JSON
       string, costed by chat_completion_text_message_bytes). Every positive
       count is a content array: two brackets and one comma per term after
       the first -- a single image still pays the brackets. */
    if (part_count > 0)
        total = add_bytes(total, 2u + (size_t)(part_count - 1));
    return total;
}

size_t chat_completion_text_part_bytes(const wchar_t *text) {
    return add_bytes(add_bytes(REQ_TEXT_PART_PREFIX_BYTES,
        json_encoded_string_size(text)), REQ_TEXT_PART_SUFFIX_BYTES);
}

size_t chat_completion_image_part_bytes(ChatBackend backend,
    const char *mime, size_t raw_bytes) {
    size_t open = backend == CHAT_BACKEND_OLLAMA
        ? REQ_OL_IMG_PREFIX_BYTES : REQ_OR_IMG_PREFIX_BYTES;
    size_t close = backend == CHAT_BACKEND_OLLAMA
        ? REQ_OL_IMG_SUFFIX_BYTES : REQ_OR_IMG_SUFFIX_BYTES;
    size_t mime_bytes = mime ? strlen(mime) : 0;
    /* The literals carry the URL's quotes; the payload between them is
       unquoted base64. */
    return add_bytes(add_bytes(add_bytes(add_bytes(open,
        REQ_DATA_URL_HEAD_BYTES), mime_bytes), REQ_DATA_URL_MID_BYTES),
        add_bytes(json_base64_payload_size(raw_bytes), close));
}

ChatMessageCost chat_completion_message_costs(ChatBackend backend,
    ChatRole role, const ChatRequestMessage *m) {
    ChatMessageCost cost = { 0, 0 };
    if (!m || !m->parts || m->part_count <= 0) {
        cost.text_bytes = chat_completion_text_message_bytes(role,
            m ? m->text : L"");
        return cost;
    }
    size_t text = chat_completion_message_frame_bytes(role, m->part_count);
    size_t attach = 0;
    for (int i = 0; i < m->part_count; i++) {
        const ChatRequestPart *part = &m->parts[i];
        if (part->kind == CHAT_PART_TEXT)
            text = add_bytes(text, chat_completion_text_part_bytes(
                part->u.text ? part->u.text : L""));
        else
            attach = add_bytes(attach, chat_completion_image_part_bytes(
                backend, part->u.image.rec ? part->u.image.rec->mime : "",
                part->u.image.byte_length));
    }
    cost.text_bytes = text;
    cost.attachment_bytes = attach;
    return cost;
}

/* One message object: string content on the fast path, otherwise a content
   array mirroring the part run in order. An image term is rejected unless its
   attachment record resolved and its bytes can actually be written. */
static bool append_message(JsonBuf *buf, ChatBackend backend,
    const ChatRequestMessage *message) {
    const char *role = role_name(message->role);
    if (!json_buf_append_raw(buf, "{\"role\":\"", 9) ||
        !json_buf_append_raw(buf, role, strlen(role)) ||
        !json_buf_append_raw(buf, "\",\"content\":", 12)) return false;
    if (!message->parts || message->part_count <= 0)
        return json_buf_append_json_string(buf, message->text) &&
            json_buf_append_raw(buf, "}", 1);
    if (!json_buf_append_raw(buf, "[", 1)) return false;
    for (int i = 0; i < message->part_count; i++) {
        const ChatRequestPart *part = &message->parts[i];
        if (i && !json_buf_append_raw(buf, ",", 1)) return false;
        if (part->kind == CHAT_PART_TEXT) {
            const wchar_t *text = part->u.text ? part->u.text : L"";
            if (!json_buf_append_raw(buf, REQ_TEXT_PART_PREFIX,
                    REQ_TEXT_PART_PREFIX_BYTES) ||
                !json_buf_append_json_string(buf, text) ||
                !json_buf_append_raw(buf, REQ_TEXT_PART_SUFFIX,
                    REQ_TEXT_PART_SUFFIX_BYTES)) return false;
        } else {
            const ChatAttachmentMeta *rec = part->u.image.rec;
            const unsigned char *bytes = part->u.image.bytes;
            size_t length = part->u.image.byte_length;
            if (!rec) return false;
            if (!bytes && length) return false;
            const char *open = backend == CHAT_BACKEND_OLLAMA
                ? REQ_OL_IMG_PREFIX : REQ_OR_IMG_PREFIX;
            const char *close = backend == CHAT_BACKEND_OLLAMA
                ? REQ_OL_IMG_SUFFIX : REQ_OR_IMG_SUFFIX;
            if (!json_buf_append_raw(buf, open, strlen(open)) ||
                !json_buf_append_raw(buf, REQ_DATA_URL_HEAD,
                    REQ_DATA_URL_HEAD_BYTES) ||
                !json_buf_append_raw(buf, rec->mime, strlen(rec->mime)) ||
                !json_buf_append_raw(buf, REQ_DATA_URL_MID,
                    REQ_DATA_URL_MID_BYTES) ||
                !json_buf_append_base64(buf, bytes, length) ||
                !json_buf_append_raw(buf, close, strlen(close))) return false;
        }
    }
    return json_buf_append_raw(buf, "]", 1) &&
        json_buf_append_raw(buf, "}", 1);
}

bool chat_completion_request_build(JsonBuf *buf, ChatBackend backend,
    const wchar_t *model, const ChatRequestMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning) {
    if (!buf) return false;
    json_buf_init(buf, 8192);
    if (!json_buf_append_raw(buf, REQUEST_PREFIX_MODEL,
            REQUEST_PREFIX_MODEL_BYTES) ||
        !json_buf_append_json_string(buf, model) ||
        !json_buf_append_raw(buf, REQUEST_PREFIX_MESSAGES,
            REQUEST_PREFIX_MESSAGES_BYTES)) return false;
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (messages[i].role == CHAT_ROLE_ERROR) continue;
        if (!first && !json_buf_append_raw(buf, ",", 1)) return false;
        first = false;
        if (!append_message(buf, backend, &messages[i])) return false;
    }
    if (backend == CHAT_BACKEND_OLLAMA) {
        if (!json_buf_append_raw(buf, REQUEST_OLLAMA_SUFFIX,
                REQUEST_OLLAMA_SUFFIX_BYTES)) return false;
    } else {
        if (reasoning && !json_buf_append_raw(buf, REQUEST_OPENROUTER_SUFFIX,
                REQUEST_OPENROUTER_SUFFIX_BYTES)) return false;
        if (!reasoning && !json_buf_append_raw(buf,
                REQUEST_OPENROUTER_PLAIN_SUFFIX,
                REQUEST_OPENROUTER_PLAIN_SUFFIX_BYTES)) return false;
        if (!chat_provider_append(buf, routing)) return false;
    }
    return json_buf_append_raw(buf, REQUEST_FINAL, REQUEST_FINAL_BYTES);
}
