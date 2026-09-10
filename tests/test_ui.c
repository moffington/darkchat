#include "../ui/ui.h"
#include "../showcase/showcase.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define NEAR(a,b) CHECK(fabsf((a)-(b))<.05f)
static Ui u;
static unsigned activated, changed;
static void event(void *user, Ui *ui, UiEvent e) {
    (void)user; (void)ui;
    if (e.kind==UI_ACTIVATE) activated++; else changed++;
}
static UiId init(UiKind kind) {
    ui_init(&u,NULL,NULL); u.on_event=event; activated=changed=0;
    return ui_add(&u,0,kind,L"");
}
static void click(UiId id) {
    UiRect r=u.nodes[id].rect;
    ui_pointer_down(&u,r.x+r.w/2,r.y+r.h/2);
    ui_pointer_up(&u,r.x+r.w/2,r.y+r.h/2);
}
static void layout_test(void) {
    UiId root=init(UI_ROW); u.nodes[root].style.padding=10; u.nodes[root].style.gap=10;
    UiId a=ui_add(&u,root,UI_BUTTON,L"A"), b=ui_add(&u,root,UI_BUTTON,L"B"), c=ui_add(&u,root,UI_BUTTON,L"C");
    u.nodes[a].style.width=ui_fixed(100);
    u.nodes[b].style.width=ui_flex(1); u.nodes[b].style.max_w=80;
    u.nodes[c].style.width=ui_flex(2);
    ui_layout(&u,500,80);
    NEAR(u.nodes[a].rect.w,100); NEAR(u.nodes[b].rect.w,80); NEAR(u.nodes[c].rect.w,280);
    NEAR(u.nodes[c].rect.x+u.nodes[c].rect.w,490);
    u.nodes[b].style.min_w=70; u.nodes[c].style.min_w=60;
    ui_layout(&u,260,80);
    NEAR(u.nodes[b].rect.w,70); NEAR(u.nodes[c].rect.w,60);
    CHECK(u.nodes[c].clip.w<=u.nodes[c].rect.w);
    ui_set_hidden(&u,b,true); ui_layout(&u,500,80);
    NEAR(u.nodes[c].rect.x,120); NEAR(u.nodes[c].rect.w,370);
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
    click(b); CHECK(u.nodes[b].checked && changed==1);
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
static void scrolling_test(void) {
    UiId outer=init(UI_SCROLL); u.nodes[outer].style.gap=0;
    UiId inner=ui_add(&u,outer,UI_SCROLL,L"Inner"); u.nodes[inner].style.height=ui_fixed(100); u.nodes[inner].style.gap=0;
    UiId last=0;
    for (int i=0;i<10;i++) last=ui_add(&u,inner,UI_BUTTON,L"Item");
    UiId bottom=ui_add(&u,outer,UI_BUTTON,L"Bottom"); u.nodes[bottom].style.height=ui_fixed(200);
    ui_layout(&u,300,200);
    NEAR(ui_scroll_max(&u,inner),200); NEAR(ui_scroll_max(&u,outer),100);
    CHECK(ui_hit_test(&u,20,95)!=last); /* Offscreen children cannot receive clicks. */
    ui_scroll(&u,20,20,250);
    NEAR(u.nodes[inner].scroll,200); NEAR(u.nodes[outer].scroll,50);
    ui_scroll(&u,20,20,-250); NEAR(u.nodes[inner].scroll,0); NEAR(u.nodes[outer].scroll,0);
    ui_focus(&u,last,true);
    NEAR(u.nodes[inner].scroll,200); CHECK(u.nodes[last].clip.h==30);
    UiRect thumb=ui_scroll_thumb(&u,inner);
    ui_pointer_down(&u,thumb.x+3,thumb.y+thumb.h/2); CHECK(u.drag_scroll==inner);
    ui_pointer_move(&u,thumb.x+3,-100); NEAR(u.nodes[inner].scroll,0);
    ui_pointer_up(&u,thumb.x+3,-100); CHECK(!u.drag_scroll);
    ui_focus(&u,inner,true); ui_key(&u,UI_KEY_END,true,false,false); NEAR(u.nodes[inner].scroll,200);
    ui_key(&u,UI_KEY_HOME,true,false,false); NEAR(u.nodes[inner].scroll,0);
    /* Content shrink clamps old scroll offsets on the next layout. */
    u.nodes[inner].scroll=200;
    for (UiId c=u.nodes[inner].first;c;c=u.nodes[c].next) ui_set_hidden(&u,c,c!=last);
    ui_layout(&u,300,200); NEAR(u.nodes[inner].scroll,0); NEAR(ui_scroll_thumb(&u,inner).h,0);
}
static void slider_test(void) {
    UiId root=init(UI_COLUMN), slider=ui_add(&u,root,UI_SLIDER,L"Value");
    ui_layout(&u,200,100);
    ui_pointer_down(&u,100,15); NEAR(u.nodes[slider].value,.5f);
    ui_pointer_move(&u,500,15); NEAR(u.nodes[slider].value,1);
    ui_pointer_move(&u,-20,15); NEAR(u.nodes[slider].value,0);
    ui_pointer_up(&u,-20,15);
    ui_focus(&u,slider,true); ui_key(&u,UI_KEY_PAGE_UP,true,false,false); NEAR(u.nodes[slider].value,.1f);
    ui_key(&u,UI_KEY_END,true,false,false); NEAR(u.nodes[slider].value,1);
    ui_key(&u,UI_KEY_RIGHT,true,false,false); NEAR(u.nodes[slider].value,1);
    CHECK(changed>=4);
}
static void text_and_capacity_test(void) {
    UiId root=init(UI_COLUMN), field=ui_add(&u,root,UI_TEXTBOX,L"abc");
    wchar_t text[UI_TEXT_CAPACITY+10];
    for (unsigned i=0;i<sizeof text/sizeof text[0]-1;i++) text[i]=L'x';
    text[sizeof text/sizeof text[0]-1]=0;
    text[UI_TEXT_CAPACITY-2]=0xd800; text[UI_TEXT_CAPACITY-1]=0xdc00;
    ui_set_text(&u,field,text); CHECK(wcslen(u.nodes[field].text)==UI_TEXT_CAPACITY-2);
    ui_set_text(&u,field,L""); CHECK(!u.nodes[field].text[0]);
    while (u.count<UI_CAPACITY-1) CHECK(ui_add(&u,root,UI_LABEL,L""));
    CHECK(!ui_add(&u,root,UI_LABEL,L"overflow") && u.overflow);
    CHECK(!ui_node(&u,UI_CAPACITY));
}
typedef struct { unsigned pushes,pops,depth,draws; } PaintCheck;
static void fill(void *user, UiRect r, UiColor c, float radius) {
    (void)c; (void)radius; PaintCheck *p=user;
    CHECK(p->depth>0); CHECK(isfinite(r.x) && isfinite(r.y) && r.w>=0 && r.h>=0); p->draws++;
}
static void stroke(void *user, UiRect r, UiColor c, float radius, float width) { (void)width; fill(user,r,c,radius); }
static void text_draw(void *user, UiRect r, const wchar_t *s, UiFont f, UiColor c, bool centered) {
    (void)s; (void)f; (void)centered; fill(user,r,c,0);
}
static void line(void *user, float x, float y, float xx, float yy, UiColor c, float width) {
    (void)user; (void)c; (void)width; CHECK(isfinite(x+y+xx+yy));
}
static void push(void *user, UiRect r) { PaintCheck *p=user; p->depth++; p->pushes++; CHECK(r.w>0 && r.h>0); }
static void pop(void *user) { PaintCheck *p=user; CHECK(p->depth>0); p->depth--; p->pops++; }
static void showcase_test(void) {
    ui_init(&u,NULL,NULL); Showcase s; CHECK(showcase_init(&s,&u));
    CHECK(u.count<UI_CAPACITY-32);
    const float widths[]={1280,1120,940,800,700,600};
    for (unsigned w=0;w<sizeof widths/sizeof widths[0];w++) {
        showcase_resize(&s,widths[w],420); ui_layout(&u,widths[w],420);
        CHECK(u.nodes[s.viewport].rect.w>=400 || widths[w]<700);
        for (int page=0;page<3;page++) {
            ui_focus(&u,s.navigation[page],true);
            ui_key(&u,UI_KEY_ENTER,true,false,false); ui_key(&u,UI_KEY_ENTER,false,false,false);
            ui_layout(&u,widths[w],420);
            PaintCheck check={0}; UiPainter painter={&check,fill,stroke,text_draw,line,push,pop,0};
            ui_paint(&u,&painter); CHECK(check.draws>20 && check.pushes==check.pops && check.depth==0);
            for (int i=0;i<u.count;i++) {
                ui_key(&u,UI_KEY_TAB,true,false,false);
                CHECK(ui_enabled(&u,u.focus));
                CHECK(u.nodes[u.focus].clip.w>0 && u.nodes[u.focus].clip.h>0);
            }
        }
    }
    ui_set_text(&u,s.filter,L"slider"); u.on_event(u.event_user,&u,(UiEvent){s.filter,UI_CHANGE});
    int visible=0;
    for (int i=0;i<8;i++) visible+=!u.nodes[s.catalog_rows[i]].hidden;
    CHECK(visible==1 && !u.nodes[s.catalog_rows[4]].hidden);
}
int main(void) {
    layout_test(); input_test(); scrolling_test(); slider_test(); text_and_capacity_test(); showcase_test();
    printf("PASS: %u assertions (layout, input, nested scroll, focus reveal, values, capacity, showcase breakpoints)\n",assertions);
    return 0;
}
