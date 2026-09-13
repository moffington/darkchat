#ifndef DARKCHAT_TRANSCRIPT_WIN32_H
#define DARKCHAT_TRANSCRIPT_WIN32_H

/* Transcript update bookkeeping for the chat host, isolated from the rest of
   the host. One rendered turn per message: every turn owns its head/body
   blocks and, when its reasoning is expanded, its own scrolling viewport; no
   control is shared between turns. The module lays the controls out in content
   coordinates inside the transcript container and repositions them as the
   container scrolls, so the whole conversation scrolls as one transcript while
   an expanded reasoning viewport scrolls independently.

   Every turn records the message identity its surfaces were built from. When
   that identity still matches, destructive content writes are skipped, so
   unchanged historical controls survive completion and layout changes with
   their reader state (selection, scroll, caret) intact. Destructive
   reformatting of a surface that currently holds a selection is deferred until
   the selection clears. Signature matches skip destructive writes only:
   visibility reconciliation, callback wiring, width-dependent measurement,
   positioning and DPI work always run. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"
#include "rich_text_win32.h"

/* Markdown streaming rebuilds the visible body at most this often; deltas
   accumulate in the message between rebuilds. */
#define CHAT_BODY_RENDER_MS 100

typedef struct {
    RichTextControl head, body, reasoning, meta;
    bool head_live, body_live, reason_live, meta_live;
    int y, head_y, reason_y, body_y, meta_y, height;
    int head_h, reason_h, body_h, meta_h;
    /* Rendered identity: the message state these surfaces were built from. */
    bool rendered_valid;
    uint64_t conversation, message, revision;
    ChatRole role;
    ChatGenerationState state;
    bool running, content_started, reasoning_open;
    wchar_t row[48];                    /* rendered reasoning-row text */
    /* Destructive writes deferred while a selection is held in a surface. */
    bool head_pending, body_pending, meta_pending, reason_pending;
} TranscriptTurn;

/* Host streaming context, refreshed by the host before every call. */
typedef struct {
    const Chat *chat;
    bool generating, content_started, reasoning_streaming;
    int request_conversation, request_message;
} TranscriptFeed;

/* Host-owned behavior the transcript surfaces need; wired once at creation. */
typedef struct {
    bool (*surface_key)(void *user, WPARAM key, bool shift, bool control,
        bool down);
    bool (*row_click)(void *user, RichTextControl *control, int line,
        bool down);
    void *user;
} TranscriptCallbacks;

typedef struct {
    HWND view;                          /* transcript container child window */
    float dpi;
    RichTextTheme theme;
    int view_scroll, view_content, view_page, view_width, view_margin, view_gap;
    int view_reason_gap, view_meta_gap, view_reason_inset;
    RichTextControl *measuring;         /* control awaiting EN_REQUESTRESIZE */
    int measured;
    TranscriptTurn turns[CHAT_MAX_MESSAGES];
    int turn_count;
    /* Last time the streaming body was rebuilt as Markdown. */
    ULONGLONG body_render_tick;
    /* Reentrancy guard: programmatic selection changes fired while this module
       writes must not recursively trigger deferred-update application. */
    bool applying;
    TranscriptCallbacks callbacks;
} Transcript;

void transcript_create(Transcript *t, HWND view, const RichTextTheme *theme,
    float dpi);
/* DPI change: re-derives every turn surface; the next layout re-measures. */
void transcript_set_dpi(Transcript *t, float dpi);
void transcript_measure_notify(Transcript *t, RichTextControl *control,
    const RECT *required);
/* Drops every turn's rendered identity, pending updates and selections, so
   the next render replaces content without deferral. */
void transcript_invalidate(Transcript *t);
/* Same, from one turn slot onward: retry/regenerate/edit slot reuse. */
void transcript_invalidate_from(Transcript *t, int from);
void transcript_render(Transcript *t, const TranscriptFeed *feed);
void transcript_refresh_turn(Transcript *t, const TranscriptFeed *feed,
    int index);
/* Throttled Markdown rebuild of one streaming turn's body; defers while the
   body holds a selection. */
void transcript_stream_body(Transcript *t, const TranscriptFeed *feed,
    int index);
void transcript_position(Transcript *t, bool follow);
void transcript_layout_from(Transcript *t, int start, bool follow);
bool transcript_pinned(const Transcript *t);
/* EN_SELCHANGE from any turn surface: applies that turn's deferred writes when
   the selection has cleared and relayouts from the affected turn. */
void transcript_selection_changed(Transcript *t, const TranscriptFeed *feed,
    RichTextControl *control);
/* Fallback sweep (idle timer): applies every deferred write whose selection
   is gone and relayouts once from the earliest changed turn. */
void transcript_apply_pending(Transcript *t, const TranscriptFeed *feed);

#endif