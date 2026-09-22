# DarkChat

DarkChat is a native OpenRouter and Ollama chat client for Windows, written in C17 with Win32, Direct2D, DirectWrite, Rich Edit, and WinHTTP.

It provides streaming responses, per-turn reasoning views, progressive Markdown rendering, a searchable per-backend model catalog, OpenRouter provider-routing controls, durable local conversation history, and the basic lifecycle tools expected from a usable desktop chat client—without Electron, a browser runtime, or third-party libraries.

> **Status:** DarkChat is functional and under active development. It is currently a focused personal desktop client rather than a finished general-purpose release.

## Current capabilities

- Stream responses from any OpenRouter model identifier over SSE, or from a local [Ollama](https://ollama.com) server through its OpenAI-compatible API.
- Switch backends under Settings > Backend. Each backend remembers its own last-used model, and choosing Ollama with no remembered local model opens the model palette first.
- Display reasoning separately for each assistant turn.
  - Reasoning is collapsed by default.
  - Live reasoning can be opened, scrolled, collapsed, and reopened without losing its place.
  - Reasoning content and duration persist across restarts.
- Render assistant Markdown progressively while streaming.
  - Headings
  - Bold, italic, and strikethrough text
  - Inline and fenced code
  - Flat ordered and unordered lists, including task markers on unordered items
  - Links
  - Blockquotes
- Preserve transcript selection and reading position while responses stream.
- Create, rename, reopen, clear, and delete conversations.
- Keep an independent autosaved draft for each conversation.
- Retry unsuccessful responses, regenerate the latest response, or edit and resend the latest user message.
- Stop an active request while retaining its partial response.
- Copy responses, transcript selections, and composer text.
- Configure the backend, the model, the global system prompt, sidebar width, and OpenRouter provider routing.
- Browse and search the active backend's model catalog with `Ctrl+Space`; offline or without a key it falls back to the current model and that backend's 16 most recently used identifiers (history is tagged per backend, so the two model lists stay isolated).
- Apply global provider routing to future OpenRouter requests: sort by price, throughput, or latency; allow or disable fallback providers; allow or deny providers that may store data; and require Zero Data Retention. Every control defaults to OpenRouter's own default and is disabled while Ollama is active.
- Show completion metadata including:
  - Time to first token
  - Total latency
  - Input and output tokens
  - OpenRouter-reported cost (local Ollama turns show `local` instead)
  - Backend, requested and actual model
  - Unusual finish reasons
- Recover interrupted generations and valid backup snapshots after a crash or torn write.

Only one request runs at a time. You can switch conversations or create a new one while a response is generating; the response remains attached to the conversation where it began.

## Build and run

DarkChat requires Windows 10 version 1703 or newer and MinGW-w64 or w64devkit with `gcc` and `windres` available on `PATH`.

```bat
chat.bat
build\darkchat.exe
```

You can also build and launch it in one command:

```bat
chat.bat run
```

Build output is written to `build\`. Compilation uses strict C17 warnings with `-Wall -Wextra -Wpedantic -Werror`.

There is no C++ runtime and no third-party dependency to install.

## OpenRouter setup

DarkChat reads the API key from an environment variable named:

```text
OPENROUTER_API_KEY
```

For the current PowerShell session:

```powershell
$env:OPENROUTER_API_KEY = "your-key-here"
.\build\darkchat.exe
```

You can instead create `OPENROUTER_API_KEY` as a Windows User environment variable. DarkChat also checks the corresponding User environment registry value, allowing an already-running desktop session to see a newly configured key.

The key is never written to conversation state or included in persisted history, and temporary key buffers are cleared before release.

Enter any valid OpenRouter model identifier in the model field, or press `Ctrl+Space` to search OpenRouter's model catalog. The catalog is fetched on demand with the same key, cached in memory for one hour, and never persisted; offline or without a key, the model palette falls back to the current model and recent history. Provider routing is configured under Settings > Provider routing.

## Ollama setup

Ollama is first-class through its **OpenAI-compatible** API, not its native `/api/chat`:

```text
http://localhost:11434/v1/chat/completions
http://localhost:11434/v1/models
```

Start the server (for example `ollama serve`), then choose Settings > Backend > Ollama. No API key, `Authorization` header, HTTP-Referer, X-Title, TLS, or OpenRouter provider object is used; DarkChat opens a direct, no-proxy WinHTTP session for localhost. A missing `OPENROUTER_API_KEY` never blocks Ollama.

The first time you switch to Ollama with no remembered local model, the model palette opens in Ollama mode and the switch commits only after you select a model. Ollama generation metadata shows `Ollama · model`, uses streamed usage via `stream_options.include_usage`, and reports its cost as `local`. If the server is not running, the failure reads `Ollama is not reachable at localhost:11434.`

The Ollama endpoint is fixed at `localhost:11434`; there is no configurable endpoint.

## Everyday controls

- `Enter` sends a message.
- `Shift+Enter` inserts a newline.
- `Ctrl+Space` opens the searchable model palette for the active backend (current model, recent models, then the backend's catalog), and `Ctrl+K` opens the command palette. Both are the same retained dark popup: type to filter (by id or name for models), use Up/Down/PageUp/PageDown/Home/End, then Enter or a single click to select; Escape cancels.
- `Ctrl+F` focuses conversation search. Enter refreshes the search and jumps to
  its first result; `F3` / `Shift+F3` move through message and reasoning hits.
- The Send button becomes Stop during generation.
- Conversation actions provide New, Rename, Delete, and Clear messages.
- Response actions provide Retry, Regenerate, Edit latest user message, Cancel edit, Copy response, and Copy transcript selection.
- DarkChat keeps an icon in the notification area. A response that takes at least five seconds and finishes while the window is not in the foreground raises one balloon naming the conversation and the terminal state (complete, failed, or interrupted); clicking it, or left-clicking the tray icon, restores the window and selects that conversation. Right-clicking the tray icon opens the same command menu as the header's overflow button, anchored at the pointer. Explorer restarts re-add the icon automatically. **Settings > Notify when long responses finish** turns the balloons off (the tray icon stays).
- The **Customization** group in the overflow menu carries the per-conversation model and prompt overrides and the prompt-profile library, described below. Its override commands and **Save current prompt as profile…** also appear in the `Ctrl+K` command palette under their own section; **Apply prompt profile**, **Edit profile…**, and **Delete profile…** stay in the overflow menu only, because they pick a profile from a submenu.

Changing the system prompt affects future requests only. It does not rewrite existing history.

Retry, regenerate, and edit-and-resend replace the latest response rather than creating branches or retaining response variants.

### Per-conversation model and prompt (Customization)

By default every conversation uses the global settings: the model slot of the
active backend and the system prompt. The Customization group in the overflow
menu (and the same commands in the Ctrl+K palette) overrides either for the
active conversation only:

- **Use model for this chat** copies the current global model into this
  conversation's override; **Clear conversation model** removes it. While the
  field and palette always show and edit the *effective* model, the model chip
  carries a `· this chat` marker whenever the conversation overrides the model
  — even when the override happens to equal the global model, because edits
  still target this conversation until it is cleared. Switching conversations
  switches the effective model with it.
- **System prompt for this conversation…** replaces the global prompt for this
  chat only. Submitting an empty field is an explicit *no-prompt* override —
  this conversation then sends no system message at all, which is distinct
  from inheriting a nonempty global prompt. Only **Use global system prompt**
  removes the override and restores inheritance.
- **Prompt profiles** are named prompts saved in a small library. Save the
  current effective prompt as a profile, apply a profile either to the global
  prompt or to the active conversation, and edit or delete profiles through
  the same submenus (each lists one item per saved profile). Applying an empty
  profile to a conversation is the explicit "no system prompt in this chat"
  override; the profile library survives Delete-all-conversations and is only
  released when the app exits.

Overrides are saved with the conversation: a restart, a message Clear, or
switching between conversations never loses them.

## Local persistence

DarkChat stores its state at:

```text
%LOCALAPPDATA%\DarkChat\state.jsonl
```

The snapshot contains conversations, messages, drafts, model history, generation metadata (including the originating backend), settings (the active backend, one last-used model per backend, and provider routing), prompt profiles, per-conversation model and system-prompt overrides, and window geometry. It does not contain the OpenRouter API key.

Persistence uses a checksummed UTF-8 JSONL format with:

- Atomic same-volume replacement
- Write-through flushing
- A validated backup snapshot
- Recovery from complete temporary snapshots
- Strict validation before loaded data is adopted
- An exclusive directory lock to prevent two instances from overwriting one another

Current writes use format 3 for an entirely uncustomized store, format 4 once
any prompt profile or per-conversation override exists, and format 5 once a
conversation carries the deliberately-empty prompt override. DarkChat loads
formats 1 through 5 and rewrites older valid snapshots in the current format
on the next save; an unsupported newer format fails closed without
overwriting it from a backup. The active backend, the per-backend models, and
each generation's backend are additive optional fields: an older snapshot
decodes as OpenRouter with no Ollama model. The completion-notification
preference (`notify_disabled`) is additive at the same version and is written
only when notifications are turned off, so an enabled store keeps the older
byte shape. Customization (prompt profiles,
per-conversation overrides, and the explicitly-empty prompt override) is
version-gated in both directions, so an older binary can never silently
erase it.

State is autosaved roughly once per second while dirty and immediately after important lifecycle actions such as sending, stopping, completing, deleting, or closing.

A running assistant message is persisted before the network request begins. If the application closes or crashes during generation, the partial response is retained and recovered as Interrupted rather than silently marked successful.

## Architecture

DarkChat now represents most of the active development in this repository.

| Area | Main files | Responsibility |
| --- | --- | --- |
| Chat model | `chat/core/chat.*` | Conversations, messages, request lifecycle, IDs, metadata, drafts, retry/regenerate/edit behavior |
| Windows host | `chat/shell/chat_host_win32.*` | Application window, worker coordination, timers, persistence scheduling, native event routing |
| Application UI | `chat/shell/chat_ui.*`, `chat/shell/actions_win32.*` | Sidebar, composer, menus, settings, commands, and visible application state |
| Transcript | `chat/transcript/transcript_win32.*`, `chat/transcript/rich_text_win32.*` | Bounded, recycled native Rich Edit slots; scrolling, selection preservation, reasoning viewports, incremental updates |
| Markdown | `chat/transcript/markdown.*` | Transactional, platform-independent Markdown subset parser |
| Completion | `chat/generation/completion_request.*`, `chat/generation/completion_winhttp.*`, `chat/generation/sse.*`, `chat/json.*` | Backend-aware request encoding, endpoint descriptors, WinHTTP streaming, SSE framing, response decoding for OpenRouter and Ollama |
| Model catalog | `chat/models/model_catalog.*`, `chat/models/model_catalog_winhttp.*`, `chat/shell/palette_win32.*` | Transient per-backend model-catalog fetch, parse/merge/filter, and the shared command/model palette |
| Provider routing | `chat/generation/provider_routing.*` | OpenRouter `provider` object construction, sharing exact bytes with the request-context budget |
| Storage | `chat/persistence/storage.*` | Checksummed JSONL snapshots, atomic replacement, backup and recovery |
| DarkUI foundation | `ui/*`, `platform/*` | Retained controls, theme, painting, layout, Direct2D/DirectWrite rendering, and accessibility infrastructure |

### Relationship to DarkUI

This repository began as **DarkUI**, a small retained-mode UI foundation and control showcase. DarkChat was built on top of that work and still uses its theme, retained controls, layout, renderer, and platform infrastructure for parts of the application shell.

DarkChat has since grown substantially beyond the original showcase. Its transcript, streaming protocol, reasoning surfaces, Markdown renderer, persistence system, lifecycle model, and native Windows integration currently live in chat-specific modules.

The original toolkit showcase remains available as:

```bat
build.bat
build\darkui.exe
```

The long-term direction is to move generally useful pieces back into the shared DarkUI foundation as the application’s interfaces mature. For now, the repository should be understood primarily as the DarkChat application with DarkUI underneath it—not as a completed general-purpose UI toolkit.

## Verification

Run the complete regression suite with:

```bat
chat.bat test
```

This builds DarkChat, runs its chat and hidden-window integration tests, and then runs the DarkUI toolkit suite through `build.bat test`.

Coverage includes:

- Conversation and message lifecycle transitions
- Retry, regenerate, edit-and-resend, cancellation, and crash recovery
- Dynamic message allocation and injected allocation failures
- Unicode, JSON, SSE, and split-boundary parsing
- OpenRouter and Ollama request and response fixtures, including byte-for-byte
  OpenRouter bodies, Ollama request bytes with no credentials or provider
  routing, and context/body size equality for both backends
- Model-catalog parsing and model-list merge/filter per backend, and offline or keyless OpenRouter fallback
- Provider-routing serialization, request-context budget accounting, persistence round-trips, and the settings-to-request seam
- Backend and per-backend model persistence, old-snapshot OpenRouter defaults, and routing-menu state under Ollama
- Reasoning ownership and live reasoning viewports
- Progressive Markdown rendering
- Selection-preserving transcript updates
- Scheduled streaming flushes
- Bounded transcript realization, slot recycling, reader-state restoration, and native-control limits
- Persistence round trips, checksums, backup recovery, and failed writes
- Direct2D/DirectWrite renderer integration
- Retained UI layout, input, scrolling, and UI Automation behavior

Tests treat warnings as errors and propagate a nonzero exit code on failure. They use isolated directories under `build\` and do not touch the user’s conversation store.

A real OpenRouter request can be checked manually after building the tests:

```powershell
$env:OPENROUTER_API_KEY = [Environment]::GetEnvironmentVariable(
    'OPENROUTER_API_KEY',
    'User'
)
.\build\test_openrouter.exe --live
Remove-Item Env:OPENROUTER_API_KEY
```

A live local Ollama request is also available when a server is running and a
model is named; it is never required for the normal suite:

```powershell
$env:DARKCHAT_OLLAMA_MODEL = "llama3.2"
.\build\test_openrouter.exe --live
Remove-Item Env:DARKCHAT_OLLAMA_MODEL
```

The automated suite does not require an API key or a local Ollama server, and
does not make live network requests.

## Current limits

- Windows only
- One active request at a time
- 128 conversations
- 512 messages per conversation
- Message text and reasoning use 255-code-unit inline residues, then heap storage;
  they have no fixed per-message length cap
- 128 MB maximum persisted snapshot
- 16 recently used model identifiers (the offline model-list fallback)
- Provider routing exposes sorting, fallback, data-collection, and ZDR controls only; per-provider `only`/`ignore`/`order` selection is not exposed, and the controls apply to OpenRouter only
- Ollama is reached at the fixed OpenAI-compatible endpoint `localhost:11434/v1`; the endpoint is not configurable
- No response branches or retained variants
- Conversation search scans current in-memory messages on demand; there is no
  persisted or background index
- Markdown images, indented code blocks, and other unsupported syntax remain
  literal; lists and blockquotes nest up to eight levels, with deeper or ambiguous
  prefixes kept verbatim. GFM tables render in-body with a 48-DIP minimum column
  and 16-DIP gutter, root-level only and capped at 24 columns; a table that cannot
  fit, or a document-wide plan failure, falls back to literal source

An allocation failure while receiving a response retains the partial response and
marks it Interrupted. The 128 MB snapshot limit is a serialized-file limit, not
a total in-memory-content limit.

Interactive clipboard and IME behavior, modal appearance, physical multi-monitor DPI transitions, and live provider behavior still require manual desktop verification. Hidden-window tests cover the underlying contracts but are not a replacement for visual review.

For the detailed behavioral and persistence contract, see [`docs/CHAT.md`](docs/CHAT.md).

