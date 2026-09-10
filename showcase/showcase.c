#include "showcase.h"
#include <string.h>
#include <wctype.h>

static UiNode *node(Showcase *s, UiId id) { return ui_node(s->ui,id); }
static UiId add(Showcase *s, UiId parent, UiKind kind, const wchar_t *text) {
    return ui_add(s->ui,parent,kind,text);
}
static UiId label(Showcase *s, UiId parent, const wchar_t *text, UiFont font, UiColorRole color) {
    UiId id=add(s,parent,UI_LABEL,text);
    UiNode *n=node(s,id);
    if (n) { n->style.font=font; n->style.foreground=color; }
    return id;
}
static UiId panel(Showcase *s, UiId parent, const wchar_t *title) {
    UiId id=add(s,parent,UI_COLUMN,L"");
    UiNode *n=node(s,id);
    if (n) { n->style.padding=16; n->style.gap=10; n->style.background=UI_PANEL; n->style.border=true; }
    label(s,id,title,UI_TITLE,UI_BRIGHT);
    return id;
}
static UiId row(Showcase *s, UiId parent) { return add(s,parent,UI_ROW,L""); }
static void height(Showcase *s, UiId id, float value) { UiNode *n=node(s,id); if (n) n->style.height=ui_fixed(value); }
static void width(Showcase *s, UiId id, float value) { UiNode *n=node(s,id); if (n) n->style.width=ui_fixed(value); }
static bool contains(const wchar_t *text, const wchar_t *query) {
    for (;*text;text++) {
        size_t i=0;
        while (query[i] && text[i] && towlower(text[i])==towlower(query[i])) i++;
        if (!query[i]) return true;
    }
    return !*query;
}
static void update(Showcase *s) {
    wchar_t text[UI_TEXT_CAPACITY];
    int percent=(int)(node(s,s->slider)->value*100+.5f);
    swprintf(text,UI_TEXT_CAPACITY,L"Intensity  /  %d%%",percent);
    ui_set_text(s->ui,s->value_label,text);
    node(s,s->progress)->value=node(s,s->slider)->value;
    swprintf(text,UI_TEXT_CAPACITY,L"%d action%s triggered",s->actions,s->actions==1?L"":L"s");
    ui_set_text(s->ui,s->summary,text);
    ui_set_text(s->ui,s->edit_summary,node(s,s->edit)->text);
    ui_set_disabled(s->ui,s->action,node(s,s->disable)->checked);
    ui_set_disabled(s->ui,s->secondary,node(s,s->disable)->checked);
    ui_invalidate(s->ui,false);
}
static void event(void *user, Ui *ui, UiEvent e) {
    Showcase *s=user;
    for (int i=0;i<3;i++) if (e.id==s->navigation[i]) {
        static const wchar_t *titles[]={L"Controls",L"Design tokens",L"Input & scrolling"};
        static const wchar_t *subtitles[]={
            L"Quiet surfaces. Precise interactions. A native toolkit in use.",
            L"The original dark palette, shared by every surface and control.",
            L"Explore keyboard focus, clipped content and nested viewports."};
        s->page=i;
        for (int j=0;j<3;j++) {
            ui_set_hidden(ui,s->pages[j],i!=j); node(s,s->navigation[j])->selected=i==j;
        }
        ui_set_text(ui,s->title,titles[i]); ui_set_text(ui,s->subtitle,subtitles[i]);
        node(s,s->viewport)->scroll=0;
        ui_set_text(ui,s->status,L"Tab to move focus  /  Enter or Space to activate");
        return;
    }
    if (e.id==s->action || e.id==s->secondary) {
        s->actions++;
        ui_set_text(ui,s->status,e.id==s->action?L"Primary action completed":L"Secondary action completed");
    } else if (e.id==s->reset) {
        s->actions=0; node(s,s->slider)->value=.64f;
        node(s,s->checkbox)->checked=true; node(s,s->toggle)->checked=true;
        node(s,s->disable)->checked=false;
        ui_set_text(ui,s->edit,L"Untitled workspace");
        ui_set_text(ui,s->filter,L"");
        for (int i=0;i<8;i++) ui_set_hidden(ui,s->catalog_rows[i],false);
        ui_set_text(ui,s->status,L"Showcase values reset");
    } else if (e.id==s->filter) {
        for (int i=0;i<8;i++) ui_set_hidden(ui,s->catalog_rows[i],!contains(node(s,s->catalog_rows[i])->text,node(s,s->filter)->text));
        ui_set_text(ui,s->status,L"Control list filtered");
    } else {
        ui_set_text(ui,s->status,e.kind==UI_CHANGE?L"Value updated":L"Control activated");
    }
    update(s);
}
bool showcase_init(Showcase *s, Ui *u) {
    memset(s,0,sizeof *s); s->ui=u;
    UiId root=add(s,0,UI_COLUMN,L""); node(s,root)->style.gap=0;
    node(s,root)->style.background=UI_BG;
    UiId toolbar=row(s,root); height(s,toolbar,50);
    node(s,toolbar)->style.padding=10; node(s,toolbar)->style.background=UI_TOOLBAR;
    UiId brand=label(s,toolbar,L"D A R K   U I",UI_SECTION,UI_ACCENT); width(s,brand,150); height(s,brand,30);
    UiId trail=label(s,toolbar,L"Foundation   /   Native control showcase",UI_BODY,UI_MUTED); height(s,trail,30);
    s->reset=add(s,toolbar,UI_BUTTON,L"Reset values"); width(s,s->reset,110);
    height(s,add(s,root,UI_SEPARATOR,L""),1);
    UiId body=row(s,root); node(s,body)->style.height=ui_flex(1); node(s,body)->style.gap=0;
    s->sidebar=add(s,body,UI_COLUMN,L""); width(s,s->sidebar,172);
    node(s,s->sidebar)->style.height=ui_flex(1); node(s,s->sidebar)->style.padding=12;
    node(s,s->sidebar)->style.background=UI_PANEL; node(s,s->sidebar)->style.gap=6;
    height(s,label(s,s->sidebar,L"WORKBENCH",UI_SECTION,UI_ACCENT),32);
    static const wchar_t *nav[]={L"Controls",L"Design tokens",L"Input & scrolling"};
    for (int i=0;i<3;i++) s->navigation[i]=add(s,s->sidebar,UI_BUTTON,nav[i]);
    node(s,s->navigation[0])->selected=true;
    height(s,label(s,s->sidebar,L"FOUNDATION",UI_SECTION,UI_FAINT),40);
    label(s,s->sidebar,L"C17 / Win32",UI_BODY,UI_MUTED);
    label(s,s->sidebar,L"Direct2D / DirectWrite",UI_SMALL,UI_MUTED);
    UiId space=add(s,s->sidebar,UI_COLUMN,L""); node(s,space)->style.height=ui_flex(1);
    label(s,s->sidebar,L"Built for the desktop",UI_SMALL,UI_FAINT);

    s->viewport=add(s,body,UI_SCROLL,L"Showcase");
    node(s,s->viewport)->style.height=ui_flex(1); node(s,s->viewport)->style.padding=24;
    node(s,s->viewport)->style.gap=8;
    s->title=label(s,s->viewport,L"Controls",UI_HEADING,UI_BRIGHT); height(s,s->title,34);
    s->subtitle=label(s,s->viewport,L"Quiet surfaces. Precise interactions. A native toolkit in use.",UI_BODY,UI_MUTED); height(s,s->subtitle,28);
    for (int i=0;i<3;i++) { s->pages[i]=add(s,s->viewport,UI_COLUMN,L""); node(s,s->pages[i])->style.gap=16; ui_set_hidden(u,s->pages[i],i!=0); }

    s->pair=row(s,s->pages[0]); node(s,s->pair)->style.gap=16;
    UiId actions=panel(s,s->pair,L"Actions");
    label(s,actions,L"A clear primary action and quiet alternatives.",UI_SMALL,UI_MUTED);
    UiId buttons=row(s,actions);
    s->action=add(s,buttons,UI_BUTTON,L"Run action"); node(s,s->action)->selected=true;
    s->secondary=add(s,buttons,UI_BUTTON,L"Secondary");
    s->disable=add(s,actions,UI_CHECKBOX,L"Disable actions");
    UiId selection=panel(s,s->pair,L"Selection");
    label(s,selection,L"Persistent choices with immediate feedback.",UI_SMALL,UI_MUTED);
    s->checkbox=add(s,selection,UI_CHECKBOX,L"Remember workspace"); node(s,s->checkbox)->checked=true;
    s->toggle=add(s,selection,UI_SWITCH,L"Live preview"); node(s,s->toggle)->checked=true;

    UiId fields=panel(s,s->pages[0],L"Text & values");
    label(s,fields,L"WORKSPACE NAME",UI_SECTION,UI_MUTED);
    s->edit=add(s,fields,UI_TEXTBOX,L"Untitled workspace");
    label(s,fields,L"Native text selection, clipboard, undo and IME while editing.",UI_SMALL,UI_MUTED);
    s->value_label=label(s,fields,L"",UI_BODY,UI_TEXT);
    s->slider=add(s,fields,UI_SLIDER,L"Intensity"); node(s,s->slider)->value=.64f;
    s->progress=add(s,fields,UI_PROGRESS,L"");
    label(s,fields,L"Drag the slider, or focus it and use arrows, Home and End.",UI_SMALL,UI_MUTED);

    UiId list=panel(s,s->pages[0],L"Control catalog");
    label(s,list,L"Filter this independently scrolling list.",UI_SMALL,UI_MUTED);
    s->filter=add(s,list,UI_TEXTBOX,L"");
    s->catalog=add(s,list,UI_SCROLL,L"Control catalog"); height(s,s->catalog,180);
    node(s,s->catalog)->style.padding=4; node(s,s->catalog)->style.gap=5;
    static const wchar_t *items[]={L"Button / actions",L"Checkbox / choices",L"Switch / settings",L"Text field / native editing",
        L"Slider / continuous values",L"Progress / feedback",L"Scroll view / content",L"Label / typography"};
    for (int i=0;i<8;i++) s->catalog_rows[i]=add(s,s->catalog,UI_BUTTON,items[i]);
    label(s,s->pages[0],L"Every sample uses the same layout, input and theme primitives.",UI_SMALL,UI_FAINT);

    UiId colors=panel(s,s->pages[1],L"Surface & color");
    static const struct { const wchar_t *name; UiColorRole role; } swatches[]={
        {L"Background   #18191C",UI_BG},{L"Panel   #1D1F23",UI_PANEL},
        {L"Toolbar   #202226",UI_TOOLBAR},{L"Border   #2F3238",UI_BORDER},
        {L"Selection   #2B303C",UI_SELECTED},{L"Accent surface   #364A62",UI_ACCENT_SOFT}};
    for (unsigned i=0;i<sizeof swatches/sizeof swatches[0];i++) {
        UiId swatch=add(s,colors,UI_ROW,L""); height(s,swatch,40);
        node(s,swatch)->style.background=swatches[i].role; node(s,swatch)->style.padding=8;
        label(s,swatch,swatches[i].name,UI_BODY,UI_BRIGHT);
    }
    UiId typography=panel(s,s->pages[1],L"Typography & rhythm");
    label(s,typography,L"Segoe UI / 20 / Heading",UI_HEADING,UI_BRIGHT);
    label(s,typography,L"Segoe UI / 13 / Section title",UI_TITLE,UI_BRIGHT);
    label(s,typography,L"Segoe UI / 12 / Body",UI_BODY,UI_TEXT);
    label(s,typography,L"SEGOE UI / 10 / LABEL",UI_SECTION,UI_ACCENT);
    label(s,typography,L"8 DIP gaps   /   16 DIP panels   /   3 DIP corners",UI_SMALL,UI_MUTED);

    UiId input=panel(s,s->pages[2],L"Keyboard navigation");
    label(s,input,L"Tab / Shift+Tab    Move between enabled controls",UI_BODY,UI_TEXT);
    label(s,input,L"Enter / Space    Activate a button or toggle a choice",UI_BODY,UI_TEXT);
    label(s,input,L"Arrows / Home / End    Adjust a focused slider",UI_BODY,UI_TEXT);
    label(s,input,L"Page Up / Page Down    Scroll the nearest viewport",UI_BODY,UI_TEXT);
    label(s,input,L"Focus automatically reveals controls outside a scroll viewport.",UI_SMALL,UI_MUTED);
    UiId scrolling=panel(s,s->pages[2],L"Nested scroll viewport");
    label(s,scrolling,L"Wheel distance passes to the outer view when the inner view reaches an edge.",UI_SMALL,UI_MUTED);
    UiId inner=add(s,scrolling,UI_SCROLL,L"Scrolling samples"); height(s,inner,260);
    node(s,inner)->style.padding=4;
    for (int i=0;i<18;i++) {
        wchar_t text[64]; swprintf(text,64,L"Focusable sample %02d",i+1);
        add(s,inner,UI_BUTTON,text);
    }
    UiId finish=panel(s,s->pages[2],L"At the edge");
    label(s,finish,L"Resize the window to see the same tree adapt.",UI_BODY,UI_MUTED);
    add(s,finish,UI_BUTTON,L"Last focus target");

    s->inspector=add(s,body,UI_SCROLL,L"Live state"); width(s,s->inspector,248);
    node(s,s->inspector)->style.height=ui_flex(1); node(s,s->inspector)->style.padding=16;
    node(s,s->inspector)->style.background=UI_PANEL; node(s,s->inspector)->style.gap=10;
    label(s,s->inspector,L"LIVE STATE",UI_SECTION,UI_ACCENT);
    height(s,label(s,s->inspector,L"A small, useful foundation",UI_TITLE,UI_BRIGHT),32);
    label(s,s->inspector,L"ACTIONS",UI_SECTION,UI_FAINT);
    s->summary=label(s,s->inspector,L"",UI_BODY,UI_TEXT);
    height(s,add(s,s->inspector,UI_SEPARATOR,L""),1);
    label(s,s->inspector,L"WORKSPACE",UI_SECTION,UI_FAINT);
    s->edit_summary=label(s,s->inspector,L"",UI_BODY,UI_TEXT);
    height(s,add(s,s->inspector,UI_SEPARATOR,L""),1);
    label(s,s->inspector,L"INTERACTION",UI_SECTION,UI_FAINT);
    label(s,s->inspector,L"Visible keyboard focus",UI_BODY,UI_MUTED);
    label(s,s->inspector,L"Captured pointer gestures",UI_BODY,UI_MUTED);
    label(s,s->inspector,L"Nested, clipped scrolling",UI_BODY,UI_MUTED);
    label(s,s->inspector,L"Per-monitor DIP layout",UI_BODY,UI_MUTED);
    height(s,add(s,root,UI_SEPARATOR,L""),1);
    UiId status=row(s,root); height(s,status,31);
    node(s,status)->style.padding=8; node(s,status)->style.background=UI_TOOLBAR;
    s->status=label(s,status,L"Ready  /  Tab to explore controls",UI_SMALL,UI_MUTED);
    /* Root creation is fixed and bounded; handle arena exhaustion before use. */
    if (u->overflow) return false;
    u->on_event=event; u->event_user=s;
    update(s);
    return true;
}
void showcase_resize(Showcase *s, float width_dips, float height_dips) {
    (void)height_dips;
    bool narrow=width_dips<700, compact=width_dips<1120, stacked=width_dips<940;
    if (narrow!=s->narrow || compact!=s->compact || stacked!=s->stacked) {
        s->narrow=narrow; s->compact=compact; s->stacked=stacked;
        /* Keep navigation available at the minimum supported window width. */
        UiNode *side=node(s,s->sidebar);
        side->style.width=ui_fixed(narrow?132:172);
        ui_set_hidden(s->ui,s->inspector,compact);
        node(s,s->pair)->kind=stacked?UI_COLUMN:UI_ROW;
        node(s,s->viewport)->style.padding=narrow?12:24;
        ui_invalidate(s->ui,true);
    }
}
