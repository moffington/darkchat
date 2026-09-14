# DarkChat — verified chat contract

Build with `chat.bat`; run `build\darkchat.exe`. `chat.bat test` is the single
reproducible verification command: it builds the app, runs all chat tests
(including the transcript-isolation, scheduled-flush, message-growth-boundary and
request-context regressions), then runs the unchanged DarkUI toolkit suite via
`build.bat test`.
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

### Message storage model

Each conversation holds its messages in one dynamic array, not a fixed inline
array: `messages`, `message_count` and `message_capacity` with the invariants
`0 <= message_count <= message_capacity <= CHAT_MAX_MESSAGES` (still 64) and
`messages == NULL` exactly when capacity is zero. A never-used conversation
allocates nothing; the first message reserves a small initial budget and
growth doubles up to the hard cap transactionally (a failed growth leaves the
original allocation valid). Only `[0, message_count)` is live; the rest of the
array is private backing storage that no caller inspects or serializes. Trims
(retry, regenerate, edit-and-resend) reduce the live count and keep the
allocation for reuse; Clear releases it entirely. Appends are transactional:
the message is built in the still-unused scratch slot and committed (id,
timestamps, count, title) only after every fallible step succeeded, so a
failed append consumes no ID and changes nothing observable. Replacement
operations preflight the **final post-operation message shape** and reserve
capacity for it before trimming or editing, so replacing the tail works even
at the 64-message cap and an allocation failure can never leave a partially
applied send/retry/regenerate/edit. Because growth reallocs, element pointers
are not stable across appending operations: identity is re-derived from the
conversation plus index or stable message ID (the transcript already stores
rendered identity by value). Deleting a conversation bytewise-moves the
surviving ones, transferring ownership of their arrays and overflow pointers,
and the vacated slot is zeroed so nothing is freed twice. Chat and its message
arrays remain UI-thread-owned; worker threads never borrow them.

Request history includes user/system messages and completed generated assistant
responses. Local welcome/error notes, running responses and all unsuccessful
assistant responses are excluded. Persisted metadata and error descriptions are
never sent as conversation text.

### Request context budget

Persisted history and the payload sent to OpenRouter are separate things. A
request carries a **bounded projection** of the conversation (`chat/context.c`, a
pure, allocation-free module): the system prompt when set, the triggering user
message, and the newest eligible history messages that fit
`CHAT_CONTEXT_BUDGET_BYTES` (64 KiB). That constant is a deliberately
conservative product policy, not a token-window guarantee: DarkChat never fetches
a model's real context length or counts tokens, so a provider can still reject a
request that fits the budget. Persisted history is never changed by a request:
nothing is deleted, summarised or rewritten, and an omission is never fabricated
into the transcript or the payload.

- Eligibility is exactly `chat_history_message`: no error notes, no local welcome
  text and no assistant message that did not finish successfully. Eligible
  messages are sent whole, including empty ones, and are never truncated.
- The system prompt and the triggering user message are indispensable. Eligible
  history is scanned newest first and kept while it fits; the first message that
  does not fit stops the scan, so only the oldest messages are dropped, the kept
  ones always stay contiguous, and messages older than that point are counted
  rather than measured. Message text is not globally capped (long replies live in
  overflow storage), so a very large dropped history costs a walk of roles and
  generation states, not of its text.
- The budget counts the **encoded request body in bytes**, measured with the
  encoder's own escape rules (`json_encoded_string_size`, shared with
  `json_buf_append_json_string`), so the size the drop decision is made on is the
  size actually sent; a test proves equality against the real request encoder.
- If the indispensable messages cannot fit, the request is not sent at all: the
  pending turn is marked Failed with the cause (system prompt, this message, or
  the two together) and the complete body size they would have needed
  (`required_bytes`), and Retry fails identically until the text or the system
  prompt is shortened. Nothing is silently truncated. A save failure outranks
  every context diagnostic.
- When a request starts with history omitted, the count appears in the status
  line (`Generating | model | 3.1 s elapsed | 5 older messages omitted`) and
  keeps appearing for the life of the request: it is request-scoped host state,
  not a message.

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
separate Chat before adopting it. Decoding reserves backing storage for each
conversation's declared message count, but a slot counts as live only once it
has been zeroed and construction has begun, so the quarantine disposes exactly
the messages it built and a decode failure can never leave partially decoded
or uninitialized state. It prefers the valid primary, then backup,
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
- Dynamic message storage: transactional doubling growth bounded by the
  64-message cap, allocation invariants after every operation, append and
  send/retry/regenerate/edit-resend transactional failure semantics under
  injected allocation failures, retained-capacity trims, a version 1 fixture
  from the previous build loading (and re-encoding) byte-identically, and
  decode allocation failures (including the conversation-array reservation and
  a mid-message failure under a poisoned, non-zero-filled allocator) recovered
  through the fallback or leaving the destination's previous content and the
  store untouched, plus ownership transfer across mid-list conversation
  deletion.
- JSON number/structure validation, optional usage values and fractional cost.
- Bounded request context (pure module): the newest-fit suffix rule at exact byte
  boundaries, oldest-first dropping with exact message counts, eligibility of
  welcome/error/unfinished/empty messages, system-prompt preservation, explicit
  system/message/combined oversize failures with truthful complete-body
  diagnostics, invalid arguments (each rejecting call zeroes its output), a
  read-only proof over overflow storage, a budget sweep asserting the suffix,
  monotonicity and size invariants, and equality between the measured size and
  the real encoded request body. A hidden-HWND suite links the real host with a
  wrapped `openrouter_request` seam: it proves an oversized send fails
  explicitly, keeps its user message whole, retries through the real send path
  with the same failure, and never invokes the client, then drives a real
  successful send with omitted history and checks exactly which messages the
  client received, that the dropped text was not among them, and that the
  omission count stays in the generating status sweep.
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

Remaining limits: 16 conversations, 64 messages each, and a 128 MB on-disk
snapshot bound. The composer and the system prompt are bounded at 16,383 UTF-16
code units; message text past that (streamed replies) lives in overflow storage,
so its ceiling is memory and the snapshot bound rather than a fixed count. A
request sends at most the 64 KiB context budget and drops the oldest eligible
history beyond it; the budget is a local proxy for prompt size, not a model's
context window. Responses past the local limit stop as Interrupted without
silently claiming success. Full
model-catalog autocomplete, response variants, and global search remain outside
this pass. Interactive clipboard/IME behavior,
modal-dialog appearance and physical multi-monitor DPI transitions still need a
manual desktop check; hidden-HWND tests do not substitute for that visual review.
