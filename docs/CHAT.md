# DarkChat — verified chat contract

Build with `chat.bat`; run `build\darkchat.exe`. `chat.bat test` is the single
reproducible verification command: it builds the app, runs all chat tests
(including the transcript-isolation, scheduled-flush and message-growth-boundary
regressions), then runs the unchanged DarkUI toolkit suite via `build.bat test`.
No third-party dependencies are required (C17, MinGW-w64, Win32).

## Daily use

- The sidebar creates and reopens conversations. Conversation menu: New, Rename,
  Delete, Clear messages. Rename survives subsequent edits and clearing. Delete
  and Clear ask for confirmation. Deleting the final conversation creates a new
  empty one with a new ID.
- Enter sends; Shift+Enter inserts a newline. Send becomes Stop while generating.
  New/open remains available during generation; the reply stays with its origin.
  History mutations and settings changes are blocked until the worker finishes.
- Response menu: Retry unsuccessful response, Regenerate last response, Edit
  latest user message, Cancel edit, Copy response, Copy transcript selection.
  Native Ctrl+C also copies selected transcript or composer text.
- Edit loads the latest user message into the composer without changing history.
  Sending replaces that user message and its response. Cancel edit or switching
  conversations abandons the edit and restores the ordinary saved draft. Editing
  text is not autosaved until sent; ordinary per-conversation drafts are autosaved.
- Settings menu edits the global system prompt and sidebar width (160–360 DIPs).
  The system prompt is prepended to future requests; changing it does not rewrite
  old messages. The model field accepts any OpenRouter identifier.
- Ctrl+Space opens model history, filtered by the field's prefix. If no entry
  matches, it shows all history. History retains the 16 most recently requested
  models; it does not fetch OpenRouter's full catalog.
- Each completed response displays a compact metadata footer: completion state,
  TTFT and latency in seconds, grouped input/output token counts, USD cost and
  the model (shown once when requested and actual match, otherwise
  requested → actual). Normal `stop` is omitted and only unusual finish reasons
  are surfaced. The status line shows the active request's model, state and
  elapsed time.
- Reasoning-capable models stream reasoning separately from the answer. Every
  assistant turn owns its own reasoning row: a subtle **Thinking… ⌄** row appears
  while a request waits or reasons, and reads **⌄ Thought for X.Xs** once it
  ends. Rows are never auto-expanded; clicking the whole row expands only that
  turn into an inset, tinted ~150 DIP read-only viewport with its own scrollbar
  that follows the live stream only while the reader stays pinned to its bottom,
  and clicking again collapses it. Reasoning streams into the turn's buffer even
  while collapsed, so opening a live turn shows what has arrived so far. If no
  reasoning ever arrives, the temporary row is removed once answer text begins.
  Each turn's reasoning and duration are independent and persist; expansion is
  per-session view state. Reasoning is never invented locally.

## Transcript rendering

The transcript is a native scroll container over per-turn layout records. Each
turn owns its Rich Edit surfaces: an assistant turn can own a
header/reasoning-row block, an optional reasoning viewport, an answer block and a
terminal metadata footer; other turns use a single block. A turn's surfaces are
created once when first needed and then retained for the life of the process:
the container measures, positions and shows only the turns that intersect the
viewport and hides the rest, so off-screen controls are not destroyed or
recycled. An unchanged historical surface keeps its selection because its
content write is skipped; an expanded reasoning viewport keeps its own inner
scroll position because it is never rebuilt while live. The container owns the
one outer scroll for the whole
conversation and repositions each realized turn's controls as it scrolls. An
expanded reasoning viewport is inset, tinted
(`UI_PANEL`) and separated by an 8 DIP gap above and below, so it reads as its
own sub-panel rather than part of the answer, and it scrolls independently: the
wheel scrolls the viewport first and chains to the transcript at its ends, so
the target never depends on which control holds focus. No control is shared
between turns, so historical assistant turns keep their own reasoning affordance
and content across switching, reload and restart.

Update bookkeeping lives in its own module (`chat/transcript_win32.c`). Every
turn records the message identity its surfaces were built from â€” conversation
id, message instance id, a per-message revision counter and the observable
rendering state. When a rebuild finds a message unchanged, destructive content
writes are skipped entirely, so unchanged historical controls survive sibling
completions, layout changes and window resizes with their selections intact.
The skip covers destructive writes only: control realization, callback wiring,
visibility reconciliation, width-dependent measurement, positioning and DPI
work always run. A destructive rewrite of a surface that currently holds a
selection is deferred until the selection clears (detected through
`EN_SELCHANGE` plus an idle sweep); while it is deferred, new deltas keep
accumulating in the message, so nothing is lost. Switching conversations or
reusing a turn
slot invalidates the affected surfaces and forces immediate replacement without
deferral, so deferred or selected content never carries across conversations. A
reused reasoning viewport whose stored identity no longer matches its message is
reloaded even while its turn is streaming, so live reasoning from one
conversation can never stay visible under another or receive that
conversation's appends.

Streaming answer deltas accumulate in the message; the active body is rebuilt
as Markdown at most once every ~100 ms (`CHAT_BODY_RENDER_MS`). The first token
renders immediately; a delta inside the window that leaves the body dirty arms a
one-shot host flush, so a burst that then pauses still renders on its own rather
than waiting for another delta to cross the interval, and the terminal event
always flushes a final render. Token bursts therefore never reparse per token.
A scheduled flush that fires while the body holds a selection is deferred
through the pending-write path instead of destroying the range. Incomplete
syntax renders literally while it streams. A rebuild relayouts only from the
streaming turn and follows the transcript scroll only while the reader stays
pinned to the bottom. Terminal metadata updates its own surface without
rewriting the body, and unchanged child windows are not repositioned or shown
again.

Metadata is a terminal-state footer only: a compact muted line
(`Complete · TTFT 18.0s · 19.4s · 44 in / 1,365 out · $0.00165`) with the model
on its own line, shown once when the requested and actual models match and as
`requested → actual` when they differ. Normal `stop` is omitted; unusual finish
reasons are surfaced. A running turn never mixes stats into the streaming answer.

### Markdown rendering

Terminal assistant output is Markdown-rendered (`chat/markdown.c`, a pure
parser with its own test suite); while a response streams, the visible body is
rebuilt from the accumulated message at most once every ~100 ms (a scheduled flush
renders a paused burst), incomplete
syntax stays literal, and the terminal event flushes the final render. User,
system and error
bodies are fully literal, fence markers included, and reasoning is never
reformatted. The supported subset: headings 1–3 (require a space after the
marker), `**bold**`, `*italic*`/`_italic_` (word-internal underscores and
asterisks stay literal), inline and fenced code (fence lines and language tags
hidden, monospace on the code tint), flat lists (textual `• ` bullets and
preserved ordered markers), `[label](http(s)://…)` links rendered as
`label (url)` so the native URL detector opens them, and blockquotes (muted,
bar-prefixed). Precedence is fences → inline code → links → emphasis; escapes
(`\*`) keep punctuation literal; malformed or unsupported syntax (tables,
images, nested lists, deeper rules) is preserved verbatim. Every Rich Edit run
sets bold, italic, face, size and background explicitly, so code tinting cannot
bleed into following text, and identical adjacent runs coalesce. The parser is
transactional and O(input)-memory: on allocation failure the body falls back
to verbatim text. Rebuilds of completed turns may reparse the message; no
render cache is kept.

The key is read from `OPENROUTER_API_KEY` in the process environment, falling back
to the Windows User environment registry value (so an existing desktop session
can pick up a newly configured key). Key buffers are cleared before release.
The key is never part of conversation state or persistence.

## Lifecycle and history contract

One request runs at a time. The UI thread owns Chat; the worker receives an owned
request snapshot and posts generation-ID-tagged events. A user turn and empty
running assistant response are saved **before** sending the network request.
Deltas update that assistant response in place. Late events are discarded.

| State | Meaning |
| --- | --- |
| Complete | `[DONE]` received, nonempty text, no provider error. Provider `length` remains Complete with its finish reason visible. |
| Cancelled | User requested Stop, including when a queued DONE races with Stop. Partial text is retained. |
| Interrupted | Window closed, a running response was recovered after a crash, connection ended during a response, `[DONE]` was missing, or the local text limit was reached. |
| Failed | Request could not start, API/provider/decoding error, missing terminal event, or completed without text. Partial text is retained. |

Retry is available for the latest failed/cancelled/interrupted response (or a user
turn without a response). Regenerate replaces the latest user turn's response,
including a successful response. Both keep exactly one copy of the user turn.
Edit-and-resend changes the latest user text and replaces its response. These
operations use the current model/system prompt and reset generation metadata.
Prior response variants are deliberately not retained; there are no branches.

Request history includes user/system messages and completed generated assistant
responses. Local welcome/error notes, running responses and all unsuccessful
assistant responses are excluded. Persisted metadata and error descriptions are
never sent as conversation text.

## Persistence format and recovery

`%LOCALAPPDATA%\DarkChat\state.jsonl` is a UTF-8, version-1 JSONL snapshot:

1. A settings record with `type: "settings"`, `version: 1`, selected conversation
   index, next ID counter, record counts, model/system prompt and geometry.
2. Zero or more `type: "model"` history records.
3. For each conversation, a `type: "conversation"` record followed by its declared
   number of `type: "message"` records.
4. A `type: "commit"` record containing the 32-bit FNV-1a checksum of every byte
   before that record (including LF separators).

Conversation IDs are numeric, store-local, monotonically allocated from a
persisted counter initially seeded from the current time. They do not depend on
names or array positions and are not reused after deletion. Conversation/message
created and modified timestamps and generation started/first-token/finished
timestamps are Unix milliseconds (currently second-resolution wall clock).
TTFT and latency use monotonic millisecond timing. A zero generation timestamp
means unknown; numeric metadata uses -1 for unavailable. Cost is supplied by
OpenRouter, never estimated locally. A crash-recovered response has no invented
finish timestamp or latency. Window size/sidebar width use DIPs; position uses
Windows workspace coordinates, with off-monitor fallback on restore. Any
reasoning a provider supplied is persisted per message alongside its answer,
with its duration; the optional fields are appended last so a version 1 snapshot
without reasoning still loads.

Writes serialize explicit fields, flush `state.tmp.jsonl`, then atomically replace
`state.jsonl` on the same volume using `MoveFileExW` with write-through. Before
replacement, the last validated primary is copied and flushed to
`state.bak.jsonl`. A failed write/rename leaves the primary intact and reports a
save error. Autosave runs once per second while state/drafts/settings are dirty,
and immediately on history actions, request start, Stop, completion and close.
At most roughly one second of recent streamed text or ordinary draft typing can
be lost on abrupt termination (long UI/disk stalls can increase that interval).

Load validates JSON, version, ranges, counts, identities and checksum into a
separate Chat before adopting it. It prefers the valid primary, then backup,
then a complete temporary snapshot; a recovered temporary file is preserved as
a backup before reuse. A corrupt primary is never rotated over a valid recovery
backup. Unsupported versions, or no valid snapshot when files exist, prevent
startup and preserve the files for manual recovery. A directory lock prevents
concurrent app instances from overwriting each other. Hardware/filesystem failure
cannot be made lossless; this is a local snapshot/backup scheme, not archival
storage or a cross-machine synchronization format.

## Verification

`chat.bat test` runs the chat/UI/Unicode/SSE split-boundary tests listed below,
then the DarkUI toolkit suite (`build.bat test`). It includes:

- Lifecycle state transitions, retry/regenerate/edit replacement, full-capacity
  retries, stable IDs after rename/clear/delete, and bounded model history.
- JSON number/structure validation, optional usage values and fractional cost.
- Storage round trips, Unicode/drafts/settings/metadata, exclusive writer lock,
  corrupt/torn snapshots, backup/temp recovery, denied temp writes and failed
  atomic replacement, and protection against unknown versions.
- Real request encoder/SSE callback fixtures for model, usage, cost, TTFT,
  finish reason and provider errors after partial content, now also the enabled
  reasoning request parameter and reasoning_details/text-summary/plain fallback
  parsing that never fabricates reasoning.
- Per-turn reasoning ownership (hidden HWND host): two assistant turns keep
  independent rows and viewports; expanding one does not affect another;
  collapsed by default while streaming; explicit expansion streams live; a
  viewport the reader scrolled up is not force-followed; answer start does not
  collapse a user-opened viewport; answer start removes the row when no reasoning
  was supplied; conversation switching and reload restore each message's
  reasoning; retry/regenerate cannot leak reasoning or expansion; cancellation
  and error paths stay coherent.
- Compact metadata footer: grouped token counts, a deduplicated model line
  (shown once when requested and actual match, else requested → actual), normal
  `stop` omitted and unusual finish reasons surfaced. A running turn's answer
  never contains stats, and no footer exists until the turn is terminal.
- Storage round-trips reasoning and its duration and still loads a version 1
  snapshot whose message lines omit the optional fields.
- Hidden native HWND host integration: stale events, switching during generation,
  partial failures, cancel/DONE races, empty replies, edit-and-resend draft
  preservation, inline reasoning row/surface transitions, reasoning persistence,
  and close/reopen interruption recovery.
- Per-message revisions keep transcript updates isolated from reader state:
  unchanged historical controls survive completion and layout changes, active
  selections are preserved, destructive reformatting of selected text is
  deferred until the selection clears, and switching conversations replaces
  content immediately. Streaming reasoning appends preserve a selection.
- A maximum-length 64-message transcript renders with a bounded, stable number of
  native controls: re-rendering and streaming one turn realize no additional
  controls, off-screen controls are hidden rather than recycled, and an unchanged
  historical turn keeps its content and selection through the stream, the
  scheduled flush and the terminal render. Render time and control counts are
  recorded by the test, not asserted as a wall-clock threshold.
- A scheduled flush renders a burst that then pauses without another delta,
  defers (without destroying the range) when the body holds a selection and
  applies it when the range clears, does not force-follow a reader scrolled up
  in an older turn (including through completion), and falls back to an
  immediate render when the timer cannot be armed so dirty text is never
  stranded. Live reasoning can be collapsed and reopened mid-stream: reopening
  reloads the accumulation and the stream resumes appending into that turn's
  viewport, and switching A → B → A while both reasoning viewports are open and
  A keeps streaming while hidden reloads A's own reasoning instead of showing
  or appending to B's.

Manual live verification (uses the actual WinHTTP client; never prints the key):

```powershell
# Run chat.bat test first to build the test executable.
$env:OPENROUTER_API_KEY = [Environment]::GetEnvironmentVariable('OPENROUTER_API_KEY', 'User')
.\build\test_openrouter.exe --live
Remove-Item Env:OPENROUTER_API_KEY
```

Reproducible result: `chat.bat test` exits zero, running the chat suite above and
the DarkUI toolkit suite. The per-turn inline reasoning architecture (its own row
and viewport per assistant turn, live streaming, follow/pin behavior, no-reasoning
removal, and version 1 persistence compatibility) and the compact metadata footer
are covered by focused hidden-HWND tests. Live OpenRouter behavior is not asserted
by any repo command; it is the manual, key-gated check shown above. Tests use
isolated directories under `build`, not the user's conversation store.

Remaining limits: 16 conversations, 64 messages each, 16,383 UTF-16 code units per
message (answer and reasoning each), and a 128 MB on-disk snapshot bound. Responses
past the local limit stop as Interrupted without silently claiming success. Full
model-catalog autocomplete, response variants, and global search remain outside
this pass. Interactive clipboard/IME behavior,
modal-dialog appearance and physical multi-monitor DPI transitions still need a
manual desktop check; hidden-HWND tests do not substitute for that visual review.
