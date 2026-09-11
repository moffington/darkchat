#include "../chat/sse.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

typedef struct { char joined[512]; int count; } Events;
static bool collect(void *user, const char *data, size_t length) {
    Events *events = (Events *)user;
    size_t used = strlen(events->joined);
    if (used + length + 2 >= sizeof events->joined) return false;
    if (events->count) events->joined[used++] = '|';
    memcpy(events->joined + used, data, length);
    events->joined[used + length] = 0;
    ++events->count;
    return true;
}

static void every_split(void) {
    const char *input = ": keepalive\r\ndata: {\"delta\":\"A\"}\r\n\r\n"
        "event: message\ndata: first\ndata: second\n\ndata: [DONE]\n\n";
    size_t length = strlen(input);
    bool all = true;
    for (size_t split = 0; split <= length; split++) {
        SseParser parser; Events events = {{0}, 0};
        sse_init(&parser);
        bool ok = sse_feed(&parser, input, split, collect, &events) &&
            sse_feed(&parser, input + split, length - split, collect, &events) &&
            sse_finish(&parser, collect, &events);
        if (!ok || events.count != 3 || strcmp(events.joined,
            "{\"delta\":\"A\"}|first\nsecond|[DONE]") != 0) all = false;
        sse_dispose(&parser);
    }
    check(all, "SSE survives every possible two-chunk split");
}

static void byte_at_a_time(void) {
    const char *input = "data: one\n\ndata: two\n\n";
    SseParser parser; Events events = {{0}, 0};
    sse_init(&parser);
    bool ok = true;
    for (size_t i = 0; input[i] && ok; i++)
        ok = sse_feed(&parser, input + i, 1, collect, &events);
    check(ok && sse_finish(&parser, collect, &events),
        "SSE accepts one-byte chunks");
    check(events.count == 2 && strcmp(events.joined, "one|two") == 0,
        "SSE dispatches complete events");
    sse_dispose(&parser);
}

int main(void) {
    every_split();
    byte_at_a_time();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall SSE checks passed\n");
    return 0;
}
