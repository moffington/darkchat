/* Deterministic Direct2D/WIC artifacts; no desktop capture or UI automation. */
#include "../platform/renderer.h"
#include "../showcase/showcase.h"
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK_HR(call) do { HRESULT h_=(call); if (FAILED(h_)) { fprintf(stderr,"%s: 0x%08lX\n",#call,(unsigned long)h_); exit(1); } } while (0)
#define RELEASE(p) do { if (p) { ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p)); (p)=NULL; } } while (0)
static void snapshot(UiRenderer *renderer, Ui *ui, Showcase *showcase, IWICImagingFactory *wic,
    int width, int height, float dpi, const wchar_t *filename) {
    IWICBitmap *bitmap=NULL;
    ID2D1RenderTarget *target=NULL;
    ID2D1SolidColorBrush *brush=NULL;
    CHECK_HR(wic->lpVtbl->CreateBitmap(wic,(UINT)width,(UINT)height,&GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap));
    D2D1_RENDER_TARGET_PROPERTIES props={0}; props.type=D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM; props.pixelFormat.alphaMode=D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX=props.dpiY=dpi;
    CHECK_HR(renderer->d2d->lpVtbl->CreateWicBitmapRenderTarget(renderer->d2d,bitmap,&props,&target));
    D2D1_COLOR_F white={1,1,1,1}; CHECK_HR(target->lpVtbl->CreateSolidColorBrush(target,&white,NULL,&brush));
    showcase_resize(showcase,width*96.f/dpi,height*96.f/dpi); ui_layout(ui,width*96.f/dpi,height*96.f/dpi);
    target->lpVtbl->BeginDraw(target);
    CHECK_HR(renderer_draw(renderer,target,brush,ui,UI_NONE));
    CHECK_HR(target->lpVtbl->EndDraw(target,NULL,NULL));
    IWICStream *stream=NULL; IWICBitmapEncoder *encoder=NULL; IWICBitmapFrameEncode *frame=NULL;
    CHECK_HR(wic->lpVtbl->CreateStream(wic,&stream));
    CHECK_HR(stream->lpVtbl->InitializeFromFilename(stream,filename,GENERIC_WRITE));
    CHECK_HR(wic->lpVtbl->CreateEncoder(wic,&GUID_ContainerFormatPng,NULL,&encoder));
    CHECK_HR(encoder->lpVtbl->Initialize(encoder,(IStream *)stream,WICBitmapEncoderNoCache));
    CHECK_HR(encoder->lpVtbl->CreateNewFrame(encoder,&frame,NULL));
    CHECK_HR(frame->lpVtbl->Initialize(frame,NULL));
    CHECK_HR(frame->lpVtbl->SetSize(frame,(UINT)width,(UINT)height));
    CHECK_HR(frame->lpVtbl->SetResolution(frame,dpi,dpi));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    CHECK_HR(frame->lpVtbl->SetPixelFormat(frame,&format));
    CHECK_HR(frame->lpVtbl->WriteSource(frame,(IWICBitmapSource *)bitmap,NULL));
    CHECK_HR(frame->lpVtbl->Commit(frame)); CHECK_HR(encoder->lpVtbl->Commit(encoder));
    RELEASE(frame); RELEASE(encoder); RELEASE(stream); RELEASE(brush); RELEASE(target); RELEASE(bitmap);
}
int main(void) {
    CHECK_HR(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED));
    IWICImagingFactory *wic=NULL;
    CHECK_HR(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&wic));
    Ui *ui=calloc(1,sizeof *ui); UiRenderer *renderer=calloc(1,sizeof *renderer); Showcase showcase;
    if (!ui || !renderer) return 1;
    ui_init(ui,NULL,NULL); CHECK_HR(renderer_init(renderer,&ui->theme));
    ui->measure=renderer_measure; ui->measure_user=renderer;
    if (!showcase_init(&showcase,ui)) return 1;
    snapshot(renderer,ui,&showcase,wic,1280,790,96,L"build/showcase-wide.png");
    snapshot(renderer,ui,&showcase,wic,600,560,96,L"build/showcase-compact.png");
    snapshot(renderer,ui,&showcase,wic,1800,1050,144,L"build/showcase-150.png");
    ui->on_event(ui->event_user,ui,(UiEvent){showcase.navigation[1],UI_ACTIVATE});
    snapshot(renderer,ui,&showcase,wic,1280,790,96,L"build/showcase-tokens.png");
    ui->on_event(ui->event_user,ui,(UiEvent){showcase.navigation[2],UI_ACTIVATE});
    snapshot(renderer,ui,&showcase,wic,1280,790,96,L"build/showcase-input.png");
    renderer_dispose(renderer); free(renderer); free(ui); RELEASE(wic); CoUninitialize();
    puts("Rendered five showcase PNGs to build/"); return 0;
}
