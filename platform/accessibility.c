#define COBJMACROS
#include <initguid.h>
#include "accessibility.h"
#include <uiautomationclient.h>
#include <uiautomationcoreapi.h>
#include <oleauto.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct UiProvider UiProvider;
struct UiAccessibility {
    LONG references;
    HWND window;
    Ui *ui;
};
struct UiProvider {
    IRawElementProviderSimple simple;
    IRawElementProviderFragment fragment;
    IRawElementProviderFragmentRoot root;
    IInvokeProvider invoke;
    LONG references;
    UiAccessibility *accessibility;
    UiId id;
};

static IRawElementProviderSimpleVtbl simple_vtable;
static IRawElementProviderFragmentVtbl fragment_vtable;
static IRawElementProviderFragmentRootVtbl root_vtable;
static IInvokeProviderVtbl invoke_vtable;

static void accessibility_add_ref(UiAccessibility *a) { InterlockedIncrement(&a->references); }
static void accessibility_release(UiAccessibility *a) {
    if (!InterlockedDecrement(&a->references)) free(a);
}
static UiProvider *from_simple(IRawElementProviderSimple *i) { return (UiProvider *)((char *)i-offsetof(UiProvider,simple)); }
static UiProvider *from_fragment(IRawElementProviderFragment *i) { return (UiProvider *)((char *)i-offsetof(UiProvider,fragment)); }
static UiProvider *from_root(IRawElementProviderFragmentRoot *i) { return (UiProvider *)((char *)i-offsetof(UiProvider,root)); }
static UiProvider *from_invoke(IInvokeProvider *i) { return (UiProvider *)((char *)i-offsetof(UiProvider,invoke)); }

static bool available(UiProvider *p) {
    return p->accessibility->ui && ui_node(p->accessibility->ui,p->id) && ui_visible(p->accessibility->ui,p->id);
}
static UiProvider *provider_new(UiAccessibility *a, UiId id) {
    if (!a || !a->ui || !ui_node(a->ui,id) || !ui_visible(a->ui,id)) return NULL;
    UiProvider *p=calloc(1,sizeof *p);
    if (!p) return NULL;
    p->simple.lpVtbl=&simple_vtable; p->fragment.lpVtbl=&fragment_vtable;
    p->root.lpVtbl=&root_vtable; p->invoke.lpVtbl=&invoke_vtable;
    p->references=1; p->accessibility=a; p->id=id; accessibility_add_ref(a);
    return p;
}
static ULONG provider_add_ref(UiProvider *p) { return (ULONG)InterlockedIncrement(&p->references); }
static ULONG provider_release(UiProvider *p) {
    ULONG references=(ULONG)InterlockedDecrement(&p->references);
    if (!references) { accessibility_release(p->accessibility); free(p); }
    return references;
}
static HRESULT provider_query(UiProvider *p, REFIID iid, void **result) {
    if (!result) return E_POINTER;
    *result=NULL;
    if (IsEqualIID(iid,&IID_IUnknown) || IsEqualIID(iid,&IID_IRawElementProviderSimple)) *result=&p->simple;
    else if (IsEqualIID(iid,&IID_IRawElementProviderFragment)) *result=&p->fragment;
    else if (IsEqualIID(iid,&IID_IRawElementProviderFragmentRoot) && p->accessibility->ui && p->id==p->accessibility->ui->root) *result=&p->root;
    else if (IsEqualIID(iid,&IID_IInvokeProvider) && available(p) && ui_node(p->accessibility->ui,p->id)->kind==UI_BUTTON) *result=&p->invoke;
    else return E_NOINTERFACE;
    provider_add_ref(p); return S_OK;
}

static HRESULT STDMETHODCALLTYPE simple_query(IRawElementProviderSimple *i, REFIID iid, void **r) { return provider_query(from_simple(i),iid,r); }
static ULONG STDMETHODCALLTYPE simple_add(IRawElementProviderSimple *i) { return provider_add_ref(from_simple(i)); }
static ULONG STDMETHODCALLTYPE simple_release(IRawElementProviderSimple *i) { return provider_release(from_simple(i)); }
static HRESULT STDMETHODCALLTYPE fragment_query(IRawElementProviderFragment *i, REFIID iid, void **r) { return provider_query(from_fragment(i),iid,r); }
static ULONG STDMETHODCALLTYPE fragment_add(IRawElementProviderFragment *i) { return provider_add_ref(from_fragment(i)); }
static ULONG STDMETHODCALLTYPE fragment_release(IRawElementProviderFragment *i) { return provider_release(from_fragment(i)); }
static HRESULT STDMETHODCALLTYPE root_query(IRawElementProviderFragmentRoot *i, REFIID iid, void **r) { return provider_query(from_root(i),iid,r); }
static ULONG STDMETHODCALLTYPE root_add(IRawElementProviderFragmentRoot *i) { return provider_add_ref(from_root(i)); }
static ULONG STDMETHODCALLTYPE root_release(IRawElementProviderFragmentRoot *i) { return provider_release(from_root(i)); }
static HRESULT STDMETHODCALLTYPE invoke_query(IInvokeProvider *i, REFIID iid, void **r) { return provider_query(from_invoke(i),iid,r); }
static ULONG STDMETHODCALLTYPE invoke_add(IInvokeProvider *i) { return provider_add_ref(from_invoke(i)); }
static ULONG STDMETHODCALLTYPE invoke_release(IInvokeProvider *i) { return provider_release(from_invoke(i)); }

static HRESULT STDMETHODCALLTYPE provider_options(IRawElementProviderSimple *i, enum ProviderOptions *result) {
    (void)i; if (!result) return E_POINTER; *result=ProviderOptions_ServerSideProvider; return S_OK;
}
static HRESULT STDMETHODCALLTYPE pattern(IRawElementProviderSimple *i, PATTERNID pattern_id, IUnknown **result) {
    if (!result) return E_POINTER;
    *result=NULL;
    if (pattern_id==UIA_InvokePatternId) {
        HRESULT hr=provider_query(from_simple(i),&IID_IInvokeProvider,(void **)result);
        return hr==E_NOINTERFACE?S_OK:hr;
    }
    return S_OK;
}
static bool keyboard_focusable(UiKind kind) {
    return kind==UI_BUTTON || kind==UI_CHECKBOX || kind==UI_SWITCH ||
        kind==UI_SLIDER || kind==UI_TEXTBOX || kind==UI_SCROLL;
}
static int control_type(UiKind kind) {
    switch (kind) {
    case UI_BUTTON: return UIA_ButtonControlTypeId;
    case UI_CHECKBOX: case UI_SWITCH: return UIA_CheckBoxControlTypeId;
    case UI_SLIDER: return UIA_SliderControlTypeId;
    case UI_TEXTBOX: return UIA_EditControlTypeId;
    case UI_PROGRESS: return UIA_ProgressBarControlTypeId;
    case UI_SEPARATOR: return UIA_SeparatorControlTypeId;
    case UI_LABEL: return UIA_TextControlTypeId;
    case UI_SCROLL: return UIA_PaneControlTypeId;
    default: return UIA_GroupControlTypeId;
    }
}
static HRESULT string_variant(VARIANT *value, const wchar_t *text) {
    V_VT(value)=VT_BSTR; V_BSTR(value)=SysAllocString(text ? text : L"");
    return V_BSTR(value) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE property(IRawElementProviderSimple *i, PROPERTYID property_id, VARIANT *value) {
    if (!value) return E_POINTER;
    VariantInit(value); UiProvider *p=from_simple(i);
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    Ui *u=p->accessibility->ui; UiNode *n=ui_node(u,p->id);
    switch (property_id) {
    case UIA_ControlTypePropertyId: V_VT(value)=VT_I4; V_I4(value)=p->id==u->root?UIA_WindowControlTypeId:control_type(n->kind); break;
    case UIA_NamePropertyId: return string_variant(value,ui_accessible_name(u,p->id));
    case UIA_HelpTextPropertyId: return string_variant(value,n->help_text);
    case UIA_AutomationIdPropertyId: {
        wchar_t id[32]; swprintf(id,32,L"darkui-%08lX",(unsigned long)p->id); return string_variant(value,id);
    }
    case UIA_FrameworkIdPropertyId: return string_variant(value,L"DarkUI");
    case UIA_HasKeyboardFocusPropertyId: V_VT(value)=VT_BOOL; V_BOOL(value)=u->focus==p->id || (p->id==u->root && !u->focus)?VARIANT_TRUE:VARIANT_FALSE; break;
    case UIA_IsKeyboardFocusablePropertyId: V_VT(value)=VT_BOOL; V_BOOL(value)=p->id==u->root || keyboard_focusable(n->kind)?VARIANT_TRUE:VARIANT_FALSE; break;
    case UIA_IsEnabledPropertyId: V_VT(value)=VT_BOOL; V_BOOL(value)=ui_enabled(u,p->id)?VARIANT_TRUE:VARIANT_FALSE; break;
    case UIA_IsOffscreenPropertyId: V_VT(value)=VT_BOOL; V_BOOL(value)=n->clip.w<=0 || n->clip.h<=0?VARIANT_TRUE:VARIANT_FALSE; break;
    default: break;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE host_provider(IRawElementProviderSimple *i, IRawElementProviderSimple **result) {
    if (!result) return E_POINTER;
    *result=NULL; UiProvider *p=from_simple(i);
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (p->id==p->accessibility->ui->root) return UiaHostProviderFromHwnd(p->accessibility->window,result);
    return S_OK;
}

static UiId next_visible(Ui *u, UiId id) {
    UiNode *n=ui_node(u,id);
    for (UiId next=n?n->next:UI_NONE;next;next=ui_node(u,next)->next) if (ui_visible(u,next)) return next;
    return UI_NONE;
}
static UiId previous_visible(Ui *u, UiId id) {
    UiNode *n=ui_node(u,id), *parent=n?ui_node(u,n->parent):NULL; UiId previous=UI_NONE;
    for (UiId child=parent?parent->first:UI_NONE;child && child!=id;child=ui_node(u,child)->next)
        if (ui_visible(u,child)) previous=child;
    return previous;
}
static UiId child_visible(Ui *u, UiId id, bool last) {
    UiNode *n=ui_node(u,id); UiId result=UI_NONE;
    for (UiId child=n?n->first:UI_NONE;child;child=ui_node(u,child)->next)
        if (ui_visible(u,child)) { result=child; if (!last) break; }
    return result;
}
static HRESULT fragment_for(UiAccessibility *a, UiId id, IRawElementProviderFragment **result) {
    if (!result) return E_POINTER;
    *result=NULL;
    if (!id) return S_OK;
    if (!a || !a->ui || !ui_node(a->ui,id) || !ui_visible(a->ui,id)) return UIA_E_ELEMENTNOTAVAILABLE;
    UiProvider *provider=provider_new(a,id); if (!provider) return E_OUTOFMEMORY;
    *result=&provider->fragment; return S_OK;
}
static HRESULT STDMETHODCALLTYPE navigate(IRawElementProviderFragment *i, enum NavigateDirection direction, IRawElementProviderFragment **result) {
    UiProvider *p=from_fragment(i); if (!result) return E_POINTER; *result=NULL;
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    Ui *u=p->accessibility->ui; UiNode *n=ui_node(u,p->id); UiId target=UI_NONE;
    if (direction==NavigateDirection_Parent) target=n->parent;
    else if (direction==NavigateDirection_NextSibling) target=next_visible(u,p->id);
    else if (direction==NavigateDirection_PreviousSibling) target=previous_visible(u,p->id);
    else if (direction==NavigateDirection_FirstChild) target=child_visible(u,p->id,false);
    else if (direction==NavigateDirection_LastChild) target=child_visible(u,p->id,true);
    return target?fragment_for(p->accessibility,target,result):S_OK;
}
static HRESULT STDMETHODCALLTYPE runtime_id(IRawElementProviderFragment *i, SAFEARRAY **result) {
    if (!result) return E_POINTER;
    *result=NULL; UiProvider *p=from_fragment(i);
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (p->id==p->accessibility->ui->root) return S_OK;
    SAFEARRAY *ids=SafeArrayCreateVector(VT_I4,0,2); if (!ids) return E_OUTOFMEMORY;
    LONG index=0, value=UiaAppendRuntimeId; HRESULT hr=SafeArrayPutElement(ids,&index,&value);
    index=1; value=(LONG)p->id; if (SUCCEEDED(hr)) hr=SafeArrayPutElement(ids,&index,&value);
    if (FAILED(hr)) { SafeArrayDestroy(ids); return hr; }
    *result=ids; return S_OK;
}
static HRESULT STDMETHODCALLTYPE bounding_rect(IRawElementProviderFragment *i, struct UiaRect *result) {
    if (!result) return E_POINTER;
    *result=(struct UiaRect){0}; UiProvider *p=from_fragment(i);
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    UiNode *n=ui_node(p->accessibility->ui,p->id); float dpi=(float)GetDpiForWindow(p->accessibility->window);
    if (dpi<=0) dpi=96;
    POINT origin={0}; ClientToScreen(p->accessibility->window,&origin);
    result->left=origin.x+n->rect.x*dpi/96; result->top=origin.y+n->rect.y*dpi/96;
    result->width=n->rect.w*dpi/96; result->height=n->rect.h*dpi/96; return S_OK;
}
static HRESULT STDMETHODCALLTYPE embedded_roots(IRawElementProviderFragment *i, SAFEARRAY **result) {
    (void)i; if (!result) return E_POINTER; *result=NULL; return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_focus(IRawElementProviderFragment *i) {
    UiProvider *p=from_fragment(i); if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!ui_enabled(p->accessibility->ui,p->id)) return UIA_E_ELEMENTNOTENABLED;
    if (p->id==p->accessibility->ui->root) {
        ui_focus(p->accessibility->ui,UI_NONE,true); SetFocus(p->accessibility->window); return S_OK;
    }
    ui_focus(p->accessibility->ui,p->id,true);
    if (p->accessibility->ui->focus!=p->id) return E_INVALIDARG;
    SetFocus(p->accessibility->window); return S_OK;
}
static HRESULT STDMETHODCALLTYPE fragment_root(IRawElementProviderFragment *i, IRawElementProviderFragmentRoot **result) {
    if (!result) return E_POINTER;
    *result=NULL; UiProvider *p=from_fragment(i);
    if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    UiProvider *root=provider_new(p->accessibility,p->accessibility->ui->root); if (!root) return E_OUTOFMEMORY;
    *result=&root->root; return S_OK;
}
static UiId element_at(Ui *u, UiId id, float x, float y) {
    UiNode *n=ui_node(u,id);
    if (!n || !ui_visible(u,id) || !ui_contains(n->clip,x,y)) return UI_NONE;
    UiId result=id;
    for (UiId child=n->first;child;child=ui_node(u,child)->next) {
        UiId hit=element_at(u,child,x,y); if (hit) result=hit;
    }
    return result;
}
static HRESULT STDMETHODCALLTYPE from_point(IRawElementProviderFragmentRoot *i, double x, double y, IRawElementProviderFragment **result) {
    UiProvider *p=from_root(i); if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT point={(LONG)x,(LONG)y}; ScreenToClient(p->accessibility->window,&point);
    float dpi=(float)GetDpiForWindow(p->accessibility->window); if (dpi<=0) dpi=96;
    UiId id=element_at(p->accessibility->ui,p->id,point.x*96/dpi,point.y*96/dpi);
    return fragment_for(p->accessibility,id?id:p->id,result);
}
static HRESULT STDMETHODCALLTYPE get_focus(IRawElementProviderFragmentRoot *i, IRawElementProviderFragment **result) {
    if (!result) return E_POINTER;
    *result=NULL;
    UiProvider *p=from_root(i); if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    UiId id=p->accessibility->ui->focus; return id?fragment_for(p->accessibility,id,result):S_OK;
}
static HRESULT STDMETHODCALLTYPE invoke_action(IInvokeProvider *i) {
    UiProvider *p=from_invoke(i); if (!available(p)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!ui_enabled(p->accessibility->ui,p->id)) return UIA_E_ELEMENTNOTENABLED;
    return PostMessageW(p->accessibility->window,UI_WM_ACCESSIBILITY_INVOKE,(WPARAM)p->id,0)?S_OK:HRESULT_FROM_WIN32(GetLastError());
}

static IRawElementProviderSimpleVtbl simple_vtable={simple_query,simple_add,simple_release,provider_options,pattern,property,host_provider};
static IRawElementProviderFragmentVtbl fragment_vtable={fragment_query,fragment_add,fragment_release,navigate,runtime_id,bounding_rect,embedded_roots,set_focus,fragment_root};
static IRawElementProviderFragmentRootVtbl root_vtable={root_query,root_add,root_release,from_point,get_focus};
static IInvokeProviderVtbl invoke_vtable={invoke_query,invoke_add,invoke_release,invoke_action};

UiAccessibility *ui_accessibility_create(HWND window, Ui *ui) {
    if (!window || !ui) return NULL;
    UiAccessibility *a=calloc(1,sizeof *a);
    if (a) { a->references=1; a->window=window; a->ui=ui; } return a;
}
void ui_accessibility_destroy(UiAccessibility *a) {
    if (a) { a->ui=NULL; a->window=NULL; accessibility_release(a); }
}
IRawElementProviderSimple *ui_accessibility_root_provider(UiAccessibility *a) {
    UiProvider *p=a && a->ui?provider_new(a,a->ui->root):NULL; return p?&p->simple:NULL;
}
LRESULT ui_accessibility_get_object(UiAccessibility *a, WPARAM wparam, LPARAM lparam) {
    if (!a || lparam!=UiaRootObjectId) return 0;
    IRawElementProviderSimple *root=ui_accessibility_root_provider(a); if (!root) return 0;
    LRESULT result=UiaReturnRawElementProvider(a->window,wparam,lparam,root); IRawElementProviderSimple_Release(root); return result;
}
bool ui_accessibility_handle_message(UiAccessibility *a, UINT message, WPARAM wparam) {
    if (!a || message!=UI_WM_ACCESSIBILITY_INVOKE) return false;
    UiId id=(UiId)wparam; UiProvider *p=provider_new(a,id);
    if (p) {
        if (ui_invoke(a->ui,id)) UiaRaiseAutomationEvent(&p->simple,UIA_Invoke_InvokedEventId);
        provider_release(p);
    }
    return true;
}
void ui_accessibility_focus_changed(UiAccessibility *a, UiId id) {
    UiProvider *p=provider_new(a,id); if (!p) return;
    UiaRaiseAutomationEvent(&p->simple,UIA_AutomationFocusChangedEventId); provider_release(p);
}
