#include "chat_ui.h"
#include <math.h>
#include <string.h>

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

/* A bordered surface that holds nothing; native Rich Edit sits on top. */
static UiId surface(ChatUi *chat_ui, UiId parent, const wchar_t *name,
    UiColorRole background) {
    UiId id = add(chat_ui, parent, UI_COLUMN, L"");
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

bool chat_ui_init(ChatUi *chat_ui, Ui *ui, Chat *chat) {
    memset(chat_ui, 0, sizeof *chat_ui);
    chat_ui->ui = ui;
    chat_ui->chat = chat;

    UiId root = add(chat_ui, UI_NONE, UI_COLUMN, L"");
    chat_ui->root = root;
    node(chat_ui, root)->style.gap = 0;
    node(chat_ui, root)->style.background = UI_BG;

    UiId toolbar = add(chat_ui, root, UI_ROW, L"");
    height(chat_ui, toolbar, 56);
    node(chat_ui, toolbar)->style.padding = 12;
    node(chat_ui, toolbar)->style.gap = 10;
    node(chat_ui, toolbar)->style.background = UI_TOOLBAR;
    UiId brand = label(chat_ui, toolbar, L"D A R K   C H A T", UI_SECTION,
        UI_ACCENT);
    width(chat_ui, brand, 190);
    height(chat_ui, brand, 30);
    chat_ui->heading = label(chat_ui, toolbar, L"", UI_BODY, UI_MUTED);
    fill_width(chat_ui, chat_ui->heading);
    height(chat_ui, chat_ui->heading, 30);
    UiId model_label = label(chat_ui, toolbar, L"MODEL", UI_SMALL, UI_FAINT);
    width(chat_ui, model_label, 48);
    height(chat_ui, model_label, 30);
    chat_ui->model = surface(chat_ui, toolbar, L"Model", UI_TRACK);
    width(chat_ui, chat_ui->model, 280);
    height(chat_ui, chat_ui->model, 30);
    ui_set_help_text(ui, chat_ui->model,
        L"Type any OpenRouter model identifier, for example provider/model, or press Ctrl+Space to browse the model catalog.");

    height(chat_ui, add(chat_ui, root, UI_SEPARATOR, L""), 1);

    UiId body = add(chat_ui, root, UI_ROW, L"");
    node(chat_ui, body)->style.height = ui_flex(1);
    node(chat_ui, body)->style.gap = 0;

    UiId sidebar = add(chat_ui, body, UI_COLUMN, L"");
    chat_ui->sidebar = sidebar;
    width(chat_ui, sidebar, (float)chat->sidebar_width);
    chat_ui->last_sidebar_width = (float)chat->sidebar_width;
    node(chat_ui, sidebar)->style.height = ui_flex(1);
    node(chat_ui, sidebar)->style.padding = 14;
    node(chat_ui, sidebar)->style.gap = 8;
    node(chat_ui, sidebar)->style.background = UI_PANEL;
    height(chat_ui, label(chat_ui, sidebar, L"CONVERSATIONS", UI_SECTION,
        UI_ACCENT), 26);
    chat_ui->search = surface(chat_ui, sidebar, L"Search conversations",
        UI_TRACK);
    height(chat_ui, chat_ui->search, 30);
    ui_set_help_text(ui, chat_ui->search,
        L"Enter searches message text and reasoning. F3 moves to the next result.");
    chat_ui->search_status = label(chat_ui, sidebar,
        L"Enter to search messages and reasoning", UI_SMALL, UI_FAINT);
    fill_width(chat_ui, chat_ui->search_status);
    height(chat_ui, chat_ui->search_status, 34);
    chat_ui->new_conversation = add(chat_ui, sidebar, UI_BUTTON,
        L"New conversation");
    fill_width(chat_ui, chat_ui->new_conversation);
    chat_ui->list = add(chat_ui, sidebar, UI_SCROLL, L"");
    fill_width(chat_ui, chat_ui->list);
    node(chat_ui, chat_ui->list)->style.height = ui_flex(1);
    node(chat_ui, chat_ui->list)->style.padding = 0;
    node(chat_ui, chat_ui->list)->style.gap = 6;
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
    label(chat_ui, sidebar, L"OpenRouter needs OPENROUTER_API_KEY", UI_SMALL,
        UI_FAINT);

    UiId main = add(chat_ui, body, UI_COLUMN, L"");
    fill_width(chat_ui, main);
    node(chat_ui, main)->style.height = ui_flex(1);
    node(chat_ui, main)->style.padding = 16;
    node(chat_ui, main)->style.gap = 10;

    chat_ui->transcript = surface(chat_ui, main, L"Transcript", UI_BG);
    node(chat_ui, chat_ui->transcript)->style.height = ui_flex(1);

    UiId compose_row = add(chat_ui, main, UI_ROW, L"");
    fill_width(chat_ui, compose_row);
    height(chat_ui, compose_row, 96);
    node(chat_ui, compose_row)->style.gap = 8;
    chat_ui->composer = surface(chat_ui, compose_row, L"Composer", UI_TRACK);
    node(chat_ui, chat_ui->composer)->style.height = ui_flex(1);
    chat_ui->send = add(chat_ui, compose_row, UI_BUTTON, L"Send");
    width(chat_ui, chat_ui->send, 104);
    node(chat_ui, chat_ui->send)->style.height = ui_flex(1);
    node(chat_ui, chat_ui->send)->selected = true;
    ui_set_accessible_name(ui, chat_ui->send, L"Send message");

    chat_ui->status = label(chat_ui, main, L"", UI_SMALL, UI_MUTED);
    fill_width(chat_ui, chat_ui->status);
    height(chat_ui, chat_ui->status, 20);

    ui_set_accessible_name(ui, root, L"DarkChat");
    chat_ui_sync(chat_ui);
    return true;
}

void chat_ui_sync(ChatUi *chat_ui) {
    Chat *chat = chat_ui->chat;
    if ((float)chat->sidebar_width != chat_ui->last_sidebar_width) {
        chat_ui->last_sidebar_width = (float)chat->sidebar_width;
        width(chat_ui, chat_ui->sidebar, (float)chat->sidebar_width);
        ui_invalidate(chat_ui->ui, true);
    }
    const ChatConversation *active = chat_active(chat);
    ui_set_text(chat_ui->ui, chat_ui->heading,
        active ? active->title : L"DarkChat");
    ui_set_text(chat_ui->ui, chat_ui->status, chat->status);
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
    ui_set_text(chat_ui->ui, chat_ui->send,
        stopping ? L"Stopping\u2026" : generating ? L"Stop" : L"Send");
    ui_set_accessible_name(chat_ui->ui, chat_ui->send,
        stopping ? L"Stopping generation" :
        generating ? L"Stop generation" : L"Send message");
    ui_set_disabled(chat_ui->ui, chat_ui->send, stopping);
    ui_invalidate(chat_ui->ui, false);
}

void chat_ui_set_search_status(ChatUi *chat_ui, const wchar_t *text) {
    ui_set_text(chat_ui->ui, chat_ui->search_status, text ? text : L"");
    ui_invalidate(chat_ui->ui, false);
}

bool chat_ui_resize(ChatUi *chat_ui, float width, float height) {
    if (width == chat_ui->last_layout_width &&
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
