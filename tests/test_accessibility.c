#define COBJMACROS
#include "../platform/accessibility.h"
#include <uiautomationclient.h>
#include <uiautomationcoreapi.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)

static UiAccessibility *accessibility;
static unsigned invoked;
static void event(void *user, Ui *ui, UiEvent e) {
    (void)user; (void)ui; if (e.kind==UI_ACTIVATE) invoked++;
}
static LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (accessibility && ui_accessibility_handle_message(accessibility,message,wparam)) return 0;
    return DefWindowProcW(window,message,wparam,lparam);
}
static IRawElementProviderFragment *navigate(IRawElementProviderFragment *from, enum NavigateDirection direction) {
    IRawElementProviderFragment *result=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_Navigate(from,direction,&result)) && result);
    return result;
}
static void check_string(IRawElementProviderSimple *provider, PROPERTYID property, const wchar_t *expected) {
    VARIANT value; VariantInit(&value);
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(provider,property,&value)));
    CHECK(V_VT(&value)==VT_BSTR && !wcscmp(V_BSTR(&value),expected)); VariantClear(&value);
}
int main(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW cls={0}; cls.hInstance=instance; cls.lpfnWndProc=test_proc; cls.lpszClassName=L"DarkUi.AccessibilityTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Accessibility test",WS_OVERLAPPEDWINDOW,
        0,0,320,200,NULL,NULL,instance,NULL); CHECK(window);

    Ui ui; ui_init(&ui,NULL,NULL); ui.on_event=event;
    UiId root=ui_add(&ui,UI_NONE,UI_COLUMN,L"Accessibility test");
    UiId label=ui_add(&ui,root,UI_LABEL,L"PRIMARY ACTION");
    UiId button=ui_add(&ui,root,UI_BUTTON,L"Run");
    UiId button2=ui_add(&ui,root,UI_BUTTON,L"Second");
    UiId icon_button=ui_add(&ui,root,UI_ICON_BUTTON,L"");
    ui_set_icon(&ui,icon_button,UI_ICON_SEND);
    ui_set_accessible_name(&ui,icon_button,L"Send message");
    ui_set_labelled_by(&ui,button,label); ui_set_help_text(&ui,button,L"Runs the primary action.");
    ui_layout(&ui,320,200);
    accessibility=ui_accessibility_create(window,&ui); CHECK(accessibility);

    IRawElementProviderSimple *root_simple=ui_accessibility_root_provider(accessibility); CHECK(root_simple);
    VARIANT value; VariantInit(&value);
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(root_simple,UIA_ControlTypePropertyId,&value)));
    CHECK(V_VT(&value)==VT_I4 && V_I4(&value)==UIA_WindowControlTypeId); VariantClear(&value);

    IRawElementProviderFragment *root_fragment=NULL;
    CHECK(SUCCEEDED(IRawElementProviderSimple_QueryInterface(root_simple,&IID_IRawElementProviderFragment,(void **)&root_fragment)));
    IRawElementProviderFragment *label_fragment=navigate(root_fragment,NavigateDirection_FirstChild);
    IRawElementProviderSimple *label_simple=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(label_fragment,&IID_IRawElementProviderSimple,(void **)&label_simple)));
    IRawElementProviderFragment *button_fragment=navigate(label_fragment,NavigateDirection_NextSibling);
    IRawElementProviderSimple *button_simple=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(button_fragment,&IID_IRawElementProviderSimple,(void **)&button_simple)));
    check_string(button_simple,UIA_NamePropertyId,L"PRIMARY ACTION");
    check_string(button_simple,UIA_HelpTextPropertyId,L"Runs the primary action.");
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(button_simple,UIA_IsEnabledPropertyId,&value)));
    CHECK(V_VT(&value)==VT_BOOL && V_BOOL(&value)==VARIANT_TRUE); VariantClear(&value);

    SAFEARRAY *runtime=NULL; CHECK(SUCCEEDED(IRawElementProviderFragment_GetRuntimeId(button_fragment,&runtime)) && runtime);
    CHECK(runtime->rgsabound[0].cElements==2); SafeArrayDestroy(runtime);
    struct UiaRect bounds; CHECK(SUCCEEDED(IRawElementProviderFragment_get_BoundingRectangle(button_fragment,&bounds)));
    CHECK(bounds.width>0 && bounds.height>0);

    IUnknown *pattern=NULL;
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPatternProvider(button_simple,UIA_InvokePatternId,&pattern)) && pattern);
    IInvokeProvider *invoke_provider=NULL;
    CHECK(SUCCEEDED(IUnknown_QueryInterface(pattern,&IID_IInvokeProvider,(void **)&invoke_provider)));
    CHECK(SUCCEEDED(IInvokeProvider_Invoke(invoke_provider)));
    MSG message;
    while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    CHECK(invoked==1);

    CHECK(SUCCEEDED(IRawElementProviderFragment_SetFocus(button_fragment)) && ui.focus==button);
    IRawElementProviderFragmentRoot *fragment_root=NULL;
    CHECK(SUCCEEDED(IRawElementProviderSimple_QueryInterface(root_simple,&IID_IRawElementProviderFragmentRoot,(void **)&fragment_root)));
    UiRect label_rect=ui_node(&ui,label)->rect;
    float dpi=(float)GetDpiForWindow(window); if (dpi<=0) dpi=96;
    POINT label_point={(LONG)((label_rect.x+2)*dpi/96),(LONG)((label_rect.y+2)*dpi/96)}; ClientToScreen(window,&label_point);
    IRawElementProviderFragment *point_element=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragmentRoot_ElementProviderFromPoint(fragment_root,label_point.x,label_point.y,&point_element)) && point_element);
    IRawElementProviderSimple *point_simple=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(point_element,&IID_IRawElementProviderSimple,(void **)&point_simple)));
    check_string(point_simple,UIA_NamePropertyId,L"PRIMARY ACTION");
    IRawElementProviderFragment *focused=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragmentRoot_GetFocus(fragment_root,&focused)) && focused);
    SAFEARRAY *focused_id=NULL; CHECK(SUCCEEDED(IRawElementProviderFragment_GetRuntimeId(focused,&focused_id)) && focused_id);
    SafeArrayDestroy(focused_id);

    CHECK(ui_remove(&ui,button));
    CHECK(IRawElementProviderSimple_GetPropertyValue(button_simple,UIA_NamePropertyId,&value)==(HRESULT)UIA_E_ELEMENTNOTAVAILABLE);

    /* Rebinding semantics: providers are stateless over live node state, so a
       retained provider observes the new name of the same runtime id, and the
       notification helpers raise events safely even with no client attached.
       Hidden nodes make the provider unavailable and the helpers no-op. */
    IRawElementProviderFragment *button2_fragment=navigate(label_fragment,NavigateDirection_NextSibling);
    IRawElementProviderSimple *button2_simple=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(button2_fragment,&IID_IRawElementProviderSimple,(void **)&button2_simple)));
    check_string(button2_simple,UIA_NamePropertyId,L"Second");
    ui_set_text(&ui,button2,L"Rebound");
    check_string(button2_simple,UIA_NamePropertyId,L"Rebound");

    /* An icon-only button exposes the Button control type, its accessible
       name and the Invoke pattern exactly like a text button. */
    IRawElementProviderFragment *icon_fragment=navigate(button2_fragment,NavigateDirection_NextSibling);
    IRawElementProviderSimple *icon_simple=NULL;
    CHECK(SUCCEEDED(IRawElementProviderFragment_QueryInterface(icon_fragment,&IID_IRawElementProviderSimple,(void **)&icon_simple)));
    check_string(icon_simple,UIA_NamePropertyId,L"Send message");
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPropertyValue(icon_simple,UIA_ControlTypePropertyId,&value)));
    CHECK(V_VT(&value)==VT_I4 && V_I4(&value)==UIA_ButtonControlTypeId); VariantClear(&value);
    IUnknown *icon_pattern=NULL;
    CHECK(SUCCEEDED(IRawElementProviderSimple_GetPatternProvider(icon_simple,UIA_InvokePatternId,&icon_pattern)) && icon_pattern);
    IInvokeProvider *icon_invoke=NULL;
    CHECK(SUCCEEDED(IUnknown_QueryInterface(icon_pattern,&IID_IInvokeProvider,(void **)&icon_invoke)));
    CHECK(SUCCEEDED(IInvokeProvider_Invoke(icon_invoke)));
    while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    CHECK(invoked==2);
    IInvokeProvider_Release(icon_invoke); IUnknown_Release(icon_pattern);
    IRawElementProviderSimple_Release(icon_simple); IRawElementProviderFragment_Release(icon_fragment);

    ui_accessibility_property_changed(accessibility,button2,L"Second",L"Rebound");
    ui_accessibility_children_invalidated(accessibility,root);
    ui_accessibility_property_changed(accessibility,UI_NONE,L"A",L"B");
    ui_set_hidden(&ui,button2,true);
    CHECK(IRawElementProviderSimple_GetPropertyValue(button2_simple,UIA_NamePropertyId,&value)==(HRESULT)UIA_E_ELEMENTNOTAVAILABLE);
    ui_accessibility_property_changed(accessibility,button2,L"Rebound",L"Hidden");
    ui_accessibility_children_invalidated(accessibility,button2);
    IRawElementProviderFragment_Release(button2_fragment);
    IRawElementProviderSimple_Release(button2_simple);

    ui_accessibility_destroy(accessibility); accessibility=NULL;
    CHECK(IRawElementProviderSimple_GetPropertyValue(label_simple,UIA_NamePropertyId,&value)==(HRESULT)UIA_E_ELEMENTNOTAVAILABLE);
    IRawElementProviderSimple_Release(point_simple); IRawElementProviderFragment_Release(point_element);
    IRawElementProviderFragment_Release(focused);
    IRawElementProviderFragmentRoot_Release(fragment_root);
    IInvokeProvider_Release(invoke_provider); IUnknown_Release(pattern);
    IRawElementProviderSimple_Release(button_simple); IRawElementProviderSimple_Release(label_simple);
    IRawElementProviderFragment_Release(button_fragment); IRawElementProviderFragment_Release(label_fragment);
    IRawElementProviderFragment_Release(root_fragment); IRawElementProviderSimple_Release(root_simple);
    DestroyWindow(window); UnregisterClassW(cls.lpszClassName,instance); CoUninitialize();
    puts("PASS: UIA tree, metadata, runtime IDs, bounds, focus, stale providers and Invoke");
    return 0;
}
