#ifndef DARKCHAT_MARKDOWN_H
#define DARKCHAT_MARKDOWN_H

/* DarkChat Markdown renderer. A pure, platform-independent pass that turns
   assistant message text into styled runs plus one paragraph record per
   paragraph, which a display layer can emit. The supported subset is
   deliberately small: headings 1-3, bold, italic, strikethrough, equal-length
   inline backtick spans and backtick/tilde fenced code, lists (including
   unordered [ ]/[xX] task markers) nested up to MD_MAX_DEPTH with mixed
   markers, blockquotes nested up to MD_MAX_DEPTH, HTTP(S) links and
   blockquotes. Unsupported or malformed syntax is kept verbatim. Precedence is
   fenced blocks, then inline code, then links, then strikethrough and
   emphasis. No tables, images, indented code blocks, lazy continuation,
   per-language highlighting or full CommonMark behavior. */
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

/* One paragraph's metadata. Blocks are a second layer over the same buffer:
    runs carry character style, blocks carry paragraph layout and nesting. A
    block covers one paragraph, excluding its terminating LF; blank paragraphs
    are not recorded. */
#define MD_MAX_DEPTH 8          /* quote levels and list levels, independently */
#define MD_MAX_INDENT 32        /* layout columns; indentation past it is kept
                                   verbatim in the text */
#define MD_QUOTE_GLYPH_COLS 2   /* "\u258C " per quote level */
#define MD_ROOT_PREFIX_COLS 3   /* root-level tolerance before a block prefix */

typedef enum {
    MD_BLOCK_PARAGRAPH = 0,
    MD_BLOCK_CODE,
    MD_BLOCK_QUOTE,
    MD_BLOCK_ITEM
} MdBlockKind;

typedef enum {
    MD_FLAG_ORDERED     = 1u << 0,
    MD_FLAG_TASK        = 1u << 1,
    MD_FLAG_CHECKED     = 1u << 2,
    MD_FLAG_CONTINUATION = 1u << 3
} MdBlockFlags;

typedef struct {
    size_t offset, length;      /* paragraph range; terminating LF excluded */
    unsigned char kind;         /* MdBlockKind */
    unsigned char quote_depth;  /* 0..MD_MAX_DEPTH */
    unsigned char list_depth;   /* 0..MD_MAX_DEPTH */
    unsigned char flags;        /* MdBlockFlags */
    unsigned char first_indent;         /* column where the visible prefix begins */
    unsigned char continuation_indent;  /* column where content and wrapped
                                           lines begin, >= first_indent */
} MdBlock;

typedef struct {
    wchar_t *text;      /* document-owned, NUL-terminated; newlines are LF */
    size_t length;      /* wchar units, excluding the terminator */
    MdRun *runs;
    int run_count, run_capacity;
    MdBlock *blocks;
    int block_count, block_capacity;
} MdDocument;

/* Transactional: on success returns true and *doc owns the rendered
   document; on failure (allocation) returns false and *doc is left zeroed so
   the caller can fall back to verbatim text. */
bool markdown_render(const wchar_t *source, MdDocument *doc);
void markdown_dispose(MdDocument *doc);

/* Test-only hook: while enabled every allocation fails, exercising the
    transactional failure path. */
void markdown_test_fail_allocations(bool enable);
/* Test-only hook: while enabled only block-array growth fails, so the block
    path is exercised on its own. */
void markdown_test_fail_blocks(bool enable);

#endif
