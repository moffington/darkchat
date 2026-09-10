# Dark UI — native foundation

A small retained UI framework and runnable control showcase in **C17, Win32,
Direct2D and DirectWrite**. The original charcoal surfaces, Segoe UI typography,
subtle separators and restrained blue accent are preserved. Image-browser
models, generated image data and application globals have been removed.

## Build and run

Windows 10 1703 or newer, with MinGW-w64 / w64devkit (`gcc` and `windres`) on PATH.
No third-party libraries or C++ runtime are required.

```bat
build.bat
build\darkui.exe

rem Build and run the regression and Direct2D integration tests:
build.bat test

rem Produce five deterministic Direct2D/WIC preview PNGs:
render.bat
```

Outputs go in `build/`. The pre-existing root `darkui.exe`, if present, is the old
prototype; launch **`build\darkui.exe`**. The build uses `-Wall -Wextra -Wpedantic
-Werror`. Test failures propagate a nonzero exit code. Close a running showcase
before rebuilding its executable.

The showcase provides functioning action buttons, disabled states, checkboxes,
switches, a slider linked to a progress bar, a workspace field, a filtered control
catalog, theme specimens and a nested scrolling exercise. Navigation stays
available at the 600 × 420 DIP minimum client size; panels stack and the secondary
inspector disappears as space decreases.

## Architecture

| Layer | Files | Responsibility |
| --- | --- | --- |
| Core | `ui/ui.h`, `ui/ui.c` | Stable IDs, owned text, tree, layout, input, focus and scrolling; no Win32 or graphics headers |
| Design | `ui/theme.c`, `ui/paint.c` | Semantic colors, typography, spacing, control appearance, renderer-independent painter callbacks |
| Rendering | `platform/renderer.*` | DirectWrite measurement/layout cache, Direct2D target and brush lifetime, target recovery; caller-owned targets supported for export |
| Platform | `platform/win32.*` | HWND/message loop, COM, DPI conversion, capture, native text editor, GDI resources and invalidation |
| Application | `showcase/showcase.*`, `main.c` | Composition, responsive breakpoints, sample state and event handling |

The platform knows nothing about the showcase. The showcase knows nothing about
HWNDs, Windows messages, Direct2D or DirectWrite. `main.c` is the composition root.
The renderer exposes a callback painter instead of putting COM calls in controls.

## Using the core

Initialize a fresh `Ui`, create exactly one root, and append children to containers.
Nodes are context-owned and IDs remain valid until `ui_init` resets that context.
Use `ui_node` to configure style, `ui_set_text` for owned text, and the hidden and
disabled setters to keep focus/capture state consistent. Check `ui_add` for
`UI_NONE`; `ui.overflow` latches invalid-parent/capacity errors.

```c
ui_init(ui, NULL, NULL);
UiId root = ui_add(ui, UI_NONE, UI_COLUMN, L"");
UiId button = ui_add(ui, root, UI_BUTTON, L"Run action");
if (!root || !button) { /* report construction failure */ }
ui_node(ui, root)->style.padding = ui->theme.padding;
ui_node(ui, button)->style.width = ui_fixed(140);
ui->on_event = handle_event;
ui->event_user = app;
```

Pass the tree through `UiWindowConfig` to `ui_win32_run`. The host installs the
renderer measurement callback, lays out the tree, routes input, paints when dirty,
and releases its resources on return. Constructing and mutating the tree belongs
on the UI thread. Event callbacks run synchronously after control state changes;
they may update properties, text and visibility, but must not reset the context
or re-enter event dispatch. Direct style/value mutations require
`ui_invalidate(ui, true)` for layout changes, or `false` for appearance changes.

### Layout contract

- Every logical size, font size, pointer coordinate and scroll offset is in DIPs.
  The host converts client pixels once; Direct2D applies target DPI once.
- Rows lay children horizontally; columns vertically. Padding and gaps are explicit.
  `ui_fixed`, `ui_auto` and weighted `ui_flex` work with min/max dimensions.
- Flex distributes the space remaining after fixed/auto children and gaps, freezing
  constrained children before redistributing. Auto dimensions use intrinsic child
  measurements. Flex in an auto-sized parent contributes its intrinsic size.
- Cross-axis flex stretches. Other cross-axis sizes align at the start. Fixed/auto
  children retain their size under pressure; overflow is clipped, not implicitly
  shrunk. Responsive composition is the application's responsibility.
- Scroll containers are vertical. Their children use intrinsic/fixed heights;
  main-axis flex does not consume the viewport. A stable gutter prevents layout
  oscillation when a scrollbar appears. Offsets clamp after resize/content changes.
- The same arranged rectangles and ancestor clips govern rendering and hit testing.
  All containers clip; children outside a viewport cannot receive pointer events.

### Input contract

- Tab and Shift+Tab follow depth-first tree order, skipping hidden/disabled subtrees.
  Focus reveals descendants through nested scroll views. Keyboard focus is drawn
  only while the window is active.
- Buttons/choices activate on matching release, with key-repeat suppression.
  Dragging away cancels a button click. Capture loss, cancellation and deactivation
  clear gesture state. Sliders retain capture while dragging beyond their bounds.
- Slider arrows change 1%, page keys change 10%, Home/End reach the bounds.
  Other controls route scrolling keys to the nearest ancestor viewport.
- Fractional wheel distance is preserved, follows system wheel-line preferences,
  and bubbles at nested scroll boundaries. Scrollbars support thumb drag and
  track paging.
- A focused textbox uses one shared Win32 `EDIT` child. The core retains the text;
  native editing supplies selection, clipboard, undo and IME. The bridge handles
  Tab traversal, Ctrl+A, field changes, clipping and DPI-sized fonts. Enter/Escape
  leave the editor; changes are immediate, so Escape does not roll them back.

### Theme and resources

Set tokens on `ui.theme` before running the host. Color roles cover background,
panel, toolbar, text hierarchy, hover, selection, accent and control states.
`ui_theme_dark()` holds the original RGB palette. Typography, default control
height, default gaps, radius and scrollbar metrics live alongside the palette;
showcase-specific composition dimensions stay in the showcase.

DirectWrite formats and a bounded LRU text-layout cache are independent of device
targets. Theme font replacement is transactional through `renderer_set_theme`.
The HWND target and its brush are released together on draw/resize failure and
recreated lazily. The host uses BeginPaint/EndPaint, skips minimized drawing and
uses bounded, paced retries after a rendering failure. There is no idle animation
timer or background render loop. Live theme switching through the host is not
exposed yet; it would also need to refresh native editor fonts/GDI brushes.

## Validation and deliberate limits

`tests/test_ui.c` covers flex constraints, clipping, disabled ancestors, focus order,
activation/cancellation, nested wheel bubbling, focus reveal, scrollbar dragging,
slider bounds, text capacity and all three pages at six widths. The core tests
need no Windows APIs. `tests/test_renderer.c` runs actual Direct2D/DirectWrite
integration against a test-owned hidden HWND at 96/144/192/240 DPI, including forced
target recreation, theme changes and repeated teardown. `render.bat` exercises the
same painter through WIC for visual review without desktop capture.

This is a foundation, not a complete widget library. It intentionally has a bounded
255-node arena, 191-code-unit text buffers, vertical scrolling, single-line text,
and one top-level host window per `ui_win32_run`. There is no node removal/reparenting,
virtualized list, popup/menu system, multiline editor, rich-text layout or custom
UI Automation provider yet. Native text fields expose native accessibility while
active; custom controls need a UIA provider before screen-reader-ready use.

See [HANDOFF.md](HANDOFF.md) for the next pass and the exact verification limits.
