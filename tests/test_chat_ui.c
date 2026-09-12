#include "../chat/chat_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

int main(void) {
    Ui *ui = (Ui *)calloc(1, sizeof *ui);
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    ChatUi *chat_ui = (ChatUi *)calloc(1, sizeof *chat_ui);
    if (!ui || !chat || !chat_ui) return 2;
    ui_init(ui, NULL, NULL);
    chat_init(chat);
    check(chat_ui_init(chat_ui, ui, chat), "chat UI initializes");
    chat_ui_resize(chat_ui, 1100, 720);
    UiNode *model = ui_node(ui, chat_ui->model);
    check(model && model->rect.w >= 279 && model->rect.h >= 29,
        "model editor placeholder remains visible");
    chat_ui_resize(chat_ui, 720, 480);
    check(model && model->rect.w >= 279 && model->rect.h >= 29,
        "model editor remains visible at the minimum window size");
    chat_ui_set_generation(chat_ui, true, false);
    UiNode *send = ui_node(ui, chat_ui->send);
    check(send && wcscmp(send->text, L"Stop") == 0 && !send->disabled,
        "generation exposes an enabled Stop button");
    chat_ui_set_generation(chat_ui, true, true);
    check(send && send->disabled, "stopping disables repeated cancellation");
    chat_ui_set_generation(chat_ui, false, false);
    check(send && wcscmp(send->text, L"Send") == 0 && !send->disabled,
        "completed generation restores Send");
    check(ui_node(ui, chat_ui->transcript) != NULL,
        "transcript placeholder remains for the per-turn container");
    free(chat_ui); free(chat); free(ui);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall chat UI checks passed\n");
    return 0;
}
