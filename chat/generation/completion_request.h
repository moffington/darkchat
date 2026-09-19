#ifndef DARKCHAT_COMPLETION_REQUEST_H
#define DARKCHAT_COMPLETION_REQUEST_H

/* Pure, backend-aware OpenAI-compatible request construction. Both the
   transport (chat/generation/completion_winhttp.c) and the request-context budget
   (chat/generation/context.c) derive the exact body bytes here, so the body the budget
   measures is structurally the body the encoder writes and the two cannot
   drift. No allocation beyond the caller's JsonBuf, no Win32.

   The two hard-coded envelopes are:
     OpenRouter: {"model":M,"messages":[...],"stream":true,
                  "reasoning":{"enabled":true}[,"provider":{...}]}
     Ollama:     {"model":M,"messages":[...],"stream":true,
                  "stream_options":{"include_usage":true}}
   OpenRouter's bytes are unchanged from the historical encoder; Ollama never
   carries OpenRouter's reasoning or provider-routing objects. */
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
   OpenRouter provider object when configured. The context budget charges this
   once. `routing` is ignored for Ollama. Pure and total. */
size_t chat_completion_envelope_bytes(ChatBackend backend, const wchar_t *model,
    const ChatProviderRouting *routing);

/* The exact bytes one message contributes, excluding the separator before it.
   Mirrors the encoder's framing and role names. */
size_t chat_completion_message_bytes(ChatRole role, const wchar_t *text);

/* Encodes the complete body into `buf` (initialized here). Error-role entries
   are skipped exactly as before. Returns false only on allocation failure. */
bool chat_completion_request_build(JsonBuf *buf, ChatBackend backend,
    const wchar_t *model, const ChatRequestMessage *messages, int count,
    const ChatProviderRouting *routing);

#endif
