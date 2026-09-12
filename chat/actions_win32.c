#include "actions_win32.h"
#include <stdlib.h>
#include <string.h>

typedef struct { HWND edit; wchar_t *text; size_t capacity; bool done, accepted, multiline; } EditDialog;
static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    EditDialog *d=(EditDialog *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) {
        d=((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)d);
    }
    if (!d) return DefWindowProcW(window,message,w,l);
    switch (message) {
    case WM_CREATE: {
        UINT dpi=GetDpiForWindow(window);
        int unit=MulDiv(10,(int)dpi,96);
        RECT r; GetClientRect(window,&r);
        d->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",d->text,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOVSCROLL|
            (d->multiline ? ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL : ES_AUTOHSCROLL),
            unit,unit,r.right-2*unit,r.bottom-6*unit,window,(HMENU)10,NULL,NULL);
        SendMessageW(d->edit,EM_SETLIMITTEXT,d->capacity-1,0);
        SendMessageW(d->edit,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
        CreateWindowExW(0,L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            r.right-19*unit,r.bottom-4*unit,8*unit,3*unit,window,(HMENU)IDOK,NULL,NULL);
        CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            r.right-10*unit,r.bottom-4*unit,9*unit,3*unit,window,(HMENU)IDCANCEL,NULL,NULL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w)==IDOK) {
            GetWindowTextW(d->edit,d->text,(int)d->capacity);
            d->accepted=true; d->done=true;
        } else if (LOWORD(w)==IDCANCEL) d->done=true;
        return 0;
    case WM_CLOSE: d->done=true; return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
bool chat_edit_dialog(HWND owner, const wchar_t *title, wchar_t *text, size_t capacity, bool multiline) {
    WNDCLASSW cls={0}; cls.lpfnWndProc=edit_proc; cls.hInstance=GetModuleHandleW(NULL);
    cls.lpszClassName=L"DarkChat.Edit"; cls.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));
    cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassW(&cls);
    EditDialog d={0}; d.text=text; d.capacity=capacity; d.multiline=multiline;
    RECT r; GetWindowRect(owner,&r);
    UINT dpi=GetDpiForWindow(owner);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,title,
        WS_CAPTION|WS_SYSMENU|WS_POPUP,r.left+40,r.top+60,MulDiv(560,dpi,96),
        MulDiv(multiline ? 350 : 150,dpi,96),owner,NULL,cls.hInstance,&d);
    if (!window) return false;
    EnableWindow(owner,FALSE); ShowWindow(window,SW_SHOW); SetFocus(d.edit);
    SendMessageW(d.edit,EM_SETSEL,0,-1);
    MSG msg;
    while (!d.done) {
        BOOL got=GetMessageW(&msg,NULL,0,0);
        if (got<=0) { if (!got) PostQuitMessage((int)msg.wParam); break; }
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE) { d.done=true; continue; }
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN &&
            (!multiline || (GetKeyState(VK_CONTROL)&0x8000))) {
            SendMessageW(window,WM_COMMAND,IDOK,0); continue;
        }
        if (!IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner,TRUE); DestroyWindow(window); SetActiveWindow(owner);
    return d.accepted;
}
bool chat_copy_text(HWND owner,const wchar_t *text) {
    size_t bytes=(wcslen(text)+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if (!memory) return false;
    void *target=GlobalLock(memory);
    if (!target) { GlobalFree(memory); return false; }
    memcpy(target,text,bytes); GlobalUnlock(memory);
    bool ok=false;
    if (OpenClipboard(owner)) {
        if (EmptyClipboard()) ok=SetClipboardData(CF_UNICODETEXT,memory)!=NULL;
        CloseClipboard();
    }
    if (!ok) GlobalFree(memory);
    return ok;
}
HMENU chat_actions_menu(void) {
    HMENU bar=CreateMenu(), conversation=CreatePopupMenu(), response=CreatePopupMenu(), settings=CreatePopupMenu();
    AppendMenuW(conversation,MF_STRING,ACTION_NEW,L"&New conversation");
    AppendMenuW(conversation,MF_STRING,ACTION_RENAME,L"&Rename...");
    AppendMenuW(conversation,MF_STRING,ACTION_DELETE,L"&Delete...");
    AppendMenuW(conversation,MF_STRING,ACTION_DELETE_ALL,L"Delete &all...");
    AppendMenuW(conversation,MF_STRING,ACTION_CLEAR,L"&Clear messages...");
    AppendMenuW(response,MF_STRING,ACTION_RETRY,L"&Retry unsuccessful response");
    AppendMenuW(response,MF_STRING,ACTION_REGENERATE,L"Re&generate last response");
    AppendMenuW(response,MF_STRING,ACTION_EDIT,L"&Edit latest user message...");
    AppendMenuW(response,MF_STRING,ACTION_CANCEL_EDIT,L"Cancel edit mode");
    AppendMenuW(response,MF_SEPARATOR,0,NULL);
    AppendMenuW(response,MF_STRING,ACTION_COPY,L"&Copy response");
    AppendMenuW(response,MF_STRING,ACTION_SELECTION,L"Copy transcript &selection");
    AppendMenuW(settings,MF_STRING,ACTION_SYSTEM,L"&System prompt...");
    AppendMenuW(settings,MF_STRING,ACTION_SIDEBAR,L"Sidebar &width...");
    AppendMenuW(settings,MF_STRING,ACTION_MODELS,L"&Model history / complete prefix (Ctrl+Space)");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)conversation,L"&Conversation");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)response,L"&Response");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)settings,L"&Settings");
    return bar;
}
