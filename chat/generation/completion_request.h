#ifndef DARKCHAT_COMPLETION_REQUEST_H
#define DARKCHAT_COMPLETION_REQUEST_H

/* Pure, backend-aware OpenAI-compatible request construction. Both the
   transport (chat/generation/completion_winhttp.c) and the request-context budget
   (chat/generation/context.c) derive the exact body bytes here, so the body the budget
   measures is structurally the body the encoder writes and the two cannot
   drift. No allocation beyond the caller's JsonBuf, no Win32.

   The two hard-coded envelopes are:
     OpenRouter: {"model":M,"messages":[...],"stream":true,
                  ["reasoning":{"enabled":true},]["provider":{...}]}
     Ollama:     {"model":M,"messages":[...],"stream":true,
                  "stream_options":{"include_usage":true}}
   OpenRouter's reasoning object is emitted only when `reasoning` is true; a
   suppressed request omits it entirely. Ollama never carries reasoning or
   provider-routing objects. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "chat/core/chat.h"
#include "chat/json.h"
#include "chat/generation/provider_routing.h"

/* One separator precedes every message after the first. */
#define CHAT_COMPLETION_SEPARATOR_BYTES 1u

/* The exact size of the framing around the messages: the opening object, the
   model string and the trailing options/closing brace, including the optional
   OpenRouter reasoning and provider objects when configured. The context
   budget charges this once. `routing` and `reasoning` are ignored for Ollama.
   Pure and total. */
size_t chat_completion_envelope_bytes(ChatBackend backend, const wchar_t *model,
    const ChatProviderRouting *routing, bool reasoning);

/* The exact bytes one TEXT-ONLY message contributes, excluding the separator
   before it: the string-content framing plus the quoted projection. This is
   the fast path of the split calculator below. */
size_t chat_completion_text_message_bytes(ChatRole role, const wchar_t *text);

/* Framing around a message's content terms: the message object with the role
   name and the fixed content framing, plus -- for a content ARRAY -- the
   brackets and one comma per term after the first. `part_count == 0` is the
   string fast path (no brackets: the content is one JSON string); every
   positive count, including a single image, is an array and pays the two
   brackets. Never counts any term itself. */
size_t chat_completion_message_frame_bytes(ChatRole role, int part_count);

/* One serialized content term, whole object, no separator:
   a TEXT term is {"type":"text","text":<string>}; an IMAGE term is the
   per-backend image part around a "data:<mime>;base64,<payload>" URL. The
   image framing literals carry the URL's own quotes, so the payload inside is
   charged unquoted (json_base64_payload_size) -- never the quoted form. */
size_t chat_completion_text_part_bytes(const wchar_t *text);
size_t chat_completion_image_part_bytes(ChatBackend backend,
    const char *mime, size_t raw_bytes);

/* The exact byte split of one message's contribution, excluding the
   separator before it. `text_bytes` is the framing, array syntax and TEXT
   terms; `attachment_bytes` is the IMAGE terms only. The sum is the total.
   Fast path (parts == NULL or part_count <= 0): attachment_bytes is 0 and
   text_bytes equals chat_completion_text_message_bytes(role, m->text).
   A run: message_frame_bytes(role, part_count) plus one term per part.
   IMAGE entries must carry a resolved rec (for the MIME) and byte_length;
   the function never reads blob bytes. Pure and total. */
typedef struct {
    size_t text_bytes;
    size_t attachment_bytes;
} ChatMessageCost;
ChatMessageCost chat_completion_message_costs(ChatBackend backend,
    ChatRole role, const ChatRequestMessage *m);

/* Encodes the complete body into `buf` (initialized here). Error-role entries
   are skipped exactly as before. A message whose view carries a part run
   encodes `content` as an array mirroring the run in order (an image-only
   message is exactly its image parts -- no synthetic text part); every other
   message keeps the plain JSON string content. `reasoning` gates OpenRouter's
   reasoning object and is ignored for Ollama. Returns false on allocation
   failure and on an untrustworthy image term (a missing attachment record,
   or anything short of real bytes to encode: no payload pointer, or a
   zero-length payload -- an empty image is never sent). */
bool chat_completion_request_build(JsonBuf *buf, ChatBackend backend,
    const wchar_t *model, const ChatRequestMessage *messages, int count,
    const ChatProviderRouting *routing, bool reasoning);

#endif
