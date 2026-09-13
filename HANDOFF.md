# Next pass

The image browser has been replaced by a native control showcase over a reusable
retained UI core. Keep the palette in `ui/theme.c` and the compact visual language.
The primary extension boundary is `ui/ui.h`; avoid adding showcase concepts to it.

**Run:** `build.bat test`, then `build\darkui.exe`. **Visual artifacts:** `render.bat`
writes wide, compact, 150% DPI, token and input-page PNGs to `build/`.

**Verified in this pass:** strict warning-free C17 build; 8,049 core/showcase
assertions; real Direct2D/DirectWrite tests at 96/144/192/240 DPI; forced target
release/recreation; transactional font replacement and repeated teardown; UIA
fragment navigation, metadata, bounds, focus, Invoke and stale-provider checks.
Wide, compact, 150% DPI and input-page WIC renders were inspected. The native
showcase launched and responded, and its accessibility tree was readable.

**Verification limit:** live Windows capture failed twice with
`SetIsBorderRequired: No such interface supported (0x80004002)`. WIC artifacts test
the actual painter, not the HWND edit overlay or non-client chrome. Physical
cross-monitor moves, IME composition and native caret/clipboard behavior still
need an interactive Windows check. Forced target recreation tests the recovery
path, not an actual GPU removal. No claim of screen-reader completeness.

Prioritize depth over widget count:

1. **Accessibility:** the Win32 UIA fragment provider now maps generation-safe IDs
   to names, roles, bounds, enabled/focused state and button Invoke, with focus and
   invocation events. Core labels/help text and a hidden-HWND provider smoke test
   are in place. Next add Toggle/RangeValue/Value patterns and property events,
   then explicitly bridge the native editor into the retained fragment tree.
2. **Lifetime and invalidation:** generation-safe removal/reparenting, focus/capture
   repair and callback mutation rules are now implemented and covered in the core
   tests. Keep the current bounded arena until actual scale requirements justify
   allocation policy; add insertion/reordering only when a concrete consumer needs it.
3. **Layout and content:** measured wrapping, baseline/cross-axis alignment, then a
   virtualized collection using the existing viewport contract. Preserve shared
   clip/hit geometry and scroll anchoring as content changes.
4. **Platform finishing:** live theme application, automated edit-bridge checks,
   monitor-transition testing, UIA-driven host smoke tests, and diagnostics for
   resource failures. Fonts/brushes must stay in sync when theme/DPI changes.

Design choices to retain: DIPs throughout the core; no Win32/COM in `ui/` or
`showcase/`; synchronous small events; no per-frame application reconstruction;
cached DirectWrite layouts; native editing rather than a partial Unicode editor;
captured gestures canceled on focus/capture loss; on-demand painting.
