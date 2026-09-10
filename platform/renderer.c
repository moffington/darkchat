#include <initguid.h>
#include "renderer.h"
#include <string.h>

#define RT(r) ((ID2D1RenderTarget *)(r)->target)
#define RELEASE(p) do { if (p) { ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p)); (p)=NULL; } } while (0)

static void clear_text(UiRenderer *r) {
    for (int i=0;i<UI_TEXT_CACHE_CAPACITY;i++) RELEASE(r->cache[i].layout);
    memset(r->cache,0,sizeof r->cache); r->stamp=0;
}
HRESULT renderer_set_theme(UiRenderer *r, const UiTheme *theme) {
    IDWriteTextFormat *formats[UI_FONT_COUNT]={0};
    IDWriteInlineObject *ellipsis[UI_FONT_COUNT]={0};
    HRESULT hr=S_OK;
    for (int i=0;i<UI_FONT_COUNT;i++) {
        hr=r->write->lpVtbl->CreateTextFormat(r->write,theme->font_family,NULL,
            (DWRITE_FONT_WEIGHT)theme->font_weight[i],DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,theme->font_size[i],L"en-US",&formats[i]);
        if (FAILED(hr)) goto fail;
        hr=formats[i]->lpVtbl->SetParagraphAlignment(formats[i],DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (FAILED(hr)) goto fail;
        hr=formats[i]->lpVtbl->SetWordWrapping(formats[i],DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(hr)) goto fail;
        hr=r->write->lpVtbl->CreateEllipsisTrimmingSign(r->write,formats[i],&ellipsis[i]);
        if (FAILED(hr)) goto fail;
    }
    clear_text(r);
    for (int i=0;i<UI_FONT_COUNT;i++) {
        RELEASE(r->formats[i]); RELEASE(r->ellipsis[i]);
        r->formats[i]=formats[i]; r->ellipsis[i]=ellipsis[i];
    }
    return S_OK;
fail:
    for (int i=0;i<UI_FONT_COUNT;i++) { RELEASE(formats[i]); RELEASE(ellipsis[i]); }
    return hr;
}
HRESULT renderer_init(UiRenderer *r, const UiTheme *theme) {
    memset(r,0,sizeof *r); r->dpi=96;
    D2D1_FACTORY_OPTIONS options={0};
    HRESULT hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&IID_ID2D1Factory,&options,(void **)&r->d2d);
    if (SUCCEEDED(hr)) hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,&IID_IDWriteFactory,(IUnknown **)&r->write);
    if (SUCCEEDED(hr)) hr=renderer_set_theme(r,theme);
    if (FAILED(hr)) renderer_dispose(r);
    return hr;
}
void renderer_drop_target(UiRenderer *r) { RELEASE(r->brush); RELEASE(r->target); }
void renderer_dispose(UiRenderer *r) {
    renderer_drop_target(r); clear_text(r);
    for (int i=0;i<UI_FONT_COUNT;i++) { RELEASE(r->formats[i]); RELEASE(r->ellipsis[i]); }
    RELEASE(r->write); RELEASE(r->d2d);
}
void renderer_resize(UiRenderer *r, UINT width, UINT height, float dpi) {
    r->dpi=dpi>0?dpi:96;
    if (!r->target) return;
    RT(r)->lpVtbl->SetDpi(RT(r),r->dpi,r->dpi);
    if (!width || !height) return;
    D2D1_SIZE_U size={width,height};
    HRESULT hr=r->target->lpVtbl->Resize(r->target,&size);
    if (FAILED(hr)) { r->last_error=hr; renderer_drop_target(r); }
}
static HRESULT target(UiRenderer *r, HWND window) {
    if (r->target) return S_OK;
    RECT client; GetClientRect(window,&client);
    D2D1_RENDER_TARGET_PROPERTIES props={0};
    props.type=D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode=D2D1_ALPHA_MODE_IGNORE;
    props.dpiX=props.dpiY=r->dpi;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd={0};
    hwnd.hwnd=window; hwnd.pixelSize=(D2D1_SIZE_U){(UINT32)client.right,(UINT32)client.bottom};
    HRESULT hr=r->d2d->lpVtbl->CreateHwndRenderTarget(r->d2d,&props,&hwnd,&r->target);
    if (SUCCEEDED(hr)) {
        D2D1_COLOR_F c={1,1,1,1};
        hr=RT(r)->lpVtbl->CreateSolidColorBrush(RT(r),&c,NULL,&r->brush);
    }
    if (FAILED(hr)) renderer_drop_target(r);
    return hr;
}
static IDWriteTextLayout *text_layout(UiRenderer *r, const wchar_t *text, UiFont font, float w, float h, bool centered) {
    if (w<=0 || h<=0 || font>=UI_FONT_COUNT) return NULL;
    UiTextCache *slot=&r->cache[0];
    for (int i=0;i<UI_TEXT_CACHE_CAPACITY;i++) {
        UiTextCache *c=&r->cache[i];
        if (c->layout && c->font==font && c->width==w && c->height==h && c->centered==centered && !wcscmp(c->text,text)) {
            c->stamp=++r->stamp; return c->layout;
        }
        if (!c->layout || (slot->layout && c->stamp<slot->stamp)) slot=c;
    }
    RELEASE(slot->layout);
    HRESULT hr=r->write->lpVtbl->CreateTextLayout(r->write,text,(UINT32)wcslen(text),r->formats[font],w,h,&slot->layout);
    if (FAILED(hr)) { r->last_error=hr; return NULL; }
    DWRITE_TRIMMING trim={DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
    hr=slot->layout->lpVtbl->SetTrimming(slot->layout,&trim,r->ellipsis[font]);
    if (SUCCEEDED(hr)) hr=slot->layout->lpVtbl->SetTextAlignment(slot->layout,centered?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(hr)) { r->last_error=hr; RELEASE(slot->layout); return NULL; }
    wcsncpy(slot->text,text,UI_TEXT_CAPACITY-1); slot->text[UI_TEXT_CAPACITY-1]=0;
    slot->font=font; slot->width=w; slot->height=h; slot->centered=centered; slot->stamp=++r->stamp;
    return slot->layout;
}
UiExtent renderer_measure(void *user, const wchar_t *text, UiFont font) {
    UiRenderer *r=user;
    IDWriteTextLayout *layout=text_layout(r,text,font,100000,1000,false);
    DWRITE_TEXT_METRICS metrics={0};
    if (layout && SUCCEEDED(layout->lpVtbl->GetMetrics(layout,&metrics)))
        return (UiExtent){metrics.widthIncludingTrailingWhitespace,metrics.height};
    return (UiExtent){0,0};
}
static D2D1_RECT_F rect(UiRect r) { return (D2D1_RECT_F){r.x,r.y,r.x+r.w,r.y+r.h}; }
typedef struct { UiRenderer *renderer; ID2D1RenderTarget *target; ID2D1SolidColorBrush *brush; } DrawContext;
static ID2D1Brush *brush(DrawContext *r, UiColor c) {
    D2D1_COLOR_F color={c.r/255.f,c.g/255.f,c.b/255.f,c.a/255.f};
    r->brush->lpVtbl->SetColor(r->brush,&color); return (ID2D1Brush *)r->brush;
}
static void fill(void *user, UiRect b, UiColor c, float radius) {
    DrawContext *r=user; D2D1_ROUNDED_RECT q={rect(b),radius,radius};
    r->target->lpVtbl->FillRoundedRectangle(r->target,&q,brush(r,c));
}
static void stroke(void *user, UiRect b, UiColor c, float radius, float width) {
    DrawContext *r=user; D2D1_ROUNDED_RECT q={rect(b),radius,radius};
    r->target->lpVtbl->DrawRoundedRectangle(r->target,&q,brush(r,c),width,NULL);
}
static void text(void *user, UiRect b, const wchar_t *s, UiFont font, UiColor c, bool centered) {
    DrawContext *r=user; IDWriteTextLayout *layout=text_layout(r->renderer,s,font,b.w,b.h,centered);
    if (layout) r->target->lpVtbl->DrawTextLayout(r->target,(D2D1_POINT_2F){b.x,b.y},layout,brush(r,c),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static void line(void *user, float x, float y, float xx, float yy, UiColor c, float width) {
    DrawContext *r=user;
    r->target->lpVtbl->DrawLine(r->target,(D2D1_POINT_2F){x,y},(D2D1_POINT_2F){xx,yy},brush(r,c),width,NULL);
}
static void push(void *user, UiRect b) {
    DrawContext *r=user; D2D1_RECT_F clip=rect(b);
    r->target->lpVtbl->PushAxisAlignedClip(r->target,&clip,D2D1_ANTIALIAS_MODE_ALIASED);
}
static void pop(void *user) { DrawContext *r=user; r->target->lpVtbl->PopAxisAlignedClip(r->target); }
HRESULT renderer_draw(UiRenderer *r, ID2D1RenderTarget *draw_target,
    ID2D1SolidColorBrush *draw_brush, Ui *ui, UiId native_textbox) {
    if (!draw_target || !draw_brush) return E_INVALIDARG;
    DrawContext context={r,draw_target,draw_brush};
    r->last_error=S_OK;
    UiPainter painter={&context,fill,stroke,text,line,push,pop,native_textbox};
    ui_paint(ui,&painter);
    return r->last_error;
}
HRESULT renderer_paint(UiRenderer *r, HWND window, Ui *ui, UiId native_textbox) {
    HRESULT hr=target(r,window);
    if (FAILED(hr)) return r->last_error=hr;
    UiColor c=ui->theme.colors[UI_BG]; D2D1_COLOR_F bg={c.r/255.f,c.g/255.f,c.b/255.f,1};
    RT(r)->lpVtbl->BeginDraw(RT(r)); RT(r)->lpVtbl->Clear(RT(r),&bg);
    renderer_draw(r,RT(r),r->brush,ui,native_textbox);
    hr=RT(r)->lpVtbl->EndDraw(RT(r),NULL,NULL);
    if (FAILED(hr)) renderer_drop_target(r);
    return r->last_error=FAILED(hr)?hr:r->last_error;
}
