#ifndef DARK_ACCESSIBILITY_H
#define DARK_ACCESSIBILITY_H

#include "../ui/ui.h"
#include <windows.h>
#include <uiautomationcore.h>

#define UI_WM_ACCESSIBILITY_INVOKE (WM_APP + 0x4d)

typedef struct UiAccessibility UiAccessibility;

UiAccessibility *ui_accessibility_create(HWND window, Ui *ui);
void ui_accessibility_destroy(UiAccessibility *accessibility);
LRESULT ui_accessibility_get_object(UiAccessibility *accessibility, WPARAM wparam, LPARAM lparam);
bool ui_accessibility_handle_message(UiAccessibility *accessibility, UINT message, WPARAM wparam);
void ui_accessibility_focus_changed(UiAccessibility *accessibility, UiId id);
/* Test/diagnostic entry point. The caller owns the returned reference. */
IRawElementProviderSimple *ui_accessibility_root_provider(UiAccessibility *accessibility);

#endif
