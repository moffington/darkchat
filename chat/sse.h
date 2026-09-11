#ifndef DARKCHAT_SSE_H
#define DARKCHAT_SSE_H

/* Incremental server-sent event parser. Input may be split at any byte. The
   callback receives one assembled data field (multiple data: lines joined by
   a newline) whenever a blank line terminates an event. */
#include <stdbool.h>
#include <stddef.h>

typedef bool (*SseEventFn)(void *user, const char *data, size_t length);

typedef struct {
    char *line, *data;
    size_t line_length, line_capacity;
    size_t data_length, data_capacity;
    bool failed;
} SseParser;

void sse_init(SseParser *parser);
bool sse_feed(SseParser *parser, const char *bytes, size_t length,
    SseEventFn callback, void *user);
bool sse_finish(SseParser *parser, SseEventFn callback, void *user);
void sse_dispose(SseParser *parser);

#endif
