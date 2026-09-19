#include "ui.h"

static UiRect inset(UiRect r, float p) {
    r.x+=p; r.y+=p; r.w=r.w>2*p?r.w-2*p:0; r.h=r.h>2*p?r.h-2*p:0; return r;
}
static UiColor color(const Ui *u, UiColorRole role) { return u->theme.colors[role]; }
static UiRect text_inset(UiRect r) { r.x+=8; r.w=r.w>16?r.w-16:0; return r; }
static float glyph_min(float a, float b) { return a < b ? a : b; }

/* Line-art glyphs drawn from the painter primitives. Every coordinate is
   derived from the target rect, so an icon scales with its control and no
   asset or icon font is involved. */
static void draw_icon(const UiPainter *p, UiRect r, UiIcon icon, UiColor c) {
    if (icon == UI_ICON_NONE) return;
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    float s = glyph_min(r.w, r.h) / 2;
    if (s > 9) s = 9;
    switch (icon) {
    case UI_ICON_HAMBURGER:
        for (int i = -1; i <= 1; i++)
            p->line(p->user, cx - s, cy + i * 4, cx + s, cy + i * 4, c, 1.6f);
        break;
    case UI_ICON_SEND:
        p->line(p->user, cx, cy + s, cx, cy - s, c, 1.7f);
        p->line(p->user, cx, cy - s, cx - s * .55f, cy - s * .3f, c, 1.7f);
        p->line(p->user, cx, cy - s, cx + s * .55f, cy - s * .3f, c, 1.7f);
        break;
    case UI_ICON_STOP:
        p->fill(p->user, (UiRect){cx - s * .62f, cy - s * .62f, s * 1.24f,
            s * 1.24f}, c, 2);
        break;
    case UI_ICON_OVERFLOW:
        for (int i = -1; i <= 1; i++)
            p->fill(p->user, (UiRect){cx + i * 6 - 1.5f, cy - 1.5f, 3, 3}, c, 1.5f);
        break;
    case UI_ICON_PLUS:
        p->line(p->user, cx - s, cy, cx + s, cy, c, 1.6f);
        p->line(p->user, cx, cy - s, cx, cy + s, c, 1.6f);
        break;
    case UI_ICON_SEARCH: {
        float ring = s * .78f;
        p->stroke(p->user, (UiRect){cx - ring - 1, cy - ring - 1,
            ring * 2, ring * 2}, c, ring, 1.5f);
        p->line(p->user, cx + ring * .55f, cy + ring * .55f,
            cx + s, cy + s, c, 1.5f);
        break;
    }
    case UI_ICON_CHEVRON_DOWN:
        p->line(p->user, cx - s * .55f, cy - s * .28f, cx, cy + s * .3f, c, 1.6f);
        p->line(p->user, cx, cy + s * .3f, cx + s * .55f, cy - s * .28f, c, 1.6f);
        break;
    case UI_ICON_CLOSE:
        p->line(p->user, cx - s * .6f, cy - s * .6f, cx + s * .6f, cy + s * .6f, c, 1.6f);
        p->line(p->user, cx - s * .6f, cy + s * .6f, cx + s * .6f, cy - s * .6f, c, 1.6f);
        break;
    default:
        break;
    }
}
static void draw(Ui *u, const UiPainter *p, UiId id) {
    UiNode *n=ui_node(u,id);
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
    case UI_LABEL: {
        /* The style may add a symmetric extra inset to the built-in 8 DIPs,
           so boxed labels can keep their text off the border. Centered text
           stays centered: both sides shrink equally. */
        UiRect text=text_inset(r);
        float extra=n->style.text_inset;
        if (extra>0) {
            text.x+=extra;
            text.w=text.w>2*extra?text.w-2*extra:0;
        }
        p->text(p->user,text,n->text,n->style.font,fg,
            n->style.text_centered);
        break;
    }
    case UI_BUTTON: {
        if (n->style.flat) {
            /* Flat rows are transparent when idle; only hover, press and
               selection receive a soft background, and no stroke: the row
               reads as part of the list, not as a filled button. */
            if (down) p->fill(p->user,inset(r,1),color(u,UI_BUTTON_DOWN),radius);
            else if (hot) p->fill(p->user,inset(r,1),color(u,UI_HOVER),radius);
            else if (n->selected) p->fill(p->user,inset(r,1),color(u,UI_SELECTED),radius);
            p->text(p->user,text_inset(r),n->text,n->style.font,
                n->selected && enabled?color(u,UI_BRIGHT):fg,
                n->style.text_centered);
            break;
        }
        UiColorRole bg=down?UI_BUTTON_DOWN:hot?UI_BUTTON_HOT:n->selected?UI_ACCENT_SOFT:UI_BUTTON_BG;
        p->fill(p->user,inset(r,1),color(u,bg),radius);
        if (n->selected) p->stroke(p->user,inset(r,.5f),color(u,UI_ACCENT_SOFT),radius,1);
        p->text(p->user,text_inset(r),n->text,n->style.font,n->selected && enabled?color(u,UI_BRIGHT):fg,true);
        break;
    }
    case UI_ICON:
        draw_icon(p,r,n->icon,fg);
        break;
    case UI_ICON_BUTTON: {
        if (n->style.flat) {
            /* Same flat semantics as the flat text button: transparent when
               idle, soft fill for hover/press/selection. */
            int flat_role = down ? UI_BUTTON_DOWN : hot ? UI_HOVER :
                n->selected ? UI_SELECTED : -1;
            if (flat_role >= 0)
                p->fill(p->user,inset(r,1),color(u,(UiColorRole)flat_role),radius);
            draw_icon(p,r,n->icon,n->selected && enabled ? color(u,UI_BRIGHT) : fg);
            break;
        }
        /* A transparent icon button (background -1) shows only its hover,
            pressed and selected surfaces; an explicit background keeps the
            idle fill as well. */
        int role = down ? UI_BUTTON_DOWN : hot ? (n->style.background >= 0 ?
            UI_BUTTON_HOT : UI_HOVER) : n->selected ? UI_ACCENT_SOFT :
            n->style.background;
        if (role >= 0) p->fill(p->user,inset(r,1),color(u,(UiColorRole)role),radius);
        if (n->selected) p->stroke(p->user,inset(r,.5f),color(u,UI_ACCENT_SOFT),radius,1);
        draw_icon(p,r,n->icon,n->selected && enabled ? color(u,UI_BRIGHT) : fg);
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
    for (UiId c=n->first;c;) {
        UiNode *child=ui_node(u,c);
        UiId next=child->next;
        draw(u,p,c); c=next;
    }
    if (n->kind==UI_SCROLL && ui_scroll_max(u,id)>0) {
        UiRect thumb=ui_scroll_thumb(u,id);
        thumb.x+=2; thumb.w=thumb.w>4?thumb.w-4:0;
        p->fill(p->user,thumb,color(u,u->drag_scroll==id?UI_ACCENT:hot?UI_MUTED:UI_FAINT),3);
    }
    /* A selected or keyboard-focused bordered surface uses the accent family,
        so native-input placeholders (model field, composer, search) can show
        focus without a bespoke painting path. */
    if (n->style.border)
        p->stroke(p->user,inset(n->rect,.5f),
            color(u,(n->selected || focused) ? UI_ACCENT : UI_BORDER),radius,1);
    /* The generic focus outline is suppressed for flat buttons: their
        selection fill is the primary focus indication (list rows such as the
        command palette), and a second ring would double the highlight. */
    if (focused && u->keyboard_focus && n->kind!=UI_TEXTBOX && !n->style.flat)
        p->stroke(p->user,inset(n->rect,1.5f),color(u,UI_ACCENT),radius,1);
    p->pop_clip(p->user);
}
void ui_paint(Ui *u, const UiPainter *p) {
    if (u->layout_dirty) ui_layout(u,u->width,u->height);
    if (u->root) draw(u,p,u->root);
    u->paint_dirty=false;
}
