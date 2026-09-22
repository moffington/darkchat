#ifndef DARK_MODE_WIN32_H
#define DARK_MODE_WIN32_H
#include <windows.h>

/* Best-effort dark-mode support for native menus and window captions.
   Every entry point is a silent no-op when a capability check fails, so the
   stock native appearance is the only guaranteed outcome and callers never
   branch on these calls. The helper owns all build-number and dynamic-API
   knowledge; nothing here allocates resources a caller must release. */

/* Resolves the undocumented surface once and applies the process-wide
   dark-mode policy. Must run before any window or menu exists. Below the
   dark-capable build range this does nothing at all. */
void dark_mode_process_init(void);

/* Grants the window permission to render dark native surfaces (menus). */
void dark_mode_window_apply(HWND window);

/* Applies the DWM dark caption attribute, choosing the attribute number the
   running build actually honours. A failed call keeps the stock caption. */
void dark_mode_titlebar_apply(HWND window);

/* Best-effort dark visual style for a native child control (e.g. an EDIT's
   scrollbars). Unsupported builds and failed calls are silent no-ops. */
void dark_mode_control_apply(HWND window);
#endif
