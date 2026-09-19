#include "ui/ui.h"
#include "showcase/showcase.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define NEAR(a,b) CHECK(fabsf((a)-(b))<.05f)
static Ui u;
#define NODE(id) (*ui_node(&u,(id)))
static unsigned activated, changed;
static bool remove_on_event;
static UiId reparent_on_event;
static void event(void *user, Ui *ui, UiEvent e) {
    (void)user; (void)ui;
    if (e.kind==UI_ACTIVATE) activated++; else changed++;
    if (remove_on_event) CHECK(ui_remove(ui,e.id));
    else if (reparent_on_event) CHECK(ui_reparent(ui,e.id,reparent_on_event));
}
static UiId init(UiKind kind) {
    ui_init(&u,NULL,NULL); u.on_event=event; activated=changed=0;
    remove_on_event=false; reparent_on_event=UI_NONE;
    return ui_add(&u,0,kind,L"");
}
static void click(UiId id) {
    UiRect r=NODE(id).rect;
    ui_pointer_down(&u,r.x+r.w/2,r.y+r.h/2);
    ui_pointer_up(&u,r.x+r.w/2,r.y+r.h/2);
}
static void layout_test(void) {
    UiId root=init(UI_ROW); NODE(root).style.padding=10; NODE(root).style.gap=10;
    UiId a=ui_add(&u,root,UI_BUTTON,L"A"), b=ui_add(&u,root,UI_BUTTON,L"B"), c=ui_add(&u,root,UI_BUTTON,L"C");
    NODE(a).style.width=ui_fixed(100);
    NODE(b).style.width=ui_flex(1); NODE(b).style.max_w=80;
    NODE(c).style.width=ui_flex(2);
    ui_layout(&u,500,80);
    NEAR(NODE(a).rect.w,100); NEAR(NODE(b).rect.w,80); NEAR(NODE(c).rect.w,280);
    NEAR(NODE(c).rect.x+NODE(c).rect.w,490);
    NODE(b).style.min_w=70; NODE(c).style.min_w=60;
    ui_layout(&u,260,80);
    NEAR(NODE(b).rect.w,70); NEAR(NODE(c).rect.w,60);
    CHECK(NODE(c).clip.w<=NODE(c).rect.w);
    ui_set_hidden(&u,b,true); ui_layout(&u,500,80);
    NEAR(NODE(c).rect.x,120); NEAR(NODE(c).rect.w,370);
    CHECK(ui_hit_test(&u,125,20)==c);
    ui_layout(&u,0,0); CHECK(ui_hit_test(&u,0,0)==UI_NONE);
}
static void input_test(void) {
    UiId root=init(UI_COLUMN);
    UiId a=ui_add(&u,root,UI_BUTTON,L"Action"), b=ui_add(&u,root,UI_CHECKBOX,L"Choice");
    UiId group=ui_add(&u,root,UI_COLUMN,L"");
    UiId disabled=ui_add(&u,group,UI_BUTTON,L"Unavailable");
    ui_set_disabled(&u,group,true); ui_layout(&u,300,300);
    click(a); CHECK(activated==1);
    ui_pointer_down(&u,10,10); ui_pointer_up(&u,400,10); CHECK(activated==1);
    ui_pointer_down(&u,10,10); ui_cancel_input(&u); ui_pointer_up(&u,10,10); CHECK(activated==1);
    click(b); CHECK(NODE(b).checked && changed==1);
    click(disabled); CHECK(activated==1);
    ui_focus(&u,0,true); ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==a);
    ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==b);
    ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==a);
    ui_key(&u,UI_KEY_TAB,true,true,false); CHECK(u.focus==b);
    ui_focus(&u,a,true);
    ui_key(&u,UI_KEY_SPACE,true,false,false); ui_key(&u,UI_KEY_SPACE,true,false,true);
    CHECK(activated==1); ui_key(&u,UI_KEY_SPACE,false,false,false); CHECK(activated==2);
    ui_key(&u,UI_KEY_ENTER,true,false,false); ui_set_active(&u,false);
    ui_key(&u,UI_KEY_ENTER,false,false,false); CHECK(activated==2);
    ui_set_active(&u,true); ui_focus(&u,a,true); ui_set_hidden(&u,a,true);
    CHECK(!u.focus && !u.pressed); ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==b);
    ui_set_disabled(&u,root,true); CHECK(!u.focus); CHECK(!ui_hit_test(&u,10,50));
    /* Late child creation must not change visual traversal order. */
    root=init(UI_COLUMN); group=ui_add(&u,root,UI_COLUMN,L"");
    b=ui_add(&u,root,UI_BUTTON,L"Second"); a=ui_add(&u,group,UI_BUTTON,L"First");
    ui_layout(&u,300,300); ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==a);
    ui_key(&u,UI_KEY_SPACE,true,false,false); ui_key(&u,UI_KEY_ENTER,false,false,false);
    CHECK(activated==0 && u.pressed==a);
    ui_key(&u,UI_KEY_SPACE,false,false,false); CHECK(activated==1);
    ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==b);
}
static void focus_bridge_test(void) {
    UiId root=init(UI_COLUMN);
    UiId a=ui_add(&u,root,UI_BUTTON,L"A");
    UiId b=ui_add(&u,root,UI_BUTTON,L"B");
    UiId c=ui_add(&u,root,UI_BUTTON,L"C");
    (void)b;
    ui_layout(&u,200,200);
    /* Forward focuses the first, reverse the last focusable. */
    CHECK(ui_focus_edge(&u,false) && u.focus==a);
    CHECK(!ui_focus_boundary(&u,false) && ui_focus_boundary(&u,true));
    ui_focus(&u,b,true);
    CHECK(!ui_focus_boundary(&u,false) && !ui_focus_boundary(&u,true));
    CHECK(ui_focus_edge(&u,true) && u.focus==c);
    CHECK(ui_focus_boundary(&u,false) && !ui_focus_boundary(&u,true));
    /* An empty tree has no edge to hand focus to. */
    ui_init(&u,NULL,NULL);
    CHECK(!ui_focus_edge(&u,false) && !ui_focus_boundary(&u,false));
}
static void scrolling_test(void) {
    UiId outer=init(UI_SCROLL); NODE(outer).style.gap=0;
    UiId inner=ui_add(&u,outer,UI_SCROLL,L"Inner"); NODE(inner).style.height=ui_fixed(100); NODE(inner).style.gap=0;
    UiId last=0;
    for (int i=0;i<10;i++) last=ui_add(&u,inner,UI_BUTTON,L"Item");
    UiId bottom=ui_add(&u,outer,UI_BUTTON,L"Bottom"); NODE(bottom).style.height=ui_fixed(200);
    ui_layout(&u,300,200);
    NEAR(ui_scroll_max(&u,inner),200); NEAR(ui_scroll_max(&u,outer),100);
    CHECK(ui_hit_test(&u,20,95)!=last); /* Offscreen children cannot receive clicks. */
    ui_scroll(&u,20,20,250);
    NEAR(NODE(inner).scroll,200); NEAR(NODE(outer).scroll,50);
    ui_scroll(&u,20,20,-250); NEAR(NODE(inner).scroll,0); NEAR(NODE(outer).scroll,0);
    ui_focus(&u,last,true);
    NEAR(NODE(inner).scroll,200); CHECK(NODE(last).clip.h==30);
    UiRect thumb=ui_scroll_thumb(&u,inner);
    ui_pointer_down(&u,thumb.x+3,thumb.y+thumb.h/2); CHECK(u.drag_scroll==inner);
    ui_pointer_move(&u,thumb.x+3,-100); NEAR(NODE(inner).scroll,0);
    ui_pointer_up(&u,thumb.x+3,-100); CHECK(!u.drag_scroll);
    ui_focus(&u,inner,true); ui_key(&u,UI_KEY_END,true,false,false); NEAR(NODE(inner).scroll,200);
    ui_key(&u,UI_KEY_HOME,true,false,false); NEAR(NODE(inner).scroll,0);
    /* Content shrink clamps old scroll offsets on the next layout. */
    NODE(inner).scroll=200;
    for (UiId c=NODE(inner).first;c;c=NODE(c).next) ui_set_hidden(&u,c,c!=last);
    ui_layout(&u,300,200); NEAR(NODE(inner).scroll,0); NEAR(ui_scroll_thumb(&u,inner).h,0);
}
static void roving_focus_test(void) {
    UiId root=init(UI_COLUMN);
    UiId scroll=ui_add(&u,root,UI_SCROLL,L"");
    NODE(scroll).style.height=ui_fixed(90); NODE(scroll).style.gap=0;
    UiId a=ui_add(&u,scroll,UI_BUTTON,L"A");
    UiId b=ui_add(&u,scroll,UI_BUTTON,L"B");
    UiId disabled=ui_add(&u,scroll,UI_BUTTON,L"Disabled");
    UiId hidden=ui_add(&u,scroll,UI_BUTTON,L"Hidden");
    UiId c=ui_add(&u,scroll,UI_BUTTON,L"C");
    UiId filler=ui_add(&u,scroll,UI_COLUMN,L"");
    NODE(filler).style.height=ui_fixed(400);
    ui_set_disabled(&u,disabled,true);
    ui_set_hidden(&u,hidden,true);
    ui_layout(&u,200,90);
    /* Arrow roving moves between the scroll's button-like items and skips
       disabled and hidden ones. */
    ui_focus(&u,a,true);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==b);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==c);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==c);
    ui_key(&u,UI_KEY_UP,true,false,false); CHECK(u.focus==b);
    ui_key(&u,UI_KEY_HOME,true,false,false); CHECK(u.focus==a);
    ui_key(&u,UI_KEY_END,true,false,false); CHECK(u.focus==c);
    CHECK(NODE(scroll).scroll>0);   /* reveal followed the rove */
    /* The direct API clamps at the container edges. */
    ui_focus(&u,a,true);
    CHECK(!ui_focus_move(&u,-1) && u.focus==a);
    CHECK(ui_focus_move(&u,1) && u.focus==b);
    ui_focus(&u,c,true);
    CHECK(!ui_focus_move(&u,1) && u.focus==c);
    /* PageUp/PageDown still scroll while an item holds focus. */
    ui_focus(&u,a,true);
    ui_key(&u,UI_KEY_PAGE_DOWN,true,false,false);
    CHECK(NODE(scroll).scroll>0);
    /* Container focus keeps the original arrow and Home/End scroll semantics. */
    ui_focus(&u,scroll,true);
    ui_key(&u,UI_KEY_END,true,false,false); NEAR(NODE(scroll).scroll,ui_scroll_max(&u,scroll));
    float bottom=NODE(scroll).scroll;
    ui_key(&u,UI_KEY_UP,true,false,false); CHECK(NODE(scroll).scroll<bottom);
    ui_key(&u,UI_KEY_DOWN,true,false,false); NEAR(NODE(scroll).scroll,bottom);
    ui_key(&u,UI_KEY_HOME,true,false,false); NEAR(NODE(scroll).scroll,0);
    /* Movement never crosses into a sibling container. */
    UiId other=ui_add(&u,root,UI_SCROLL,L"");
    NODE(other).style.height=ui_fixed(90); NODE(other).style.gap=0;
    (void)ui_add(&u,other,UI_BUTTON,L"Z");
    ui_layout(&u,200,200);
    ui_focus(&u,c,true);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==c);
    CHECK(!ui_focus_move(&u,1));
    /* Only button-like items inside a scroll rove; others refuse safely. */
    ui_focus(&u,scroll,true);
    CHECK(!ui_focus_move(&u,1));
    UiId plain=ui_add(&u,root,UI_BUTTON,L"Plain");
    ui_layout(&u,200,200);
    ui_focus(&u,plain,true);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==plain);
    CHECK(!ui_focus_move(&u,1));
}
static void nested_roving_focus_test(void) {
    UiId root=init(UI_COLUMN);
    UiId outer=ui_add(&u,root,UI_SCROLL,L"");
    NODE(outer).style.height=ui_fixed(120); NODE(outer).style.gap=0;
    UiId a=ui_add(&u,outer,UI_BUTTON,L"A");
    UiId inner=ui_add(&u,outer,UI_SCROLL,L"");
    NODE(inner).style.height=ui_fixed(60); NODE(inner).style.gap=0;
    UiId b=ui_add(&u,inner,UI_BUTTON,L"B");
    UiId inner_filler=ui_add(&u,inner,UI_COLUMN,L"");
    NODE(inner_filler).style.height=ui_fixed(200);
    UiId c=ui_add(&u,outer,UI_BUTTON,L"C");
    UiId outer_filler=ui_add(&u,outer,UI_COLUMN,L"");
    NODE(outer_filler).style.height=ui_fixed(400);
    ui_layout(&u,200,120);
    /* A descendant scroll is a traversal boundary: the outer group skips the
       inner item entirely, so A reaches C and never B. */
    ui_focus(&u,a,true);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==c);
    ui_key(&u,UI_KEY_UP,true,false,false); CHECK(u.focus==a);
    ui_key(&u,UI_KEY_END,true,false,false); CHECK(u.focus==c);
    ui_key(&u,UI_KEY_HOME,true,false,false); CHECK(u.focus==a);
    /* An inner item roves only within its own scroll. */
    ui_focus(&u,b,true);
    CHECK(!ui_focus_move(&u,-1) && !ui_focus_move(&u,1) && u.focus==b);
    ui_key(&u,UI_KEY_DOWN,true,false,false); CHECK(u.focus==b);
    ui_key(&u,UI_KEY_UP,true,false,false); CHECK(u.focus==b);
    /* Home/End from an inner item never target outer items. */
    ui_key(&u,UI_KEY_HOME,true,false,false); CHECK(u.focus==b);
    ui_key(&u,UI_KEY_END,true,false,false); CHECK(u.focus==b);
}
static void scroll_api_test(void) {
    UiId root=init(UI_SCROLL); NODE(root).style.gap=0;
    UiId a=ui_add(&u,root,UI_BUTTON,L"A"), b=ui_add(&u,root,UI_BUTTON,L"B");
    UiId filler=ui_add(&u,root,UI_COLUMN,L""); NODE(filler).style.height=ui_fixed(500);
    ui_layout(&u,200,100);
    NEAR(ui_scroll_offset(&u,root),0);
    NEAR(ui_scroll_viewport_h(&u,root),100);
    CHECK(!ui_layout_pending(&u));
    /* Absolute set clamps to the scroll range and marks layout only on change. */
    ui_scroll_to(&u,root,1000);
    NEAR(ui_scroll_offset(&u,root),ui_scroll_max(&u,root));
    CHECK(ui_layout_pending(&u));
    ui_layout(&u,200,100);
    ui_scroll_to(&u,root,-50); NEAR(ui_scroll_offset(&u,root),0);
    CHECK(ui_layout_pending(&u));
    ui_layout(&u,200,100);
    ui_scroll_to(&u,root,0); CHECK(!ui_layout_pending(&u));
    /* A spacer child drives the extent exactly. */
    NEAR(ui_scroll_max(&u,root),460);
    /* Non-scroll or invalid ids read as zero and set nothing. */
    CHECK(ui_scroll_offset(&u,b)==0 && ui_scroll_viewport_h(&u,b)==0);
    CHECK(ui_scroll_offset(&u,UI_NONE)==0 && ui_scroll_viewport_h(&u,UI_NONE)==0);
    ui_scroll_to(&u,UI_NONE,42); CHECK(!ui_layout_pending(&u));
    ui_scroll_to(&u,a,42); CHECK(!ui_layout_pending(&u));
    (void)a; (void)b;
}
static void slider_test(void) {
    UiId root=init(UI_COLUMN), slider=ui_add(&u,root,UI_SLIDER,L"Value");
    ui_layout(&u,200,100);
    ui_pointer_down(&u,100,15); NEAR(NODE(slider).value,.5f);
    ui_pointer_move(&u,500,15); NEAR(NODE(slider).value,1);
    ui_pointer_move(&u,-20,15); NEAR(NODE(slider).value,0);
    ui_pointer_up(&u,-20,15);
    ui_focus(&u,slider,true); ui_key(&u,UI_KEY_PAGE_UP,true,false,false); NEAR(NODE(slider).value,.1f);
    ui_key(&u,UI_KEY_END,true,false,false); NEAR(NODE(slider).value,1);
    ui_key(&u,UI_KEY_RIGHT,true,false,false); NEAR(NODE(slider).value,1);
    CHECK(changed>=4);
}
static void text_and_capacity_test(void) {
    UiId root=init(UI_COLUMN), field=ui_add(&u,root,UI_TEXTBOX,L"abc");
    wchar_t text[UI_TEXT_CAPACITY+10];
    for (unsigned i=0;i<sizeof text/sizeof text[0]-1;i++) text[i]=L'x';
    text[sizeof text/sizeof text[0]-1]=0;
    text[UI_TEXT_CAPACITY-2]=0xd800; text[UI_TEXT_CAPACITY-1]=0xdc00;
    ui_set_text(&u,field,text); CHECK(wcslen(NODE(field).text)==UI_TEXT_CAPACITY-2);
    ui_set_text(&u,field,L""); CHECK(!NODE(field).text[0]);
    while (u.count<UI_CAPACITY-1) CHECK(ui_add(&u,root,UI_LABEL,L""));
    CHECK(!ui_add(&u,root,UI_LABEL,L"overflow") && u.overflow);
    CHECK(!ui_node(&u,UI_CAPACITY));
}
static void lifetime_test(void) {
    UiId root=init(UI_COLUMN);
    UiId left=ui_add(&u,root,UI_COLUMN,L"Left");
    UiId right=ui_add(&u,root,UI_COLUMN,L"Right");
    UiId group=ui_add(&u,left,UI_COLUMN,L"Group");
    UiId button=ui_add(&u,group,UI_BUTTON,L"Move me");
    UiId sibling=ui_add(&u,left,UI_BUTTON,L"Sibling");
    CHECK(u.count==6);
    CHECK(!ui_reparent(&u,root,right));
    CHECK(!ui_reparent(&u,left,group));
    CHECK(ui_reparent(&u,button,right));
    CHECK(NODE(button).parent==right && NODE(right).first==button && NODE(right).last==button);
    CHECK(NODE(group).first==UI_NONE && NODE(group).last==UI_NONE);
    CHECK(NODE(left).first==group && NODE(left).last==sibling && NODE(group).next==sibling);

    ui_layout(&u,300,300); ui_focus(&u,button,true);
    ui_pointer_down(&u,NODE(button).rect.x+2,NODE(button).rect.y+2);
    CHECK(u.focus==button && u.pressed==button);
    ui_set_disabled(&u,left,true);
    CHECK(ui_reparent(&u,button,left));
    CHECK(!u.focus && !u.pressed && !u.drag_scroll);

    UiNode *old_slot=ui_node(&u,group);
    CHECK(ui_remove(&u,group));
    CHECK(!ui_node(&u,group) && u.count==5);
    UiId replacement=ui_add(&u,right,UI_BUTTON,L"Replacement");
    CHECK(replacement && replacement!=group && ui_node(&u,replacement)==old_slot);
    ui_set_text(&u,group,L"stale");
    CHECK(!wcscmp(NODE(replacement).text,L"Replacement"));

    ui_set_disabled(&u,left,false);
    UiId parent=ui_add(&u,right,UI_COLUMN,L"Subtree");
    UiId child=ui_add(&u,parent,UI_BUTTON,L"Captured child");
    ui_layout(&u,300,300); ui_focus(&u,child,true);
    u.hot=u.pressed=child;
    CHECK(ui_remove(&u,parent));
    CHECK(!ui_node(&u,parent) && !ui_node(&u,child));
    CHECK(!u.focus && !u.hot && !u.pressed && !u.drag_scroll);

    /* Event callbacks may remove or reparent their own target. */
    UiId action=ui_add(&u,right,UI_BUTTON,L"One shot");
    ui_layout(&u,300,300); remove_on_event=true; click(action);
    CHECK(activated==1 && !ui_node(&u,action) && !u.focus);
    remove_on_event=false;
    UiId destination=ui_add(&u,root,UI_COLUMN,L"Destination");
    action=ui_add(&u,right,UI_BUTTON,L"Relocate");
    ui_layout(&u,300,300); reparent_on_event=destination; click(action);
    CHECK(activated==2 && NODE(action).parent==destination && NODE(destination).last==action);
    reparent_on_event=UI_NONE;
    CHECK(!ui_remove(&u,action^(1u<<8))); /* The wrong generation is invalid. */
}
static void accessibility_metadata_test(void) {
    UiId root=init(UI_COLUMN), label=ui_add(&u,root,UI_LABEL,L"WORKSPACE NAME");
    UiId field=ui_add(&u,root,UI_TEXTBOX,L"draft");
    CHECK(!wcscmp(ui_accessible_name(&u,field),L"draft"));
    ui_set_labelled_by(&u,field,label);
    CHECK(!wcscmp(ui_accessible_name(&u,field),L"WORKSPACE NAME"));
    ui_set_accessible_name(&u,field,L"Workspace");
    ui_set_help_text(&u,field,L"Enter a workspace name.");
    CHECK(!wcscmp(ui_accessible_name(&u,field),L"Workspace"));
    CHECK(!wcscmp(NODE(field).help_text,L"Enter a workspace name."));
    ui_set_accessible_name(&u,field,L""); CHECK(!wcscmp(ui_accessible_name(&u,field),L"WORKSPACE NAME"));
    CHECK(ui_remove(&u,label)); CHECK(!wcscmp(ui_accessible_name(&u,field),L"draft"));
}
typedef struct { unsigned pushes,pops,depth,draws; } PaintCheck;
static bool last_text_centered;
static void fill(void *user, UiRect r, UiColor c, float radius) {
    (void)c; (void)radius; PaintCheck *p=user;
    CHECK(p->depth>0); CHECK(isfinite(r.x) && isfinite(r.y) && r.w>=0 && r.h>=0); p->draws++;
}
static void stroke(void *user, UiRect r, UiColor c, float radius, float width) { (void)width; fill(user,r,c,radius); }
static void text_draw(void *user, UiRect r, const wchar_t *s, UiFont f, UiColor c, bool centered) {
    (void)s; (void)f; last_text_centered=centered; fill(user,r,c,0);
}
static void line(void *user, float x, float y, float xx, float yy, UiColor c, float width) {
    (void)user; (void)c; (void)width; CHECK(isfinite(x+y+xx+yy));
}
static void push(void *user, UiRect r) { PaintCheck *p=user; p->depth++; p->pushes++; CHECK(r.w>0 && r.h>0); }
static void pop(void *user) { PaintCheck *p=user; CHECK(p->depth>0); p->depth--; p->pops++; }
static void icon_test(void) {
    UiId root=init(UI_COLUMN);
    UiId icon=ui_add(&u,root,UI_ICON,L"");
    ui_set_icon(&u,icon,UI_ICON_SEARCH);
    UiId button=ui_add(&u,root,UI_ICON_BUTTON,L"");
    ui_set_icon(&u,button,UI_ICON_SEND);
    ui_set_accessible_name(&u,button,L"Send message");
    UiId centered=ui_add(&u,root,UI_LABEL,L"Centered");
    NODE(centered).style.text_centered=true;
    ui_layout(&u,200,140);
    CHECK(NODE(icon).measured.w>0 && NODE(icon).measured.h>0);
    /* The static icon is not focusable; the icon button is a Tab stop and
       activates like a text button. */
    ui_key(&u,UI_KEY_TAB,true,false,false); CHECK(u.focus==button);
    click(button); CHECK(activated==1);
    CHECK(ui_invoke(&u,button) && activated==2);
    ui_set_icon(&u,button,UI_ICON_STOP);
    CHECK(NODE(button).icon==UI_ICON_STOP);
    CHECK(!ui_invoke(&u,icon));   /* static icons never invoke */
    PaintCheck check={0}; UiPainter painter={&check,fill,stroke,text_draw,line,push,pop,0};
    last_text_centered=false;
    ui_paint(&u,&painter);
    CHECK(last_text_centered);
    CHECK(check.draws>0 && check.pushes==check.pops && check.depth==0);
}
/* Flat-row paint probe: records the first fill and text covering a probe
   point, plus any stroke covering it. */
static float probe_x, probe_y;
static unsigned probe_hits, probe_stroke_hits;
static bool probe_color_valid, probe_text_centered;
static UiColor probe_color, probe_text_color;
static UiRect probe_text_rect;
static void probe_fill(void *user, UiRect r, UiColor c, float radius) {
    (void)user; (void)radius;
    if (!ui_contains(r,probe_x,probe_y)) return;
    if (!probe_color_valid) probe_color=c;
    probe_color_valid=true;
    probe_hits++;
}
static void probe_stroke(void *user, UiRect r, UiColor c, float radius, float width) {
    (void)user; (void)c; (void)radius; (void)width;
    if (ui_contains(r,probe_x,probe_y)) probe_stroke_hits++;
}
static void probe_text(void *user, UiRect r, const wchar_t *s, UiFont f, UiColor c, bool centered) {
    (void)user; (void)s; (void)f;
    probe_text_rect=r;
    if (ui_contains(r,probe_x,probe_y)) {
        probe_text_color=c; probe_text_centered=centered;
    }
}
static void probe_line(void *user, float x, float y, float xx, float yy, UiColor c, float width) {
    (void)user; (void)x; (void)y; (void)xx; (void)yy; (void)c; (void)width;
}
static void probe_push(void *user, UiRect r) { (void)user; (void)r; }
static void probe_pop(void *user) { (void)user; }
static void flat_button_test(void) {
    UiId root=init(UI_COLUMN);
    UiId classic=ui_add(&u,root,UI_BUTTON,L"Classic");
    UiId flat=ui_add(&u,root,UI_BUTTON,L"Flat row");
    NODE(classic).style.width=ui_flex(1);
    NODE(flat).style.flat=true;
    NODE(flat).style.width=ui_flex(1);
    NODE(flat).style.height=ui_fixed(30);
    ui_layout(&u,300,200);
    UiPainter probe={NULL,probe_fill,probe_stroke,probe_text,probe_line,
        probe_push,probe_pop,0};
    /* Idle classic button keeps its filled surface... */
    probe_x=NODE(classic).rect.x+NODE(classic).rect.w/2;
    probe_y=NODE(classic).rect.y+NODE(classic).rect.h/2;
    probe_hits=probe_stroke_hits=0; probe_color_valid=false;
    probe_text_centered=false;
    ui_paint(&u,&probe);
    CHECK(probe_hits>=1);
    CHECK(probe_color.r==u.theme.colors[UI_BUTTON_BG].r &&
        probe_color.g==u.theme.colors[UI_BUTTON_BG].g &&
        probe_color.b==u.theme.colors[UI_BUTTON_BG].b);
    CHECK(probe_text_centered);
    /* ...while the idle flat row is fully transparent: no fill and no
       stroke covers it. */
    probe_x=NODE(flat).rect.x+NODE(flat).rect.w/2;
    probe_y=NODE(flat).rect.y+NODE(flat).rect.h/2;
    probe_hits=probe_stroke_hits=0; probe_color_valid=false;
    probe_text_centered=false;
    ui_paint(&u,&probe);
    CHECK(probe_hits==0 && probe_stroke_hits==0);
    CHECK(probe_text_centered==false);       /* flat rows align left */
    /* Hover receives a soft fill. */
    u.hot=flat;
    probe_hits=0; probe_color_valid=false;
    ui_paint(&u,&probe);
    CHECK(probe_hits==1 &&
        probe_color.r==u.theme.colors[UI_HOVER].r &&
        probe_color.g==u.theme.colors[UI_HOVER].g &&
        probe_color.b==u.theme.colors[UI_HOVER].b);
    /* Selection replaces the hover fill and brightens the text. */
    u.hot=UI_NONE; NODE(flat).selected=true;
    probe_hits=probe_stroke_hits=0; probe_color_valid=false;
    probe_text_color.r=probe_text_color.g=probe_text_color.b=0;
    ui_paint(&u,&probe);
    CHECK(probe_hits==1 && probe_stroke_hits==0 &&
        probe_color.r==u.theme.colors[UI_SELECTED].r &&
        probe_color.g==u.theme.colors[UI_SELECTED].g &&
        probe_color.b==u.theme.colors[UI_SELECTED].b);
    CHECK(probe_text_color.r==u.theme.colors[UI_BRIGHT].r);
    /* The generic keyboard-focus outline is suppressed on flat rows: the
        selection fill is the primary focus indication, so keyboard focus
        adds no second ring. */
    u.focus=flat; u.keyboard_focus=true;
    probe_hits=probe_stroke_hits=0; probe_color_valid=false;
    ui_paint(&u,&probe);
    CHECK(probe_hits==1 && probe_stroke_hits==0 &&
        probe_color.r==u.theme.colors[UI_SELECTED].r);
    /* A classic focused button keeps its generic outline. */
    u.focus=classic; NODE(flat).selected=false;
    probe_x=NODE(classic).rect.x+NODE(classic).rect.w/2;
    probe_y=NODE(classic).rect.y+NODE(classic).rect.h/2;
    probe_hits=probe_stroke_hits=0; probe_color_valid=false;
    ui_paint(&u,&probe);
    CHECK(probe_stroke_hits>=1);
    u.focus=UI_NONE; u.keyboard_focus=false;
    /* Press paints the pressed fill. */
    NODE(flat).selected=false;
    u.hot=flat; u.pressed=flat; u.key_pressed=true;
    probe_x=NODE(flat).rect.x+NODE(flat).rect.w/2;
    probe_y=NODE(flat).rect.y+NODE(flat).rect.h/2;
    probe_hits=0; probe_color_valid=false;
    ui_paint(&u,&probe);
    CHECK(probe_hits==1 &&
        probe_color.r==u.theme.colors[UI_BUTTON_DOWN].r &&
        probe_color.g==u.theme.colors[UI_BUTTON_DOWN].g &&
        probe_color.b==u.theme.colors[UI_BUTTON_DOWN].b);
    u.hot=u.pressed=UI_NONE; u.key_pressed=false;
}
/* Boxed labels can carry an extra symmetric text inset (filter fields keep
   their text off the border); the default stays at the built-in 8 DIPs. */
static void label_inset_test(void) {
    UiId root=init(UI_COLUMN);
    UiId boxed=ui_add(&u,root,UI_LABEL,L"Boxed");
    NODE(boxed).style.background=UI_TRACK;
    NODE(boxed).style.border=true;
    NODE(boxed).style.text_inset=6;
    ui_layout(&u,300,120);
    UiPainter probe={NULL,probe_fill,probe_stroke,probe_text,probe_line,
        probe_push,probe_pop,0};
    probe_x=probe_y=-1000; probe_color_valid=false; probe_text_centered=false;
    ui_paint(&u,&probe);
    UiRect drawn=probe_text_rect;
    /* Last text drawn is the boxed label (tree order). */
    NEAR(drawn.x,NODE(boxed).rect.x+8+6);
    NEAR(drawn.w,NODE(boxed).rect.w-16-12);
    /* A plain label keeps the built-in inset... */
    probe_text_rect=(UiRect){0,0,0,0};
    { UiPainter probe2={NULL,probe_fill,probe_stroke,probe_text,probe_line,
        probe_push,probe_pop,0};
      /* Probe a fresh tree to capture the plain label's own draw. */
      UiId single=init(UI_COLUMN);
      UiId hint=ui_add(&u,single,UI_LABEL,L"Hint");
      NODE(hint).style.height=ui_fixed(40);
      ui_layout(&u,300,80);
      probe_x=NODE(hint).rect.x+40; probe_y=NODE(hint).rect.y+20;
      ui_paint(&u,&probe2);
      CHECK(ui_contains(probe_text_rect,probe_x,probe_y));
      NEAR(probe_text_rect.x,NODE(hint).rect.x+8); }
}
static void showcase_test(void) {
    ui_init(&u,NULL,NULL); Showcase s; CHECK(showcase_init(&s,&u));
    CHECK(u.count<UI_CAPACITY-32);
    const float widths[]={1280,1120,940,800,700,600};
    for (unsigned w=0;w<sizeof widths/sizeof widths[0];w++) {
        showcase_resize(&s,widths[w],420); ui_layout(&u,widths[w],420);
        CHECK(NODE(s.viewport).rect.w>=400 || widths[w]<700);
        for (int page=0;page<3;page++) {
            ui_focus(&u,s.navigation[page],true);
            ui_key(&u,UI_KEY_ENTER,true,false,false); ui_key(&u,UI_KEY_ENTER,false,false,false);
            ui_layout(&u,widths[w],420);
            PaintCheck check={0}; UiPainter painter={&check,fill,stroke,text_draw,line,push,pop,0};
            ui_paint(&u,&painter); CHECK(check.draws>20 && check.pushes==check.pops && check.depth==0);
            for (int i=0;i<u.count;i++) {
                ui_key(&u,UI_KEY_TAB,true,false,false);
                CHECK(ui_enabled(&u,u.focus));
                CHECK(NODE(u.focus).clip.w>0 && NODE(u.focus).clip.h>0);
            }
        }
    }
    ui_set_text(&u,s.filter,L"slider"); u.on_event(u.event_user,&u,(UiEvent){s.filter,UI_CHANGE});
    int visible=0;
    for (int i=0;i<8;i++) visible+=!NODE(s.catalog_rows[i]).hidden;
    CHECK(visible==1 && !NODE(s.catalog_rows[4]).hidden);
}
int main(void) {
    layout_test(); input_test(); focus_bridge_test(); scrolling_test(); roving_focus_test(); nested_roving_focus_test(); scroll_api_test(); slider_test(); text_and_capacity_test(); lifetime_test(); accessibility_metadata_test(); icon_test(); flat_button_test(); label_inset_test(); showcase_test();
    printf("PASS: %u assertions (layout, input, nested scroll, focus reveal, values, lifetime, capacity, icons, flat rows, label inset, showcase breakpoints)\n",assertions);
    return 0;
}
