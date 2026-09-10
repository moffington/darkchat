#include "ui.h"

UiTheme ui_theme_dark(void) {
    /* The original approved SeedVault palette, now expressed as semantic tokens. */
    UiTheme t = {
        .colors = {
            {24,25,28,255}, {29,31,35,255}, {32,34,38,255}, {47,50,56,255},
            {199,203,209,255}, {235,238,242,255}, {139,145,154,255}, {93,98,106,255},
            {37,40,46,255}, {43,48,60,255}, {105,151,205,255}, {54,74,98,255},
            {38,41,47,255}, {48,52,60,255}, {32,35,40,255}, {43,46,53,255}
        },
        .font_size = {10,12,13,10,20},
        .font_weight = {400,400,600,600,600},
        .font_family = L"Segoe UI",
        .radius = 3, .control_height = 30, .padding = 16, .gap = 8,
        .scrollbar_width = 10, .min_thumb = 28
    };
    return t;
}
