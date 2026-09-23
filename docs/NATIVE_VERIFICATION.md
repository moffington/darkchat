# DarkChat native-product verification matrix

This is the repeatable manual pass for the native-product polish cycle
(command palette, model-palette migration, keyboard navigation, conversation
customization, prompt profiles, export/import, code-block copy, dark menus,
notifications, and IME/DPI/accessibility hardening).

Automated coverage is the first gate:

```bat
chat.bat test
```

It builds the application, runs every chat suite, and then runs the DarkUI
toolkit suite via `build.bat test`. The table below notes which automated suite
already covers each contract; the manual steps still need a real desktop
(keyboard/IME/clipboard, modal and menu appearance, and physical monitor
transitions are not reproducible in hidden windows).

## Matrix

| Area | Manual steps | Expected | Automated coverage |
|------|--------------|----------|--------------------|
| Keyboard-only | Launch and never touch the mouse: F6 cycles regions, Tab spans retained/native controls, arrows move within the sidebar, Enter opens a conversation, F2 renames, Delete deletes, Ctrl+K / Ctrl+Space open the palettes, Escape returns to the composer | A full session is possible; focus never strands on a hidden control | `test_chat_host.c` `navigation_suite`, `model_palette_suite`, `hardening_suite`; `test_ui.c` roving focus |
| Palette — commands | Ctrl+K, type a filter, arrow over disabled rows, Enter, Escape; resize/change DPI while open | Disabled rows are skipped; exactly one action fires; the owner window is re-enabled; no visual artifacts | `test_chat_host.c` `palette_suite`, `model_palette_suite`; `test_palette.c`, `test_commands.c` |
| Palette — models (parity) | Ctrl+Space with and without a key, offline, with Ollama active, during a fetch, and on rapid reopen | Same behavior as the retired picker: current + history first, in-place refresh, filter preserved, cancel safe | `test_chat_host.c` `model_palette_suite`; `test_palette.c`, `test_model_catalog*.c` |
| Palette — model presentation | 100/150/200% DPI, long ids/names, empty filter, no matches | Dark, aligned, readable, no clipped rows | `test_chat_host.c` palette DPI assertions; visual pass required for appearance |
| Conversation overrides | Set model + prompt per conversation, switch conversations, send, clear the override | The chip marks `· this chat`; the request uses the override; clearing restores the global value; other conversations are unaffected | `test_chat_host.c`, `test_chat.c`, `test_context.c`, `test_chat_ui.c`, `test_commands.c` |
| Prompt profiles | Create/edit/delete/apply a profile to the global prompt and to a conversation; test the cap | The prompt applies verbatim; delete cannot corrupt history; the cap rejects cleanly | `test_chat.c`, `test_storage.c`, `test_commands.c`, `test_chat_host.c` |
| Export — Markdown/JSON | Export the active conversation and all conversations; open the files in an editor with Unicode, code blocks, tables, and reasoning | Valid UTF-8; stable schema; accurate metadata; no secrets | `test_export.c`, `test_chat_host.c` `export_suite` |
| Import — JSON | Round-trip this app's own export; try malformed, truncated, and over-cap input | New conversations only; sanitized metadata; a clean error on bad input | `test_import.c`, `test_chat_host.c` `import_suite` |
| Import — Markdown | Round-trip the Markdown export; try plain Markdown without headers | Recognized structure; fallback to a single message; content preserved verbatim | `test_import.c`, `test_chat_host.c` `import_suite` |
| Code copy | Hover blocks (streaming, completed, with nearby tables); click the pill; Ctrl+Shift+C with and without a selection | The pill appears only over code blocks; the exact text is copied; status feedback; no selection loss | `test_chat_host.c` `code_copy_suite`; `test_markdown_win.c` ranges |
| Notifications | Long generation with the window unfocused; click the balloon; restart Explorer; toggle the preference off | One balloon with the correct title; click restores and selects; the tray survives an Explorer restart; the toggle persists | `test_chat_host.c` `notify_suite`, `test_storage.c` |
| IME | Compose Japanese/Chinese in the composer, search, and palette filter; press Ctrl+Space to toggle the IME | Composition is never sent as a message; Ctrl+Space toggles the IME (it does not open the model palette); the palette filter accepts committed characters only | `test_chat_host.c` `model_palette_suite` (IME yield + VK_PROCESSKEY); the OS IME itself is manual |
| DPI / multi-monitor | Drag across mixed-DPI monitors, maximize/restore, 200% scaling, and restore a geometry saved on a now-removed monitor | No blur or clipping; fonts and controls reflow; an off-monitor geometry restores on-screen | `test_chat_host.c` `hardening_suite` (host WM_DPICHANGED, saved-geometry predicate), palette DPI assertions; physical transitions are manual |
| Accessibility | Narrator/NVDA over the shell, sidebar, palette, transcript, and notifications; UIA Inspect | Names/roles/states are present; palette focus changes are announced; decorative elements are ignored | `test_accessibility.c`, `test_chat_host.c` palette UIA assertions; announcement quality is manual |
| Persistence v4/v5 and downgrade | Save with profiles and overrides; load the v1 fixture; simulate an older build against a v5 file | v1–v3 load and migrate; v4/v5 load correctly; an older build fails closed and preserves files | `test_storage.c`, `test_chat.c`, `tests/state-v1-fixture.jsonl` |
| Recovery | Kill the process during generation; corrupt the primary snapshot; leave a torn temporary | Existing recovery semantics hold under the v4/v5 grammar | `test_storage.c`, `test_chat_host.c` |

## Known limitations

These were consciously deferred from the hardening pass and are not regressions:

- **Palette filter IME composition.** The palette filter consumes committed
  characters only; in-progress IME composition in the filter is not supported.
  Ctrl+Space is yielded to the IME (see the IME row), and Ctrl+K remains the
  canonical palette shortcut.
- **UIA selection and announcements.** Palette rows expose names and the Invoke
  pattern and raise focus-changed events, but the `SelectionItem` pattern and
  explicit palette open/close or notification announcements are not
  implemented. High-contrast / forced-colors theming of the popup and
  transcript is not implemented. Recorded here rather than fixed.
- **Dark system menus.** The dark menu bar treatment relies on an undocumented
  uxtheme path behind a runtime capability check; on OS builds where it is
  unavailable, the native light menu is used by design. Nothing depends on the
  dark path.
- **Notifications.** Completion notifications are tray balloons only; WinRT
  toasts are not used. Balloon appearance is governed by the OS.
- **No response branching, images, syntax highlighting, concurrent generation,
  configurable Ollama endpoint, cross-machine sync, transcript search index,
  multiple windows, or drag-and-drop reordering.** See the README and
  `CHAT.md` for the full behavior contract.

## Related documents

- [`CHAT.md`](CHAT.md) — behavioral and persistence contract.
- [`../README.md`](../README.md) — user-facing overview and setup.
