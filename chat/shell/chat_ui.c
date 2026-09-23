#include "chat/shell/chat_ui.h"
#include <math.h>
#include <string.h>

/* Composition metrics, in DIPs. A small fixed scale (4/6/8/10/12/16/24)
   keeps spacing consistent across the header, sidebar and composer. */
#define CHAT_HEADER_HEIGHT 56
#define CHAT_HEADER_PADDING 11
#define CHAT_HEADER_GAP 8
#define CHAT_ICON_BUTTON 34
#define CHAT_MODEL_CHIP_WIDTH 260
#define CHAT_MODEL_CHEVRON_WIDTH 28
#define CHAT_SIDEBAR_PADDING 10
#define CHAT_SIDEBAR_GAP 8
#define CHAT_SIDEBAR_ROW_GAP 2
#define CHAT_ROW_ACTION_HEIGHT 34
#define CHAT_SEARCH_HEIGHT 34
#define CHAT_SEARCH_STATUS_HEIGHT 28
#define CHAT_SEND_BUTTON 34
#define CHAT_STATUS_HEIGHT 18
#define CHAT_COMPOSER_AREA_HEIGHT 132
#define CHAT_EMPTY_HERO_MAX_WIDTH 520

static UiId add(ChatUi *chat_ui, UiId parent, UiKind kind, const wchar_t *text) {
    return ui_add(chat_ui->ui, parent, kind, text);
}

static UiNode *node(ChatUi *chat_ui, UiId id) { return ui_node(chat_ui->ui, id); }

static UiId label(ChatUi *chat_ui, UiId parent, const wchar_t *text, UiFont font,
    UiColorRole color) {
    UiId id = add(chat_ui, parent, UI_LABEL, text);
    UiNode *item = node(chat_ui, id);
    if (item) { item->style.font = font; item->style.foreground = color; }
    return id;
}

static UiId icon_button(ChatUi *chat_ui, UiId parent, UiIcon icon,
    const wchar_t *name, const wchar_t *help) {
    UiId id = add(chat_ui, parent, UI_ICON_BUTTON, L"");
    UiNode *item = node(chat_ui, id);
    if (item) {
        item->icon = icon;
        item->style.background = -1;   /* transparent idle surface */
        item->style.padding = 0;
    }
    if (name) ui_set_accessible_name(chat_ui->ui, id, name);
    if (help) ui_set_help_text(chat_ui->ui, id, help);
    return id;
}

static void height(ChatUi *chat_ui, UiId id, float dips) {
    UiNode *item = node(chat_ui, id);
    if (item) item->style.height = ui_fixed(dips);
}

static void width(ChatUi *chat_ui, UiId id, float dips) {
    UiNode *item = node(chat_ui, id);
    if (item) item->style.width = ui_fixed(dips);
}

static void fill_width(ChatUi *chat_ui, UiId id) {
    UiNode *item = node(chat_ui, id);
    if (item) item->style.width = ui_flex(1);
}

static void fill_height(ChatUi *chat_ui, UiId id) {
    UiNode *item = node(chat_ui, id);
    if (item) item->style.height = ui_flex(1);
}

/* A bordered surface that holds nothing (or only decoration); native Rich
   Edit sits on top. */
static UiId surface(ChatUi *chat_ui, UiId parent, UiKind kind,
    const wchar_t *name, UiColorRole background) {
    UiId id = add(chat_ui, parent, kind, L"");
    UiNode *item = node(chat_ui, id);
    if (item) {
        item->style.padding = 0;
        item->style.border = true;
        item->style.background = background;
        item->style.width = ui_flex(1);
    }
    if (name && *name) ui_set_accessible_name(chat_ui->ui, id, name);
    return id;
}

/* Distance from one conversation row to the next: the button's measured
   height plus the list's inter-child gap. The windowing math is exact only
   when both spacers and rows use this pitch. */
static float row_pitch(const ChatUi *chat_ui) {
    const UiNode *list = ui_node(chat_ui->ui, chat_ui->list);
    if (!list) return 0;
    return chat_ui->ui->theme.control_height + list->style.gap;
}

/* Visible window derived from the laid-out viewport and the current scroll
   position: the window never shows more rows than exist, and the offset
   never exceeds the last full window. */
static void window_extent(const ChatUi *chat_ui, int count, int *offset_out,
    int *visible_out) {
    float pitch = row_pitch(chat_ui);
    float viewport = ui_scroll_viewport_h(chat_ui->ui, chat_ui->list);
    float scroll = ui_scroll_offset(chat_ui->ui, chat_ui->list);
    int needed = pitch > 0 && viewport > 0 ? (int)ceilf(viewport / pitch) + 1 : 1;
    if (needed < 1) needed = 1;
    int visible = count < needed ? count : needed;
    if (visible < 0) visible = 0;
    int max_offset = count - visible;
    if (max_offset < 0) max_offset = 0;
    int offset = pitch > 0 ? (int)floorf(scroll / pitch) : 0;
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    *offset_out = offset;
    *visible_out = visible;
}

/* Row titles are fixed-height, flex-width buttons: their text can never
   change layout, so titles update with paint-only invalidation. ui_set_text
   would force a full relayout on every scroll rebind. */
static bool row_set_title(ChatUi *chat_ui, UiId id, const wchar_t *title) {
    UiNode *item = node(chat_ui, id);
    if (!item) return false;
    if (!title) title = L"";
    size_t length = wcslen(title);
    if (length >= UI_TEXT_CAPACITY) length = UI_TEXT_CAPACITY - 1;
    if (length && title[length - 1] >= 0xd800 && title[length - 1] <= 0xdbff) --length;
    if (wcslen(item->text) == length && !wmemcmp(item->text, title, length)) return false;
    wmemmove(item->text, title, length);
    item->text[length] = 0;
    ui_invalidate(chat_ui->ui, false);
    return true;
}

/* Spacer heights only ever change through here; a change dirties layout. */
static bool set_spacer_height(ChatUi *chat_ui, UiId id, float dips) {
    UiNode *item = node(chat_ui, id);
    if (!item) return false;
    if (item->style.height.kind == UI_FIXED &&
        fabsf(item->style.height.value - dips) < 0.01f) return false;
    item->style.height = ui_fixed(dips);
    ui_invalidate(chat_ui->ui, true);
    return true;
}

static bool set_hidden(ChatUi *chat_ui, UiId id, bool hidden) {
    UiNode *item = node(chat_ui, id);
    if (!item || item->hidden == hidden) return false;
    ui_set_hidden(chat_ui->ui, id, hidden);
    return true;
}

static bool set_disabled(ChatUi *chat_ui, UiId id, bool disabled) {
    UiNode *item = node(chat_ui, id);
    if (!item || item->disabled == disabled) return false;
    ui_set_disabled(chat_ui->ui, id, disabled);
    return true;
}

bool chat_ui_sidebar_visible(const ChatUi *chat_ui) {
    if (!chat_ui) return false;
    return chat_ui->narrow ? chat_ui->drawer_open : chat_ui->sidebar_open;
}

bool chat_ui_narrow_drawer_open(const ChatUi *chat_ui) {
    return chat_ui && chat_ui->narrow && chat_ui->drawer_open;
}

/* Reflects the effective sidebar state into the tree: the panel either
   participates in layout at the persisted width or is removed from it
   entirely, and the hamburger carries the expanded state and the matching
   accessible name. */
static void apply_sidebar_state(ChatUi *chat_ui) {
    bool visible = chat_ui_sidebar_visible(chat_ui);
    bool collapsed = !visible;
    UiNode *panel = node(chat_ui, chat_ui->sidebar);
    bool changed = false;
    if (panel) {
        if (panel->hidden != collapsed) {
            ui_set_hidden(chat_ui->ui, chat_ui->sidebar, collapsed);
            changed = true;
        }
        float panel_width = (float)chat_ui->chat->sidebar_width;
        if (panel->style.width.kind != UI_FIXED ||
            fabsf(panel->style.width.value - panel_width) >= 0.01f) {
            panel->style.width = ui_fixed(panel_width);
            changed = true;
        }
    }
    UiNode *button = node(chat_ui, chat_ui->hamburger);
    if (button) {
        if (button->selected != visible) {
            button->selected = visible;
            changed = true;
        }
        ui_set_accessible_name(chat_ui->ui, chat_ui->hamburger,
            visible ? L"Hide conversation sidebar"
                    : L"Show conversation sidebar");
    }
    if (changed) ui_invalidate(chat_ui->ui, true);
}

bool chat_ui_toggle_sidebar(ChatUi *chat_ui) {
    if (!chat_ui) return false;
    bool persisted = false;
    if (chat_ui->narrow) {
        chat_ui->drawer_open = !chat_ui->drawer_open;
    } else {
        chat_ui->sidebar_open = !chat_ui->sidebar_open;
        chat_ui->chat->sidebar_collapsed = chat_ui->sidebar_open ? 0 : 1;
        persisted = true;
    }
    apply_sidebar_state(chat_ui);
    return persisted;
}

void chat_ui_close_drawer(ChatUi *chat_ui) {
    if (!chat_ui || !chat_ui->drawer_open) return;
    chat_ui->drawer_open = false;
    apply_sidebar_state(chat_ui);
}

bool chat_ui_reveal_sidebar(ChatUi *chat_ui) {
    if (!chat_ui || chat_ui_sidebar_visible(chat_ui)) return false;
    bool persisted = false;
    if (chat_ui->narrow) {
        chat_ui->drawer_open = true;
    } else {
        chat_ui->sidebar_open = true;
        chat_ui->chat->sidebar_collapsed = 0;
        persisted = true;
    }
    apply_sidebar_state(chat_ui);
    return persisted;
}

bool chat_ui_init(ChatUi *chat_ui, Ui *ui, Chat *chat) {
    memset(chat_ui, 0, sizeof *chat_ui);
    chat_ui->ui = ui;
    chat_ui->chat = chat;
    chat_ui->sidebar_open = chat->sidebar_collapsed == 0;

    UiId root = add(chat_ui, UI_NONE, UI_COLUMN, L"");
    chat_ui->root = root;
    node(chat_ui, root)->style.gap = 0;
    node(chat_ui, root)->style.background = UI_BG;

    /* ---- Header: hamburger, title, model chip, overflow ---- */
    UiId header = add(chat_ui, root, UI_ROW, L"");
    chat_ui->header = header;
    height(chat_ui, header, CHAT_HEADER_HEIGHT);
    node(chat_ui, header)->style.padding = CHAT_HEADER_PADDING;
    node(chat_ui, header)->style.gap = CHAT_HEADER_GAP;
    node(chat_ui, header)->style.background = UI_TOOLBAR;

    chat_ui->hamburger = icon_button(chat_ui, header, UI_ICON_HAMBURGER,
        L"Show conversation sidebar", L"Show or hide the conversation list.");
    width(chat_ui, chat_ui->hamburger, CHAT_ICON_BUTTON);
    height(chat_ui, chat_ui->hamburger, CHAT_ICON_BUTTON);

    chat_ui->heading = label(chat_ui, header, L"", UI_TITLE, UI_BRIGHT);
    fill_width(chat_ui, chat_ui->heading);
    node(chat_ui, chat_ui->heading)->style.min_w = 40;
    height(chat_ui, chat_ui->heading, 30);

    UiId model_group = add(chat_ui, header, UI_ROW, L"");
    height(chat_ui, model_group, 30);
    width(chat_ui, model_group, (float)(CHAT_MODEL_CHIP_WIDTH + 4 +
        CHAT_MODEL_CHEVRON_WIDTH));
    node(chat_ui, model_group)->style.gap = 4;
    node(chat_ui, model_group)->style.padding = 0;

    chat_ui->model = surface(chat_ui, model_group, UI_COLUMN, L"Model",
        UI_TRACK);
    width(chat_ui, chat_ui->model, CHAT_MODEL_CHIP_WIDTH);
    height(chat_ui, chat_ui->model, 30);
    ui_set_help_text(ui, chat_ui->model,
        L"Type any model identifier for the active backend, or browse the catalog.");

    chat_ui->model_picker = icon_button(chat_ui, model_group,
        UI_ICON_CHEVRON_DOWN, L"Choose model",
        L"Browse the model catalog for the active backend (Ctrl+Space).");
    width(chat_ui, chat_ui->model_picker, CHAT_MODEL_CHEVRON_WIDTH);
    height(chat_ui, chat_ui->model_picker, 30);

    chat_ui->overflow = icon_button(chat_ui, header, UI_ICON_OVERFLOW,
        L"More actions", L"Conversation, response and settings commands.");
    width(chat_ui, chat_ui->overflow, CHAT_ICON_BUTTON);
    height(chat_ui, chat_ui->overflow, CHAT_ICON_BUTTON);

    height(chat_ui, add(chat_ui, root, UI_SEPARATOR, L""), 1);

    UiId body = add(chat_ui, root, UI_ROW, L"");
    fill_height(chat_ui, body);
    node(chat_ui, body)->style.gap = 0;

    /* ---- Sidebar: New, search, windowed list, footer ---- */
    UiId sidebar = add(chat_ui, body, UI_COLUMN, L"");
    chat_ui->sidebar = sidebar;
    width(chat_ui, sidebar, (float)chat->sidebar_width);
    chat_ui->last_sidebar_width = (float)chat->sidebar_width;
    fill_height(chat_ui, sidebar);
    node(chat_ui, sidebar)->style.padding = CHAT_SIDEBAR_PADDING;
    node(chat_ui, sidebar)->style.gap = CHAT_SIDEBAR_GAP;
    node(chat_ui, sidebar)->style.background = UI_PANEL;

    chat_ui->new_conversation = add(chat_ui, sidebar, UI_BUTTON,
        L"New conversation");
    fill_width(chat_ui, chat_ui->new_conversation);
    height(chat_ui, chat_ui->new_conversation, CHAT_ROW_ACTION_HEIGHT);
    ui_set_accessible_name(ui, chat_ui->new_conversation, L"New conversation");

    UiId search = surface(chat_ui, sidebar, UI_ROW, L"Search conversations",
        UI_TRACK);
    chat_ui->search = search;
    height(chat_ui, search, CHAT_SEARCH_HEIGHT);
    node(chat_ui, search)->style.gap = 0;
    ui_set_help_text(ui, search,
        L"Enter searches message text and reasoning. F3 moves to the next result.");
    UiId search_icon = add(chat_ui, search, UI_ICON, L"");
    width(chat_ui, search_icon, 26);
    fill_height(chat_ui, search_icon);
    node(chat_ui, search_icon)->icon = UI_ICON_SEARCH;
    node(chat_ui, search_icon)->style.foreground = UI_FAINT;

    chat_ui->search_status = label(chat_ui, sidebar,
        L"Enter to search messages and reasoning", UI_SMALL, UI_FAINT);
    fill_width(chat_ui, chat_ui->search_status);
    height(chat_ui, chat_ui->search_status, CHAT_SEARCH_STATUS_HEIGHT);

    chat_ui->list = add(chat_ui, sidebar, UI_SCROLL, L"");
    fill_width(chat_ui, chat_ui->list);
    fill_height(chat_ui, chat_ui->list);
    node(chat_ui, chat_ui->list)->style.padding = 0;
    node(chat_ui, chat_ui->list)->style.gap = CHAT_SIDEBAR_ROW_GAP;
    ui_set_accessible_name(ui, chat_ui->list, L"Conversations");
    /* The windowed row pool starts empty; chat_ui_sidebar_remap grows it to
       exactly the visible window. The spacers sandwich the rows so the laid
       out extent always covers the whole list. */
    chat_ui->top_spacer = add(chat_ui, chat_ui->list, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->top_spacer);
    height(chat_ui, chat_ui->top_spacer, 0);
    ui_set_hidden(ui, chat_ui->top_spacer, true);
    chat_ui->bottom_spacer = add(chat_ui, chat_ui->list, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->bottom_spacer);
    height(chat_ui, chat_ui->bottom_spacer, 0);
    ui_set_hidden(ui, chat_ui->bottom_spacer, true);

    chat_ui->footer = label(chat_ui, sidebar, L"", UI_SMALL, UI_FAINT);
    fill_width(chat_ui, chat_ui->footer);
    height(chat_ui, chat_ui->footer, 18);

    /* ---- Main: centered transcript and composer ---- */
    UiId main = add(chat_ui, body, UI_COLUMN, L"");
    chat_ui->main = main;
    fill_width(chat_ui, main);
    fill_height(chat_ui, main);
    node(chat_ui, main)->style.padding = 0;
    node(chat_ui, main)->style.gap = 0;

    chat_ui->transcript = add(chat_ui, main, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->transcript);
    fill_height(chat_ui, chat_ui->transcript);
    node(chat_ui, chat_ui->transcript)->style.padding = 0;
    node(chat_ui, chat_ui->transcript)->style.background = UI_BG;
    ui_set_accessible_name(ui, chat_ui->transcript, L"Transcript");

    /* Empty-state hero, centered horizontally and vertically over the
       full-window transcript background. It is hidden by chat_ui_sync as
       soon as the active conversation has a message, at which point the host
       shows the native transcript container instead. */
    chat_ui->empty = add(chat_ui, chat_ui->transcript, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->empty);
    fill_height(chat_ui, chat_ui->empty);
    node(chat_ui, chat_ui->empty)->style.padding = 24;
    node(chat_ui, chat_ui->empty)->style.gap = 0;
    node(chat_ui, chat_ui->empty)->style.background = UI_BG;
    ui_set_accessible_name(ui, chat_ui->empty, L"Empty conversation");
    UiId empty_top = add(chat_ui, chat_ui->empty, UI_COLUMN, L"");
    fill_width(chat_ui, empty_top);
    fill_height(chat_ui, empty_top);
    UiId empty_center = add(chat_ui, chat_ui->empty, UI_ROW, L"");
    fill_width(chat_ui, empty_center);
    node(chat_ui, empty_center)->style.gap = 0;
    UiId empty_left = add(chat_ui, empty_center, UI_COLUMN, L"");
    fill_width(chat_ui, empty_left);
    UiId hero = add(chat_ui, empty_center, UI_COLUMN, L"");
    /* A large flex weight lets the hero claim up to its cap first; the side
       spacers then split only the remainder, which centers it. An equal
       flex split would cap it at a third of the row instead. */
    node(chat_ui, hero)->style.width = ui_flex(50);
    node(chat_ui, hero)->style.max_w = CHAT_EMPTY_HERO_MAX_WIDTH;
    node(chat_ui, hero)->style.gap = 8;
    node(chat_ui, hero)->style.padding = 0;
    chat_ui->empty_title = label(chat_ui, hero, L"Start a conversation",
        UI_HEADING, UI_BRIGHT);
    fill_width(chat_ui, chat_ui->empty_title);
    node(chat_ui, chat_ui->empty_title)->style.text_centered = true;
    height(chat_ui, chat_ui->empty_title, 32);
    chat_ui->empty_hint = label(chat_ui, hero,
        L"Type a message below to begin. Ctrl+Space chooses a model.",
        UI_BODY, UI_MUTED);
    fill_width(chat_ui, chat_ui->empty_hint);
    node(chat_ui, chat_ui->empty_hint)->style.text_centered = true;
    height(chat_ui, chat_ui->empty_hint, 22);
    UiId empty_right = add(chat_ui, empty_center, UI_COLUMN, L"");
    fill_width(chat_ui, empty_right);
    UiId empty_bottom = add(chat_ui, chat_ui->empty, UI_COLUMN, L"");
    fill_width(chat_ui, empty_bottom);
    fill_height(chat_ui, empty_bottom);

    chat_ui->composer_area = add(chat_ui, main, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->composer_area);
    height(chat_ui, chat_ui->composer_area, CHAT_COMPOSER_AREA_HEIGHT);
    node(chat_ui, chat_ui->composer_area)->style.padding = 6;
    node(chat_ui, chat_ui->composer_area)->style.gap = 6;

    UiId composer_row = add(chat_ui, chat_ui->composer_area, UI_ROW, L"");
    fill_width(chat_ui, composer_row);
    fill_height(chat_ui, composer_row);
    node(chat_ui, composer_row)->style.gap = 0;

    UiId composer_left = add(chat_ui, composer_row, UI_COLUMN, L"");
    fill_width(chat_ui, composer_left);
    UiId card = surface(chat_ui, composer_row, UI_ROW, L"Composer",
        UI_TRACK);
    chat_ui->composer_card = card;
    node(chat_ui, card)->style.width = ui_flex(50);
    node(chat_ui, card)->style.max_w = CHAT_CONTENT_WIDTH_DIPS;
    fill_height(chat_ui, card);
    node(chat_ui, card)->style.padding = 6;
    node(chat_ui, card)->style.gap = 6;
    chat_ui->composer = add(chat_ui, card, UI_COLUMN, L"");
    fill_width(chat_ui, chat_ui->composer);
    fill_height(chat_ui, chat_ui->composer);
    node(chat_ui, chat_ui->composer)->style.padding = 0;
    node(chat_ui, chat_ui->composer)->style.background = -1;
    node(chat_ui, chat_ui->composer)->style.border = false;
    ui_set_accessible_name(ui, chat_ui->composer, L"Message");

    UiId send_wrap = add(chat_ui, card, UI_COLUMN, L"");
    width(chat_ui, send_wrap, CHAT_SEND_BUTTON);
    fill_height(chat_ui, send_wrap);
    node(chat_ui, send_wrap)->style.gap = 0;
    node(chat_ui, send_wrap)->style.padding = 0;
    UiId send_top = add(chat_ui, send_wrap, UI_COLUMN, L"");
    fill_width(chat_ui, send_top);
    fill_height(chat_ui, send_top);
    /* The primary action and the reasoning toggle stack as one centered
       group: Send on top, the brain directly below it. */
    UiId send_group = add(chat_ui, send_wrap, UI_COLUMN, L"");
    fill_width(chat_ui, send_group);
    node(chat_ui, send_group)->style.gap = 6;
    node(chat_ui, send_group)->style.padding = 0;
    chat_ui->send = icon_button(chat_ui, send_group, UI_ICON_SEND,
        L"Send message", L"Send the message. Shift+Enter inserts a newline.");
    width(chat_ui, chat_ui->send, CHAT_SEND_BUTTON);
    height(chat_ui, chat_ui->send, CHAT_SEND_BUTTON);
    node(chat_ui, chat_ui->send)->selected = true;
    chat_ui->reasoning = icon_button(chat_ui, send_group, UI_ICON_BRAIN,
        L"Reasoning on", L"Turn reasoning on or off for this conversation (OpenRouter).");
    width(chat_ui, chat_ui->reasoning, CHAT_SEND_BUTTON);
    height(chat_ui, chat_ui->reasoning, CHAT_SEND_BUTTON);
    UiId send_bottom = add(chat_ui, send_wrap, UI_COLUMN, L"");
    fill_width(chat_ui, send_bottom);
    fill_height(chat_ui, send_bottom);
    UiId composer_right = add(chat_ui, composer_row, UI_COLUMN, L"");
    fill_width(chat_ui, composer_right);

    UiId status_row = add(chat_ui, chat_ui->composer_area, UI_ROW, L"");
    fill_width(chat_ui, status_row);
    height(chat_ui, status_row, CHAT_STATUS_HEIGHT);
    node(chat_ui, status_row)->style.gap = 0;
    UiId status_left = add(chat_ui, status_row, UI_COLUMN, L"");
    fill_width(chat_ui, status_left);
    chat_ui->status = label(chat_ui, status_row, L"", UI_SMALL, UI_MUTED);
    node(chat_ui, chat_ui->status)->style.width = ui_flex(50);
    node(chat_ui, chat_ui->status)->style.max_w = CHAT_CONTENT_WIDTH_DIPS;
    node(chat_ui, chat_ui->status)->style.text_centered = true;
    node(chat_ui, chat_ui->status)->style.padding = 0;
    fill_height(chat_ui, chat_ui->status);
    UiId status_right = add(chat_ui, status_row, UI_COLUMN, L"");
    fill_width(chat_ui, status_right);

    ui_set_accessible_name(ui, root, L"DarkChat");
    apply_sidebar_state(chat_ui);
    chat_ui_sync(chat_ui);
    return true;
}

void chat_ui_sync(ChatUi *chat_ui) {
    Chat *chat = chat_ui->chat;
    if ((float)chat->sidebar_width != chat_ui->last_sidebar_width) {
        chat_ui->last_sidebar_width = (float)chat->sidebar_width;
        ui_invalidate(chat_ui->ui, true);
    }
    /* The explicit preference is owned by Chat (and persisted); mirror it so
       an external change (for example a storage load or settings edit) is
       reflected on the next flush. The narrow-width drawer is session state
       and is never overwritten here. */
    chat_ui->sidebar_open = chat->sidebar_collapsed == 0;
    apply_sidebar_state(chat_ui);

    const ChatConversation *active = chat_active(chat);
    ui_set_text(chat_ui->ui, chat_ui->heading,
        active ? active->title : L"DarkChat");
    ui_set_text(chat_ui->ui, chat_ui->status, chat->status);
    bool empty = !active || active->message_count == 0;
    set_hidden(chat_ui, chat_ui->empty, !empty);

    /* The brain button mirrors the active conversation's session-only
       reasoning preference, so switching conversations updates it. */
    chat_ui_set_reasoning(chat_ui, chat_effective_reasoning(chat, active));

    /* The chip shows the effective model of the active conversation. When a
        per-conversation override is active (presence, not value: an override
        equal to the global model still edits independently), a "this chat"
        marker explains that the field below targets this conversation. */
    bool model_here = active &&
        (chat->backend == CHAT_BACKEND_OLLAMA
            ? active->ollama_model[0] : active->model[0]) != 0;
    wchar_t model[UI_TEXT_CAPACITY];
    _snwprintf(model, UI_TEXT_CAPACITY, L"%ls \u00b7 %ls%ls",
        chat_backend_name(chat->backend), chat_effective_model(chat, active),
        model_here ? L" \u00b7 this chat" : L"");
    model[UI_TEXT_CAPACITY - 1] = 0;
    wchar_t accessible[UI_TEXT_CAPACITY];
    _snwprintf(accessible, UI_TEXT_CAPACITY, L"Model: %ls%ls",
        chat_effective_model(chat, active),
        model_here ? L" (set for this chat)" : L"");
    accessible[UI_TEXT_CAPACITY - 1] = 0;
    ui_set_accessible_name(chat_ui->ui, chat_ui->model, accessible);
    ui_set_help_text(chat_ui->ui, chat_ui->model, model);

    wchar_t footer[UI_TEXT_CAPACITY];
    _snwprintf(footer, UI_TEXT_CAPACITY, L"%ls \u00b7 Ctrl+Space for models",
        chat_backend_name(chat->backend));
    footer[UI_TEXT_CAPACITY - 1] = 0;
    ui_set_text(chat_ui->ui, chat_ui->footer, footer);
}

void chat_ui_set_focus_ring(ChatUi *chat_ui, UiId id, bool focused) {
    UiNode *item = node(chat_ui, id);
    if (!item || item->selected == focused) return;
    item->selected = focused;
    if (id == chat_ui->composer_card) chat_ui->composer_focused = focused;
    ui_invalidate(chat_ui->ui, false);
}

/* Records a title change for the report. Entries are deduplicated by row:
   a row that rebinds or renames twice within one flush keeps the first
   (pre-flush) old title, and the host reads the final name at raise time. */
static void report_name_changed(ChatUiRemapReport *report, UiId id,
    const wchar_t *old_title) {
    if (!report) return;
    for (int i = 0; i < report->name_changed_count; i++)
        if (report->name_changed[i].id == id) return;
    if (report->name_changed_count >= CHAT_MAX_CONVERSATIONS) return;
    wcsncpy(report->name_changed[report->name_changed_count].old_title,
        old_title, CHAT_TITLE_TEXT);
    report->name_changed[report->name_changed_count].id = id;
    ++report->name_changed_count;
}

void chat_ui_remap_report_clear(ChatUiRemapReport *report) {
    if (report) memset(report, 0, sizeof *report);
}

bool chat_ui_sidebar_remap(ChatUi *chat_ui, ChatUiRemapReport *report) {
    Chat *chat = chat_ui->chat;
    int count = chat->conversation_count;
    int old_pool = chat_ui->window_valid ? chat_ui->pool_count : 0;
    int old_offset = chat_ui->window_valid ? chat_ui->window_offset : -1;
    int offset, visible;
    window_extent(chat_ui, count, &offset, &visible);

    bool changed = false;
    bool mapping_changed = !chat_ui->window_valid || offset != old_offset;

    /* Grow the pool under the bottom spacer, shrink from the tail; the
       spacer always stays the last list child. */
    while (chat_ui->pool_count < visible) {
        UiId row = add(chat_ui, chat_ui->list, UI_BUTTON, L"");
        if (!row) break; /* UI_CAPACITY headroom is asserted by tests. */
        fill_width(chat_ui, row);
        chat_ui->rows[chat_ui->pool_count++] = row;
        ui_reparent(chat_ui->ui, chat_ui->bottom_spacer, chat_ui->list);
        changed = true;
    }
    while (chat_ui->pool_count > visible) {
        ui_remove(chat_ui->ui, chat_ui->rows[--chat_ui->pool_count]);
        changed = true;
    }
    /* Pool growth and shrink both change the logical row set, independent
       of which bindings happen to differ. */
    if (chat_ui->pool_count != old_pool) {
        mapping_changed = true;
        changed = true;
    }

    float pitch = row_pitch(chat_ui);
    float gap = pitch - chat_ui->ui->theme.control_height;
    int remaining = count - offset - visible;
    bool top_hidden = offset <= 0;
    bool bottom_hidden = remaining <= 0;
    changed |= set_hidden(chat_ui, chat_ui->top_spacer, top_hidden);
    changed |= set_hidden(chat_ui, chat_ui->bottom_spacer, bottom_hidden);
    if (!top_hidden)
        changed |= set_spacer_height(chat_ui, chat_ui->top_spacer,
            offset * pitch - gap);
    if (!bottom_hidden)
        changed |= set_spacer_height(chat_ui, chat_ui->bottom_spacer,
            (float)remaining * pitch - gap);

    UiId focus = chat_ui->ui->focus;
    for (int j = 0; j < chat_ui->pool_count; j++) {
        const ChatConversation *conversation =
            &chat->conversations[offset + j];
        uint64_t tag = conversation->id;
        const wchar_t *title = conversation->title;
        bool known = chat_ui->window_valid && j < old_pool;
        uint64_t old_tag = known ? chat_ui->row_tags[j] : 0;
        const wchar_t *old_title =
            known ? chat_ui->row_titles[j] : L"";

        /* Name changes are reported by title text alone: a rebinding with an
           identical title raises no NamePropertyChanged, a rename with an
           unchanged binding still does. */
        if (known && wcscmp(old_title, title) != 0)
            report_name_changed(report, chat_ui->rows[j], old_title);
        if (!known || old_tag != tag) mapping_changed = true;
        if (report && known && old_tag != tag && focus == chat_ui->rows[j])
            report->focus_binding_changed = true;

        if (!known || old_tag != tag) {
            UiNode *item = node(chat_ui, chat_ui->rows[j]);
            if (item) item->tag = (uintptr_t)tag;
        }
        changed |= row_set_title(chat_ui, chat_ui->rows[j], title);
        UiNode *item = node(chat_ui, chat_ui->rows[j]);
        if (item) {
            bool selected = offset + j == chat->active;
            if (item->selected != selected) {
                item->selected = selected;
                ui_invalidate(chat_ui->ui, false);
                changed = true;
            }
        }
        chat_ui->row_tags[j] = tag;
        wcsncpy(chat_ui->row_titles[j], title, CHAT_TITLE_TEXT);
    }
    if (report) report->mapping_changed |= mapping_changed;

    changed |= set_disabled(chat_ui, chat_ui->new_conversation,
        count >= CHAT_MAX_CONVERSATIONS);

    chat_ui->window_offset = offset;
    chat_ui->window_rows = visible;
    chat_ui->window_valid = true;
    return changed;
}

void chat_ui_request_reveal(ChatUi *chat_ui, uint64_t conversation_id) {
    chat_ui->pending_reveal = conversation_id;
}

bool chat_ui_apply_reveal(ChatUi *chat_ui) {
    if (!chat_ui->pending_reveal) return false;
    Chat *chat = chat_ui->chat;
    uint64_t id = chat_ui->pending_reveal;
    int index = chat_index_of_id(chat, id);
    if (index < 0) { chat_ui->pending_reveal = 0; return false; }
    float pitch = row_pitch(chat_ui);
    float viewport = ui_scroll_viewport_h(chat_ui->ui, chat_ui->list);
    if (pitch <= 0 || viewport <= 0) return false; /* no extent yet: retry */
    chat_ui->pending_reveal = 0;
    /* Visibility is geometry, not pool membership: the pool carries one
       overscan row that may sit outside the viewport or only partly in it. */
    float row_top = index * pitch;
    float row_bottom = row_top + chat_ui->ui->theme.control_height;
    float scroll = ui_scroll_offset(chat_ui->ui, chat_ui->list);
    if (row_top >= scroll && row_bottom <= scroll + viewport) return false;
    /* Minimal move: top-align above the viewport, bottom-align below using
       the row's own bottom edge (excluding the trailing gap). */
    float target = row_top < scroll ? row_top : row_bottom - viewport;
    ui_scroll_to(chat_ui->ui, chat_ui->list, target);
    ui_layout(chat_ui->ui, chat_ui->ui->width, chat_ui->ui->height);
    return true;
}

void chat_ui_set_generation(ChatUi *chat_ui, bool generating, bool stopping) {
    ui_set_icon(chat_ui->ui, chat_ui->send, generating ? UI_ICON_STOP :
        UI_ICON_SEND);
    ui_set_accessible_name(chat_ui->ui, chat_ui->send,
        stopping ? L"Stopping generation" :
        generating ? L"Stop generation" : L"Send message");
    ui_set_help_text(chat_ui->ui, chat_ui->send,
        stopping ? L"Waiting for the provider to acknowledge the stop." :
        generating ? L"Stop the response; partial text is kept." :
        L"Send the message. Shift+Enter inserts a newline.");
    ui_set_disabled(chat_ui->ui, chat_ui->send, stopping);
    ui_invalidate(chat_ui->ui, false);
}

void chat_ui_set_reasoning(ChatUi *chat_ui, bool enabled) {
    UiNode *item = node(chat_ui, chat_ui->reasoning);
    if (!item) return;
    UiColorRole foreground = enabled ? UI_BRIGHT : UI_FAINT;
    if (item->selected == enabled && item->style.foreground == foreground)
        return;
    item->selected = enabled;
    item->style.foreground = foreground;
    ui_set_accessible_name(chat_ui->ui, chat_ui->reasoning,
        enabled ? L"Reasoning on" : L"Reasoning off");
    ui_set_help_text(chat_ui->ui, chat_ui->reasoning,
        enabled ? L"Reasoning is on for this conversation. Click to turn it off (OpenRouter)."
                : L"Reasoning is off for this conversation. Click to turn it on (OpenRouter).");
    ui_invalidate(chat_ui->ui, false);
}

void chat_ui_set_search_status(ChatUi *chat_ui, const wchar_t *text) {
    ui_set_text(chat_ui->ui, chat_ui->search_status, text ? text : L"");
    ui_invalidate(chat_ui->ui, false);
}

bool chat_ui_resize(ChatUi *chat_ui, float width, float height) {
    bool narrow = width < CHAT_UI_SIDEBAR_BREAKPOINT;
    bool state_changed = narrow != chat_ui->narrow;
    if (state_changed) {
        chat_ui->narrow = narrow;
        /* Crossing into or out of the narrow mode always starts with the
           temporary drawer closed; the explicit preference is untouched and
           applies again at wide widths. */
        chat_ui->drawer_open = false;
        apply_sidebar_state(chat_ui);
    }
    if (!state_changed && width == chat_ui->last_layout_width &&
        height == chat_ui->last_layout_height &&
        !ui_layout_pending(chat_ui->ui)) return false;
    chat_ui->last_layout_width = width;
    chat_ui->last_layout_height = height;
    ui_layout(chat_ui->ui, width, height);
    return true;
}

void chat_ui_event(void *user, Ui *ui, UiEvent event) {
    ChatUi *chat_ui = (ChatUi *)user;
    ChatCommandFn command = chat_ui->command;
    if (!command) return;
    if (event.id == chat_ui->send) {
        command(chat_ui->command_user, CHAT_COMMAND_SEND, -1);
    } else if (event.id == chat_ui->new_conversation) {
        command(chat_ui->command_user, CHAT_COMMAND_NEW_CONVERSATION, -1);
    } else if (event.id == chat_ui->hamburger) {
        command(chat_ui->command_user, CHAT_COMMAND_TOGGLE_SIDEBAR, -1);
    } else if (event.id == chat_ui->overflow) {
        command(chat_ui->command_user, CHAT_COMMAND_OVERFLOW, -1);
    } else if (event.id == chat_ui->model_picker) {
        command(chat_ui->command_user, CHAT_COMMAND_MODEL_PICKER, -1);
    } else if (event.id == chat_ui->reasoning) {
        command(chat_ui->command_user, CHAT_COMMAND_TOGGLE_REASONING, -1);
    } else {
        /* Selection is identity-based: the row's tag holds the stable
           conversation id, resolved to an index only at this instant. */
        for (int j = 0; j < chat_ui->pool_count; j++) {
            if (event.id == chat_ui->rows[j]) {
                UiNode *item = ui_node(ui, event.id);
                int index = item ? chat_index_of_id(chat_ui->chat,
                    (uint64_t)item->tag) : -1;
                if (index >= 0)
                    command(chat_ui->command_user, CHAT_COMMAND_SELECT, index);
                return;
            }
        }
    }
    (void)ui;
}

UiRect chat_ui_rect(const ChatUi *chat_ui, UiId id) {
    UiNode *item = ui_node(chat_ui->ui, id);
    return item ? item->rect : (UiRect){0};
}
