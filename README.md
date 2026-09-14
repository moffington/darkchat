# DarkChat

DarkChat is a native OpenRouter chat client for Windows, written in C17 with Win32, Direct2D, DirectWrite, Rich Edit, and WinHTTP.

It provides streaming responses, per-turn reasoning views, progressive Markdown rendering, durable local conversation history, and the basic lifecycle tools expected from a usable desktop chat client—without Electron, a browser runtime, or third-party libraries.

> **Status:** DarkChat is functional and under active development. It is currently a focused personal desktop client rather than a finished general-purpose release.

## Current capabilities

- Stream responses from any OpenRouter model identifier over SSE.
- Display reasoning separately for each assistant turn.
  - Reasoning is collapsed by default.
  - Live reasoning can be opened, scrolled, collapsed, and reopened without losing its place.
  - Reasoning content and duration persist across restarts.
- Render assistant Markdown progressively while streaming.
  - Headings
  - Bold and italic text
  - Inline and fenced code
  - Flat ordered and unordered lists
  - Links
  - Blockquotes
- Preserve transcript selection and reading position while responses stream.
- Create, rename, reopen, clear, and delete conversations.
- Keep an independent autosaved draft for each conversation.
- Retry unsuccessful responses, regenerate the latest response, or edit and resend the latest user message.
- Stop an active request while retaining its partial response.
- Copy responses, transcript selections, and composer text.
- Configure the model, global system prompt, and sidebar width.
- Recall the 16 most recently used model identifiers with `Ctrl+Space`.
- Show completion metadata including:
  - Time to first token
  - Total latency
  - Input and output tokens
  - OpenRouter-reported cost
  - Requested and actual model
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

Enter any valid OpenRouter model identifier in the model field. DarkChat currently does not download OpenRouter’s model catalog or expose provider-routing controls.

## Everyday controls

- `Enter` sends a message.
- `Shift+Enter` inserts a newline.
- `Ctrl+Space` opens recently used model identifiers.
- `Ctrl+F` focuses conversation search. Enter refreshes the search and jumps to
  its first result; `F3` / `Shift+F3` move through message and reasoning hits.
- The Send button becomes Stop during generation.
- Conversation actions provide New, Rename, Delete, and Clear messages.
- Response actions provide Retry, Regenerate, Edit latest user message, Cancel edit, Copy response, and Copy transcript selection.

Changing the system prompt affects future requests only. It does not rewrite existing history.

Retry, regenerate, and edit-and-resend replace the latest response rather than creating branches or retaining response variants.

## Local persistence

DarkChat stores its state at:

```text
%LOCALAPPDATA%\DarkChat\state.jsonl
```

The snapshot contains conversations, messages, drafts, model history, generation metadata, settings, and window geometry. It does not contain the OpenRouter API key.

Persistence uses a checksummed UTF-8 JSONL format with:

- Atomic same-volume replacement
- Write-through flushing
- A validated backup snapshot
- Recovery from complete temporary snapshots
- Strict validation before loaded data is adopted
- An exclusive directory lock to prevent two instances from overwriting one another

State is autosaved roughly once per second while dirty and immediately after important lifecycle actions such as sending, stopping, completing, deleting, or closing.

A running assistant message is persisted before the network request begins. If the application closes or crashes during generation, the partial response is retained and recovered as Interrupted rather than silently marked successful.

## Architecture

DarkChat now represents most of the active development in this repository.

| Area | Main files | Responsibility |
| --- | --- | --- |
| Chat model | `chat/chat.*` | Conversations, messages, request lifecycle, IDs, metadata, drafts, retry/regenerate/edit behavior |
| Windows host | `chat/chat_host_win32.*` | Application window, worker coordination, timers, persistence scheduling, native event routing |
| Application UI | `chat/chat_ui.*`, `chat/actions_win32.*` | Sidebar, composer, menus, settings, commands, and visible application state |
| Transcript | `chat/transcript_win32.*`, `chat/rich_text_win32.*` | Per-turn native controls, scrolling, selection preservation, reasoning viewports, incremental updates |
| Markdown | `chat/markdown.*` | Transactional, platform-independent Markdown subset parser |
| OpenRouter | `chat/openrouter_winhttp.*`, `chat/sse.*`, `chat/json.*` | Request encoding, WinHTTP streaming, SSE framing, response decoding |
| Storage | `chat/storage.*` | Checksummed JSONL snapshots, atomic replacement, backup and recovery |
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
- OpenRouter request and response fixtures
- Reasoning ownership and live reasoning viewports
- Progressive Markdown rendering
- Selection-preserving transcript updates
- Scheduled streaming flushes
- Long-transcript control stability
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

The automated suite does not require an API key or make live OpenRouter requests.

## Current limits

- Windows only
- One active request at a time
- 16 conversations
- 64 messages per conversation
- 16,383 UTF-16 code units each for answer and reasoning text
- 128 MB maximum persisted snapshot
- 16 recently used model identifiers
- No full model-catalog browser
- No provider-routing UI
- No response branches or retained variants
- Conversation search scans current in-memory messages on demand; there is no
  persisted or background index
- Markdown tables, images, nested lists, and other unsupported syntax remain literal

Responses that reach the local text limit are retained and marked Interrupted rather than incorrectly reported as complete.

Interactive clipboard and IME behavior, modal appearance, physical multi-monitor DPI transitions, and live provider behavior still require manual desktop verification. Hidden-window tests cover the underlying contracts but are not a replacement for visual review.

For the detailed behavioral and persistence contract, see [`docs/CHAT.md`](docs/CHAT.md).
