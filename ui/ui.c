#include "ui.h"
#include <math.h>
#include <string.h>

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float clamp(float v, float a, float b) { return maxf(a, minf(v, b)); }
static uint8_t slot(UiId id) { return (uint8_t)(id & 0xffu); }
static UiId handle(uint8_t slot_id, uint32_t generation) { return (generation << 8) | slot_id; }
static bool valid(const Ui *u, UiId id) {
    uint8_t s=slot(id);
    return s && u->nodes[s].alive && u->nodes[s].generation==(id >> 8);
}
static UiNode *node(Ui *u, UiId id) { return &u->nodes[slot(id)]; }
static const UiNode *const_node(const Ui *u, UiId id) { return &u->nodes[slot(id)]; }
static bool container(UiKind k) { return k == UI_ROW || k == UI_COLUMN || k == UI_SCROLL; }
static bool focusable(UiKind k) {
    return k == UI_BUTTON || k == UI_CHECKBOX || k == UI_SWITCH ||
           k == UI_SLIDER || k == UI_TEXTBOX || k == UI_SCROLL;
}
bool ui_visible(const Ui *u, UiId id) {
    if (!valid(u, id)) return false;
    for (; id; id = const_node(u,id)->parent) if (const_node(u,id)->hidden) return false;
    return true;
}
bool ui_enabled(const Ui *u, UiId id) {
    if (!ui_visible(u, id)) return false;
    for (; id; id = const_node(u,id)->parent) if (const_node(u,id)->disabled) return false;
    return true;
}
bool ui_contains(UiRect r, float x, float y) {
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;
}
UiRect ui_intersect(UiRect a, UiRect b) {
    float x = maxf(a.x,b.x), y = maxf(a.y,b.y);
    return (UiRect){x,y,maxf(0,minf(a.x+a.w,b.x+b.w)-x),maxf(0,minf(a.y+a.h,b.y+b.h)-y)};
}
UiSize ui_auto(void) { return (UiSize){UI_AUTO,0}; }
UiSize ui_fixed(float dips) { return (UiSize){UI_FIXED,maxf(0,dips)}; }
UiSize ui_flex(float weight) { return (UiSize){UI_FLEX,maxf(.001f,weight)}; }
UiNode *ui_node(Ui *u, UiId id) { return valid(u,id) ? node(u,id) : NULL; }
void ui_invalidate(Ui *u, bool layout) { u->paint_dirty = true; u->layout_dirty |= layout; }
void ui_init(Ui *u, UiMeasureFn measure, void *user) {
    memset(u,0,sizeof *u);
    u->theme = ui_theme_dark(); u->measure = measure; u->measure_user = user;
    u->window_active = true;
    ui_invalidate(u,true);
}
void ui_set_text(Ui *u, UiId id, const wchar_t *text) {
    UiNode *n = ui_node(u,id);
    if (!n) return;
    if (!text) text = L"";
    if (text == n->text) return;
    size_t len = wcslen(text);
    if (len >= UI_TEXT_CAPACITY) len = UI_TEXT_CAPACITY-1;
    /* Never truncate a UTF-16 surrogate pair between its two code units. */
    if (len && text[len-1] >= 0xd800 && text[len-1] <= 0xdbff) --len;
    if (wcslen(n->text) == len && !wmemcmp(n->text,text,len)) return;
    wmemmove(n->text,text,len); n->text[len] = 0;
    ui_invalidate(u,true);
}
UiId ui_add(Ui *u, UiId parent, UiKind kind, const wchar_t *text) {
    if (u->count+1 >= UI_CAPACITY || (parent && (!valid(u,parent) || !container(node(u,parent)->kind))) ||
        (!parent && u->root)) { u->overflow = true; return UI_NONE; }
    uint8_t s;
    if (u->free_slot) { s=u->free_slot; u->free_slot=u->nodes[s].free_next; }
    else s=++u->next_slot;
    UiNode *n=&u->nodes[s];
    uint32_t generation=n->generation;
    memset(n,0,sizeof *n); n->generation=generation; n->alive=true;
    UiId id=handle(s,generation); u->count++;
    n->parent = parent; n->kind = kind;
    n->style = (UiStyle){.width=ui_flex(1),.height=ui_auto(),
        .background=-1,.foreground=UI_TEXT,.font=UI_BODY};
    if (container(kind)) n->style.gap = u->theme.gap;
    if (parent) {
        UiNode *p = node(u,parent);
        if (p->last) node(u,p->last)->next = id; else p->first = id;
        p->last = id;
    } else u->root = id;
    ui_set_text(u,id,text);
    ui_invalidate(u,true);
    return id;
}
static void copy_text(wchar_t *destination, const wchar_t *text) {
    if (!text) text=L"";
    size_t length=wcslen(text);
    if (length>=UI_TEXT_CAPACITY) length=UI_TEXT_CAPACITY-1;
    if (length && text[length-1]>=0xd800 && text[length-1]<=0xdbff) --length;
    wmemmove(destination,text,length); destination[length]=0;
}
void ui_set_accessible_name(Ui *u, UiId id, const wchar_t *name) {
    UiNode *n=ui_node(u,id); if (n) copy_text(n->accessible_name,name);
}
void ui_set_help_text(Ui *u, UiId id, const wchar_t *help_text) {
    UiNode *n=ui_node(u,id); if (n) copy_text(n->help_text,help_text);
}
void ui_set_labelled_by(Ui *u, UiId id, UiId label) {
    UiNode *n=ui_node(u,id);
    if (n && (!label || ui_node(u,label))) n->labelled_by=label;
}
const wchar_t *ui_accessible_name(Ui *u, UiId id) {
    UiNode *n=ui_node(u,id);
    if (!n) return L"";
    if (n->accessible_name[0]) return n->accessible_name;
    UiNode *label=ui_node(u,n->labelled_by);
    return label && label->text[0] ? label->text : n->text;
}
static void unlink(Ui *u, UiId id) {
    UiNode *n=node(u,id);
    UiNode *p=node(u,n->parent);
    UiId previous=UI_NONE;
    for (UiId c=p->first;c && c!=id;c=node(u,c)->next) previous=c;
    if (previous) node(u,previous)->next=n->next; else p->first=n->next;
    if (p->last==id) p->last=previous;
    n->parent=n->next=UI_NONE;
}
static void release_subtree(Ui *u, UiId id) {
    UiNode *n=node(u,id);
    for (UiId child=n->first;child;) {
        UiId next=node(u,child)->next;
        release_subtree(u,child); child=next;
    }
    uint8_t s=slot(id);
    uint32_t generation=(n->generation+1u)&0x00ffffffu;
    memset(n,0,sizeof *n); n->generation=generation;
    n->free_next=u->free_slot; u->free_slot=s; u->count--;
}
static void sanitize(Ui *u) {
    if (!ui_enabled(u,u->focus)) u->focus = UI_NONE;
    if (!ui_enabled(u,u->hot)) u->hot = UI_NONE;
    if (!ui_enabled(u,u->pressed)) { u->pressed = UI_NONE; u->key_pressed = false; }
    if (!ui_enabled(u,u->drag_scroll)) u->drag_scroll = UI_NONE;
}
bool ui_remove(Ui *u, UiId id) {
    if (!valid(u,id)) return false;
    if (id==u->root) u->root=UI_NONE; else unlink(u,id);
    release_subtree(u,id); sanitize(u); ui_invalidate(u,true); return true;
}
bool ui_reparent(Ui *u, UiId id, UiId parent) {
    if (!valid(u,id) || !valid(u,parent) || id==u->root || !container(node(u,parent)->kind)) return false;
    for (UiId p=parent;p;p=node(u,p)->parent) if (p==id) return false;
    unlink(u,id);
    UiNode *n=node(u,id), *p=node(u,parent);
    n->parent=parent;
    if (p->last) node(u,p->last)->next=id; else p->first=id;
    p->last=id; sanitize(u); ui_invalidate(u,true); return true;
}
void ui_set_hidden(Ui *u, UiId id, bool hidden) {
    UiNode *n = ui_node(u,id);
    if (n && n->hidden != hidden) { n->hidden=hidden; sanitize(u); ui_invalidate(u,true); }
}
void ui_set_disabled(Ui *u, UiId id, bool disabled) {
    UiNode *n = ui_node(u,id);
    if (n && n->disabled != disabled) { n->disabled=disabled; sanitize(u); ui_invalidate(u,false); }
}
static float limited(float x, float minimum, float maximum) {
    return clamp(x,maxf(0,minimum),maximum > 0 ? maxf(minimum,maximum) : 1e7f);
}
static UiExtent measure(Ui *u, UiId id) {
    UiNode *n = node(u,id);
    if (n->hidden) return n->measured = (UiExtent){0,0};
    float w=0,h=0;
    if (container(n->kind)) {
        int count=0;
        for (UiId c=n->first;c;c=node(u,c)->next) {
            UiExtent e = measure(u,c);
            if (node(u,c)->hidden) continue;
            ++count;
            if (n->kind == UI_ROW) { w+=e.w; h=maxf(h,e.h); }
            else { w=maxf(w,e.w); h+=e.h; }
        }
        float gaps = maxf(0,(float)count-1)*n->style.gap;
        if (n->kind == UI_ROW) w+=gaps; else h+=gaps;
        w+=2*n->style.padding; h+=2*n->style.padding;
    } else {
        UiExtent e = u->measure ? u->measure(u->measure_user,n->text,n->style.font) :
            (UiExtent){(float)wcslen(n->text)*7,16};
        w=e.w; h=maxf(20,e.h);
        if (n->kind != UI_LABEL && n->kind != UI_SEPARATOR) {
            w+=24; h=u->theme.control_height;
        }
        if (n->kind == UI_CHECKBOX) w+=24;
        if (n->kind == UI_SWITCH) w+=40;
        if (n->kind == UI_SLIDER || n->kind == UI_TEXTBOX) w=maxf(160,w);
        if (n->kind == UI_PROGRESS) { w=120; h=6; }
        if (n->kind == UI_SEPARATOR) { w=1; h=1; }
    }
    if (n->style.width.kind == UI_FIXED) w=n->style.width.value;
    if (n->style.height.kind == UI_FIXED) h=n->style.height.value;
    n->measured = (UiExtent){limited(w,n->style.min_w,n->style.max_w),limited(h,n->style.min_h,n->style.max_h)};
    return n->measured;
}
float ui_scroll_max(const Ui *u, UiId id) {
    if (!valid(u,id)) return 0;
    const UiNode *n=const_node(u,id);
    return maxf(0,n->content_height-n->viewport.h);
}
UiRect ui_scroll_thumb(const Ui *u, UiId id) {
    if (!valid(u,id)) return (UiRect){0};
    const UiNode *n=const_node(u,id);
    float maximum=ui_scroll_max(u,id), height=n->viewport.h;
    if (maximum<=0 || height<=0) return (UiRect){0};
    float h=minf(height,maxf(u->theme.min_thumb,height*height/n->content_height));
    return (UiRect){n->rect.x+n->rect.w-u->theme.scrollbar_width,
        n->viewport.y+(height-h)*n->scroll/maximum,u->theme.scrollbar_width,h};
}
static void arrange(Ui *u, UiId id, UiRect rect, UiRect clip) {
    UiNode *n=node(u,id);
    n->rect=rect; n->clip=ui_intersect(rect,clip);
    if (n->hidden || !container(n->kind)) return;
    float pad=n->style.padding;
    UiRect inner={rect.x+pad,rect.y+pad,maxf(0,rect.w-2*pad),maxf(0,rect.h-2*pad)};
    n->viewport=inner;
    bool row=n->kind == UI_ROW, scroll=n->kind == UI_SCROLL;
    /* A stable scrollbar gutter avoids width oscillation at overflow boundaries. */
    if (scroll) inner.w=maxf(0,inner.w-u->theme.scrollbar_width);
    float available=row ? inner.w : inner.h;
    float sizes[UI_CAPACITY]={0}, total=0, weights=0;
    bool flexible[UI_CAPACITY]={0};
    int count=0;
    for (UiId c=n->first;c;c=node(u,c)->next) {
        UiNode *child=node(u,c);
        if (child->hidden) continue;
        ++count;
        UiSize size=row ? child->style.width : child->style.height;
        flexible[slot(c)]=size.kind == UI_FLEX && !scroll;
        if (flexible[slot(c)]) weights+=size.value;
        else { sizes[slot(c)]=row ? child->measured.w : child->measured.h; total+=sizes[slot(c)]; }
    }
    float gaps=maxf(0,(float)count-1)*n->style.gap;
    float remaining=maxf(0,available-total-gaps);
    /* Freeze constrained flex children, then redistribute the remaining space. */
    for (int pass=0;pass<count && weights>0;pass++) {
        bool froze=false;
        float pass_space=remaining, pass_weights=weights;
        for (UiId c=n->first;c;c=node(u,c)->next) if (flexible[slot(c)]) {
            UiStyle *s=&node(u,c)->style;
            float weight=row ? s->width.value : s->height.value;
            float proposed=pass_space*weight/pass_weights;
            float actual=limited(proposed,row?s->min_w:s->min_h,row?s->max_w:s->max_h);
            sizes[slot(c)]=actual;
            if (fabsf(actual-proposed)>.01f) {
                flexible[slot(c)]=false; remaining=maxf(0,remaining-actual); weights-=weight; froze=true;
            }
        }
        if (!froze) break;
    }
    float extent=gaps;
    for (UiId c=n->first;c;c=node(u,c)->next) if (!node(u,c)->hidden) extent+=sizes[slot(c)];
    n->content_height=row ? inner.h : extent;
    n->scroll=scroll ? clamp(n->scroll,0,ui_scroll_max(u,id)) : 0;
    float cursor=(row?inner.x:inner.y)-n->scroll;
    UiRect child_clip=ui_intersect(n->clip,inner);
    for (UiId c=n->first;c;c=node(u,c)->next) {
        UiNode *child=node(u,c);
        if (child->hidden) continue;
        UiSize cross=row ? child->style.height : child->style.width;
        float cross_space=row?inner.h:inner.w;
        float cross_size=cross.kind==UI_FLEX ? cross_space : row?child->measured.h:child->measured.w;
        cross_size=limited(cross_size,row?child->style.min_h:child->style.min_w,
            row?child->style.max_h:child->style.max_w);
        UiRect r=row ? (UiRect){cursor,inner.y,sizes[slot(c)],cross_size} : (UiRect){inner.x,cursor,cross_size,sizes[slot(c)]};
        arrange(u,c,r,child_clip); cursor+=sizes[slot(c)]+n->style.gap;
    }
}
void ui_layout(Ui *u, float width, float height) {
    u->width=maxf(0,width); u->height=maxf(0,height);
    sanitize(u);
    if (u->root) {
        measure(u,u->root);
        UiRect r={0,0,u->width,u->height}; arrange(u,u->root,r,r);
    }
    u->layout_dirty=false;
    if (u->pointer_known) u->hot=ui_hit_test(u,u->pointer_x,u->pointer_y);
    u->paint_dirty=true;
}
static void ensure_layout(Ui *u) { if (u->layout_dirty) ui_layout(u,u->width,u->height); }
static UiId hit(const Ui *u, UiId id, float x, float y) {
    const UiNode *n=const_node(u,id);
    if (!ui_enabled(u,id) || !ui_contains(n->clip,x,y)) return UI_NONE;
    if (n->kind==UI_SCROLL && ui_scroll_max(u,id)>0 &&
        x>=n->rect.x+n->rect.w-u->theme.scrollbar_width) return id;
    UiId result=UI_NONE;
    for (UiId c=n->first;c;c=const_node(u,c)->next) { UiId h=hit(u,c,x,y); if (h) result=h; }
    return result ? result : focusable(n->kind) ? id : UI_NONE;
}
UiId ui_hit_test(const Ui *u, float x, float y) { return u->root ? hit(u,u->root,x,y) : UI_NONE; }
static void emit(Ui *u, UiId id, UiEventKind kind) {
    ui_invalidate(u,false);
    if (u->on_event) u->on_event(u->event_user,u,(UiEvent){id,kind});
}
static void value(Ui *u, UiId id, float v) {
    UiNode *n=node(u,id); v=clamp(v,0,1);
    if (fabsf(n->value-v)>.0001f) { n->value=v; emit(u,id,UI_CHANGE); }
}
static void activate(Ui *u, UiId id) {
    if (!ui_enabled(u,id)) return;
    UiNode *n=node(u,id);
    if (n->kind==UI_CHECKBOX || n->kind==UI_SWITCH) { n->checked=!n->checked; emit(u,id,UI_CHANGE); }
    else if (n->kind==UI_BUTTON) emit(u,id,UI_ACTIVATE);
}
bool ui_invoke(Ui *u, UiId id) {
    UiNode *n=ui_node(u,id);
    if (!n || n->kind!=UI_BUTTON || !ui_enabled(u,id)) return false;
    activate(u,id); return true;
}
static void reveal(Ui *u, UiId id) {
    ensure_layout(u);
    for (UiId p=node(u,id)->parent;p;p=node(u,p)->parent) {
        UiNode *n=node(u,p);
        if (n->kind!=UI_SCROLL) continue;
        UiRect r=node(u,id)->rect;
        float delta=0;
        if (r.y<n->viewport.y) delta=r.y-n->viewport.y;
        else if (r.y+r.h>n->viewport.y+n->viewport.h) delta=minf(r.y-n->viewport.y,r.y+r.h-n->viewport.y-n->viewport.h);
        float next=clamp(n->scroll+delta,0,ui_scroll_max(u,p));
        if (next!=n->scroll) { n->scroll=next; ui_layout(u,u->width,u->height); }
    }
}
void ui_focus(Ui *u, UiId id, bool keyboard) {
    if (id && (!ui_enabled(u,id) || !focusable(node(u,id)->kind))) return;
    if (u->focus!=id) ui_cancel_input(u);
    u->focus=id; u->keyboard_focus=keyboard;
    if (id) reveal(u,id);
    ui_invalidate(u,false);
}
void ui_cancel_input(Ui *u) {
    u->pressed=u->drag_scroll=UI_NONE; u->key_pressed=false; ui_invalidate(u,false);
}
void ui_set_active(Ui *u, bool active) {
    u->window_active=active;
    if (!active) { ui_cancel_input(u); ui_pointer_leave(u); }
    ui_invalidate(u,false);
}
static void slider_at(Ui *u, UiId id, float x) {
    UiRect r=node(u,id)->rect;
    value(u,id,(x-r.x-8)/maxf(1,r.w-16));
}
void ui_pointer_move(Ui *u, float x, float y) {
    ensure_layout(u);
    u->pointer_known=true; u->pointer_x=x; u->pointer_y=y;
    UiId h=ui_hit_test(u,x,y);
    if (h!=u->hot) { u->hot=h; ui_invalidate(u,false); }
    if (u->drag_scroll) {
        UiNode *n=node(u,u->drag_scroll); UiRect thumb=ui_scroll_thumb(u,u->drag_scroll);
        n->scroll=clamp((y-n->viewport.y-u->drag_offset)/maxf(1,n->viewport.h-thumb.h)*ui_scroll_max(u,u->drag_scroll),0,ui_scroll_max(u,u->drag_scroll));
        ui_invalidate(u,true); ensure_layout(u);
    } else if (u->pressed && !u->key_pressed && node(u,u->pressed)->kind==UI_SLIDER) slider_at(u,u->pressed,x);
}
void ui_pointer_leave(Ui *u) { u->pointer_known=false; if (u->hot) { u->hot=UI_NONE; ui_invalidate(u,false); } }
void ui_pointer_down(Ui *u, float x, float y) {
    ui_cancel_input(u); ui_pointer_move(u,x,y);
    UiId id=u->hot;
    if (!id) { ui_focus(u,UI_NONE,false); return; }
    ui_focus(u,id,false); u->pressed=id;
    UiNode *n=node(u,id);
    if (n->kind==UI_SLIDER) slider_at(u,id,x);
    if (n->kind==UI_SCROLL && x>=n->rect.x+n->rect.w-u->theme.scrollbar_width && ui_scroll_max(u,id)>0) {
        UiRect thumb=ui_scroll_thumb(u,id);
        if (ui_contains(thumb,x,y)) { u->drag_scroll=id; u->drag_offset=y-thumb.y; }
        else { n->scroll=clamp(n->scroll+(y<thumb.y?-1:1)*n->viewport.h,0,ui_scroll_max(u,id)); ui_invalidate(u,true); }
    }
    ui_invalidate(u,false);
}
void ui_pointer_up(Ui *u, float x, float y) {
    ui_pointer_move(u,x,y);
    UiId pressed=u->pressed;
    bool fire=pressed && !u->key_pressed && pressed==u->hot;
    ui_cancel_input(u);
    if (fire) activate(u,pressed);
}
void ui_scroll(Ui *u, float x, float y, float delta) {
    ensure_layout(u);
    UiId id=ui_hit_test(u,x,y);
    /* Bubble unused wheel distance to enclosing scroll viewports. */
    for (;id && fabsf(delta)>.01f;id=node(u,id)->parent) {
        UiNode *n=node(u,id);
        if (n->kind!=UI_SCROLL) continue;
        float old=n->scroll; n->scroll=clamp(old+delta,0,ui_scroll_max(u,id));
        delta-=n->scroll-old;
        if (n->scroll!=old) ui_invalidate(u,true);
    }
    ensure_layout(u);
}
static void focus_order(const Ui *u, UiId id, UiId *order, int *count) {
    if (!ui_enabled(u,id)) return;
    if (focusable(const_node(u,id)->kind)) order[(*count)++]=id;
    for (UiId c=const_node(u,id)->first;c;c=const_node(u,c)->next) focus_order(u,c,order,count);
}
static void focus_next(Ui *u, bool reverse) {
    /* Traverse tree order, independent of when children were appended. */
    UiId order[UI_CAPACITY]; int count=0, current=-1;
    if (u->root) focus_order(u,u->root,order,&count);
    for (int i=0;i<count;i++) if (order[i]==u->focus) current=i;
    if (count) {
        int next=current<0 ? (reverse?count-1:0) : (current+(reverse?-1:1)+count)%count;
        ui_focus(u,order[next],true); return;
    }
    ui_focus(u,UI_NONE,true);
}
void ui_key(Ui *u, UiKey key, bool down, bool shift, bool repeat) {
    ensure_layout(u);
    if (key==UI_KEY_TAB && down) { focus_next(u,shift); return; }
    if (key==UI_KEY_ESCAPE && down) { ui_cancel_input(u); return; }
    UiId id=u->focus;
    if (!ui_enabled(u,id)) return;
    UiNode *n=node(u,id); u->keyboard_focus=true;
    if (key==UI_KEY_ENTER || key==UI_KEY_SPACE) {
        if (n->kind==UI_BUTTON || n->kind==UI_CHECKBOX || n->kind==UI_SWITCH) {
            if (down && !repeat && !u->pressed) { u->pressed=id; u->key_pressed=true; u->activation_key=key; }
            else if (!down && u->pressed==id && u->key_pressed && u->activation_key==key) { ui_cancel_input(u); activate(u,id); }
            ui_invalidate(u,false);
        }
        return;
    }
    if (!down) return;
    if (n->kind==UI_SLIDER) {
        if (key==UI_KEY_LEFT || key==UI_KEY_DOWN) value(u,id,n->value-.01f);
        if (key==UI_KEY_RIGHT || key==UI_KEY_UP) value(u,id,n->value+.01f);
        if (key==UI_KEY_PAGE_UP) value(u,id,n->value+.1f);
        if (key==UI_KEY_PAGE_DOWN) value(u,id,n->value-.1f);
        if (key==UI_KEY_HOME) value(u,id,0);
        if (key==UI_KEY_END) value(u,id,1);
    } else if (n->kind!=UI_TEXTBOX) {
        UiId p=id;
        while (p && node(u,p)->kind!=UI_SCROLL) p=node(u,p)->parent;
        if (p) {
            UiNode *s=node(u,p); float next=s->scroll;
            if (key==UI_KEY_UP) next-=u->theme.control_height;
            if (key==UI_KEY_DOWN) next+=u->theme.control_height;
            if (key==UI_KEY_PAGE_UP) next-=s->viewport.h*.9f;
            if (key==UI_KEY_PAGE_DOWN) next+=s->viewport.h*.9f;
            if (key==UI_KEY_HOME) next=0;
            if (key==UI_KEY_END) next=ui_scroll_max(u,p);
            s->scroll=clamp(next,0,ui_scroll_max(u,p));
            ui_invalidate(u,true);
        }
    }
}
