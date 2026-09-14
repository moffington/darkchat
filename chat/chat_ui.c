#include "chat_ui.h"
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
        L"Type any OpenRouter model identifier, for example provider/model, then press Enter.");

    height(chat_ui, add(chat_ui, root, UI_SEPARATOR, L""), 1);

    UiId body = add(chat_ui, root, UI_ROW, L"");
    node(chat_ui, body)->style.height = ui_flex(1);
    node(chat_ui, body)->style.gap = 0;

    UiId sidebar = add(chat_ui, body, UI_COLUMN, L"");
    chat_ui->sidebar = sidebar;
    width(chat_ui, sidebar, (float)chat->sidebar_width);
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
    for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++) {
        chat_ui->conversations[i] = add(chat_ui, chat_ui->list, UI_BUTTON, L"");
        fill_width(chat_ui, chat_ui->conversations[i]);
        ui_set_hidden(ui, chat_ui->conversations[i], true);
    }
    label(chat_ui, sidebar, L"Replies need OPENROUTER_API_KEY", UI_SMALL,
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
    width(chat_ui, chat_ui->sidebar, (float)chat->sidebar_width);
    const ChatConversation *active = chat_active(chat);
    ui_set_text(chat_ui->ui, chat_ui->heading,
        active ? active->title : L"DarkChat");
    ui_set_text(chat_ui->ui, chat_ui->status, chat->status);
    for (int i = 0; i < CHAT_MAX_CONVERSATIONS; i++) {
        UiId id = chat_ui->conversations[i];
        if (i < chat->conversation_count) {
            ui_set_hidden(chat_ui->ui, id, false);
            ui_set_text(chat_ui->ui, id, chat->conversations[i].title);
            UiNode *item = node(chat_ui, id);
            if (item) item->selected = i == chat->active;
        } else {
            ui_set_hidden(chat_ui->ui, id, true);
        }
    }
    ui_set_disabled(chat_ui->ui, chat_ui->new_conversation,
        chat->conversation_count >= CHAT_MAX_CONVERSATIONS);
    ui_invalidate(chat_ui->ui, true);
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

void chat_ui_resize(ChatUi *chat_ui, float width, float height) {
    ui_layout(chat_ui->ui, width, height);
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
        for (int i = 0; i < chat_ui->chat->conversation_count; i++) {
            if (event.id == chat_ui->conversations[i]) {
                command(chat_ui->command_user, CHAT_COMMAND_SELECT, i);
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

