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
/* Raises UIA_NamePropertyChanged for a node whose text meaning changed
   (windowed-list rebinding, renames). Both strings are the logical old and
   new accessible names; the node itself must still be visible. No-op when
   the node is absent or hidden. */
void ui_accessibility_property_changed(UiAccessibility *accessibility, UiId id,
    const wchar_t *old_text, const wchar_t *new_text);
/* Raises UIA_StructureChanged (ChildrenInvalidated) on a container whose
   logical child set or binding changed — including a windowed list that
   rebound every visible row to different content without any visibility
   change. No-op when the container is absent or hidden. */
void ui_accessibility_children_invalidated(UiAccessibility *accessibility, UiId id);
/* Test/diagnostic entry point. The caller owns the returned reference. */
IRawElementProviderSimple *ui_accessibility_root_provider(UiAccessibility *accessibility);

#endif
