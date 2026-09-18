#ifndef DARKCHAT_CHAT_UI_H
#define DARKCHAT_CHAT_UI_H

/* Retained DarkUI chrome for DarkChat: a restrained application header
   (hamburger, conversation title, editable model chip with a catalog button,
   overflow menu), a collapsible conversation sidebar, the centered
   transcript placeholder, the composer card with its compact Send/Stop
   control, and the status line. The native Rich Edit surfaces are positioned
   over the placeholders by the host, which queries their arranged rectangles
   here.

   Responsive sidebar model. The user's explicit preference is the persisted
   `Chat::sidebar_collapsed` flag and only applies at or above
   CHAT_UI_SIDEBAR_BREAKPOINT. Below the breakpoint the sidebar is an
   in-flow temporary drawer: it starts closed on every crossing, the
   hamburger opens it, Escape closes it, and the transcript/composer reflow
   into the reclaimed width. Nothing overlaps because the drawer participates
   in layout; it is not painted over the conversation surface.

   The conversation list is a windowed row pool: DarkUI nodes exist only for
   the visible window (bounded by the laid-out viewport and the conversation
   count, never by CHAT_MAX_CONVERSATIONS), and each row carries the stable
   conversation id in its node tag. Scrolling rebinds the pool; selection is
   resolved through the tag at activation time, so deletion or any future
   reordering can never mis-target a row. */
#include "../ui/ui.h"
#include "chat.h"

/* Client width in DIPs below which the sidebar switches to the temporary
   drawer behavior. One named breakpoint instead of scattered width checks. */
#define CHAT_UI_SIDEBAR_BREAKPOINT 900.0f

/* Left inset of the native search edit inside its placeholder, leaving room
   for the retained search glyph. */
#define CHAT_UI_SEARCH_ICON_INSET 26.0f

typedef enum {
    CHAT_COMMAND_NONE,
    CHAT_COMMAND_SEND,
    CHAT_COMMAND_NEW_CONVERSATION,
    CHAT_COMMAND_SELECT,
    /* Open the retained command menu (all Conversation/Response/Settings
       actions) anchored to the overflow button. */
    CHAT_COMMAND_OVERFLOW,
    /* Open the model catalog picker for the active backend. */
    CHAT_COMMAND_MODEL_PICKER,
    /* Flip the sidebar: the explicit preference at wide widths, the
       temporary drawer at narrow widths. */
    CHAT_COMMAND_TOGGLE_SIDEBAR
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
    UiId root, heading, model, model_picker, overflow, hamburger, search,
        search_status, new_conversation, list, transcript, composer, send,
        status, empty, empty_title, empty_hint, header, sidebar, main,
        composer_area, composer_card, footer;
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
    /* Responsive sidebar state: `sidebar_open` mirrors the persisted
       preference (it applies at and above the breakpoint), `narrow` is the
       effective breakpoint state of the last layout, and `drawer_open` is
       the session-only temporary drawer. */
    bool sidebar_open, narrow, drawer_open;
    bool composer_focused;
    ChatCommandFn command;
    void *command_user;
} ChatUi;

bool chat_ui_init(ChatUi *chat_ui, Ui *ui, Chat *chat);
/* Reflects heading, status, empty-state visibility and the sidebar footer
   into the tree. Row bindings, spacers and the windowed selection live in
   chat_ui_sidebar_remap. */
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
   state without rebuilding the retained tree. The icon changes too; the
   accessible name tracks the action. */
void chat_ui_set_generation(ChatUi *chat_ui, bool generating, bool stopping);
/* Toggles the sidebar through one command: at wide widths it flips and
   persists the explicit preference (written to Chat::sidebar_collapsed) and
   returns true so the host can mark storage dirty; at narrow widths it
   flips the temporary drawer and returns false (nothing persisted). */
bool chat_ui_toggle_sidebar(ChatUi *chat_ui);
/* True when the narrow-width temporary drawer is currently open. Escape
   closes it through chat_ui_close_drawer. */
bool chat_ui_narrow_drawer_open(const ChatUi *chat_ui);
void chat_ui_close_drawer(ChatUi *chat_ui);
/* Ensures the sidebar (or the narrow-width temporary drawer) is visible, so
   a command that targets the conversation list or search can focus and place
   it. Returns true when the persisted wide-width preference changed. */
bool chat_ui_reveal_sidebar(ChatUi *chat_ui);
/* Effective sidebar visibility for the last layout: the preference at wide
   widths, the drawer at narrow widths. */
bool chat_ui_sidebar_visible(const ChatUi *chat_ui);
/* Marks a placeholder as owning native keyboard focus so its border and
   state reflect focus (composer card, model chip, search field). */
void chat_ui_set_focus_ring(ChatUi *chat_ui, UiId id, bool focused);
void chat_ui_set_search_status(ChatUi *chat_ui, const wchar_t *text);
/* Lays out the tree when the client size changed, the responsive sidebar
   state changed, or a relayout is owed. Returns true when ui_layout ran. */
bool chat_ui_resize(ChatUi *chat_ui, float width, float height);
void chat_ui_event(void *user, Ui *ui, UiEvent event);
/* Arranged rectangle for a laid-out node, or an empty rectangle. */
UiRect chat_ui_rect(const ChatUi *chat_ui, UiId id);

#endif
