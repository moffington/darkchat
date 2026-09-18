#include "ui.h"

UiTheme ui_theme_dark(void) {
    /* Disciplined near-black chat palette: one neutral ramp from background
       through panel and input surfaces, a single blue accent family, and a
       clear bright/text/muted/faint hierarchy. */
    UiTheme t = {
        .colors = {
            {15,16,19,255},    /* UI_BG: application/transcript background */
            {20,21,25,255},    /* UI_PANEL: sidebar and nested panels */
            {18,19,23,255},    /* UI_TOOLBAR: header strip */
            {42,45,52,255},    /* UI_BORDER */
            {222,226,233,255}, /* UI_TEXT */
            {243,245,249,255}, /* UI_BRIGHT */
            {154,160,171,255}, /* UI_MUTED */
            {107,113,123,255}, /* UI_FAINT */
            {28,30,36,255},    /* UI_HOVER */
            {34,38,47,255},    /* UI_SELECTED */
            {92,138,246,255},  /* UI_ACCENT */
            {38,55,95,255},    /* UI_ACCENT_SOFT */
            {26,28,33,255},    /* UI_BUTTON_BG */
            {35,38,45,255},    /* UI_BUTTON_HOT */
            {43,47,56,255},    /* UI_BUTTON_DOWN */
            {23,25,29,255}     /* UI_TRACK: inputs, composer, chips */
        },
        .font_size = {11,13,15,11,19},
        .font_weight = {400,400,600,700,600},
        .font_family = L"Segoe UI",
        .radius = 6, .control_height = 30, .padding = 16, .gap = 8,
        .scrollbar_width = 10, .min_thumb = 28
    };
    return t;
}
