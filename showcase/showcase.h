#ifndef DARK_SHOWCASE_H
#define DARK_SHOWCASE_H
#include "../ui/ui.h"
typedef struct {
    Ui *ui;
    UiId sidebar, inspector, viewport, title, subtitle, status, summary;
    UiId navigation[3], pages[3], pair, action, secondary, checkbox, toggle;
    UiId slider, progress, value_label, edit, edit_summary, disable, reset;
    UiId catalog, catalog_rows[8], filter;
    int page, actions;
    bool compact, stacked, narrow;
} Showcase;
bool showcase_init(Showcase *s, Ui *ui);
void showcase_resize(Showcase *s, float width, float height);
#endif
