#include "chat/chat_host_win32.h"
#include "chat/chat.h"
#include <stdlib.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command,
    int show) {
    (void)previous; (void)command;
    Ui *ui = (Ui *)calloc(1, sizeof *ui);
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!ui || !chat) { free(ui); free(chat); return 1; }
    ui_init(ui, NULL, NULL);
    chat_init(chat);
    ChatHostConfig config = {
        ui, chat, L"DarkChat \u2014 Offline Preview", 1100, 720, 720, 480
    };
    int result = chat_host_run(instance, show, &config);
    free(chat);
    free(ui);
    return result;
}

