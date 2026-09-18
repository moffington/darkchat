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
static int routing_sort_action(ChatProviderSort sort) {
    switch (sort) {
    case CHAT_PROVIDER_SORT_PRICE: return ACTION_ROUTING_SORT_PRICE;
    case CHAT_PROVIDER_SORT_THROUGHPUT: return ACTION_ROUTING_SORT_THROUGHPUT;
    case CHAT_PROVIDER_SORT_LATENCY: return ACTION_ROUTING_SORT_LATENCY;
    default: return ACTION_ROUTING_SORT_DEFAULT;
    }
}
void chat_actions_sync_routing(HMENU menu, const Chat *chat) {
    if (!menu || !chat) return;
    const ChatProviderRouting *routing = &chat->provider_routing;
    /* Backend radios: a no-op for menus that do not carry them. */
    CheckMenuRadioItem(menu, ACTION_BACKEND_OPENROUTER, ACTION_BACKEND_OLLAMA,
        chat->backend == CHAT_BACKEND_OLLAMA ? ACTION_BACKEND_OLLAMA
                                             : ACTION_BACKEND_OPENROUTER,
        MF_BYCOMMAND);
    CheckMenuRadioItem(menu, ACTION_ROUTING_SORT_DEFAULT,
        ACTION_ROUTING_SORT_LATENCY, routing_sort_action(routing->sort),
        MF_BYCOMMAND);
    CheckMenuItem(menu, ACTION_ROUTING_ALLOW_FALLBACKS,
        MF_BYCOMMAND | (routing->disallow_fallbacks ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(menu, ACTION_ROUTING_DATA_COLLECTION,
        MF_BYCOMMAND | (routing->data_collection == CHAT_DATA_COLLECTION_DENY
            ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(menu, ACTION_ROUTING_ZDR,
        MF_BYCOMMAND | (routing->zdr ? MF_CHECKED : MF_UNCHECKED));
    /* Provider routing is OpenRouter-only: gray the whole group while a local
       Ollama backend is active so the controls cannot imply an effect. The
       host also ignores a stale activation with an explicit status. */
    UINT enable = MF_BYCOMMAND |
        (chat->backend == CHAT_BACKEND_OLLAMA ? MF_GRAYED : MF_ENABLED);
    for (UINT id = ACTION_ROUTING_SORT_DEFAULT; id <= ACTION_ROUTING_ZDR; id++)
        EnableMenuItem(menu, id, enable);
}
HMENU chat_actions_menu(const Chat *chat) {
    /* A popup root, not a menu bar: the only consumer tracks it directly
       with TrackPopupMenu, which does not render a CreateMenu() bar (it
       displays as an empty box). The three groups stay submenus. */
    HMENU bar=CreatePopupMenu(), conversation=CreatePopupMenu(), response=CreatePopupMenu(), settings=CreatePopupMenu();
    HMENU routing=CreatePopupMenu(), backend=CreatePopupMenu();
    AppendMenuW(conversation,MF_STRING,ACTION_NEW,L"&New conversation");
    AppendMenuW(conversation,MF_STRING,ACTION_RENAME,L"&Rename...");
    AppendMenuW(conversation,MF_STRING,ACTION_DELETE,L"&Delete...");
    AppendMenuW(conversation,MF_STRING,ACTION_DELETE_ALL,L"Delete &all...");
    AppendMenuW(conversation,MF_STRING,ACTION_CLEAR,L"&Clear messages...");
    AppendMenuW(conversation,MF_SEPARATOR,0,NULL);
    AppendMenuW(conversation,MF_STRING,ACTION_SEARCH,L"&Search conversations (Ctrl+F)");
    AppendMenuW(response,MF_STRING,ACTION_RETRY,L"&Retry unsuccessful response");
    AppendMenuW(response,MF_STRING,ACTION_REGENERATE,L"Re&generate last response");
    AppendMenuW(response,MF_STRING,ACTION_EDIT,L"&Edit latest user message...");
    AppendMenuW(response,MF_STRING,ACTION_CANCEL_EDIT,L"Cancel edit mode");
    AppendMenuW(response,MF_SEPARATOR,0,NULL);
    AppendMenuW(response,MF_STRING,ACTION_COPY,L"&Copy response");
    AppendMenuW(response,MF_STRING,ACTION_SELECTION,L"Copy transcript &selection");
    AppendMenuW(settings,MF_STRING,ACTION_SYSTEM,L"&System prompt...");
    AppendMenuW(settings,MF_STRING,ACTION_SIDEBAR,L"Sidebar &width...");
    AppendMenuW(settings,MF_STRING,ACTION_MODELS,L"&Choose model... (Ctrl+Space)");
    AppendMenuW(backend,MF_STRING,ACTION_BACKEND_OPENROUTER,L"&OpenRouter");
    AppendMenuW(backend,MF_STRING,ACTION_BACKEND_OLLAMA,L"&Ollama (local)");
    chat_actions_sync_routing(backend,chat);
    AppendMenuW(settings,MF_POPUP,(UINT_PTR)backend,L"&Backend");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_SORT_DEFAULT,L"Sort: &Default (balanced)");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_SORT_PRICE,L"Sort: Prefer lowest &price");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_SORT_THROUGHPUT,L"Sort: Prefer highest t&hroughput");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_SORT_LATENCY,L"Sort: Prefer lowest &latency");
    AppendMenuW(routing,MF_SEPARATOR,0,NULL);
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_ALLOW_FALLBACKS,L"Allow fallback &providers");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_DATA_COLLECTION,L"Allow providers that may store &data");
    AppendMenuW(routing,MF_STRING,ACTION_ROUTING_ZDR,L"Require &zero data retention");
    chat_actions_sync_routing(routing,chat);
    AppendMenuW(settings,MF_POPUP,(UINT_PTR)routing,L"Provider &routing");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)conversation,L"&Conversation");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)response,L"&Response");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)settings,L"&Settings");
    return bar;
}
