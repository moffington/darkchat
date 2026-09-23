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

size_t chat_completion_message_bytes(ChatRole role, const wchar_t *text) {
    return REQUEST_MESSAGE_RAW_BYTES + role_name_bytes(role) +
        json_encoded_string_size(text);
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
        const char *role = role_name(messages[i].role);
        if (!json_buf_append_raw(buf, "{\"role\":\"", 9) ||
            !json_buf_append_raw(buf, role, strlen(role)) ||
            !json_buf_append_raw(buf, "\",\"content\":", 12) ||
            !json_buf_append_json_string(buf, messages[i].text) ||
            !json_buf_append_raw(buf, "}", 1)) return false;
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
