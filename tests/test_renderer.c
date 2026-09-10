/* Integration test owns its hidden window; it does not interact with user apps. */
#include "../platform/renderer.h"
#include "../showcase/showcase.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
int main(void) {
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED)));
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW cls={0}; cls.hInstance=instance; cls.lpfnWndProc=DefWindowProcW; cls.lpszClassName=L"DarkUi.RendererTest";
    CHECK(RegisterClassW(&cls));
    HWND window=CreateWindowW(cls.lpszClassName,L"Renderer test",WS_OVERLAPPEDWINDOW,0,0,1280,800,NULL,NULL,instance,NULL);
    CHECK(window);
    Ui *ui=calloc(1,sizeof *ui); UiRenderer *renderer=calloc(1,sizeof *renderer);
    CHECK(ui && renderer);
    for (int iteration=0;iteration<3;iteration++) {
        ui_init(ui,NULL,NULL);
        CHECK(SUCCEEDED(renderer_init(renderer,&ui->theme)));
        ui->measure=renderer_measure; ui->measure_user=renderer;
        Showcase showcase; CHECK(showcase_init(&showcase,ui));
        UiExtent before=renderer_measure(renderer,L"DPI invariant",UI_BODY);
        CHECK(before.w>10 && before.h>10);
        for (int scale=1;scale<=4;scale++) {
            float dpi=scale*48.f+48;
            renderer_resize(renderer,1280,800,dpi);
            showcase_resize(&showcase,1280*96/dpi,800*96/dpi);
            ui_layout(ui,1280*96/dpi,800*96/dpi);
            CHECK(SUCCEEDED(renderer_paint(renderer,window,ui,UI_NONE)));
            CHECK(renderer->target && renderer->brush);
            UiExtent after=renderer_measure(renderer,L"DPI invariant",UI_BODY);
            CHECK(fabsf(before.w-after.w)<.001f && fabsf(before.h-after.h)<.001f);
            float dx=0,dy=0;
            ((ID2D1RenderTarget *)renderer->target)->lpVtbl->GetDpi((ID2D1RenderTarget *)renderer->target,&dx,&dy);
            CHECK(dx==dpi && dy==dpi);
            /* Force the exact release/recreate path used for device loss. */
            renderer_drop_target(renderer); CHECK(!renderer->target && !renderer->brush);
            CHECK(SUCCEEDED(renderer_paint(renderer,window,ui,UI_NONE)));
        }
        UiTheme larger=ui->theme; larger.font_size[UI_BODY]=18;
        CHECK(SUCCEEDED(renderer_set_theme(renderer,&larger)));
        CHECK(renderer_measure(renderer,L"DPI invariant",UI_BODY).w>before.w);
        renderer_dispose(renderer); renderer_dispose(renderer); /* Idempotent teardown. */
        CHECK(!renderer->target && !renderer->write && !renderer->d2d);
    }
    free(ui); free(renderer); DestroyWindow(window); UnregisterClassW(cls.lpszClassName,instance); CoUninitialize();
    puts("PASS: DirectWrite metrics, 96/144/192/240 DPI, target recreation, theme replacement, repeated teardown");
    return 0;
}
