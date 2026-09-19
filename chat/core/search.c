#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat/core/search.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool folded_equal(const wchar_t *left, const wchar_t *right,
    size_t length) {
    if (length > INT_MAX) return false;
    return CompareStringOrdinal(left, (int)length, right, (int)length, TRUE) ==
        CSTR_EQUAL;
}

static size_t first_match(const wchar_t *text, const wchar_t *query,
    size_t query_length) {
    size_t length = wcslen(text);
    if (query_length > length || query_length > INT_MAX) return SIZE_MAX;
    for (size_t i = 0; i <= length - query_length; i++)
        if (folded_equal(text + i, query, query_length)) return i;
    return SIZE_MAX;
}

static bool high_surrogate(wchar_t value) {
    return value >= 0xd800 && value <= 0xdbff;
}

static bool low_surrogate(wchar_t value) {
    return value >= 0xdc00 && value <= 0xdfff;
}

static void snippet(const wchar_t *text, size_t match, size_t match_length,
    wchar_t out[CHAT_SEARCH_SNIPPET]) {
    size_t length = wcslen(text);
    size_t maximum = CHAT_SEARCH_SNIPPET - 1;
    size_t start = 0, end = length;
    if (length > maximum) {
        size_t core = maximum - 6; /* room for both possible "..." markers */
        size_t focus = match_length < core ? match_length : core;
        size_t before = (core - focus) / 2;
        start = match > before ? match - before : 0;
        if (start + core > length) start = length - core;
        end = start + core;
        /* Never expose half of a UTF-16 surrogate pair at a snippet edge. */
        if (start && start < length && low_surrogate(text[start]) &&
            high_surrogate(text[start - 1])) ++start;
        if (end && end < length && low_surrogate(text[end]) &&
            high_surrogate(text[end - 1])) --end;
    }

    size_t used = 0;
    if (start) {
        out[used++] = L'.'; out[used++] = L'.'; out[used++] = L'.';
    }
    for (size_t i = start; i < end && used < maximum; i++) {
        wchar_t value = text[i];
        if (value == L'\r') {
            value = L' ';
            if (i + 1 < end && text[i + 1] == L'\n') ++i;
        } else if (value == L'\n' || value == L'\t') {
            value = L' ';
        }
        out[used++] = value;
    }
    if (end < length && used + 3 <= maximum) {
        out[used++] = L'.'; out[used++] = L'.'; out[used++] = L'.';
    }
    out[used] = 0;
}

static bool append_result(ChatSearchResults *results,
    const ChatConversation *conversation, const ChatMessage *message,
    ChatSearchField field, const wchar_t *text, size_t offset,
    size_t query_length) {
    if (results->count == results->capacity) {
        size_t capacity = results->capacity ? results->capacity * 2 : 8;
        if (capacity < results->capacity ||
            capacity > SIZE_MAX / sizeof *results->items) return false;
        ChatSearchResult *items = (ChatSearchResult *)realloc(results->items,
            capacity * sizeof *items);
        if (!items) return false;
        results->items = items;
        results->capacity = capacity;
    }
    ChatSearchResult *result = &results->items[results->count++];
    memset(result, 0, sizeof *result);
    result->conversation_id = conversation->id;
    result->message_id = message->id;
    result->message_revision = message->revision;
    result->field = field;
    result->role = message->role;
    result->match_offset = offset;
    result->match_length = query_length;
    snippet(text, offset, query_length, result->snippet);
    return true;
}

void chat_search_results_dispose(ChatSearchResults *results) {
    if (!results) return;
    free(results->query);
    free(results->items);
    memset(results, 0, sizeof *results);
}

bool chat_search_build(const Chat *chat, const wchar_t *query,
    ChatSearchOptions options, ChatSearchResults *out) {
    if (!chat || !query || !out) return false;
    ChatSearchResults built;
    memset(&built, 0, sizeof built);
    size_t query_length = wcslen(query);
    if (!query_length) {
        chat_search_results_dispose(out);
        return true;
    }
    if (query_length > (SIZE_MAX / sizeof(wchar_t)) - 1) return false;
    built.query = (wchar_t *)malloc((query_length + 1) * sizeof *built.query);
    if (!built.query) return false;
    memcpy(built.query, query, (query_length + 1) * sizeof *built.query);

    for (int c = 0; c < chat->conversation_count; c++) {
        const ChatConversation *conversation = &chat->conversations[c];
        for (size_t m = 0; m < conversation->message_count; m++) {
            const ChatMessage *message = &conversation->messages[m];
            const wchar_t *body = chat_message_text(message);
            size_t offset = first_match(body, query, query_length);
            if (offset != SIZE_MAX && !append_result(&built, conversation,
                    message, CHAT_SEARCH_BODY, body, offset, query_length))
                goto failed;
            if (options.include_reasoning) {
                const wchar_t *reasoning = chat_message_reasoning(message);
                offset = first_match(reasoning, query, query_length);
                if (offset != SIZE_MAX && !append_result(&built, conversation,
                        message, CHAT_SEARCH_REASONING, reasoning, offset,
                        query_length)) goto failed;
            }
        }
    }
    chat_search_results_dispose(out);
    *out = built;
    return true;

failed:
    chat_search_results_dispose(&built);
    return false;
}

bool chat_search_resolve(const Chat *chat, const ChatSearchResults *results,
    size_t index, ChatSearchTarget *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!chat || !results || !out || !results->query || index >= results->count)
        return false;
    const ChatSearchResult *result = &results->items[index];
    for (int c = 0; c < chat->conversation_count; c++) {
        const ChatConversation *conversation = &chat->conversations[c];
        if (conversation->id != result->conversation_id) continue;
        for (size_t m = 0; m < conversation->message_count; m++) {
            const ChatMessage *message = &conversation->messages[m];
            if (message->id != result->message_id) continue;
            if (message->revision != result->message_revision ||
                message->role != result->role) return false;
            const wchar_t *text = result->field == CHAT_SEARCH_REASONING
                ? chat_message_reasoning(message) : chat_message_text(message);
            size_t length = wcslen(text);
            if (result->match_offset > length ||
                result->match_length > length - result->match_offset ||
                wcslen(results->query) != result->match_length ||
                !folded_equal(text + result->match_offset, results->query,
                    result->match_length)) return false;
            out->conversation = c;
            out->message = (int)m;
            out->field = result->field;
            return true;
        }
        return false;
    }
    return false;
}
