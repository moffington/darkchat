#ifndef DARKCHAT_MARKDOWN_H
#define DARKCHAT_MARKDOWN_H

/* DarkChat Markdown renderer. A pure, platform-independent pass that turns
   assistant message text into styled runs a display layer can emit. The
   supported subset is deliberately small: headings 1-3, bold, italic,
   strikethrough, equal-length inline backtick spans and fenced code, flat
   lists (including unordered [ ]/[xX] task markers), HTTP(S) links and
   blockquotes. Unsupported or
   malformed syntax is kept verbatim. Precedence is fenced blocks, then inline
   code, then links, then strikethrough and emphasis. No tables, images, nested
   lists, per-language highlighting or full CommonMark behavior. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef enum {
    MD_STYLE_BOLD   = 1u << 0,
    MD_STYLE_ITALIC = 1u << 1,
    MD_STYLE_STRIKE = 1u << 2,
    MD_STYLE_MONO   = 1u << 3,
    MD_STYLE_CODE   = 1u << 4,
    MD_STYLE_MUTED  = 1u << 5
} MdStyleFlags;

/* One styled span. Text lives in the document's own buffer; offset and length
   address it, so a run owns no memory of its own. */
typedef struct {
    size_t offset, length;
    unsigned style;
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
