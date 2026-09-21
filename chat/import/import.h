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

#endif
