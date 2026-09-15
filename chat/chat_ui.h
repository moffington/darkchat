#ifndef DARKCHAT_CHAT_UI_H
#define DARKCHAT_CHAT_UI_H

/* Retained DarkUI chrome for DarkChat: toolbar, conversation sidebar, model
   field placeholder, transcript/composer placeholders, send button and status
   line. The native Rich Edit surfaces are positioned over the placeholders by
   the host, which queries their arranged rectangles here.

   The conversation list is a windowed row pool: DarkUI nodes exist only for
   the visible window (bounded by the laid-out viewport and the conversation
   count, never by CHAT_MAX_CONVERSATIONS), and each row carries the stable
   conversation id in its node tag. Scrolling rebinds the pool; selection is
   resolved through the tag at activation time, so deletion or any future
   reordering can never mis-target a row. */
#include "../ui/ui.h"
#include "chat.h"

typedef enum {
    CHAT_COMMAND_NONE,
    CHAT_COMMAND_SEND,
    CHAT_COMMAND_NEW_CONVERSATION,
    CHAT_COMMAND_SELECT
} ChatCommand;

typedef void (*ChatCommandFn)(void *user, ChatCommand command, int index);

/* What the remaps of one flush changed, for the host to translate into UIA
   notifications. The report ACCUMULATES across remaps within one flush
   (a reveal causes a second remap; its findings must not overwrite the
   first remap's). Clear it with chat_ui_remap_report_clear before the
   flush's first remap.
   `mapping_changed` covers the logical row set: offset, visible bindings or
   pool membership. `name_changed` lists rows whose title text changed,
   whether or not their binding changed; entries are deduplicated by row and
   carry the pre-flush old title — the host reads the final name at raise
   time. `focus_binding_changed` means the focused row's stable id changed
   and screen readers should re-announce it; remap never moves focus. */
typedef struct {
    bool mapping_changed;
    bool focus_binding_changed;
    struct {
        UiId id;
        wchar_t old_title[CHAT_TITLE_TEXT];
    } name_changed[CHAT_MAX_CONVERSATIONS];
    int name_changed_count;
} ChatUiRemapReport;

typedef struct {
    Ui *ui;
    Chat *chat;
    UiId root, heading, model, search, search_status, new_conversation, list,
        transcript, composer, send, status;
    UiId sidebar;
    /* Windowed conversation list: two spacers sandwich the live rows so the
       laid-out extent always equals the full list (count*pitch - gap). */
    UiId top_spacer, bottom_spacer;
    UiId rows[CHAT_MAX_CONVERSATIONS];      /* only [0, pool_count) are live */
    int pool_count;                          /* DarkUI nodes currently in list */
    /* Diff cache for the no-op remap guarantee. */
    int window_offset, window_rows;
    uint64_t row_tags[CHAT_MAX_CONVERSATIONS];
    wchar_t row_titles[CHAT_MAX_CONVERSATIONS][CHAT_TITLE_TEXT];
    bool window_valid;
    /* Latest-wins reveal request by stable conversation id (0 = none). */
    uint64_t pending_reveal;
    float last_layout_width, last_layout_height, last_sidebar_width;
    ChatCommandFn command;
    void *command_user;
} ChatUi;

bool chat_ui_init(ChatUi *chat_ui, Ui *ui, Chat *chat);
/* Reflects heading, status and sidebar width into the tree. Row bindings,
   spacers and the windowed selection live in chat_ui_sidebar_remap. */
void chat_ui_sync(ChatUi *chat_ui);
/* Re-derives the visible window from the laid-out viewport and current
   scroll position, grows or shrinks the row pool to match, and diffs every
   row's binding, title and selection. Mutates and invalidates only on real
   change; a no-op remap touches nothing. Never adjusts scroll. The report
   accumulates; see ChatUiRemapReport. */
bool chat_ui_sidebar_remap(ChatUi *chat_ui, ChatUiRemapReport *report);
/* Resets a remap report; call once before the first remap of a flush. */
void chat_ui_remap_report_clear(ChatUiRemapReport *report);
/* Requests that the conversation be scrolled into the window after the next
   layout. Applied by chat_ui_apply_reveal, which must run after a layout
   (load time precedes the tree, so the reveal id is parked until the extent
   exists). */
void chat_ui_request_reveal(ChatUi *chat_ui, uint64_t conversation_id);
/* Consumes the pending reveal: resolves the id, moves the scroll minimally
   (top-align above, bottom-align below, no-op when visible) and lays out.
   Returns true when the scroll position changed. Unresolvable ids drop. */
bool chat_ui_apply_reveal(ChatUi *chat_ui);
/* Changes the primary action between Send, Stop, and the disabled stopping
   state without rebuilding the retained tree. */
void chat_ui_set_generation(ChatUi *chat_ui, bool generating, bool stopping);
void chat_ui_set_search_status(ChatUi *chat_ui, const wchar_t *text);
/* Lays out the tree when the client size changed or a relayout is owed.
   Returns true when ui_layout actually ran. */
bool chat_ui_resize(ChatUi *chat_ui, float width, float height);
void chat_ui_event(void *user, Ui *ui, UiEvent event);
/* Arranged rectangle for a laid-out node, or an empty rectangle. */
UiRect chat_ui_rect(const ChatUi *chat_ui, UiId id);

#endif
