#include "platform/dark_mode_win32.h"
#include <stdbool.h>
#include <string.h>
#include <dwmapi.h>

/* All the undocumented surface this helper touches is resolved at run time;
   nothing is linked against uxtheme. Ordinals are community-verified
   knowledge and are re-confirmed in the manual test matrix (see the commit
   message). Two build families are known, and ordinal 135 is deliberately
   bound to exactly one typed function per family, never both:
     - 17763..18361 (Windows 10 1809): ordinal 135 = AllowDarkModeForApp(BOOL)
     - >= 18362 (Windows 10 1903+, incl. Windows 11):
           ordinal 135 = SetPreferredAppMode(int)
   AllowDarkModeForWindow is ordinal 133 in both families. The 1703/1709/1803
   baseline resolves nothing: those builds keep the stock appearance. */

typedef struct {
    ULONG size;
    ULONG major, minor, build, platform;
    WCHAR service_pack[128];
} DarkOsVersionInfo; /* RTL_OSVERSIONINFOW layout, without header dependence */

typedef LONG (WINAPI *RtlGetVersionFn)(DarkOsVersionInfo *info);
typedef int (WINAPI *SetPreferredAppModeFn)(int mode);
typedef BOOL (WINAPI *AllowDarkModeForAppFn)(BOOL allow);
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND window, BOOL allow);
/* The real uxtheme export returns HRESULT; the value is ignored, but the
   pointer type must match it exactly. */
typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND window, LPCWSTR sub_app,
    LPCWSTR id_list);

enum {
    DARK_APP_MODE_DEFAULT = 0,
    DARK_APP_MODE_ALLOW_DARK = 1,
    DARK_APP_MODE_FORCE_DARK = 2,
    DARK_APP_MODE_FORCE_LIGHT = 3
};

/* -Wcast-function-type exempts void (*)(void), so GetProcAddress results are
   routed through it before receiving their exact per-family types. */
#define DARK_PROC(type, module, name) \
    ((type)(void (*)(void))GetProcAddress((module), (name)))

static bool attempted;
static bool dark_capable;
static DWORD build_number;
static HMODULE theme;
static AllowDarkModeForWindowFn allow_window;
static SetWindowThemeFn set_theme;

static bool real_build_number(DWORD *build) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    RtlGetVersionFn get =
        DARK_PROC(RtlGetVersionFn, ntdll, "RtlGetVersion");
    if (!get) return false;
    DarkOsVersionInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (get(&info) != 0 || !build) return false;
    *build = info.build;
    return true;
}

void dark_mode_process_init(void) {
    if (attempted) return;
    attempted = true;
    DWORD build = 0;
    if (!real_build_number(&build) || build < 17763) return;
    theme = LoadLibraryW(L"uxtheme.dll");
    if (!theme) return;
    if (build >= 18362) {
        SetPreferredAppModeFn set_mode =
            DARK_PROC(SetPreferredAppModeFn, theme, MAKEINTRESOURCEA(135));
        if (set_mode) set_mode(DARK_APP_MODE_FORCE_DARK);
    } else {
        AllowDarkModeForAppFn allow_app =
            DARK_PROC(AllowDarkModeForAppFn, theme, MAKEINTRESOURCEA(135));
        if (allow_app) allow_app(TRUE);
    }
    allow_window =
        DARK_PROC(AllowDarkModeForWindowFn, theme, MAKEINTRESOURCEA(133));
    set_theme = DARK_PROC(SetWindowThemeFn, theme, "SetWindowTheme");
    dark_capable = true;
    build_number = build;
}

void dark_mode_window_apply(HWND window) {
    if (!window || !allow_window) return;
    allow_window(window, TRUE);
}

void dark_mode_titlebar_apply(HWND window) {
    if (!window || !dark_capable) return;
    /* The DWM attribute renumbered from 19 to 20 around build 19041 (20H1);
       1809/1903 builds only honour 19. A failed call is silent by design. */
    DWORD attribute = build_number >= 19041 ? 20 : 19;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, attribute, &dark, sizeof dark);
}

void dark_mode_control_apply(HWND window) {
    if (!window || !set_theme) return;
    set_theme(window, L"DarkMode_Explorer", NULL);
}
