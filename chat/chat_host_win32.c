#include "chat_host_win32.h"
#include "chat_ui.h"
#include "rich_text_win32.h"
#include "../platform/renderer.h"
#include "../platform/accessibility.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
    ChatHostConfig config;
    UiRenderer renderer;
    UiAccessibility *accessibility;
    HWND window;
    ChatUi chat_ui;
    RichTextControl transcript, composer, field;
    RichTextTheme rich_theme;
    HBRUSH background;
    float dpi;
    bool tracking, minimized;
    unsigned retries;
    UiId accessibility_focus;
} ChatHost;

static void flush(ChatHost *host);
static void render_transcript(ChatHost *host);
static void perform_send(ChatHost *host);

static int px(ChatHost *host, float dips) {
    return (int)lroundf(dips * host->dpi / 96.0f);
}

static float dip(ChatHost *host, int pixels) {
    return pixels * 96.0f / host->dpi;
}

static COLORREF rgb(UiColor color) { return RGB(color.r, color.g, color.b); }

/* Sizes a native surface to its placeholder, insetting for the drawn border. */
static void place(ChatHost *host, RichTextControl *control, UiId id,
    float inset) {
    UiNode *item = ui_node(host->config.ui, id);
    if (!item || !control->window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += inset;
    area.y += inset;
    area.w -= 2 * inset;
    area.h -= 2 * inset;
    if (area.w <= 2 || area.h <= 2) { ShowWindow(control->window, SW_HIDE); return; }
    int x = px(host, area.x), y = px(host, area.y);
    SetWindowPos(control->window, NULL, x, y, px(host, area.x + area.w) - x,
        px(host, area.y + area.h) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(control->window, SW_SHOWNOACTIVATE);
}

/* The single-line field is vertically centered inside its placeholder. */
static void place_field(ChatHost *host) {
    UiNode *item = ui_node(host->config.ui, host->chat_ui.model);
    if (!item || !host->field.window) return;
    UiRect area = ui_intersect(item->rect, item->clip);
    area.x += 1;
    area.w -= 2;
    float line = host->rich_theme.ui_size * 1.8f;
    float top = area.y + (area.h - line) / 2;
    if (top < area.y) top = area.y;
    float height = line < area.h ? line : area.h;
    if (area.w <= 2 || height <= 2) { ShowWindow(host->field.window, SW_HIDE); return; }
    int x = px(host, area.x), y = px(host, top);
    SetWindowPos(host->field.window, NULL, x, y, px(host, area.x + area.w) - x,
        px(host, top + height) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(host->field.window, SW_SHOWNOACTIVATE);
}

static void layout(ChatHost *host) {
    if (host->minimized) return;
    RECT client;
    GetClientRect(host->window, &client);
    chat_ui_resize(&host->chat_ui, dip(host, client.right), dip(host, client.bottom));
    place(host, &host->transcript, host->chat_ui.transcript, 1.0f);
    place(host, &host->composer, host->chat_ui.composer, 1.0f);
    place_field(host);
}

static void flush(ChatHost *host) {
    layout(host);
    if (host->accessibility &&
        host->accessibility_focus != host->config.ui->focus) {
        host->accessibility_focus = host->config.ui->focus;
        ui_accessibility_focus_changed(host->accessibility,
            host->accessibility_focus ? host->accessibility_focus
                                      : host->config.ui->root);
    }
    if (GetCapture() == host->window && !host->config.ui->pressed &&
        !host->config.ui->drag_scroll) ReleaseCapture();
    if (host->config.ui->paint_dirty) InvalidateRect(host->window, NULL, FALSE);
}

static void sync_model(ChatHost *host) {
    wchar_t buffer[CHAT_MODEL_TEXT];
    rich_text_get_text(&host->field, buffer, CHAT_MODEL_TEXT);
    wchar_t *model = buffer;
    while (*model == L' ' || *model == L'\t') ++model;
    size_t length = wcslen(model);
    while (length && (model[length - 1] == L' ' || model[length - 1] == L'\t'))
        model[--length] = 0;
    if (length) {
        wcsncpy(host->config.chat->model, model, CHAT_MODEL_TEXT - 1);
        host->config.chat->model[CHAT_MODEL_TEXT - 1] = 0;
    }
    /* Never let the visible field and the stored model disagree: an empty field
       falls back to the last valid model, which is written back into the field. */
    rich_text_set_text(&host->field, host->config.chat->model);
}

static void field_blur(void *user) { sync_model((ChatHost *)user); }


static void focus_surface(ChatHost *host, bool reverse) {
    HWND order[3] = { host->field.window, host->composer.window,
        host->transcript.window };
    HWND focus = GetFocus();
    int current = -1;
    for (int i = 0; i < 3; i++) if (order[i] == focus) current = i;
    int next = current < 0 ? 0 : (current + (reverse ? -1 : 1) + 3) % 3;
    SetFocus(order[next]);
}

static void render_transcript(ChatHost *host) {
    rich_text_clear(&host->transcript);
    const ChatConversation *conversation = chat_active(host->config.chat);
    if (!conversation) return;
    for (int i = 0; i < conversation->message_count; i++)
        rich_text_append_message(&host->transcript,
            conversation->messages[i].role, conversation->messages[i].text);
    rich_text_scroll_to_end(&host->transcript);
}

static void set_status(ChatHost *host, const wchar_t *text) {
    wcsncpy(host->config.chat->status, text, CHAT_STATUS_TEXT - 1);
    host->config.chat->status[CHAT_STATUS_TEXT - 1] = 0;
    chat_ui_sync(&host->chat_ui);
    flush(host);
}

static void perform_send(ChatHost *host) {
    Chat *chat = host->config.chat;
    wchar_t prompt[CHAT_MESSAGE_TEXT];
    rich_text_get_text(&host->composer, prompt, CHAT_MESSAGE_TEXT);
    if (!prompt[0]) {
        set_status(host, L"Nothing to send");
        return;
    }
    /* Every send stores a user message and a reply; refuse up front rather than
       leaving the transcript with content the conversation cannot hold. */
    if (chat_remaining(chat) < 2) {
        set_status(host, L"Conversation is full \u2014 start a new one");
        return;
    }
    sync_model(host);
    if (chat_append(chat, CHAT_ROLE_USER, prompt) < 0) {
        set_status(host, L"Could not store the message");
        return;
    }
    rich_text_append_message(&host->transcript, CHAT_ROLE_USER, prompt);
    rich_text_set_text(&host->composer, L"");
    wchar_t reply[CHAT_MESSAGE_TEXT];
    chat_fake_reply(chat, prompt, reply, CHAT_MESSAGE_TEXT);
    /* Only display the reply once it is actually part of the conversation. */
    if (chat_append(chat, CHAT_ROLE_ASSISTANT, reply) >= 0)
        rich_text_append_message(&host->transcript, CHAT_ROLE_ASSISTANT, reply);
    chat_ui_sync(&host->chat_ui);
    flush(host);
}

static void command(void *user, ChatCommand code, int index) {
    ChatHost *host = (ChatHost *)user;
    Chat *chat = host->config.chat;
    if (code == CHAT_COMMAND_SEND) {
        perform_send(host);
    } else if (code == CHAT_COMMAND_NEW_CONVERSATION) {
        if (chat_new_conversation(chat) >= 0) {
            render_transcript(host);
            rich_text_set_text(&host->composer, L"");
            chat_ui_sync(&host->chat_ui);
        }
    } else if (code == CHAT_COMMAND_SELECT) {
        if (chat_select_conversation(chat, index)) {
            render_transcript(host);
            chat_ui_sync(&host->chat_ui);
        }
    }
    ui_invalidate(host->config.ui, false);
    flush(host);
}

static bool surface_key(void *user, WPARAM key, bool shift, bool control,
    bool down) {
    (void)control;
    ChatHost *host = (ChatHost *)user;
    if (key == VK_TAB && down) { focus_surface(host, shift); return true; }
    return false;
}

static bool composer_submit(void *user) {
    perform_send((ChatHost *)user);
    return true;
}

static bool field_submit(void *user) {
    ChatHost *host = (ChatHost *)user;
    sync_model(host);
    SetFocus(host->composer.window);
    return true;
}

static bool key_from_win32(WPARAM key, UiKey *out) {
    switch (key) {
    case VK_TAB: *out = UI_KEY_TAB; break;
    case VK_RETURN: *out = UI_KEY_ENTER; break;
    case VK_SPACE: *out = UI_KEY_SPACE; break;
    case VK_ESCAPE: *out = UI_KEY_ESCAPE; break;
    case VK_LEFT: *out = UI_KEY_LEFT; break;
    case VK_RIGHT: *out = UI_KEY_RIGHT; break;
    case VK_UP: *out = UI_KEY_UP; break;
    case VK_DOWN: *out = UI_KEY_DOWN; break;
    case VK_HOME: *out = UI_KEY_HOME; break;
    case VK_END: *out = UI_KEY_END; break;
    case VK_PRIOR: *out = UI_KEY_PAGE_UP; break;
    case VK_NEXT: *out = UI_KEY_PAGE_DOWN; break;
    default: return false;
    }
    return true;
}

static void wheel(ChatHost *host, WPARAM w, LPARAM l) {
    Ui *u = host->config.ui;
    POINT point = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(host->window, &point);
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    float step = lines == WHEEL_PAGESCROLL ? u->height * .9f
                                           : lines * u->theme.control_height;
    float delta = -(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * step;
    ui_scroll(u, dip(host, point.x), dip(host, point.y), delta);
    flush(host);
}

static void host_event(void *user, Ui *ui, UiEvent event) {
    (void)ui;
    ChatHost *host = (ChatHost *)user;
    chat_ui_event(&host->chat_ui, host->config.ui, event);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w,
    LPARAM l) {
    ChatHost *host = (ChatHost *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        host = ((CREATESTRUCTW *)l)->lpCreateParams;
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)host);
    }
    if (!host) return DefWindowProcW(window, message, w, l);
    Ui *u = host->config.ui;
    switch (message) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(window);
        host->dpi = dpi ? (float)dpi : 96;
        host->renderer.dpi = host->dpi;
        if (!rich_text_library_open()) return -1;
        rich_text_theme(&host->rich_theme, &u->theme);
        if (!rich_text_create_transcript(&host->transcript, window, 1,
            &host->rich_theme, host->dpi)) return -1;
        if (!rich_text_create_composer(&host->composer, window, 2,
            &host->rich_theme, host->dpi)) return -1;
        if (!rich_text_create_field(&host->field, window, 3, &host->rich_theme,
            host->dpi, host->config.chat->model)) return -1;
        host->composer.on_submit = composer_submit;
        host->composer.on_key = surface_key;
        host->composer.user = host;
        host->field.on_submit = field_submit;
        host->field.on_key = surface_key;
        host->field.on_blur = field_blur;
        host->field.user = host;
        host->transcript.on_key = surface_key;
        host->transcript.user = host;
        if (!ui_accessible_name(u, u->root)[0])
            ui_set_accessible_name(u, u->root, host->config.title);
        host->accessibility = ui_accessibility_create(window, u);
        if (!host->accessibility) return -1;
        render_transcript(host);
        return 0;
    }
    case WM_GETOBJECT: {
        LRESULT result = ui_accessibility_get_object(host->accessibility, w, l);
        if (result) return result;
        break;
    }
    case UI_WM_ACCESSIBILITY_INVOKE:
        ui_accessibility_handle_message(host->accessibility, message, w);
        flush(host);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        if (!host->minimized) {
            layout(host);
            HRESULT hr = renderer_paint(&host->renderer, window, u, UI_NONE);
            if (FAILED(hr)) {
                FillRect(paint.hdc, &paint.rcPaint, host->background);
                wchar_t error[96];
                swprintf(error, 96, L"DarkChat: rendering failed (0x%08lX)\n",
                    (unsigned long)hr);
                OutputDebugStringW(error);
                if (host->retries++ < 3) SetTimer(window, 1, 250, NULL);
            } else {
                host->retries = 0;
                KillTimer(window, 1);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (w == 1) { KillTimer(window, 1); InvalidateRect(window, NULL, FALSE); }
        return 0;
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)l;
        if (host->transcript.window &&
            header->hwndFrom == host->transcript.window &&
            rich_text_handle_notify(&host->transcript, l)) return 0;
        break;
    }
    case WM_SIZE:
        host->minimized = w == SIZE_MINIMIZED;
        if (!host->minimized) {
            RECT r;
            GetClientRect(window, &r);
            renderer_resize(&host->renderer, (UINT)r.right, (UINT)r.bottom,
                host->dpi);
            ui_invalidate(u, true);
            flush(host);
        }
        return 0;
    case WM_DPICHANGED: {
        host->dpi = (float)HIWORD(w);
        RECT *r = (RECT *)l;
        rich_text_set_dpi(&host->field, host->dpi);
        rich_text_set_dpi(&host->composer, host->dpi);
        rich_text_set_dpi(&host->transcript, host->dpi);
        renderer_resize(&host->renderer, 0, 0, host->dpi);
        ui_invalidate(u, true);
        SetWindowPos(window, NULL, r->left, r->top, r->right - r->left,
            r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        flush(host);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)l;
        UINT dpi = host->dpi > 0 ? (UINT)host->dpi : GetDpiForSystem();
        RECT r = { 0, 0, MulDiv(host->config.min_width, (int)dpi, 96),
            MulDiv(host->config.min_height, (int)dpi, 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        info->ptMinTrackSize.x = r.right - r.left;
        info->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!host->tracking) {
            TRACKMOUSEEVENT tracking = { sizeof tracking, TME_LEAVE, window, 0 };
            host->tracking = TrackMouseEvent(&tracking) != FALSE;
        }
        ui_pointer_move(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        flush(host);
        return 0;
    case WM_MOUSELEAVE:
        host->tracking = false; ui_pointer_leave(u); flush(host); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        layout(host);
        ui_pointer_down(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        if (u->pressed) SetCapture(window);
        flush(host);
        return 0;
    case WM_LBUTTONUP:
        ui_pointer_up(u, dip(host, GET_X_LPARAM(l)), dip(host, GET_Y_LPARAM(l)));
        flush(host);
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != window) { ui_cancel_input(u); InvalidateRect(window, NULL, FALSE); }
        return 0;
    case WM_CANCELMODE:
        ui_cancel_input(u);
        if (GetCapture() == window) ReleaseCapture();
        flush(host);
        return 0;
    case WM_MOUSEWHEEL: wheel(host, w, l); return 0;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        UiKey key;
        if (key_from_win32(w, &key)) {
            ui_key(u, key, message == WM_KEYDOWN,
                (GetKeyState(VK_SHIFT) & 0x8000) != 0, (l & (1L << 30)) != 0);
            flush(host);
            return 0;
        }
        break;
    }
    case WM_CHAR: return 0;
    case WM_SETFOCUS: ui_set_active(u, true); flush(host); return 0;
    case WM_KILLFOCUS:
        if ((HWND)w != host->field.window &&
            (HWND)w != host->composer.window &&
            (HWND)w != host->transcript.window) {
            ui_set_active(u, false);
            flush(host);
        }
        return 0;
    case WM_ACTIVATE:
        ui_set_active(u, LOWORD(w) != WA_INACTIVE);
        flush(host);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        renderer_drop_target(&host->renderer);
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, w, l);
}

int chat_host_run(HINSTANCE instance, int show, const ChatHostConfig *config) {
    if (!config || !config->ui || !config->chat) return 1;
    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) return 1;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    ChatHost *host = (ChatHost *)calloc(1, sizeof *host);
    int result = 1;
    if (!host) { CoUninitialize(); return result; }
    host->config = *config;
    host->dpi = (float)GetDpiForSystem();
    if (!chat_ui_init(&host->chat_ui, config->ui, config->chat)) goto cleanup;
    host->chat_ui.command = command;
    host->chat_ui.command_user = host;
    config->ui->on_event = host_event;
    config->ui->event_user = host;
    HRESULT hr = renderer_init(&host->renderer, &config->ui->theme);
    if (FAILED(hr)) goto cleanup;
    config->ui->measure = renderer_measure;
    config->ui->measure_user = &host->renderer;
    host->background = CreateSolidBrush(rgb(config->ui->theme.colors[UI_BG]));
    if (!host->background) goto cleanup;
    WNDCLASSEXW cls = { 0 };
    cls.cbSize = sizeof cls;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = L"DarkChat.Window";
    if (!RegisterClassExW(&cls)) goto cleanup;
    RECT bounds = { 0, 0, px(host, (float)config->width),
        px(host, (float)config->height) };
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0,
        (UINT)host->dpi);
    HWND window = CreateWindowExW(0, cls.lpszClassName, config->title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, NULL, NULL,
        instance, host);
    if (window) {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
        ShowWindow(window, show);
        UpdateWindow(window);
        if (host->composer.window) SetFocus(host->composer.window);
        MSG message;
        BOOL got;
        while ((got = GetMessageW(&message, NULL, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        result = got < 0 ? 1 : (int)message.wParam;
        if (IsWindow(window)) DestroyWindow(window);
    }
    UnregisterClassW(cls.lpszClassName, instance);
cleanup:
    ui_accessibility_destroy(host->accessibility);
    config->ui->measure = NULL;
    config->ui->measure_user = NULL;
    renderer_dispose(&host->renderer);
    if (host->background) DeleteObject(host->background);
    rich_text_library_close();
    free(host);
    CoUninitialize();
    if (result) MessageBoxW(NULL,
        L"DarkChat could not initialize. Check the graphics runtime and available resources.",
        L"DarkChat", MB_OK | MB_ICONERROR);
    return result;
}





