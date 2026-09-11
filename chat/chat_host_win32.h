#ifndef DARKCHAT_CHAT_HOST_WIN32_H
#define DARKCHAT_CHAT_HOST_WIN32_H

/* Chat-specific Win32 host. Owns the HWND, message loop, DarkUI renderer and
   accessibility tree, the retained chrome, and the three native Rich Edit
   surfaces. Reuses the toolkit but stays on the app branch. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../ui/ui.h"
#include "chat.h"

typedef struct {
    Ui *ui;
    Chat *chat;
    const wchar_t *title;
    int width, height;             /* initial client size in DIPs */
    int min_width, min_height;     /* minimum client size in DIPs */
} ChatHostConfig;

int chat_host_run(HINSTANCE instance, int show, const ChatHostConfig *config);

#endif
