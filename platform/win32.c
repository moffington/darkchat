#include "win32.h"
#include "accessibility.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
    UiWindowConfig config;
    UiRenderer renderer;
    UiAccessibility *accessibility;
    HWND window, edit;
    WNDPROC edit_proc;
    UiId editing;
    HFONT edit_font;
    UiFont edit_font_role;
    HBRUSH edit_background, background;
    float dpi;
    bool tracking, syncing, edit_change, minimized;
    unsigned retries;
    UiId accessibility_focus;
} UiHost;

static void flush(UiHost *host);
static int px(UiHost *h, float dip) { return (int)lroundf(dip*h->dpi/96.f); }
static float dip(UiHost *h, int pixel) { return pixel*96.f/h->dpi; }
static COLORREF rgb(UiColor color) { return RGB(color.r,color.g,color.b); }
static bool key_from_win32(WPARAM w, UiKey *key) {
    switch (w) {
    case VK_TAB: *key=UI_KEY_TAB; break;
    case VK_RETURN: *key=UI_KEY_ENTER; break;
    case VK_SPACE: *key=UI_KEY_SPACE; break;
    case VK_ESCAPE: *key=UI_KEY_ESCAPE; break;
    case VK_LEFT: *key=UI_KEY_LEFT; break;
    case VK_RIGHT: *key=UI_KEY_RIGHT; break;
    case VK_UP: *key=UI_KEY_UP; break;
    case VK_DOWN: *key=UI_KEY_DOWN; break;
    case VK_HOME: *key=UI_KEY_HOME; break;
    case VK_END: *key=UI_KEY_END; break;
    case VK_PRIOR: *key=UI_KEY_PAGE_UP; break;
    case VK_NEXT: *key=UI_KEY_PAGE_DOWN; break;
    default: return false;
    }
    return true;
}
static bool key_event(UiHost *h, UINT message, WPARAM w, LPARAM l) {
    UiKey key;
    if (!key_from_win32(w,&key)) return false;
    ui_key(h->config.ui,key,message==WM_KEYDOWN,(GetKeyState(VK_SHIFT)&0x8000)!=0,(l&(1L<<30))!=0);
    flush(h); return true;
}
static void wheel(UiHost *h, WPARAM w, LPARAM l) {
    POINT point={GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h->window,&point);
    UINT lines=3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    float step=lines==WHEEL_PAGESCROLL ? h->config.ui->height*.9f : lines*h->config.ui->theme.control_height;
    float delta=-(float)GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*step;
    ui_scroll(h->config.ui,dip(h,point.x),dip(h,point.y),delta); flush(h);
}
static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    UiHost *h=(UiHost *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (!h) return DefWindowProcW(window,message,w,l);
    if (message==WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    if (message==WM_KEYDOWN && w=='A' && (GetKeyState(VK_CONTROL)&0x8000)) {
        SendMessageW(window,EM_SETSEL,0,-1); return 0;
    }
    if ((message==WM_KEYDOWN || message==WM_KEYUP) && (w==VK_TAB || w==VK_ESCAPE || w==VK_RETURN)) {
        if (w==VK_TAB) key_event(h,message,w,l);
        else if (message==WM_KEYDOWN) {
            ui_focus(h->config.ui,UI_NONE,true); SetFocus(h->window); flush(h);
        }
        return 0;
    }
    if (message==WM_CHAR && (w==L'\t' || w==L'\r' || w==27)) return 0;
    if (message==WM_MOUSEWHEEL) { wheel(h,w,l); return 0; }
    if (message==WM_SETFOCUS) { ui_set_active(h->config.ui,true); }
    if (message==WM_KILLFOCUS && (HWND)w!=h->window) {
        ui_set_active(h->config.ui,false); InvalidateRect(h->window,NULL,FALSE);
    }
    return CallWindowProcW(h->edit_proc,window,message,w,l);
}
static bool update_font(UiHost *h, UiFont role) {
    UiTheme *t=&h->config.ui->theme;
    HFONT font=CreateFontW(-px(h,t->font_size[role]),0,0,0,t->font_weight[role],FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,t->font_family);
    if (!font) return false;
    SendMessageW(h->edit,WM_SETFONT,(WPARAM)font,TRUE);
    if (h->edit_font) DeleteObject(h->edit_font);
    h->edit_font=font; h->edit_font_role=role; return true;
}
static void sync_edit(UiHost *h) {
    if (!h->edit || h->syncing) return;
    h->syncing=true;
    Ui *u=h->config.ui;
    UiNode *n=ui_node(u,u->focus);
    UiId id=n && ui_enabled(u,u->focus) && n->kind==UI_TEXTBOX ? u->focus : UI_NONE;
    UiRect area={0};
    if (id) {
        if (h->edit_font_role!=n->style.font) update_font(h,n->style.font);
        area=(UiRect){n->rect.x+8,n->rect.y+(n->rect.h-u->theme.font_size[n->style.font]*1.5f)/2,
            n->rect.w>16?n->rect.w-16:0,u->theme.font_size[n->style.font]*1.5f};
        UiRect clipped=ui_intersect(area,n->clip);
        if (clipped.w<=0 || clipped.h<=0) id=UI_NONE;
    }
    if (!id) {
        if (GetFocus()==h->edit) SetFocus(h->window);
        ShowWindow(h->edit,SW_HIDE); h->editing=UI_NONE;
    } else {
        bool changed=h->editing!=id;
        if (changed) {
            h->editing=id; SetWindowTextW(h->edit,n->text);
            SendMessageW(h->edit,EM_SETSEL,0,-1);
        } else if (!h->edit_change) {
            wchar_t text[UI_TEXT_CAPACITY]; GetWindowTextW(h->edit,text,UI_TEXT_CAPACITY);
            if (wcscmp(text,n->text)) SetWindowTextW(h->edit,n->text);
        }
        UiRect clip=ui_intersect(area,n->clip);
        int x=px(h,area.x), y=px(h,area.y);
        SetWindowPos(h->edit,NULL,x,y,px(h,area.x+area.w)-x,px(h,area.y+area.h)-y,SWP_NOZORDER|SWP_NOACTIVATE);
        HRGN region=CreateRectRgn(px(h,clip.x)-x,px(h,clip.y)-y,px(h,clip.x+clip.w)-x,px(h,clip.y+clip.h)-y);
        if (region && !SetWindowRgn(h->edit,region,TRUE)) DeleteObject(region);
        ShowWindow(h->edit,SW_SHOWNOACTIVATE);
        if (u->window_active && GetFocus()!=h->edit) SetFocus(h->edit);
    }
    h->syncing=false;
}
static void layout(UiHost *h) {
    if (h->minimized) return;
    RECT r; GetClientRect(h->window,&r);
    float width=dip(h,r.right), height=dip(h,r.bottom);
    if (h->config.on_resize) h->config.on_resize(h->config.user,width,height);
    Ui *u=h->config.ui;
    bool resized=u->width!=width || u->height!=height;
    if (u->layout_dirty || resized) ui_layout(u,width,height);
    if (resized && u->focus) ui_focus(u,u->focus,u->keyboard_focus);
}
static void flush(UiHost *h) {
    layout(h); sync_edit(h);
    if (h->accessibility && h->accessibility_focus!=h->config.ui->focus) {
        h->accessibility_focus=h->config.ui->focus;
        ui_accessibility_focus_changed(h->accessibility,h->accessibility_focus?h->accessibility_focus:h->config.ui->root);
    }
    if (GetCapture()==h->window && !h->config.ui->pressed && !h->config.ui->drag_scroll) ReleaseCapture();
    if (h->config.ui->paint_dirty) InvalidateRect(h->window,NULL,FALSE);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    UiHost *h=(UiHost *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) {
        h=((CREATESTRUCTW *)l)->lpCreateParams; h->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)h);
    }
    if (!h) return DefWindowProcW(window,message,w,l);
    Ui *u=h->config.ui;
    switch (message) {
    case WM_CREATE: {
        UINT dpi=GetDpiForWindow(window); h->dpi=dpi?(float)dpi:96;
        h->renderer.dpi=h->dpi;
        h->edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,
            0,0,0,0,window,(HMENU)(INT_PTR)1,GetModuleHandleW(NULL),NULL);
        if (!h->edit) return -1;
        SetWindowLongPtrW(h->edit,GWLP_USERDATA,(LONG_PTR)h);
        h->edit_proc=(WNDPROC)SetWindowLongPtrW(h->edit,GWLP_WNDPROC,(LONG_PTR)edit_proc);
        SendMessageW(h->edit,EM_SETLIMITTEXT,UI_TEXT_CAPACITY-1,0);
        SendMessageW(h->edit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
        if (!update_font(h,UI_BODY)) return -1;
        if (!ui_accessible_name(u,u->root)[0]) ui_set_accessible_name(u,u->root,h->config.title);
        h->accessibility=ui_accessibility_create(window,u);
        if (!h->accessibility) return -1;
        return 0;
    }
    case WM_GETOBJECT: {
        LRESULT result=ui_accessibility_get_object(h->accessibility,w,l);
        if (result) return result;
        break;
    }
    case UI_WM_ACCESSIBILITY_INVOKE:
        ui_accessibility_handle_message(h->accessibility,message,w); flush(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint; BeginPaint(window,&paint);
        if (!h->minimized) {
            layout(h); sync_edit(h);
            HRESULT hr=renderer_paint(&h->renderer,window,u,h->editing);
            if (FAILED(hr)) {
                FillRect(paint.hdc,&paint.rcPaint,h->background);
                wchar_t error[96]; swprintf(error,96,L"Dark UI: rendering failed (0x%08lX)\n",(unsigned long)hr);
                OutputDebugStringW(error);
                /* Retry a transient target loss without a busy invalidation loop. */
                if (h->retries++<3) SetTimer(window,1,250,NULL);
            } else { h->retries=0; KillTimer(window,1); }
        }
        EndPaint(window,&paint); return 0;
    }
    case WM_TIMER:
        if (w==1) { KillTimer(window,1); InvalidateRect(window,NULL,FALSE); }
        return 0;
    case WM_SIZE:
        h->minimized=w==SIZE_MINIMIZED;
        if (!h->minimized) {
            RECT r; GetClientRect(window,&r);
            renderer_resize(&h->renderer,(UINT)r.right,(UINT)r.bottom,h->dpi);
            ui_invalidate(u,true); flush(h);
        }
        return 0;
    case WM_DPICHANGED: {
        h->dpi=(float)HIWORD(w);
        RECT *r=(RECT *)l;
        update_font(h,h->edit_font_role);
        renderer_resize(&h->renderer,0,0,h->dpi);
        ui_invalidate(u,true);
        SetWindowPos(window,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        flush(h); return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info=(MINMAXINFO *)l;
        UINT dpi=h->dpi>0?(UINT)h->dpi:GetDpiForSystem();
        RECT r={0,0,MulDiv(h->config.min_width,(int)dpi,96),MulDiv(h->config.min_height,(int)dpi,96)};
        AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
        info->ptMinTrackSize.x=r.right-r.left; info->ptMinTrackSize.y=r.bottom-r.top;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!h->tracking) {
            TRACKMOUSEEVENT tracking={sizeof tracking,TME_LEAVE,window,0};
            h->tracking=TrackMouseEvent(&tracking)!=FALSE;
        }
        ui_pointer_move(u,dip(h,GET_X_LPARAM(l)),dip(h,GET_Y_LPARAM(l))); flush(h); return 0;
    }
    case WM_MOUSELEAVE: h->tracking=false; ui_pointer_leave(u); flush(h); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window); layout(h);
        ui_pointer_down(u,dip(h,GET_X_LPARAM(l)),dip(h,GET_Y_LPARAM(l)));
        if (u->pressed) SetCapture(window);
        flush(h);
        if (h->editing) {
            /* Forward the initiating click so the native editor places its caret. */
            if (GetCapture()==window) ReleaseCapture();
            ui_cancel_input(u);
            POINT point={GET_X_LPARAM(l),GET_Y_LPARAM(l)}; MapWindowPoints(window,h->edit,&point,1);
            SendMessageW(h->edit,WM_LBUTTONDOWN,w,MAKELPARAM(point.x,point.y));
        }
        return 0;
    }
    case WM_LBUTTONUP:
        ui_pointer_up(u,dip(h,GET_X_LPARAM(l)),dip(h,GET_Y_LPARAM(l))); flush(h); return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l!=window) { ui_cancel_input(u); InvalidateRect(window,NULL,FALSE); }
        return 0;
    case WM_CANCELMODE:
        ui_cancel_input(u); if (GetCapture()==window) ReleaseCapture(); flush(h); return 0;
    case WM_MOUSEWHEEL: wheel(h,w,l); return 0;
    case WM_KEYDOWN: case WM_KEYUP:
        if (key_event(h,message,w,l)) return 0;
        break;
    case WM_CHAR: return 0;
    case WM_SETFOCUS: ui_set_active(u,true); flush(h); return 0;
    case WM_KILLFOCUS:
        if ((HWND)w!=h->edit) { ui_set_active(u,false); flush(h); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w)==WA_INACTIVE) { ui_set_active(u,false); flush(h); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l)==HTCLIENT) {
            UiNode *n=ui_node(u,u->hot);
            SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(n && n->kind==UI_TEXTBOX?32513:32512))); return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w,rgb(u->theme.colors[UI_TEXT]));
        SetBkColor((HDC)w,rgb(u->theme.colors[UI_TRACK]));
        return (LRESULT)h->edit_background;
    case WM_COMMAND:
        if ((HWND)l==h->edit && HIWORD(w)==EN_CHANGE && !h->syncing && h->editing) {
            wchar_t text[UI_TEXT_CAPACITY]; GetWindowTextW(h->edit,text,UI_TEXT_CAPACITY);
            h->edit_change=true; ui_set_text(u,h->editing,text);
            if (u->on_event) u->on_event(u->event_user,u,(UiEvent){h->editing,UI_CHANGE});
            flush(h); h->edit_change=false;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window,1); renderer_drop_target(&h->renderer); PostQuitMessage(0); return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    }
    return DefWindowProcW(window,message,w,l);
}
int ui_win32_run(HINSTANCE instance, int show, const UiWindowConfig *config) {
    if (!config || !config->ui) return 1;
    /* Windows 10 1703+ baseline. Manifest also declares PerMonitorV2. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError()!=ERROR_ACCESS_DENIED) return 1;
    HRESULT com=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    UiHost *h=calloc(1,sizeof *h);
    int result=1;
    if (!h) { CoUninitialize(); return result; }
    h->config=*config; h->dpi=(float)GetDpiForSystem();
    HRESULT hr=renderer_init(&h->renderer,&config->ui->theme);
    if (FAILED(hr)) goto cleanup;
    config->ui->measure=renderer_measure; config->ui->measure_user=&h->renderer;
    h->edit_background=CreateSolidBrush(rgb(config->ui->theme.colors[UI_TRACK]));
    h->background=CreateSolidBrush(rgb(config->ui->theme.colors[UI_BG]));
    if (!h->edit_background || !h->background) goto cleanup;
    WNDCLASSEXW cls={0}; cls.cbSize=sizeof cls; cls.lpfnWndProc=window_proc;
    cls.hInstance=instance; cls.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); cls.lpszClassName=L"DarkUi.Foundation";
    if (!RegisterClassExW(&cls)) goto cleanup;
    RECT bounds={0,0,px(h,(float)config->width),px(h,(float)config->height)};
    AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0,(UINT)h->dpi);
    HWND window=CreateWindowExW(0,cls.lpszClassName,config->title,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,NULL,NULL,instance,h);
    if (window) {
        BOOL dark=TRUE; DwmSetWindowAttribute(window,20,&dark,sizeof dark);
        ShowWindow(window,show); UpdateWindow(window);
        MSG message;
        BOOL got;
        while ((got=GetMessageW(&message,NULL,0,0))>0) { TranslateMessage(&message); DispatchMessageW(&message); }
        result=got<0?1:(int)message.wParam;
        if (IsWindow(window)) DestroyWindow(window);
    }
    UnregisterClassW(cls.lpszClassName,instance);
cleanup:
    ui_accessibility_destroy(h->accessibility);
    config->ui->measure=NULL; config->ui->measure_user=NULL;
    renderer_dispose(&h->renderer);
    if (h->edit_font) DeleteObject(h->edit_font);
    if (h->edit_background) DeleteObject(h->edit_background);
    if (h->background) DeleteObject(h->background);
    free(h); CoUninitialize();
    if (result) MessageBoxW(NULL,L"Dark UI could not initialize. Check the graphics runtime and available resources.",L"Dark UI",MB_OK|MB_ICONERROR);
    return result;
}
