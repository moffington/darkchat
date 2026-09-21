#ifndef DARKCHAT_IMPORT_H
#define DARKCHAT_IMPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "chat/core/chat.h"

/* Pure, transactional import of DarkChat's own JSON export (chat/export). No
   Win32 and no file I/O: the caller owns the bytes. The importer copies the
   input exactly, validates it completely (strict JSON string decoding, strict
   UTF-8, full schema and bounds), builds every new conversation inside a
   detached temporary Chat, and only then transfers ownership into the
   destination. A malformed, oversized or allocation-failing import therefore
   leaves the destination semantically untouched: no conversation, message,
   active selection, status or id change is observable on failure.

   The consumed schema is exactly what chat_export_json emits:
     { "format": "darkchat.export", "version": 1, "exported_at": <n>,
       "conversations": [ { "id", "title", "created_at", "modified_at",
         "model"?, "ollama_model"?, "system_prompt"?,
         "system_prompt_present"?, "messages": [ { "role", "created_at",
         "modified_at", "text", "reasoning"?, "generation": {...} } ] } ] }

   Unknown/missing recognized fields: unknown fields are ignored (forward
   compatibility); a duplicated recognized field is rejected. Every
   conversation and message must carry the fields the exporter always emits —
   including the conversation `id` and the per-message `generation` object.
   File ids are validated as integers in the exported domain, then ignored, and
   every imported conversation and message receives a fresh id from the
   destination's persisted counter. The `generation` object is required and
   shape-checked (it must be an object) but its members are deliberately not
   inspected because imported turns are sanitized: chat_generation_init runs
   for every message and only assistant turns are marked
   CHAT_GENERATION_COMPLETE, so no cost, token, finish-reason, error, backend
   or timing metadata is imported. Conversation-level model and system-prompt
   overrides are restored. */

/* Broad resource guard mirroring the export and storage limits. */
#define CHAT_IMPORT_LIMIT (128u * 1024u * 1024u)

typedef enum {
    CHAT_IMPORT_OK = 0,
    /* Not JSON, wrong format/version, malformed field, an empty conversations
       array, an empty title, or a duplicated recognized field. */
    CHAT_IMPORT_MALFORMED,
    /* The input exceeds CHAT_IMPORT_LIMIT or a decoded value exceeds its bound. */
    CHAT_IMPORT_TOO_LARGE,
    /* Importing would exceed the conversation or message capacity. */
    CHAT_IMPORT_CAPACITY,
    /* Allocation failure; the destination is untouched. */
    CHAT_IMPORT_OOM
} ChatImportStatus;

typedef struct {
    int conversations_added;
    int messages_added;
} ChatImportStats;

/* Imports `length` bytes of NUL-free UTF-8 JSON into `chat`. `json` may be any
   pointer with at least `length` readable bytes (it need not be terminated);
   the importer never retains it. `stats` may be NULL. On failure the
   destination Chat is semantically unchanged; on success the new conversations
   are appended, the previously active conversation stays active, and `stats`
   reports what was added. */
ChatImportStatus chat_import_json(Chat *chat, const char *json, size_t length,
    ChatImportStats *stats);

/* Imports `length` bytes of Markdown into `chat`, reusing the same
   transactional guarantees and fresh-id policy as chat_import_json.

   If the bytes at offset zero carry the export's authoritative payload opener
   (`<!-- darkchat.export:`) exactly, the JSON between it and the first `-->`
   is parsed by chat_import_json: the presentation body is ignored, so a
   DarkChat Markdown export round-trips exactly (including model/system-prompt
   overrides, reasoning, roles and timestamps). A payload opener with a
   missing, empty or invalid payload is rejected; it never falls back to the
   body, because the opener claims authority over the file.

   Otherwise the whole file is imported as one new conversation holding a
   single CHAT_ROLE_USER message: the entire content, copied verbatim, so role
   headings like `## User` inside a body are never re-parsed into history. The
   conversation title is the first line beginning exactly `# ` (a deliberately
   simple, fence-unaware scan) with trailing CR/space/tab removed; when that is
   absent or empty, `fallback_title` (when non-empty); otherwise
   "Imported conversation".

   A single leading UTF-8 BOM is removed before parsing; the fallback message
   preserves everything after the BOM (its first unit is never U+FEFF), and a
   BOM-only file is empty and therefore malformed. `markdown` may be any
   pointer with at least `length` readable bytes and need not be terminated;
   the importer never retains it. `stats` may be NULL. */
ChatImportStatus chat_import_markdown(Chat *chat, const char *markdown,
    size_t length, const wchar_t *fallback_title, ChatImportStats *stats);

/* Copies at most `capacity - 1` code units of `source[0..length)` into `out`,
   always terminating it. When truncation falls between a surrogate pair the
   dangling high surrogate is dropped, so a caller never stores half of an
   astral character in a fixed title buffer. Writes nothing when `capacity` is
   zero; a NULL `source` is treated as empty. */
void chat_import_copy_title(wchar_t *out, size_t capacity,
    const wchar_t *source, size_t length);

#endif
