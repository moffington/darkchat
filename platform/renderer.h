#ifndef DARK_RENDERER_H
#define DARK_RENDERER_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include "../ui/ui.h"

#define UI_TEXT_CACHE_CAPACITY 256
typedef struct {
    IDWriteTextLayout *layout;
    wchar_t text[UI_TEXT_CAPACITY];
    UiFont font;
    float width, height;
    bool centered;
    unsigned long stamp;
} UiTextCache;
typedef struct {
    ID2D1Factory *d2d;
    IDWriteFactory *write;
    IDWriteTextFormat *formats[UI_FONT_COUNT];
    IDWriteInlineObject *ellipsis[UI_FONT_COUNT];
    ID2D1HwndRenderTarget *target;
    ID2D1SolidColorBrush *brush;
    UiTextCache cache[UI_TEXT_CACHE_CAPACITY];
    unsigned long stamp;
    float dpi;
    HRESULT last_error;
} UiRenderer;

HRESULT renderer_init(UiRenderer *r, const UiTheme *theme);
HRESULT renderer_set_theme(UiRenderer *r, const UiTheme *theme);
void renderer_dispose(UiRenderer *r);
void renderer_drop_target(UiRenderer *r);
void renderer_resize(UiRenderer *r, UINT width, UINT height, float dpi);
UiExtent renderer_measure(void *r, const wchar_t *text, UiFont font);
HRESULT renderer_paint(UiRenderer *r, HWND window, Ui *ui, UiId native_textbox);
/* Draw to a caller-owned Direct2D target (e.g. a WIC bitmap for export/tests).
   The brush must belong to that target. Does not BeginDraw/EndDraw or own either. */
HRESULT renderer_draw(UiRenderer *r, ID2D1RenderTarget *target,
    ID2D1SolidColorBrush *brush, Ui *ui, UiId native_textbox);
#endif
