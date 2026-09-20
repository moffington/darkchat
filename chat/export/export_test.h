#ifndef DARKCHAT_EXPORT_TEST_H
#define DARKCHAT_EXPORT_TEST_H

/* Test-only seams for the export size cap. These are deliberately absent from
   the production header (chat/export/export.h) so host code cannot bypass
   CHAT_EXPORT_LIMIT. They implement the same contract as the public
   serializers with an explicit byte limit, so an over-limit export is testable
   without allocating the full cap. */
#include "chat/export/export.h"

bool chat_export_json_limited(const Chat *chat, int conversation, bool all,
    int64_t exported_at, size_t limit, JsonBuf *out);
bool chat_export_markdown_limited(const Chat *chat, int conversation, bool all,
    int64_t exported_at, size_t limit, JsonBuf *out);

#endif
