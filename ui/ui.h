#ifndef DARK_UI_H
#define DARK_UI_H

/* Reusable UI core. No operating-system or graphics API dependencies.
   Geometry, typography and input coordinates are always 96-DPI DIPs.
   Nodes and their text are owned by the context; IDs are opaque generation
   handles and remain stable until their node is removed or the context is reset.
   All operations run on the owning UI thread. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define UI_CAPACITY 256
#define UI_TEXT_CAPACITY 192
typedef uint32_t UiId;
#define UI_NONE ((UiId)0)

typedef struct { float x, y, w, h; } UiRect;
typedef struct { float w, h; } UiExtent;
typedef struct { uint8_t r, g, b, a; } UiColor;
typedef enum {
    UI_BG, UI_PANEL, UI_TOOLBAR, UI_BORDER, UI_TEXT, UI_BRIGHT,
    UI_MUTED, UI_FAINT, UI_HOVER, UI_SELECTED, UI_ACCENT, UI_ACCENT_SOFT,
    UI_BUTTON_BG, UI_BUTTON_HOT, UI_BUTTON_DOWN, UI_TRACK, UI_COLOR_COUNT
} UiColorRole;
typedef enum { UI_SMALL, UI_BODY, UI_TITLE, UI_SECTION, UI_HEADING, UI_FONT_COUNT } UiFont;
typedef struct {
    UiColor colors[UI_COLOR_COUNT];
    float font_size[UI_FONT_COUNT];
    int font_weight[UI_FONT_COUNT];
    wchar_t font_family[48];
    float radius, control_height, padding, gap, scrollbar_width, min_thumb;
} UiTheme;
UiTheme ui_theme_dark(void);

typedef enum {
    UI_COLUMN, UI_ROW, UI_SCROLL, UI_LABEL, UI_BUTTON, UI_CHECKBOX,
    UI_SWITCH, UI_SLIDER, UI_TEXTBOX, UI_PROGRESS, UI_SEPARATOR,
    UI_ICON, UI_ICON_BUTTON
} UiKind;
/* Line-art glyphs drawn with the painter primitives, so no icon font or
   external asset is required. UI_ICON_NONE draws nothing. */
typedef enum {
    UI_ICON_NONE = 0, UI_ICON_HAMBURGER, UI_ICON_SEND, UI_ICON_STOP,
    UI_ICON_OVERFLOW, UI_ICON_PLUS, UI_ICON_SEARCH, UI_ICON_CHEVRON_DOWN,
    UI_ICON_CLOSE, UI_ICON_COUNT
} UiIcon;
typedef enum { UI_AUTO, UI_FIXED, UI_FLEX } UiSizeKind;
typedef struct { UiSizeKind kind; float value; } UiSize;
UiSize ui_auto(void);
UiSize ui_fixed(float dips);
UiSize ui_flex(float weight);
typedef struct {
    UiSize width, height;
    float min_w, min_h, max_w, max_h; /* max == 0 means unbounded */
    float padding, gap;
    int background; /* UiColorRole, or -1 for transparent */
    UiColorRole foreground;
    UiFont font;
    bool border;
    bool text_centered; /* labels only: center horizontally in their rect */
} UiStyle;

typedef struct {
    UiId parent, first, last, next;
    UiKind kind;
    UiStyle style;
    UiRect rect, clip, viewport;
    UiExtent measured;
    wchar_t text[UI_TEXT_CAPACITY];
    wchar_t accessible_name[UI_TEXT_CAPACITY];
    wchar_t help_text[UI_TEXT_CAPACITY];
    UiId labelled_by;
    UiIcon icon; /* UI_ICON / UI_ICON_BUTTON glyph */
    bool hidden, disabled, checked, selected;
    float value; /* slider/progress: normalized [0,1] */
    float scroll, content_height;
    uintptr_t tag; /* application-owned identifier */
    uint32_t generation;
    uint8_t free_next;
    bool alive;
} UiNode;
typedef enum { UI_ACTIVATE, UI_CHANGE } UiEventKind;
typedef struct { UiId id; UiEventKind kind; } UiEvent;
typedef enum {
    UI_KEY_TAB, UI_KEY_ENTER, UI_KEY_SPACE, UI_KEY_ESCAPE,
    UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN
} UiKey;
typedef struct Ui Ui;
typedef void (*UiEventFn)(void *user, Ui *ui, UiEvent event);
typedef UiExtent (*UiMeasureFn)(void *user, const wchar_t *text, UiFont font);
struct Ui {
    UiNode nodes[UI_CAPACITY];
    uint16_t count;
    uint8_t next_slot, free_slot;
    UiId root, hot, focus, pressed, drag_scroll;
    bool keyboard_focus, window_active, layout_dirty, paint_dirty, overflow;
    bool key_pressed, pointer_known;
    UiKey activation_key;
    float width, height, pointer_x, pointer_y, drag_offset;
    UiTheme theme;
    UiMeasureFn measure;
    void *measure_user;
    UiEventFn on_event;
    void *event_user;
};

/* Rendering contract: callbacks consume DIPs and respect nested clip scopes.
   Text is single-line, vertically centered, clipped and end-ellipsized. */
typedef struct {
    void *user;
    void (*fill)(void *, UiRect, UiColor, float radius);
    void (*stroke)(void *, UiRect, UiColor, float radius, float width);
    void (*text)(void *, UiRect, const wchar_t *, UiFont, UiColor, bool centered);
    void (*line)(void *, float, float, float, float, UiColor, float width);
    void (*push_clip)(void *, UiRect);
    void (*pop_clip)(void *);
    UiId native_textbox; /* omit text while the platform editor overlays it */
} UiPainter;

void ui_init(Ui *ui, UiMeasureFn measure, void *measure_user);
/* Returns UI_NONE and latches overflow on exhaustion/invalid parent. */
UiId ui_add(Ui *ui, UiId parent, UiKind kind, const wchar_t *text);
/* Removes id and its descendants. All handles to them become invalid. */
bool ui_remove(Ui *ui, UiId id);
/* Moves a non-root node to the end of a container. Cycles are rejected. */
bool ui_reparent(Ui *ui, UiId id, UiId parent);
UiNode *ui_node(Ui *ui, UiId id);
void ui_invalidate(Ui *ui, bool layout);
void ui_set_text(Ui *ui, UiId id, const wchar_t *text);
/* Sets the glyph of a UI_ICON / UI_ICON_BUTTON node. Paint-only: never
   affects layout, so a state swap (Send <-> Stop) repaints in place. */
void ui_set_icon(Ui *ui, UiId id, UiIcon icon);
void ui_set_accessible_name(Ui *ui, UiId id, const wchar_t *name);
void ui_set_help_text(Ui *ui, UiId id, const wchar_t *help_text);
void ui_set_labelled_by(Ui *ui, UiId id, UiId label);
const wchar_t *ui_accessible_name(Ui *ui, UiId id);
void ui_set_hidden(Ui *ui, UiId id, bool hidden);
void ui_set_disabled(Ui *ui, UiId id, bool disabled);
bool ui_visible(const Ui *ui, UiId id);
bool ui_enabled(const Ui *ui, UiId id);
/* Performs the default action of an enabled button. */
bool ui_invoke(Ui *ui, UiId id);
void ui_layout(Ui *ui, float width, float height);
void ui_paint(Ui *ui, const UiPainter *painter);
UiId ui_hit_test(const Ui *ui, float x, float y);
void ui_pointer_move(Ui *ui, float x, float y);
void ui_pointer_leave(Ui *ui);
void ui_pointer_down(Ui *ui, float x, float y);
void ui_pointer_up(Ui *ui, float x, float y);
void ui_cancel_input(Ui *ui);
void ui_scroll(Ui *ui, float x, float y, float delta_dips);
void ui_key(Ui *ui, UiKey key, bool down, bool shift, bool repeat);
void ui_focus(Ui *ui, UiId id, bool keyboard);
/* Focuses the first focusable node in tree order (forward) or the last
   (reverse). Returns false when nothing is focusable. */
bool ui_focus_edge(Ui *ui, bool reverse);
/* True when the focused node is the last focusable in tree order (forward)
   or the first (reverse), i.e. a Tab in that direction leaves the tree.
   A host that mixes retained controls with native child windows uses this to
   hand focus across the boundary instead of wrapping inside the tree. */
bool ui_focus_boundary(const Ui *ui, bool reverse);
/* Moves keyboard focus by `delta` items among the enabled, visible button-like
   items (button, icon button, checkbox, switch) of the focused node's nearest
   UI_SCROLL ancestor, in tree order. `delta` is clamped to the container's
   first/last item, so movement never wraps into a sibling container, and
   sliders, textboxes and scroll containers keep their own arrow semantics.
   Returns true only when focus changed. Focus is applied with keyboard state,
   so reveal runs and keyboard focus is recorded. */
bool ui_focus_move(Ui *ui, int delta);
void ui_set_active(Ui *ui, bool active);
UiRect ui_scroll_thumb(const Ui *ui, UiId id);
float ui_scroll_max(const Ui *ui, UiId id);
/* Absolute read access to a scroll container's state: the current offset and
   the laid-out viewport height (0 before the first layout or when id is not
   a scroll node). Windowed lists derive their visible range from these. */
float ui_scroll_offset(const Ui *ui, UiId id);
float ui_scroll_viewport_h(const Ui *ui, UiId id);
/* Sets the absolute scroll offset, clamped to [0, ui_scroll_max]. Marks the
   layout dirty only when the value actually changes. */
void ui_scroll_to(Ui *ui, UiId id, float offset);
/* True when the tree changed since the last ui_layout and a relayout is
   owed before painting or reading arranged geometry. */
bool ui_layout_pending(const Ui *ui);
bool ui_contains(UiRect r, float x, float y);
UiRect ui_intersect(UiRect a, UiRect b);

#endif
