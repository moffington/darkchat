#include "sse.h"
#include <stdlib.h>
#include <string.h>

#define SSE_MAX_EVENT (16u * 1024u * 1024u)

static bool reserve(char **buffer, size_t *capacity, size_t needed) {
    if (needed <= *capacity) return true;
    size_t next = *capacity ? *capacity : 256;
    while (next < needed) {
        if (next > SSE_MAX_EVENT / 2) { next = SSE_MAX_EVENT; break; }
        next *= 2;
    }
    if (next < needed || next > SSE_MAX_EVENT) return false;
    char *grown = (char *)realloc(*buffer, next);
    if (!grown) return false;
    *buffer = grown;
    *capacity = next;
    return true;
}

void sse_init(SseParser *parser) { memset(parser, 0, sizeof *parser); }

static bool dispatch(SseParser *parser, SseEventFn callback, void *user) {
    if (!parser->data_length) return true;
    /* The final newline was the separator added after the last data: line. */
    size_t length = parser->data_length - 1;
    if (!reserve(&parser->data, &parser->data_capacity, length + 1)) return false;
    parser->data[length] = 0;
    bool ok = !callback || callback(user, parser->data, length);
    parser->data_length = 0;
    return ok;
}

static bool line(SseParser *parser, SseEventFn callback, void *user) {
    size_t length = parser->line_length;
    if (length && parser->line[length - 1] == '\r') --length;
    if (!length) return dispatch(parser, callback, user);
    if (parser->line[0] == ':') return true;
    const char *colon = (const char *)memchr(parser->line, ':', length);
    size_t field = colon ? (size_t)(colon - parser->line) : length;
    if (field != 4 || memcmp(parser->line, "data", 4) != 0) return true;
    size_t start = colon ? field + 1 : length;
    if (start < length && parser->line[start] == ' ') ++start;
    size_t value = length - start;
    if (parser->data_length + value + 1 > SSE_MAX_EVENT) return false;
    if (!reserve(&parser->data, &parser->data_capacity,
        parser->data_length + value + 2)) return false;
    if (value) memcpy(parser->data + parser->data_length,
        parser->line + start, value);
    parser->data_length += value;
    parser->data[parser->data_length++] = '\n';
    return true;
}

bool sse_feed(SseParser *parser, const char *bytes, size_t length,
    SseEventFn callback, void *user) {
    if (!parser || parser->failed || (!bytes && length)) return false;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == '\n') {
            if (!line(parser, callback, user)) { parser->failed = true; return false; }
            parser->line_length = 0;
        } else {
            if (parser->line_length + 1 > SSE_MAX_EVENT ||
                !reserve(&parser->line, &parser->line_capacity,
                    parser->line_length + 1)) {
                parser->failed = true;
                return false;
            }
            parser->line[parser->line_length++] = bytes[i];
        }
    }
    return true;
}

bool sse_finish(SseParser *parser, SseEventFn callback, void *user) {
    if (!parser || parser->failed) return false;
    if (parser->line_length) {
        if (!line(parser, callback, user)) { parser->failed = true; return false; }
        parser->line_length = 0;
    }
    if (!dispatch(parser, callback, user)) { parser->failed = true; return false; }
    return true;
}

void sse_dispose(SseParser *parser) {
    if (!parser) return;
    free(parser->line);
    free(parser->data);
    memset(parser, 0, sizeof *parser);
}
