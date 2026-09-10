#include "ui.h"

static UiRect inset(UiRect r, float p) {
    r.x+=p; r.y+=p; r.w=r.w>2*p?r.w-2*p:0; r.h=r.h>2*p?r.h-2*p:0; return r;
}
static UiColor color(const Ui *u, UiColorRole role) { return u->theme.colors[role]; }
static UiRect text_inset(UiRect r) { r.x+=8; r.w=r.w>16?r.w-16:0; return r; }
static void draw(Ui *u, const UiPainter *p, UiId id) {
    UiNode *n=&u->nodes[id];
    UiRect r=n->rect;
    if (n->hidden || n->clip.w<=0 || n->clip.h<=0) return;
    bool enabled=ui_enabled(u,id), hot=u->hot==id && enabled;
    bool down=u->pressed==id && (hot || u->key_pressed);
    bool focused=u->focus==id && u->window_active;
    UiColor fg=color(u,enabled?n->style.foreground:UI_FAINT);
    float radius=u->theme.radius;
    p->push_clip(p->user,n->clip);
    if (n->style.background>=0 && n->style.background<UI_COLOR_COUNT)
        p->fill(p->user,r,color(u,(UiColorRole)n->style.background),0);
    switch (n->kind) {
    case UI_LABEL:
        p->text(p->user,r,n->text,n->style.font,fg,false); break;
    case UI_BUTTON: {
        UiColorRole bg=down?UI_BUTTON_DOWN:hot?UI_BUTTON_HOT:n->selected?UI_ACCENT_SOFT:UI_BUTTON_BG;
        p->fill(p->user,inset(r,1),color(u,bg),radius);
        if (n->selected) p->stroke(p->user,inset(r,.5f),color(u,UI_ACCENT_SOFT),radius,1);
        p->text(p->user,text_inset(r),n->text,n->style.font,n->selected && enabled?color(u,UI_BRIGHT):fg,true);
        break;
    }
    case UI_CHECKBOX: {
        UiRect box={r.x+2,r.y+(r.h-16)/2,16,16};
        p->fill(p->user,box,color(u,n->checked?UI_ACCENT_SOFT:hot?UI_BUTTON_HOT:UI_BUTTON_BG),radius);
        p->stroke(p->user,box,color(u,enabled && n->checked?UI_ACCENT:UI_FAINT),radius,1);
        if (n->checked) {
            UiColor check=color(u,enabled?UI_BRIGHT:UI_FAINT);
            p->line(p->user,box.x+4,box.y+8,box.x+7,box.y+11,check,1.5f);
            p->line(p->user,box.x+7,box.y+11,box.x+12,box.y+5,check,1.5f);
        }
        p->text(p->user,(UiRect){r.x+28,r.y,r.w>28?r.w-28:0,r.h},n->text,n->style.font,fg,false);
        break;
    }
    case UI_SWITCH: {
        UiRect track={r.x+2,r.y+(r.h-18)/2,32,18};
        p->fill(p->user,track,color(u,n->checked?UI_ACCENT_SOFT:hot?UI_BUTTON_HOT:UI_TRACK),9);
        UiRect knob={track.x+(n->checked?17:3),track.y+3,12,12};
        p->fill(p->user,knob,color(u,enabled?(n->checked?UI_ACCENT:UI_MUTED):UI_FAINT),6);
        p->text(p->user,(UiRect){r.x+44,r.y,r.w>44?r.w-44:0,r.h},n->text,n->style.font,fg,false);
        break;
    }
    case UI_SLIDER: {
        float w=r.w>16?r.w-16:0, v=n->value<0?0:n->value>1?1:n->value;
        UiRect track={r.x+8,r.y+r.h/2-2,w,4};
        p->fill(p->user,track,color(u,UI_TRACK),2);
        track.w*=v;
        p->fill(p->user,track,color(u,enabled?UI_ACCENT:UI_FAINT),2);
        UiRect knob={r.x+2+w*v,r.y+r.h/2-6,12,12};
        p->fill(p->user,knob,color(u,enabled?(hot || focused?UI_BRIGHT:UI_ACCENT):UI_FAINT),6);
        break;
    }
    case UI_TEXTBOX:
        p->fill(p->user,inset(r,1),color(u,UI_TRACK),radius);
        p->stroke(p->user,inset(r,.5f),color(u,focused?UI_ACCENT:hot?UI_FAINT:UI_BORDER),radius,1);
        if (p->native_textbox!=id) p->text(p->user,text_inset(r),n->text,n->style.font,fg,false);
        break;
    case UI_PROGRESS: {
        p->fill(p->user,r,color(u,UI_TRACK),radius);
        r.w*=n->value<0?0:n->value>1?1:n->value;
        p->fill(p->user,r,color(u,UI_ACCENT),radius); break;
    }
    case UI_SEPARATOR:
        p->fill(p->user,r,color(u,UI_BORDER),0); break;
    default: break;
    }
    for (UiId c=n->first;c;c=u->nodes[c].next) draw(u,p,c);
    if (n->kind==UI_SCROLL && ui_scroll_max(u,id)>0) {
        UiRect thumb=ui_scroll_thumb(u,id);
        thumb.x+=2; thumb.w=thumb.w>4?thumb.w-4:0;
        p->fill(p->user,thumb,color(u,u->drag_scroll==id?UI_ACCENT:hot?UI_MUTED:UI_FAINT),3);
    }
    if (n->style.border) p->stroke(p->user,inset(n->rect,.5f),color(u,UI_BORDER),radius,1);
    if (focused && u->keyboard_focus && n->kind!=UI_TEXTBOX)
        p->stroke(p->user,inset(n->rect,1.5f),color(u,UI_ACCENT),radius,1);
    p->pop_clip(p->user);
}
void ui_paint(Ui *u, const UiPainter *p) {
    if (u->layout_dirty) ui_layout(u,u->width,u->height);
    if (u->root) draw(u,p,u->root);
    u->paint_dirty=false;
}
