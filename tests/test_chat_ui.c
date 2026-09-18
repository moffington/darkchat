#include "../chat/chat_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

static ChatCommand last_command = CHAT_COMMAND_NONE;
static int last_index = -1;
static unsigned commands;
static void recorder(void *user, ChatCommand command, int index) {
    (void)user;
    last_command = command;
    last_index = index;
    ++commands;
}

static float pitch_of(const ChatUi *chat_ui) {
    return chat_ui->ui->theme.control_height +
        ui_node(chat_ui->ui, chat_ui->list)->style.gap;
}

/* Pool size the remap must derive: bounded by the viewport and the count. */
static int expected_pool(const ChatUi *chat_ui, int count) {
    float pitch = pitch_of(chat_ui);
    float viewport = ui_scroll_viewport_h(chat_ui->ui, chat_ui->list);
    int needed = viewport > 0 ? (int)ceilf(viewport / pitch) + 1 : 1;
    return count < needed ? count : needed;
}

/* Fresh-report remap: the report accumulates across remaps within one
   flush, so a test inspecting a single remap clears it first. */
static bool remap_fresh(ChatUi *chat_ui, ChatUiRemapReport *report) {
    chat_ui_remap_report_clear(report);
    return chat_ui_sidebar_remap(chat_ui, report);
}

/* Paint-dirty observation: painting consumes paint_dirty, so a later no-op
   remap must leave it cleared. */
typedef struct { unsigned fills; } PaintCounter;
static void count_fill(void *user, UiRect r, UiColor c, float radius) {
    (void)user; (void)r; (void)c; (void)radius;
}
static void count_stroke(void *user, UiRect r, UiColor c, float radius,
    float width) { (void)user; (void)r; (void)c; (void)radius; (void)width; }
static void count_text(void *user, UiRect r, const wchar_t *s, UiFont f,
    UiColor c, bool centered) { (void)user; (void)r; (void)s; (void)f; (void)c; (void)centered; }
static void count_line(void *user, float x, float y, float xx, float yy,
    UiColor c, float width) { (void)user; (void)x; (void)y; (void)xx; (void)yy; (void)c; (void)width; }
static void count_push(void *user, UiRect r) { PaintCounter *p = user; (void)r; ++p->fills; }
static void count_pop(void *user) { (void)user; }
static void consume_paint(Ui *ui) {
    PaintCounter counter = {0};
    UiPainter painter = {&counter, count_fill, count_stroke, count_text,
        count_line, count_push, count_pop, 0};
    ui_paint(ui, &painter);
}

int main(void) {
    Ui *ui = (Ui *)calloc(1, sizeof *ui);
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    ChatUi *chat_ui = (ChatUi *)calloc(1, sizeof *chat_ui);
    if (!ui || !chat || !chat_ui) return 2;
    ui_init(ui, NULL, NULL);
    chat_init(chat);
    check(chat_ui_init(chat_ui, ui, chat), "chat UI initializes");
    chat_ui->command = recorder;
    chat_ui->command_user = NULL;
    ui->on_event = chat_ui_event;
    ui->event_user = chat_ui;
    check(chat_ui_resize(chat_ui, 1100, 720), "first resize lays out");
    UiNode *model = ui_node(ui, chat_ui->model);
    check(model && model->rect.w >= 200 && model->rect.h >= 29,
        "model editor placeholder remains visible");
    UiNode *search = ui_node(ui, chat_ui->search);
    check(search && search->rect.w > 100 && search->rect.h >= 29,
        "conversation search placeholder remains visible");

    float pitch = pitch_of(chat_ui);
    check(fabsf(pitch - (ui->theme.control_height + 2.0f)) < .01f,
        "row pitch is control height plus gap");
    float gap = ui_node(ui, chat_ui->list)->style.gap;
    UiId list = chat_ui->list;

    /* First remap builds exactly one row for one conversation. */
    ChatUiRemapReport report;
    check(remap_fresh(chat_ui, &report), "first remap changes state");
    check(report.mapping_changed && !report.focus_binding_changed &&
        report.name_changed_count == 0,
        "the first remap reports a pool-membership change only");
    check(chat_ui->pool_count == 1 && chat_ui->window_offset == 0,
        "the pool is sized to the conversation count");
    UiNode *row0 = ui_node(ui, chat_ui->rows[0]);
    check(row0 && !row0->hidden &&
        row0->tag == (uintptr_t)chat->conversations[0].id,
        "the row tag carries the stable conversation id");
    check(row0 && !wcscmp(row0->text, chat->conversations[0].title),
        "the row shows the conversation title");
    check(row0 && row0->selected, "the active conversation is selected");
    check(ui_node(ui, chat_ui->top_spacer)->hidden &&
        ui_node(ui, chat_ui->bottom_spacer)->hidden,
        "both spacers hide when the list fits");
    ui_layout(ui, 1100, 720);
    check(fabsf(ui_node(ui, list)->content_height - (pitch - gap)) < .01f,
        "the extent covers exactly one conversation");

    /* A no-op remap mutates and invalidates nothing. */
    ui_layout(ui, 1100, 720);
    consume_paint(ui);
    check(!remap_fresh(chat_ui, &report), "a no-op remap is detected");
    check(!report.mapping_changed && report.name_changed_count == 0,
        "a no-op remap reports nothing");
    check(!ui_layout_pending(ui), "a no-op remap never dirties layout");
    check(!ui->paint_dirty, "a no-op remap never dirties paint");

    /* Grow to 30 conversations: the pool tracks the viewport, the spacers
       pad the extent to the full list height. */
    for (int i = 1; i < 30; i++) chat_new_conversation(chat);
    int needed = expected_pool(chat_ui, 30);
    check(remap_fresh(chat_ui, &report) && report.mapping_changed,
        "conversation growth remaps the window");
    check(chat_ui->pool_count == needed && chat_ui->window_rows == needed,
        "the pool is bounded by the visible window, not the count");
    ui_layout(ui, 1100, 720);
    check(fabsf(ui_node(ui, list)->content_height - (30 * pitch - gap)) < .05f,
        "the extent covers the whole list");
    for (int j = 0; j < chat_ui->pool_count; j++)
        check(ui_node(ui, chat_ui->rows[j])->tag ==
            (uintptr_t)chat->conversations[chat_ui->window_offset + j].id,
            "row j binds to conversation offset+j by id");

    /* Windowing matrix across offsets: exact extent, spacers, positions. */
    int max_offset = 30 - needed;
    int offsets[3] = {0, 1, max_offset};
    int previous_offset = chat_ui->window_offset;
    for (int o = 0; o < 3; o++) {
        ui_scroll_to(ui, list, offsets[o] * pitch);
        bool changed = remap_fresh(chat_ui, &report);
        check(changed == (offsets[o] != previous_offset),
            "remap reports a change exactly when the window moved");
        check(chat_ui->window_offset == offsets[o],
            "the offset is derived from the scroll position");
        previous_offset = offsets[o];
        ui_layout(ui, 1100, 720);
        check(fabsf(ui_node(ui, list)->content_height - (30 * pitch - gap)) < .05f,
            "the extent is independent of the offset");
        int remaining = 30 - offsets[o] - needed;
        check(ui_node(ui, chat_ui->top_spacer)->hidden == (offsets[o] == 0),
            "the top spacer hides exactly at offset zero");
        check(ui_node(ui, chat_ui->bottom_spacer)->hidden == (remaining <= 0),
            "the bottom spacer hides at the last window");
        if (offsets[o] > 0)
            check(fabsf(ui_node(ui, chat_ui->top_spacer)->style.height.value -
                (offsets[o] * pitch - gap)) < .01f,
                "the top spacer height lands row zero on its slot");
        if (remaining > 0)
            check(fabsf(ui_node(ui, chat_ui->bottom_spacer)->style.height.value -
                ((float)remaining * pitch - gap)) < .01f,
                "the bottom spacer height completes the extent");
        float scroll = ui_scroll_offset(ui, list);
        float viewport_y = ui_node(ui, list)->viewport.y;
        for (int j = 0; j < chat_ui->pool_count; j++)
            check(fabsf(ui_node(ui, chat_ui->rows[j])->rect.y -
                (viewport_y - scroll + (offsets[o] + j) * pitch)) < .05f,
                "row j sits at its full-list position");
    }
    ui_scroll_to(ui, list, 0);
    remap_fresh(chat_ui, &report);

    /* Identity dispatch: activating a row selects the conversation its tag
       carries, resolved at activation time. */
    commands = 0;
    int expected = chat_ui->window_offset + 2;
    check(ui_invoke(ui, chat_ui->rows[2]), "a row invokes");
    check(commands == 1 && last_command == CHAT_COMMAND_SELECT &&
        last_index == expected,
        "row activation selects by stable id");
    /* After a deletion shifts every index, the same row still selects the
       conversation it now displays - never a stale index. */
    uint64_t tag_before = (uint64_t)ui_node(ui, chat_ui->rows[2])->tag;
    chat->active = 0;
    check(chat_delete(chat), "the first conversation deletes");
    check(remap_fresh(chat_ui, &report) && report.mapping_changed,
        "deletion remaps the window");
    uint64_t tag_after = (uint64_t)ui_node(ui, chat_ui->rows[2])->tag;
    check(tag_before != tag_after, "the row rebinds after deletion");
    commands = 0;
    check(ui_invoke(ui, chat_ui->rows[2]), "the rebound row invokes");
    check(commands == 1 && last_command == CHAT_COMMAND_SELECT &&
        last_index == chat_index_of_id(chat, tag_after),
        "activation resolves the displayed conversation, not a stale index");

    /* Scrolling never snaps back to the active row. */
    int count = chat->conversation_count;
    ui_scroll_to(ui, list, 1e6f);
    needed = expected_pool(chat_ui, count);
    remap_fresh(chat_ui, &report);
    check(chat_ui->window_offset == count - needed,
        "the window rests at the bottom of the list");
    check(!chat_ui_apply_reveal(chat_ui),
        "no reveal fires without a pending request");
    check(chat_ui->window_offset == count - needed,
        "an ordinary scroll never snaps back to the active row");

    /* Reveal: explicit request moves the window minimally; in-window and
       unresolvable requests are no-ops. */
    chat_ui_request_reveal(chat_ui, chat->conversations[0].id);
    check(chat_ui_apply_reveal(chat_ui), "an out-of-window reveal scrolls");
    check(remap_fresh(chat_ui, &report),
        "the revealed window remaps");
    check(chat_ui->window_offset == 0 &&
        ui_node(ui, chat_ui->rows[0])->tag ==
            (uintptr_t)chat->conversations[0].id,
        "the revealed conversation is bound and selected at the top");
    chat_ui_request_reveal(chat_ui, chat->conversations[0].id);
    check(!chat_ui_apply_reveal(chat_ui),
        "an in-window reveal is a no-op");
    check(chat_ui->pending_reveal == 0, "the reveal request is consumed");
    chat_ui_request_reveal(chat_ui, 1234567890123456789ULL);
    check(!chat_ui_apply_reveal(chat_ui),
        "an unresolvable reveal drops silently");

    /* Reveal visibility is geometry, not pool membership: the second-to-last
       pool row is the partially visible overscan and must still reveal. */
    count = chat->conversation_count;
    needed = expected_pool(chat_ui, count);
    ui_scroll_to(ui, list, 0);
    remap_fresh(chat_ui, &report);
    {
        int probe = needed - 2;
        float viewport = ui_scroll_viewport_h(ui, list);
        float row_top = probe * pitch;
        float row_bottom = row_top + ui->theme.control_height;
        check(row_top < viewport && row_bottom > viewport,
            "the probe row spans the viewport bottom edge");
        chat_ui_request_reveal(chat_ui, chat->conversations[probe].id);
        check(chat_ui_apply_reveal(chat_ui),
            "a partly visible row still reveals");
        check(fabsf(ui_scroll_offset(ui, list) - (row_bottom - viewport)) < .05f,
            "the reveal aligns the row bottom with the viewport bottom");
        remap_fresh(chat_ui, &report);
        check(chat_ui->window_offset == 0,
            "the minimal partial-visibility reveal keeps the offset");
        /* Fully-visible rows reveal as no-ops. */
        chat_ui_request_reveal(chat_ui, chat->conversations[probe].id);
        check(!chat_ui_apply_reveal(chat_ui),
            "a fully visible row does not reveal");
    }

    /* Bottom alignment uses the row's own bottom edge, excluding the
       trailing gap. */
    ui_scroll_to(ui, list, 0);
    remap_fresh(chat_ui, &report);
    {
        int deep = count - 1;
        float viewport = ui_scroll_viewport_h(ui, list);
        chat_ui_request_reveal(chat_ui, chat->conversations[deep].id);
        check(chat_ui_apply_reveal(chat_ui), "a far row reveals");
        float expected = deep * pitch + ui->theme.control_height - viewport;
        check(fabsf(ui_scroll_offset(ui, list) - expected) < .05f,
            "bottom alignment excludes the trailing gap");
        remap_fresh(chat_ui, &report);
        check(chat_ui->window_offset <= deep &&
            chat_ui->window_offset + chat_ui->pool_count > deep,
            "the bottom-aligned target lands in the window");
    }
    ui_scroll_to(ui, list, 0);
    remap_fresh(chat_ui, &report);

    /* Rename: same binding, changed title -> NamePropertyChanged only. */
    ui_layout(ui, 1100, 720);
    consume_paint(ui);
    wchar_t old_title[CHAT_TITLE_TEXT];
    wcscpy(old_title, chat->conversations[0].title);
    check(chat_rename(chat, L"Renamed conversation"), "the conversation renames");
    check(remap_fresh(chat_ui, &report), "a rename remaps");
    check(!report.mapping_changed, "a rename does not change the mapping");
    check(report.name_changed_count == 1 &&
        report.name_changed[0].id == chat_ui->rows[0] &&
        !wcscmp(report.name_changed[0].old_title, old_title),
        "the rename reports the old title for NamePropertyChanged");
    check(!ui_layout_pending(ui) && ui->paint_dirty,
        "a row title change repaints without relayout");
    consume_paint(ui);

    /* The report accumulates across remaps and deduplicates by row: the
       first (pre-flush) old title survives later remaps of the same row. */
    {
        wchar_t first_old[CHAT_TITLE_TEXT];
        wcscpy(first_old, chat->conversations[0].title);
        wcscpy(chat->conversations[0].title, L"Accumulate one");
        check(remap_fresh(chat_ui, &report) && report.name_changed_count == 1,
            "the first remap records the title change");
        wcscpy(chat->conversations[0].title, L"Accumulate two");
        check(chat_ui_sidebar_remap(chat_ui, &report) &&
            report.name_changed_count == 1 &&
            !wcscmp(report.name_changed[0].old_title, first_old) &&
            !wcscmp(ui_node(ui, chat_ui->rows[0])->text, L"Accumulate two"),
            "the second remap merges, keeping the pre-flush old title");
        check(!chat_ui_sidebar_remap(chat_ui, &report),
            "a no-change remap changes nothing");
        check(report.name_changed_count == 1,
            "a no-change remap preserves the accumulated report");
    }

    /* Same-title rebind: a row that now shows a different conversation with
       an identical title raises ChildrenInvalidated but no name event. */
    wcscpy(chat->conversations[1].title, chat->conversations[0].title);
    remap_fresh(chat_ui, &report);
    consume_paint(ui);
    chat->active = 0;
    check(chat_delete(chat), "the top conversation deletes");
    check(remap_fresh(chat_ui, &report) && report.mapping_changed,
        "a same-title rebind changes the mapping");
    bool row0_named = false;
    for (int i = 0; i < report.name_changed_count; i++)
        if (report.name_changed[i].id == chat_ui->rows[0]) row0_named = true;
    check(!row0_named,
        "an identical title raises no NamePropertyChanged on rebind");

    /* Focus follows the node; a scroll rebind of the focused row is
       reported, and removing the focused row drops focus to the root. */
    remap_fresh(chat_ui, &report);
    consume_paint(ui);
    ui_focus(ui, chat_ui->rows[1], true);
    check(ui->focus == chat_ui->rows[1], "a row takes keyboard focus");
    ui_scroll_to(ui, list, ui_scroll_offset(ui, list) + pitch);
    check(remap_fresh(chat_ui, &report) &&
        report.focus_binding_changed,
        "a scroll rebind of the focused row is reported");
    consume_paint(ui);
    UiId focused_row = chat_ui->rows[chat_ui->pool_count - 1];
    ui_focus(ui, focused_row, true);
    while (chat->conversation_count > 2) {
        chat->active = chat->conversation_count - 1;
        chat_delete(chat);
    }
    check(remap_fresh(chat_ui, &report), "the pool shrinks");
    check(chat_ui->pool_count == 2, "the pool tracks the shrunken count");
    check(ui->focus == UI_NONE, "a removed focused row drops focus");

    /* The New button disables exactly at the 128-conversation cap. */
    while (chat->conversation_count < CHAT_MAX_CONVERSATIONS)
        chat_new_conversation(chat);
    check(remap_fresh(chat_ui, &report),
        "the cap remaps the window");
    check(ui_node(ui, chat_ui->new_conversation)->disabled,
        "the New button disables at the cap");
    needed = expected_pool(chat_ui, CHAT_MAX_CONVERSATIONS);
    check(chat_ui->pool_count == needed,
        "the pool never exceeds the visible window even at the cap");
    check(ui->count <= UI_CAPACITY - 15,
        "the node census stays well below UI_CAPACITY");
    check(chat_delete(chat), "a conversation deletes below the cap");
    check(remap_fresh(chat_ui, &report), "the deletion remaps");
    check(!ui_node(ui, chat_ui->new_conversation)->disabled,
        "deleting below the cap re-enables the New button");

    /* Pool growth and shrink each mark the mapping change on their own,
       even when every surviving row keeps its binding and the offset
       stays put. */
    ui_scroll_to(ui, list, 0);
    remap_fresh(chat_ui, &report);
    while (chat->conversation_count > 10) {
        chat->active = chat->conversation_count - 1;
        chat_delete(chat);
    }
    check(remap_fresh(chat_ui, &report) && report.mapping_changed,
        "a pool shrink alone marks the mapping change");
    check(chat_ui->pool_count == 10, "the pool tracks the shrunken count");
    while (chat->conversation_count < 20) chat_new_conversation(chat);
    check(remap_fresh(chat_ui, &report) && report.mapping_changed,
        "a pool growth alone marks the mapping change");
    check(chat_ui->pool_count == expected_pool(chat_ui, 20),
        "the pool tracks the regrown count");

    /* Resize gating: an unchanged size skips the layout entirely. */
    ui_layout(ui, 1100, 720);
    consume_paint(ui);
    check(!chat_ui_resize(chat_ui, 1100, 720),
        "an unchanged resize is skipped");
    ui_invalidate(ui, true);
    check(chat_ui_resize(chat_ui, 1100, 720),
        "a pending relayout runs on the next resize call");
    check(chat_ui_resize(chat_ui, 720, 480), "a changed size lays out");
    check(model && model->rect.w >= 200 && model->rect.h >= 29,
        "model editor remains visible at the minimum window size");
    check(search && search->rect.w > 100 && search->rect.h >= 29,
        "conversation search remains visible at the minimum window size");

    /* A reveal requested before any extent exists waits for layout. */
    {
        Ui *ui2 = (Ui *)calloc(1, sizeof *ui2);
        Chat *chat2 = (Chat *)calloc(1, sizeof *chat2);
        ChatUi *chat_ui2 = (ChatUi *)calloc(1, sizeof *chat_ui2);
        if (!ui2 || !chat2 || !chat_ui2) return 2;
        ui_init(ui2, NULL, NULL);
        chat_init(chat2);
        for (int i = 1; i < 40; i++) chat_new_conversation(chat2);
        check(chat_ui_init(chat_ui2, ui2, chat2), "a second chat UI initializes");
        chat_ui_request_reveal(chat_ui2, chat2->conversations[39].id);
        check(!chat_ui_apply_reveal(chat_ui2),
            "a reveal before the first layout waits");
        check(chat_ui2->pending_reveal == chat2->conversations[39].id,
            "the pre-layout reveal is parked by id");
        check(chat_ui_resize(chat_ui2, 1100, 720), "the second UI lays out");
        remap_fresh(chat_ui2, &report);
        ui_layout(ui2, 1100, 720);
        check(chat_ui_apply_reveal(chat_ui2),
            "the parked reveal applies once the extent exists");
        remap_fresh(chat_ui2, &report);
        check(chat_ui2->window_offset > 0,
            "the parked reveal scrolled the window to the target");
        free(chat_ui2); chat_dispose(chat2); free(chat2); free(ui2);
    }

    chat_ui_set_search_status(chat_ui, L"2/4 Assistant message: result");
    UiNode *search_status = ui_node(ui, chat_ui->search_status);
    check(search_status && wcsstr(search_status->text, L"2/4") != NULL,
        "search result status updates without rebuilding the UI");
    chat_ui_set_generation(chat_ui, true, false);
    UiNode *send = ui_node(ui, chat_ui->send);
    check(send && send->icon == UI_ICON_STOP && !send->disabled,
        "generation exposes an enabled Stop control");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->send), L"Stop generation"),
        "the Stop control carries its accessible name");
    chat_ui_set_generation(chat_ui, true, true);
    check(send && send->disabled, "stopping disables repeated cancellation");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->send),
        L"Stopping generation"),
        "the stopping state announces itself");
    chat_ui_set_generation(chat_ui, false, false);
    check(send && send->icon == UI_ICON_SEND && !send->disabled,
        "completed generation restores Send");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->send), L"Send message"),
        "the restored Send control carries its accessible name");
    check(ui_node(ui, chat_ui->transcript) != NULL,
        "transcript placeholder remains for the per-turn container");

    /* ---- Redesigned composition: header, sidebar, composer ---- */
    check(chat_ui_resize(chat_ui, 1100, 720),
        "the composition checks lay out at the desktop size");
    UiNode *header = ui_node(ui, chat_ui->header);
    UiNode *hamburger = ui_node(ui, chat_ui->hamburger);
    UiNode *overflow = ui_node(ui, chat_ui->overflow);
    UiNode *heading = ui_node(ui, chat_ui->heading);
    UiNode *sidebar = ui_node(ui, chat_ui->sidebar);
    UiNode *main = ui_node(ui, chat_ui->main);
    UiNode *card = ui_node(ui, chat_ui->composer_card);
    UiNode *composer = ui_node(ui, chat_ui->composer);
    UiNode *status = ui_node(ui, chat_ui->status);
    check(header && hamburger && overflow && heading && sidebar && main &&
        card && composer && status, "the redesigned nodes all exist");
    check(hamburger->rect.x >= header->rect.x &&
        hamburger->rect.x + hamburger->rect.w <= heading->rect.x &&
        model->rect.x >= heading->rect.x + heading->rect.w &&
        overflow->rect.x >= model->rect.x + model->rect.w &&
        overflow->rect.x + overflow->rect.w <= header->rect.x + header->rect.w,
        "header controls never overlap at 1100 DIP");
    check(main->rect.x >= sidebar->rect.x + sidebar->rect.w &&
        card->rect.x >= main->rect.x &&
        card->rect.x + card->rect.w <= main->rect.x + main->rect.w &&
        send->rect.x >= card->rect.x &&
        send->rect.x + send->rect.w <= card->rect.x + card->rect.w &&
        card->rect.y + card->rect.h <= main->rect.y + main->rect.h &&
        status->rect.y >= card->rect.y + card->rect.h &&
        status->rect.y + status->rect.h <=
            main->rect.y + main->rect.h,
        "the transcript and composer stay inside the main column");

    check(!sidebar->hidden &&
        fabsf(sidebar->rect.w - (float)chat->sidebar_width) < .01f,
        "the wide sidebar is expanded at the persisted width");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->hamburger),
        L"Hide conversation sidebar"),
        "the hamburger announces the expanded state");

    /* Wide toggle: flips and persists the explicit preference, removes the
       panel from layout, and hands the reclaimed width to the main column. */
    check(chat_ui_toggle_sidebar(chat_ui), "the wide toggle persists a change");
    check(chat->sidebar_collapsed == 1, "the collapsed preference persists");
    check(sidebar->hidden, "the collapsed sidebar leaves the layout");
    ui_layout(ui, 1100, 720);
    check(main->rect.x == sidebar->rect.x &&
        main->rect.w == 1100 - sidebar->rect.x,
        "the conversation surface reclaims the collapsed width");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->hamburger),
        L"Show conversation sidebar"),
        "the hamburger announces the collapsed state");
    check(chat_ui_toggle_sidebar(chat_ui) && !sidebar->hidden &&
        chat->sidebar_collapsed == 0,
        "expanding again restores and persists the preference");
    ui_layout(ui, 1100, 720);

    /* Reveal: a search-style command restores a collapsed wide sidebar and
       persists the expanded preference. */
    check(chat_ui_toggle_sidebar(chat_ui) && chat->sidebar_collapsed == 1 &&
        sidebar->hidden, "the sidebar collapses again");
    check(chat_ui_reveal_sidebar(chat_ui) && !sidebar->hidden &&
        chat->sidebar_collapsed == 0,
        "revealing a collapsed sidebar restores the preference");
    check(!chat_ui_reveal_sidebar(chat_ui),
        "revealing an already visible sidebar is a no-op");
    ui_layout(ui, 1100, 720);

    /* Narrow widths: the sidebar auto-collapses to a temporary drawer that
       still participates in layout, opens on demand, and never persists. */
    check(chat_ui_resize(chat_ui, 760, 650), "the narrow layout applies");
    check(chat_ui->narrow && sidebar->hidden,
        "the narrow layout starts with the drawer closed");
    check(!chat_ui_narrow_drawer_open(chat_ui),
        "no temporary drawer is open by default");
    check(!chat_ui_toggle_sidebar(chat_ui),
        "the narrow toggle is session state, not a preference");
    check(chat_ui_narrow_drawer_open(chat_ui) && !sidebar->hidden,
        "the hamburger opens the in-flow drawer");
    ui_layout(ui, 760, 650);
    check(main->rect.x >= sidebar->rect.x + sidebar->rect.w &&
        main->rect.w > 400,
        "the open drawer reflows the conversation surface beside it");
    check(hamburger->rect.x + hamburger->rect.w <= heading->rect.x &&
        model->rect.x >= heading->rect.x + heading->rect.w &&
        overflow->rect.x + overflow->rect.w <=
            header->rect.x + header->rect.w,
        "header controls never overlap at 760 DIP");
    chat_ui_close_drawer(chat_ui);
    check(sidebar->hidden && !chat_ui_narrow_drawer_open(chat_ui),
        "Escape-equivalent close removes the temporary drawer");
    check(chat->sidebar_collapsed == 0,
        "closing the drawer leaves the explicit preference untouched");
    /* Reveal: a command that targets the list or search opens the narrow
       drawer without touching the explicit preference. */
    check(!chat_ui_reveal_sidebar(chat_ui) &&
        chat_ui_narrow_drawer_open(chat_ui) && !sidebar->hidden &&
        chat->sidebar_collapsed == 0,
        "revealing opens the narrow drawer without persisting");
    check(!chat_ui_reveal_sidebar(chat_ui),
        "revealing an already visible drawer is a no-op");
    chat_ui_close_drawer(chat_ui);
    check(chat_ui_resize(chat_ui, 1100, 720), "the wide layout returns");
    check(!chat_ui->narrow && !sidebar->hidden,
        "the explicit preference reapplies at wide widths");
    ui_layout(ui, 1100, 720);

    /* Focus rings and accessible names on the retained placeholders. */
    check(!card->selected, "the composer card starts unfocused");
    chat_ui_set_focus_ring(chat_ui, chat_ui->composer_card, true);
    check(card->selected, "composer focus marks the card");
    chat_ui_set_focus_ring(chat_ui, chat_ui->composer_card, false);
    check(!card->selected, "composer blur clears the card");
    check(wcsstr(ui_accessible_name(ui, chat_ui->model), L"Model:") != NULL,
        "the model chip exposes its accessible name");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->overflow), L"More actions"),
        "the overflow control exposes its accessible name");
    check(!wcscmp(ui_accessible_name(ui, chat_ui->search),
        L"Search conversations"),
        "the search field exposes its accessible name");

    /* Empty state: visible only while the active conversation has no
       messages. */
    UiNode *empty = ui_node(ui, chat_ui->empty);
    UiNode *empty_title = ui_node(ui, chat_ui->empty_title);
    check(empty && empty->hidden,
        "a conversation with messages hides the empty state");
    check(empty_title && empty_title->style.text_centered,
        "the empty-state hero text is centered");
    chat_clear(chat);
    chat_ui_sync(chat_ui);
    check(empty && !empty->hidden,
        "an empty conversation reveals the empty state");
    check(!sidebar->hidden, "clearing messages does not disturb the sidebar");

    free(chat_ui); chat_dispose(chat); free(chat); free(ui);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat UI checks passed\n");
    return 0;
}
