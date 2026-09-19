#ifndef DARKCHAT_SEARCH_H
#define DARKCHAT_SEARCH_H

/* Transient conversation search. Results own no Chat pointers and identify a
   matched message only by its persisted conversation/message ids. The result
   array and query are dynamically owned, so their storage has no dependency on
   the current conversation or message limits. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "chat/core/chat.h"

#define CHAT_SEARCH_SNIPPET 96

typedef enum {
    CHAT_SEARCH_BODY,
    CHAT_SEARCH_REASONING
} ChatSearchField;

typedef struct {
    uint64_t conversation_id, message_id, message_revision;
    ChatSearchField field;
    ChatRole role;
    size_t match_offset, match_length;
    /* At most 95 UTF-16 code units, with CR/LF/tab flattened and "..."
       marking omitted context. It is display context, not source text. */
    wchar_t snippet[CHAT_SEARCH_SNIPPET];
} ChatSearchResult;

typedef struct {
    wchar_t *query;
    ChatSearchResult *items;
    size_t count, capacity;
} ChatSearchResults;

typedef struct {
    bool include_reasoning;
} ChatSearchOptions;

typedef struct {
    int conversation, message;
    ChatSearchField field;
} ChatSearchTarget;

/* Searches every live message body, including local, error, partial and failed
   messages. Reasoning is searched only when requested; titles, drafts and the
   global system prompt are not conversation-message results. Matching is an
   ordinal, locale-independent Unicode case-insensitive substring comparison:
   canonically different spellings and length-changing folds are distinct.
   Each matching field contributes its first match, in conversation/message
   order with body before reasoning.

   Empty queries successfully replace `out` with an empty result set. A failed
   allocation leaves `out` unchanged. */
bool chat_search_build(const Chat *chat, const wchar_t *query,
    ChatSearchOptions options, ChatSearchResults *out);

/* Resolves a retained result against current state. It fails after the matched
   message is deleted, replaced, or observably edited, but survives unrelated
   message growth and conversation compaction because indices are rediscovered
   from stable ids. */
bool chat_search_resolve(const Chat *chat, const ChatSearchResults *results,
    size_t index, ChatSearchTarget *out);

void chat_search_results_dispose(ChatSearchResults *results);

#endif
