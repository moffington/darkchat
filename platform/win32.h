#ifndef DARK_WIN32_H
#define DARK_WIN32_H
#include "renderer.h"

typedef void (*UiResizeFn)(void *user, float width, float height);
typedef struct {
    Ui *ui;
    const wchar_t *title;
    UiResizeFn on_resize;
    void *user;
    int width, height, min_width, min_height; /* client DIPs */
} UiWindowConfig;

/* Owns renderer, COM apartment, HWND, edit bridge and message loop until close. */
int ui_win32_run(HINSTANCE instance, int show, const UiWindowConfig *config);
#endif
