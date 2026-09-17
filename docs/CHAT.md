# DarkChat — verified chat contract

Build with `chat.bat`; run `build\darkchat.exe`. `chat.bat test` is the single
reproducible verification command: it builds the app, runs all chat tests
(including the transcript-isolation, scheduled-flush, message-growth-boundary,
request-context, model-catalog, provider-routing, realization-policy and
slot-pool regressions), then runs the unchanged DarkUI toolkit suite via
`build.bat test`.
No third-party dependencies are required (C17, MinGW-w64, Win32).

## Daily use

- The sidebar creates and reopens conversations. Conversation menu: New, Rename,
  Delete, Clear messages. Rename survives subsequent edits and clearing. Delete
  and Clear ask for confirmation. Deleting the final conversation creates a new
  empty one with a new ID.
- The conversation list is virtualized: DarkUI retains buttons only for the
  rows currently in view (bounded by the sidebar viewport, not the 128-
  conversation cap), and top/bottom spacers keep the scrollbar sized to the
  full list. Every row carries the conversation's stable ID, so activation,
  search jumps and UIA Invoke always open the conversation actually displayed;
  scrolling never snaps back to the active row, which is revealed only by
  selection, deletion, a new conversation, search navigation or startup.
  Screen readers are notified of row rebinding through UIA name-changed and
  structure-changed events; keyboard Tab, Enter, arrows, PgUp/PgDn, Home/End
  behave as before.
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
  old messages. The model field still accepts any OpenRouter identifier typed
  directly.
- Settings > Provider routing applies global OpenRouter provider routing to
  future requests: sorting (Default — balanced, or price/throughput/latency),
  fallback providers on/off, whether providers that may store data are allowed,
  and a request-level **Require zero data retention** requirement. Every control
  defaults to OpenRouter's own default, so an untouched DarkChat sends no
  `provider` object at all. The ZDR item is only DarkChat's request-level
  requirement: unchecked it does not turn off ZDR enforced by the OpenRouter
  account, it just adds nothing. A change applies to the next request and never
  rewrites history. A routing restriction (no fallbacks, data-collection deny,
  or ZDR) can make a request fail when no eligible provider exists; the failure
  surfaces through the ordinary Failed/Retry path.
- Ctrl+Space opens the model picker (Settings > Choose model...). It opens
  immediately with the current model and the recent-model history, then fills in
  from OpenRouter's catalog when the fetch completes, refreshing in place
  without losing the filter text or the selected id. The filter matches
  case-insensitively against a model's id and display name; Up/Down move, Enter
  or a double-click selects, Escape cancels. The merged list is the current
  model (when set), then history in most-recent order, then the catalog in API
  order, deduplicated by id; ids longer than the model field are skipped and
  counted. The picker never blocks startup: it fetches on first open, caches in
  memory for one hour, retries on a later open after a failure, and falls back
  to the current model and history when offline or when OPENROUTER_API_KEY is
  unset. Only a confirmed selection replaces the model; a model joins history
  only when a request is sent, and the catalog is never persisted.
- Ctrl+F focuses the sidebar search field. Enter performs a fresh on-demand
  search over every live message body and reasoning field; F3 and Shift+F3 move
  between retained results. Matching is locale-independent ordinal Unicode
  case-insensitive comparison without normalization or length-changing folds.
  Results retain persisted conversation/message IDs rather than array indices;
  stale edited, deleted, or replaced messages do not resolve. A reasoning hit
  opens that turn's reasoning viewport before the transcript minimally reveals
  it. Search queries, snippets, and results are transient and are never saved.
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

The transcript is a native scroll container over per-message layout records.
Each record (`chat/transcript_win32.h`, `TranscriptRecord`) is lightweight and
owns no windows: it stores the message identity its surfaces were built from
by value (conversation id, message id, per-message revision and answer-text
revision), the deferred-write debt per surface, the visibility state, the
geometry and a measurement stamp, plus the index of the realized slot bound
to it. A realized slot (`TranscriptSlot`) owns the native Rich Edit surfaces:
an assistant shape can require a header/reasoning-row block, an optional
reasoning viewport, an answer block and a terminal metadata footer; other
shapes require only a body. A slot binds at most one record and a record at
most one slot. Control ids are slot-based (`100 + slot*4 + surface`), and a
fresh binding generation prevents a recycled slot number from certifying a
different message. In the shipped bounded mode, a slot and its HWND cells are
reused as records enter and leave the realization window; logical identity,
layout, deferred-write debt and saved reader state remain record-owned. The
container owns one outer scroll for the conversation and positions only live
surfaces. An expanded reasoning viewport is inset, tinted
(`UI_PANEL`) and separated by an 8 DIP gap above and below, so it reads as its
own sub-panel rather than part of the answer, and it scrolls independently: the
wheel scrolls the viewport first and chains to the transcript at its ends, so
the target never depends on which control holds focus.

Which records must be realized, how many slots are required, and which slot a
new binding takes are pure policy decisions (`chat/transcript_policy.h` — no
Win32, no allocation): geometry-derived visibility (edge-touch excluded,
unmeasured heights read at the minimum height), class protection (streaming,
focused, selection/debt-bearing, expanded reasoning), rank/index realization
ordering with the LRU stamp reserved for eviction only, the dynamic required
slot capacity, fail-closed victim selection, and exact message-instance
identity validation. The 512-slot arena is created in one allocation; native
surfaces remain lazy. `transcript_dispose` frees the arena exactly once per
host from the ownership layer after the complete window hierarchy is destroyed,
not from the parent `WM_DESTROY` path while child surfaces can still reference
their `GWLP_USERDATA`.

Realization is selected per transcript by `transcript_set_bounded`. A zeroed or
directly initialized `Transcript` retains the compatible retain-all behavior:
every record prepares on each render and the policy capacity is diagnostic only.
`chat_main.c` explicitly enables bounded realization for the shipped
application.

In bounded mode, the fixed-point realize/measure loop binds actionable records
in the strict viewport plus the 300-DIP overscan band, then protects Tier-A
streaming or focused records anywhere and admits the newest 24 Tier-B
debt-bearing or expanded-reasoning records. Shape-aware selection prefers
matching reusable slots and consumes pristine slots last. `slot_limit` starts
at zero and only rises to the governed requirement:
`ceil(page/h_min)+1 + ceil((2*overscan)/h_min) + 2 Tier-A + 24 Tier-B + 2
spares`, clamped to the 512-slot arena. Every bounded selection stays inside
that limit. Existing bound slots are not continuously trimmed merely because a
Tier-B record falls outside the 24-record membership allowance. Only a failed
selection at the raised limit can force-evict the oldest LRU off-window,
non-Tier-A binding; viewport and Tier-A bindings are not forced victims.

Heights are consumed as exact only when their stamp still matches width, DPI,
theme epoch, identity, shape, binding certification and debt state. Estimates
never certify geometry and are retried. All scroll, reveal and selection paths
run the realization loop before placement, so strict-visible records are
realized. While interactive resizing is active, strictly off-screen records
keep their previous heights with stale stamps, strict-visible records continue
to measure, and `WM_EXITSIZEMOVE` performs the settling render. A surface gets
at most one creation or destructive-write attempt per render epoch; missing
surfaces retry on later rendering, a streaming delta, the re-armed one-shot
flush and the one-second sweep. A missing resize notification uses a flagged
line-count estimate, and a missing measurer uses an arithmetic estimate, both
followed by later exact retries. `TranscriptStats` exposes bind, reuse,
eviction, HWND, measurement, retry, convergence, capacity and reader-state
diagnostics; it is not a wall-clock acceptance limit. At most `4 * 512 + 1`
arena cells can ever have HWNDs (four cells per slot plus the measurer); live
and peak counts derive from live handles, so a recreated handle does not grow
that arena accounting.

### Reader position: follow mode, anchors and switching

The transcript distinguishes bottom-following (`BOTTOM`: the newest content
pulls the scroll) from a free reader (`FREE`: the reader's position is held
against changes above it). Only an explicit user scroll can leave
bottom-follow, and only a user scroll landing at the bottom re-enters it;
every mutation pass — streaming deltas, completion, deferred writes, height
corrections, resize/DPI reflow, reasoning toggles, eviction — preserves the
mode. A free reader's position is a scroll anchor: a stable (conversation,
message) identity plus a surface family and a pixel offset, resolved against
fresh geometry on every placement so growth above the reader is absorbed
instead of shifting content under them. The anchor's offset is inside the
surface: only scroll 0 is the top-of-transcript anchor — a position inside
the top margin above the first turn is named with a negative offset and
restores exactly — and a positive offset is clamped to the surface's last
pixel when the surface shrinks, never to the next surface's top. A
scrollbar thumb drag owns the position while held; whether it ends with a
release or is cancelled (`WM_CANCELMODE`, which either the container or the
top-level window may receive), it finishes through the same end-of-drag
path: the final position is qualified and captured as the fresh anchor.

Each conversation's anchor lives in a per-conversation table keyed by stable
conversation id (a fixed linear array, never indexed by id — ids are
monotone store counters and can be sparse). Switching conversations saves
the departing reader's anchor and loads the arriving conversation's entry:
a valid saved anchor restores FREE and the saved position exactly; a
missing or stale entry sets BOTTOM and clears the anchor, so a fresh
conversation is never blessed with FREE and nothing to restore. The save
direction is symmetric: a reader who left a conversation without a position
(bottom-following, or an anchor that no longer names that conversation)
clears the conversation's stored entry rather than leaving an older FREE
position behind, so a later return lands where the reader actually left
it. The load direction resolves the saved message against the arriving
conversation: an entry whose message no longer exists (deleted, or
replaced with a fresh identity by a retry/regenerate) is stale — BOTTOM,
anchor cleared, entry dropped. A render prunes table entries whose
conversation no longer exists — a session can mint far more distinct ids
than the table's 128 slots through delete/create churn, so entries are
reused rather than assumed exhaustible. Reader-state captures taken at
eviction (per-surface selection ranges, the reasoning viewport's inner
scroll) ride on the record, are merged into any earlier capture of the
same record (a surface that cannot be read keeps its previous capture, so
evicting a partially recreated record cannot erase state), count as
decision-time debt (Tier-B protection), and are cleared only after their
successful restoration — applied after the placement transactions, which
reset a re-shown reasoning viewport's inner scroll — so a failed surface
creation retains them for the next render's retry.

### Recycling state boundaries

Ordinary unbinding and reuse preserve the record's rendered identity,
measurement state, deferred-write debt and session view state; a nonempty
surface selection and a live reasoning viewport's inner scroll are captured
before eviction and restored after the same message instance is rebound.
Forced eviction uses the same record-owned captures, including across a failed
rebind, and never changes `reasoning_open`. A replacement message or a switch
to a different conversation invalidates the old surface identity and does not
transfer its deferred writes, selection or reasoning content to the new
message. Empty selections and a non-live reasoning viewport have no capture.
Reasoning content, duration and message identities are persisted; selections,
inner reasoning scroll, follow anchors, search results and reasoning expansion
are session view state and are not persisted across restart.

Reader focus is tracked through the Rich Edit `EN_SETFOCUS`/`EN_KILLFOCUS`
notifications arriving over `WM_COMMAND` (no event-mask bit is required).
When a focused surface is about to be hidden, invalidated, rebound,
switched away, or torn down — including the turn-slot reset a
send/retry/regenerate performs — focus moves first through the transcript's
host callback to the composer — ordinary reader flow continues at the
input, and the transcript container never decides where focus lands. Window
teardown transfers focus to the top-level window in `WM_CLOSE`
immediately before `DestroyWindow`, so no transcript child is ever
destroyed while it holds keyboard focus.

Off-screen geometry is measured through one shared measurement surface: a
read-only Rich Edit block, child of the transcript container, kept
`WS_VISIBLE` but parked at `TRANSCRIPT_MEASURE_OFFX` beyond the client's right
edge so the container clips it away entirely. A truly hidden Rich Edit stops
re-laying-out its text — `EM_REQUESTRESIZE` then answers from the stale,
over-wrapped layout the control was created at — so the surface must stay
visible-style while never painting. Its width is the transcript's content
width from creation and is re-asserted before each content batch (width before
content). Because a Rich Edit recomputes its line wrap only when a width
transaction is immediately followed by `EM_REQUESTRESIZE`, every measurement
re-asserts the width first — a real resize when it differs, and in bounded
mode a one-pixel down-and-back cycle when it already matches — and only then requests the
resize; spontaneous notifications fired outside a measurement are rejected by
the awaiting-control guard. The measuring surface equals an equivalent live
surface for identical content, width, DPI, theme and formatting: the
hidden-HWND suite asserts exact quality and equal heights on both surfaces
and equal cached heights for every family a record owns.

Update bookkeeping lives in its own module (`chat/transcript_win32.c`). Every
turn records the message identity its surfaces were built from — conversation
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
If the body surface is missing (never created, or its creation failed this
render) the flush retry keeps the flush armed and re-arms the one-shot timer at
the throttle interval — through a raw `SetTimer`, never through the scheduler
whose `SetTimer`-failure fallback calls the flush, so no recursion — until the
surface is recreated or the generation ends; the next incoming delta, the 1 Hz
sweep and the next render are the additional retry paths.
A scheduled flush that fires while the body holds a selection is deferred
through the pending-write path instead of destroying the range. Incomplete
inline syntax renders literally while it streams; an opened unterminated fence
renders its remaining source as code. A rebuild relayouts only from the
streaming turn and follows only when the stored follow mode is `BOTTOM`.
Terminal metadata updates its own surface without rewriting the body. Skipping
a destructive content write does not skip placement or visibility
reconciliation for a live surface.

Metadata is a terminal-state footer only: a compact muted line
(`Complete · TTFT 18.0s · 19.4s · 44 in / 1,365 out · $0.00165`) with the model
on its own line, shown once when the requested and actual models match and as
`requested → actual` when they differ. Normal `stop` is omitted; unusual finish
reasons are surfaced. A running turn never mixes stats into the streaming answer.

### Markdown rendering

Terminal assistant output is Markdown-rendered (`chat/markdown.c`, a pure
parser with its own test suite); while a response streams, the visible body is
rebuilt from the accumulated message at most once every ~100 ms (a scheduled flush
renders a paused burst), incomplete inline syntax stays literal, an opened fence
keeps its remaining content coded until a valid closer arrives, and the terminal
event flushes the final render. User, system and error bodies are fully literal,
fence markers included, and reasoning is never reformatted. The supported subset:
headings 1–3 (require a space after the marker), `**bold**`, `*italic*`/`_italic_`
(word-internal underscores and asterisks stay literal), `~~strikethrough~~`
(paired non-space content only), inline code with equal-length backtick runs on
one source line (different-length runs stay raw content; unmatched runs stay
literal), and fenced code with a backtick or tilde opener of at least three
delimiters after up to three leading spaces. Fences use permissive hidden info
strings, including backtick-fence info strings containing backticks. A closer
uses the same delimiter, is at least as long as its opener, and
has only spaces or tabs after it; shorter, wrong-character and text-suffixed
candidates remain raw code content, while longer closers are accepted. Fenced
content is unparsed and removes at most the opener's leading spaces from each
line; an unterminated fence keeps the remaining document as code. Fence lines and
info strings are hidden, and code is monospace on the code tint.

Lists use textual `• ` bullets, `☐ `/`☑ ` unordered task markers at any
supported depth, and preserved ordered markers; blockquotes are muted and
bar-prefixed with one `▌ ` per level. Nesting is conservative and bounded:
a quote is `>` (each level with an optional single space, so `> > x` and `>> x`
both nest) up to 8 levels; a nested list item must be indented to its parent's
content column, so a shallower indent is a sibling item or a continuation, never
a new level; the last quote marker needs a space or the end of line after it, so
`> >x` is one quote containing `>x`. A continuation line — indented past an open
item's marker — inherits that item's layout and pads the absent marker with
spaces, which keeps the bars and the content aligned with the item it continues.
Indentation past 32 columns clamps the layout and keeps the remaining source
whitespace verbatim, tabs included, while a ninth quote or list level, a
four-space indented root marker or any other unsupported prefix leaves the whole
line literal; ordinary leading whitespace is preserved as text. Lazy
continuation, indented code blocks, `+` bullets and fences inside quotes are not
recognized. `[label](http(s)://…)` links render as `label` with the destination
recorded beside the styled runs and applied as link formatting; the display opens
that stored destination from `EN_LINK`, while a bare URL the native detector
recognizes still opens from its visible range. Malformed or non-HTTP(S) links
stay verbatim. Precedence is
fences → inline code → links → strikethrough → emphasis; escapes (`\*`) keep
punctuation literal; malformed or unsupported
syntax (tables, images, deeper rules) is preserved verbatim.

Each paragraph also records layout metadata — quote depth, list depth, item
flags and the two indent columns — next to the styled runs. Rich Edit applies
`dxStartIndent` and `dxOffset` from it (a positive `dxOffset` hangs wrapped
lines inside the synthesized marker) and clears both across the whole document
before every write, so a verbatim body, a literal fallback or a rebuilt document
never inherits an indent; adjacent paragraphs sharing a layout are formatted
with one selection. Every
Rich Edit run sets bold, italic, strikethrough, face, size and background
explicitly, so code tinting cannot bleed into following text, and identical
adjacent runs coalesce. The parser is transactional and O(input)-memory: on
allocation failure — including failure of the paragraph array alone — the body
falls back to verbatim text. Rebuilds of completed
turns may reparse the message; no render cache is kept.

The key is read from `OPENROUTER_API_KEY` in the process environment, falling back
to the Windows User environment registry value (so an existing desktop session
can pick up a newly configured key). Key buffers are cleared before release.
The same key authenticates the model-catalog fetch (`GET /api/v1/models`); its
owned header buffer is cleared before release too.
The key is never part of conversation state or persistence.

## Lifecycle and history contract

One request runs at a time. The UI thread owns Chat; the worker receives an owned
request snapshot and posts generation-ID-tagged events. A user turn and empty
running assistant response are saved **before** sending the network request —
a flush-and-wait through the background snapshot writer, so the guarantee
stays synchronous and a failed save still refuses to send.
Deltas update that assistant response in place. Late events are discarded.

| State | Meaning |
| --- | --- |
| Complete | `[DONE]` received, nonempty text, no provider error. Provider `length` remains Complete with its finish reason visible. |
| Cancelled | User requested Stop, including when a queued DONE races with Stop. Partial text is retained. |
| Interrupted | Window closed, a running response was recovered after a crash, connection ended during a response, `[DONE]` was missing, or appending streamed content failed. |
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
`0 <= message_count <= message_capacity <= CHAT_MAX_MESSAGES` (512) and
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
at the message cap and an allocation failure can never leave a partially
applied send/retry/regenerate/edit. Because growth reallocs, element pointers
are not stable across appending operations: identity is re-derived from the
conversation plus index or stable message ID (the transcript already stores
rendered identity by value). Deleting a conversation bytewise-moves the
surviving ones, transferring ownership of their arrays and overflow pointers,
and the vacated slot is zeroed so nothing is freed twice. Chat and its message
arrays remain UI-thread-owned; worker threads never borrow them, and
asynchronous persistence uses `chat_snapshot` — a deep, transactional copy
handed to one background writer thread.

Each message keeps only a small inline residue — at most 255 UTF-16 code units
for text and for reasoning — and promotes to heap-backed overflow storage on
first growth past it, so the fixed per-message cost is small and snapshot
copies pay for the live text rather than per-message slack. Composer input,
per-conversation drafts and the system prompt keep a separate 16,383-code-unit
product limit and never share the message residue. A fixed-residue
amplification gate in the chat suite bounds the structural copies a save can
hold simultaneously — live state plus the saver's pending, in-flight and
constructing snapshots, the latter built before the displaced pending copy is
disposed — at the 128-conversation × 512-message bound to ≤ 1 GiB of
fixed struct/slack cost. The gate deliberately excludes heap-backed live text,
which is proportional to actual content, so it is not a complete worst-case
memory bound.

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
  When provider routing is configured the body also carries the optional
  `provider` object, whose exact bytes are added to the measured envelope by the
  same pure builder the encoder uses, so the measured size stays exact.
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

`%LOCALAPPDATA%\DarkChat\state.jsonl` is a UTF-8, version-3 JSONL snapshot:

1. A settings record with `type: "settings"`, `version: 3`, selected conversation
   index, next ID counter, record counts, model/system prompt, geometry, and the
   optional provider-routing fields (`provider_sort`, `provider_no_fallbacks`,
   `provider_data_collection`, `provider_zdr`).
2. Zero or more `type: "model"` history records.
3. For each conversation, a `type: "conversation"` record followed by its declared
   number of `type: "message"` records, each carrying its stable `"id"`.
4. A `type: "commit"` record containing the 32-bit FNV-1a checksum of every byte
   before that record (including LF separators).

The record layout is identical in formats 1–3. Format 2 raised the conversation
limit from 16 to 128, and format 3 raised the per-conversation message limit
from 64 to 512. Provider-routing settings are additive optional fields appended
last, emitted only when non-default and defaulted when absent; like the optional
reasoning fields they do not change the format version, so the emitted version
stays 3 and older snapshots load with OpenRouter's routing defaults. This build
decodes all three versions, so an old snapshot
migrates to format 3 on its next save. **Downgrade contract:** an older
(version-1 or version-2) build that encounters a format 3 or newer file treats
it as an unsupported version and stops immediately — it does not fall back to
`state.bak.jsonl`, never overwrites the newer primary with a stale backup, and
disables writes until a build that understands the format runs again. (A count
above an older build's bound in an otherwise current file would be
indistinguishable from corruption, which is why each bound change required a
version bump rather than relying on the count check.)

Conversation IDs are numeric, store-local, monotonically allocated from a
persisted counter initially seeded from the current time. They do not depend on
names or array positions and are not reused after deletion. Every message also
carries a stable identity drawn from the same counter and persisted in its
record (`"id"`): retry/regenerate keep the user turn's id and give the fresh
assistant turn a new one; edit-and-resend preserves the edited user id and
creates a new assistant id. Conversation/message created
and modified timestamps and generation started/first-token/finished
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
save error.

Saves run on a background snapshot writer (`chat/saver.c`): the UI thread builds
a deep, immutable copy of the whole Chat (`chat_snapshot` — exact live-count
message arrays, overflow storage reallocated per message so nothing is
aliased) and hands ownership to one writer thread, which alone calls the
storage module — the writer lock, backup rotation and recovery semantics are
unchanged and stay single-threaded. The pending handoff slot is latest-wins: a
snapshot that has not started writing is displaced by the next handoff and
disposed, so a burst of handoffs can never queue work. Every accepted handoff
receives a monotonically increasing submission (attempt) id, separate from the
mutation counter, because two handoffs can share one counter and the counter
alone cannot identify an attempt; the pending and in-flight jobs each retain
their own attempt id and captured counter, and every posted result repeats
them. A flush-and-wait is released only by its own attempt or a genuinely
later covering one — never by an older in-flight attempt that merely shares
the same counter. Results are interpreted in attempt order: any result at or
below the newest already-interpreted attempt is stale and ignored, and a
failure older than the newest *submitted* attempt is superseded and
suppressed — that newer attempt's snapshot contains all of the failed one's
state and is guaranteed to complete and post its own authoritative result
(every accepted submission either completes with one posted result or is
displaced before starting, in which case the displacement's own result
reports), so a superseded failure can neither latch the failure state, write
the failure status nor suppress the status sweep, and an older success can
neither mark newer unsaved mutations durable nor hide a newer failure.
`dirty` clears only when the newest handoff's completion succeeds; a
processed failure latches, reports through the status line, explicitly keeps
dirty set, and the one-second autosave retries.

Autosave hands a snapshot off once per second while state/drafts/settings are
dirty, and immediately on history actions, Stop and completion. Two sites keep
the synchronous durability contract through a flush-and-wait: the save before
sending a network request (a failed flush still refuses to send, and save
failure still outranks every context diagnostic) and the final save on close
(the "lose unsaved changes?" prompt reflects real durability, not a handoff in
flight). At most roughly one second of recent streamed text or ordinary draft
typing can be lost on abrupt termination (long UI/disk stalls can increase that
interval). On close the writer joins before the storage lock is released.

Load validates JSON, version, ranges, counts, identities and checksum into a
separate Chat before adopting it. Message identity is validated strictly: a
present `"id"` must be an exact integer in `[1, next_id]` as the settings
record stored it, and it must be globally unused — not equal to any other
message's id or any conversation's id anywhere in the snapshot. A missing
`"id"` (a snapshot written before the field existed) is migrated: the message
receives `next_id + 1, next_id + 2, …` in file order and the counter is
bumped; those synthesized values are never validated against a persisted id,
because persisted ids are always checked against the counter exactly as it
was stored. Exhausting that exact-integer ceiling while synthesizing is
corruption, not a resource condition, and fails the whole snapshot. Any
present-but-invalid id — zero, negative, fractional, a string, above the
stored counter, duplicated or globally colliding — is corruption and rejects
the snapshot, which then falls back to the backup/complete-temporary recovery
below; there is no tolerant or self-repairing interpretation. An accepted
id-less snapshot (for example, one whose message `"id"` fields were removed and
whose checksum was recomputed) receives fresh IDs above the stored counter;
this is identity migration, not compatibility with an older executable.
Decoding reserves backing storage for each
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

`chat.bat test` runs `test_sse`, `test_json`, `test_chat`, `test_markdown`,
`test_markdown_win`, `test_transcript_slots`, `test_chat_ui`, `test_lifecycle`,
`test_context`, `test_transcript_policy`, `test_search`, `test_storage`,
`test_openrouter`, `test_model_catalog`, `test_model_catalog_worker` and
`test_chat_host`, then the DarkUI toolkit suite through
`build.bat test`. The relevant final-regression inventory is:

- `test_chat`: the 512-message cap, transactional dynamic-array growth,
  inline-to-overflow boundaries and failures, replacement at the cap, and
  deep snapshot copy/isolation.
- `test_storage`: format-1/2 migration, format-3 512-message and long-overflow
  round trips, over-cap rejection, ID validation/migration, recovery and
  unsupported-version fail-closed behavior.
- `test_transcript_policy` and `test_transcript_slots`: visibility/protection,
  shape-aware slot and forced-victim policy, raise-only capacity formula and
  limits, plus the one-allocation slot arena lifecycle.
- `test_chat_host` (`default_suite`, `seam_toggle_suite`, `bounded_suite`):
  real hidden-HWND integration for recycling, foreign-content prevention,
  exact/estimated measurement and retries, resize/DPI settling, streaming and
  deferred writes, BOTTOM/FREE anchoring, search reveal, focus transfer, and
  forced-eviction restoration. The bounded suite asserts visible realization,
  bound slots no greater than `slot_limit`, created HWNDs no greater than
  `4 * slot_limit + 1`, and zero exhaustion refusals in its saturation case.

The complete coverage includes:

- Lifecycle state transitions, retry/regenerate/edit replacement, full-capacity
  retries, stable IDs after rename/clear/delete, and bounded model history.
- Dynamic message storage: transactional doubling growth bounded by the
   512-message cap, allocation invariants after every operation, append and
  send/retry/regenerate/edit-resend transactional failure semantics under
  injected allocation failures, retained-capacity trims, a version 1 fixture
  from the previous build loading and migrating to deterministic nonzero ids
  with a bumped counter, idempotent save/load after migration and byte-stable
  repeated saves, and decode allocation failures (including the
  conversation-array reservation and a mid-message failure under a poisoned,
  non-zero-filled allocator) recovered through the fallback or leaving the
   destination's previous content and the store untouched, plus ownership
   transfer across mid-list conversation deletion.
- Inline residue: exact set/append boundaries at capacity−1/capacity/capacity+1,
  promotion to overflow, shrinking back inline and re-promotion, repeated
  streamed heap growth, transactional failure of the inline-to-overflow
  transitions (inline content, length and revision untouched), snapshot
   capacities of promoted text/reasoning, and the fixed-residue amplification
   gate (4 structural copies × 128 conversations × the 512-message bound
   ≤ 1 GiB of fixed struct/slack cost; heap-backed live text excluded).
- Stable message identities: new-format id roundtrips, old/new mixed records in
  both orders, deterministic migration in file order, and rejection of every
  invalid id form (zero, negative, fractional, string, above the stored
  counter, duplicate, message/conversation collision in both directions, a
  later persisted id equal to a synthesized one, and counter exhaustion),
  primary rejection recovering from a valid backup, write disabling when no
  snapshot can be recovered, downgrade simulation (ids stripped, checksum
  recomputed, reload remigrates) and tolerated unknown optional fields.
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
- Transcript realization policy (pure module): geometry-derived visibility
  with edge-touch exclusion and unmeasured heights read at the minimum
  height, class protection (streaming, focused, debt, expanded), rank/index
  realization ordering with the LRU stamp excluded from it, exact
  |visible ∪ protected| required capacity with clamped spare slots and
  page/count monotonicity, the dynamic geometric required slot capacity
  (`ceil(page/h_min)+1` plus `ceil(2*overscan/h_min)`, two Tier-A slots, the
  Tier-B allowance and two spares, clamped to the arena) with page
  monotonicity and input floors, fail-closed victim selection that returns
  the slot position (never the represented record index) with LRU ranking
  among evictable slots only, and exact message-instance identity
  validation.
- Transcript slot-pool lifecycle (wrapped calloc): dispose on zeroed state,
  deterministic pool-allocation failure that leaves every record unbound and
  the transcript safe, successful creation with exactly one allocation and a
  fully unbound pool, idempotent double dispose, accessor refusals after
  disposal, and — in the hidden-HWND host suite — disposal only after the
  window hierarchy is fully destroyed.
- Storage round trips with stable message ids, Unicode/drafts/settings/metadata,
  exclusive writer lock, corrupt/torn snapshots, backup/temp recovery, denied
  temp writes and failed atomic replacement, and protection against unknown
  versions.
- Background snapshot saving (`chat_snapshot` unit tests and the hidden-HWND
  host): deep copies of inline and overflow storage that never alias the
  source and carry exactly the live message count, transactional failure at
  every position of one combined allocation sequence with the source
  untouched, snapshot isolation across a paused in-flight write (the file
  lands in the pre-mutation shape, and the store is read only while the
  writer is idle), per-attempt result ordering (a stale duplicate never
  claims durability, a late older failure never re-latches behind a newer
  success), failure latch with dirty preserved and retry through the writer,
  deterministic latest-wins coalescing with the writer's storage passes
  bounded by count, shutdown draining a provably pending handoff, and the
  pre-request flush gate still refusing to send when the writer reports
  failure.
- Real request encoder/SSE callback fixtures for model, usage, cost, TTFT,
  finish reason and provider errors after partial content, now also the enabled
  reasoning request parameter and reasoning_details/text-summary/plain fallback
  parsing that never fabricates reasoning.
- Provider routing: an all-default routing sends no `provider` object at all;
  each control serializes to its exact request JSON shape and only non-default
  keys are emitted in a fixed order; the request-context budget charges exactly
  the provider bytes (pinned to the byte) and its oversize diagnostic includes
  them; storage round-trips the optional fields (emitted only when non-default,
  old snapshots load the OpenRouter defaults, and a present field with the wrong
  type or an out-of-range value rejects the snapshot); and the hidden host
  drives the Settings submenu actions and proves the next request carries the
  configured routing through the client seam.
- Model catalog: pure parse of `data[].id/name/context_length` with required
  vs optional typing, Unicode names, exact-id dedupe, the retention cap, id
  length cancellation, malformed roots, an ordinal case-insensitive id/name
  substring filter, the current/history/catalog merge order, and transactional
  allocation failures; and the one-shot worker through an injected transport
  for success, non-2xx, oversized bodies, missing key, allocation failure,
  duplicate-start rejection, failed completion post and cancel-and-join
  shutdown. The hidden host exercises the picker seam for exactly one fetch on
  first open, success refresh, filter/selection preservation across an open
  refresh, accept/cancel, stale-generation rejection, keyed-offline history
  fallback with retry, and a close deferred until the modal loop unwinds.
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
  snapshot whose message lines omit both the optional reasoning fields and the
  stable message ids.
- Hidden native HWND host integration: stale events, switching during generation,
  partial failures, cancel/DONE races, empty replies, edit-and-resend draft
  preservation, inline reasoning row/surface transitions, reasoning persistence,
  and close/reopen interruption recovery.
- Per-message revisions keep transcript updates isolated from reader state:
  unchanged historical controls survive completion and layout changes, active
  selections are preserved, destructive reformatting of selected text is
  deferred until the selection clears, and switching conversations replaces
  content immediately. Streaming reasoning appends preserve a selection.
- The retain-all compatibility fixture verifies a maximum-length 512-message
  transcript's stable controls and selection through streaming, the scheduled
  flush and terminal rendering. The bounded fixture verifies a window-shaped
  realization set, recycled slots/HWND cells, bounded native-control counts,
  revisit reuse and reader-state restoration. Render time and control counts are
  recorded by tests, not asserted as wall-clock thresholds.
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
- Bounded realization engine (separate hidden-HWND fixture with the seam
  enabled and Rich Edit creation wrapped for per-surface failure injection):
  a pristine first render binds a window-shaped subset (never all 80 records)
  with the shared measurement surface created lazily and arena diagnostics
  exact; seam toggling retains and reuses every window; an A-short → B-long
  conversation replacement rebinds B onto A's departed slots with zero new
  HWNDs and zero foreign content, and switching back restores A's own text;
  nonlocal jumps to top/middle/bottom realize and render the right content
  with revisits consuming no pristine slots and no new HWNDs; deferred
  selected content keeps displayed-true geometry (old height, preserved
  range, no claimed revision) until the selection clears and the debt
  applies; per-surface creation failures (body, head, footer, reasoning
  viewport) block exactly one attempt per render, stay absent without
  disturbing the other surfaces, and recover through the delta arm, the
  re-armed flush timer and later renders — during streaming, without foreign
  content; a destroyed measurement surface degrades one pass to flagged
  arithmetic estimates (never silently wrong heights) and recovers to exact
  stamps, with cached heights equal to live measurements wherever both are
  exact; and a focused equality property feeds the same production setters
  with identical content to the measurement surface and the live surfaces —
  verbatim, wrapping markdown and head label — requiring exact quality and
  equal heights on both, plus cached-equals-live for body, head and footer.
   A real view-resize storm keeps every step's viewport realized, relaxes
   off-screen exactness (stamps left stale for strictly off-screen records
   only), and the settle render plus DPI changes restore exact stamps at the
   new width and DPI; the equality regression covers user blocks, wrapping
   markdown, head labels and the metadata footer. Convergence bookkeeping
   asserts no cap fallback and no degraded settle. Explicit regressions
   cover the follow/anchor lifecycle and the governed capacity: send-while-
   FREE through the bounded send path holds the anchor and never
   force-follows; per-conversation anchors restore FREE from a valid saved
   anchor, land BOTTOM on a fresh conversation (never FREE with no anchor),
   and survive a churn of far more than 128 distinct conversation ids
   (entries for deleted conversations are pruned); binding accumulates
   inside the raise-only governed slot limit (exceeding the Tier-B
   allowance alone evicts nothing), forced Tier-B eviction fires only when
   selection inside the limit returns -1, and the limit and HWND arena
   stay bounded by the limit; eviction restores per-surface selection
   ranges and the reasoning viewport's inner scroll, retaining captures
   across creation failure; and reader focus reaches the host composer
   through the focus-release callback after hides and switches.

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
isolated directories under `build`, not the user's conversation store. Search
matching, snippets, invalidation, stable-ID resolution and hidden-host jumps are
also covered.

Remaining limits: 128 conversations, 512 messages each, and a 128 MB serialized
snapshot bound. The conversation cap fails closed — the New button disables and
creation is refused; nothing is ever evicted. Snapshots declaring more than 128
conversations are rejected as corruption (the backup recovery path still
applies). The composer, drafts and the system prompt are bounded at 16,383
UTF-16 code units; each message keeps only a 255-unit inline residue, and
message text past it (streamed replies) lives in heap-backed overflow storage,
so it has no fixed per-message length cap. The on-disk snapshot limit constrains
successful persistence, not live heap usage. A
fixed-residue amplification gate bounds the structural copies a save can hold
(live, pending, in-flight and constructing) at the 512-message bound
to ≤ 1 GiB of fixed struct/slack cost; heap-backed live text is excluded.
Load-time global identity validation is a deliberate quadratic scan (there is
no index): a maximum store of 65,536 messages can require roughly 2.15 billion
prior-id comparisons, a known scaling risk deferred to a future pass. A
request sends at most the 64 KiB context budget and drops the oldest eligible
history beyond it; the budget is a local proxy for prompt size, not a model's
context window. An allocation failure while appending streamed content retains
the partial response as Interrupted. Conversation search is an on-demand scan
with no persisted or background index. The model catalog is fetched on demand in
one unpaginated request, held in memory only and never persisted, so an offline
restart falls back to history until a later fetch succeeds. Provider routing
exposes sorting, fallback, data-collection and ZDR controls only; per-provider
`only`/`ignore`/`order` selection, quantization, and price/performance
thresholds are not exposed. Response variants remain outside this pass.
Interactive clipboard/IME behavior, modal-dialog appearance and physical
multi-monitor DPI transitions still need a manual desktop check; hidden-HWND
tests do not substitute for that visual review.
