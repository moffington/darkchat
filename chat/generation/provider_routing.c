#include "chat/generation/provider_routing.h"
#include <string.h>

void chat_provider_routing_init(ChatProviderRouting *routing) {
    if (routing) memset(routing, 0, sizeof *routing);
}

static const char *sort_field(ChatProviderSort sort) {
    switch (sort) {
    case CHAT_PROVIDER_SORT_PRICE: return "\"sort\":\"price\"";
    case CHAT_PROVIDER_SORT_THROUGHPUT: return "\"sort\":\"throughput\"";
    case CHAT_PROVIDER_SORT_LATENCY: return "\"sort\":\"latency\"";
    default: return "";
    }
}

/* Collects the non-default routing key fragments in their fixed order into
   `fields` (at most four) and returns how many were written. A count of zero
   means every control is at its OpenRouter default, so no provider object is
   sent at all. Pure, allocation-free. */
static size_t provider_fields(const ChatProviderRouting *routing,
    const char *fields[4]) {
    if (!routing) return 0;
    size_t count = 0;
    if (routing->sort != CHAT_PROVIDER_SORT_DEFAULT)
        fields[count++] = sort_field(routing->sort);
    if (routing->disallow_fallbacks)
        fields[count++] = "\"allow_fallbacks\":false";
    if (routing->data_collection == CHAT_DATA_COLLECTION_DENY)
        fields[count++] = "\"data_collection\":\"deny\"";
    if (routing->zdr)
        fields[count++] = "\"zdr\":true";
    return count;
}

/* The exact bytes chat_provider_append writes. A pure summation over the same
   field list the appender emits, so it neither allocates nor has a failure
   path that could report a smaller size: it is either 0 (nothing is sent) or
   the exact non-zero envelope size. */
size_t chat_provider_envelope_bytes(const ChatProviderRouting *routing) {
    const char *fields[4];
    size_t count = provider_fields(routing, fields);
    if (!count) return 0;
    size_t total = sizeof ",\"provider\":{" - 1;
    for (size_t i = 0; i < count; i++) total += strlen(fields[i]);
    total += count - 1;   /* the comma between adjacent fields */
    total += 1;           /* the closing brace */
    return total;
}

bool chat_provider_append(JsonBuf *buf, const ChatProviderRouting *routing) {
    if (!buf) return false;
    const char *fields[4];
    size_t count = provider_fields(routing, fields);
    if (!count) return true;
    if (!json_buf_append_raw(buf, ",\"provider\":{",
            sizeof ",\"provider\":{" - 1)) return false;
    for (size_t i = 0; i < count; i++) {
        if (i && !json_buf_append_raw(buf, ",", 1)) return false;
        if (!json_buf_append_raw(buf, fields[i], strlen(fields[i])))
            return false;
    }
    return json_buf_append_raw(buf, "}", 1);
}
