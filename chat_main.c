#include "chat/chat_host_win32.h"
#include "chat/chat.h"
#include <stdlib.h>

/* The key lives only in this process-lifetime buffer: it is read once from the
   environment, handed to the HTTP worker, and never stored in Chat, printed,
   or persisted. */
static char api_key[8192];

static void load_api_key(void) {
    wchar_t wide[4096];
    wide[0] = 0;
    DWORD length = GetEnvironmentVariableW(L"OPENROUTER_API_KEY", wide, 4096);
    if (!length) {
        DWORD bytes=sizeof wide;
        if (RegGetValueW(HKEY_CURRENT_USER,L"Environment",L"OPENROUTER_API_KEY",
            RRF_RT_REG_SZ,NULL,wide,&bytes)==ERROR_SUCCESS) length=(DWORD)wcslen(wide);
    }
    if (length > 0 && length < 4096)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, api_key, (int)sizeof api_key,
            NULL, NULL);
    api_key[sizeof api_key - 1] = 0;
    SecureZeroMemory(wide, sizeof wide);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command,
    int show) {
    (void)previous; (void)command;
    Ui *ui = (Ui *)calloc(1, sizeof *ui);
    Chat *chat = (Chat *)calloc(1, sizeof *chat);
    if (!ui || !chat) { free(ui); free(chat); return 1; }
    ui_init(ui, NULL, NULL);
    chat_init(chat);
    load_api_key();
    ChatHostConfig config = {
        ui, chat, L"DarkChat", 1100, 720, 720, 480, api_key
    };
    int result = chat_host_run(instance, show, &config);
    SecureZeroMemory(api_key, sizeof api_key);
    free(chat);
    free(ui);
    return result;
}
