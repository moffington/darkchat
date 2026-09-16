#ifndef DARKCHAT_MARKDOWN_H
#define DARKCHAT_MARKDOWN_H

/* DarkChat Markdown renderer. A pure, platform-independent pass that turns
   assistant message text into styled runs a display layer can emit. The
   supported subset is deliberately small: headings 1-3, bold, italic,
   strikethrough, inline and fenced code, flat lists, HTTP(S) links and
   blockquotes. Unsupported or malformed syntax is kept verbatim. Precedence
   is fenced blocks, then inline code, then links, then strikethrough and
   emphasis. No tables, images, nested lists, per-language highlighting or full
   CommonMark behavior. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* One styled span. Text lives in the document's own buffer; offset and length
   address it, so a run owns no memory of its own. */
typedef struct {
    size_t offset, length;
    bool bold, italic, strike, mono, code, muted;
    int heading;        /* 0 = body text, 1..3 = heading */
} MdRun;

typedef struct {
    wchar_t *text;      /* document-owned, NUL-terminated; newlines are LF */
    size_t length;      /* wchar units, excluding the terminator */
    MdRun *runs;
    int run_count, run_capacity;
} MdDocument;

/* Transactional: on success returns true and *doc owns the rendered
   document; on failure (allocation) returns false and *doc is left zeroed so
   the caller can fall back to verbatim text. */
bool markdown_render(const wchar_t *source, MdDocument *doc);
void markdown_dispose(MdDocument *doc);

/* Test-only hook: while enabled every allocation fails, exercising the
   transactional failure path. */
void markdown_test_fail_allocations(bool enable);

#endif
