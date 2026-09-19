#ifndef DARKCHAT_PROVIDER_ROUTING_H
#define DARKCHAT_PROVIDER_ROUTING_H

/* Pure OpenRouter provider-routing serialization. Both the request builder
   (chat/generation/completion_request.c) and the request-context budget
   (chat/generation/context.c) derive the exact `provider` object bytes here, so the body
   the budget measures is structurally the body the encoder writes and the two
   cannot drift. The object is OpenRouter-only: the Ollama envelope never
   carries it. No allocation, no Win32. */
#include <stdbool.h>
#include <stddef.h>
#include "chat/core/chat.h"
#include "chat/json.h"

/* Sets the OpenRouter defaults (the all-zero representation): balanced load
   balancing, fallbacks allowed, data collection allowed, no ZDR requirement. */
void chat_provider_routing_init(ChatProviderRouting *routing);

/* The exact byte count chat_provider_append writes for this routing,
   including the leading comma; 0 when every control is at its default (no
   provider object is sent). Pure, allocation-free and total: it is computed
   by summing the same fragments the appender emits, so it has no failure path
   and can never report a smaller size than what is written. Tolerates NULL
   (0). */
size_t chat_provider_envelope_bytes(const ChatProviderRouting *routing);

/* Appends `,"provider":{...}` when any control differs from OpenRouter's
   default and nothing when all are default, preserving OpenRouter defaults.
   Returns false only on a JsonBuf allocation failure. Tolerates NULL (appends
   nothing). */
bool chat_provider_append(JsonBuf *buf, const ChatProviderRouting *routing);

#endif
