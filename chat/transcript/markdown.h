#ifndef DARKCHAT_MARKDOWN_H
#define DARKCHAT_MARKDOWN_H

/* DarkChat Markdown renderer. A pure, platform-independent pass that turns
   assistant message text into styled runs plus one paragraph record per
   paragraph, which a display layer can emit. The supported subset is
   deliberately small: headings 1-3, bold, italic, strikethrough, equal-length
   inline backtick spans and backtick/tilde fenced code, lists (including
   unordered [ ]/[xX] task markers) nested up to MD_MAX_DEPTH with mixed
   markers, blockquotes nested up to MD_MAX_DEPTH, HTTP(S) links rendered as
   their label with the destination recorded separately for the display
   layer, and root-level GFM tables (header plus delimiter row, alignment
   colons, escaped pipes, optional edge pipes, ragged rows normalized to the
   header column count) recorded as MdTable/MdTableRow/MdTableCell metadata
   for the display layer to lay out. Unsupported or malformed syntax is kept
   verbatim. Precedence is fenced blocks, then inline code, then links, then
   strikethrough and emphasis. No images, indented code blocks, lazy
   continuation, per-language highlighting or full CommonMark behavior. */
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
    MD_BLOCK_ITEM,
    MD_BLOCK_TABLE
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
    int table_index;                    /* MD_BLOCK_TABLE -> tables[]; else -1 */
} MdBlock;

/* One rendered fenced code block: the emitted range of a complete fence,
    markers excluded, in document order. A fence emits one MdCodeFence spanning
    every content line and the newline separators between them; the separator
    newline before the fence's first content line is never included, so adjacent
    fences stay distinct records. An empty fence, or one that emits no
    characters at all, records nothing. A blank fence line still emits its
    interior newline when the fence follows preceding text, and that newline is
    part of the range. */
typedef struct {
    size_t offset, length;               /* control/document text range */
} MdCodeFence;

/* GFM-style tables. A recognized table is recorded as one MD_BLOCK_TABLE block
   plus an MdTable, one MdTableRow per source row and one MdTableCell per
   column. Recognized tables emit parsed cell contents into the document buffer
   and record their original normalized source separately in the literals
   arena, so a caller that inspects only the emitted text does not see the
   original source. Callers must inspect the table metadata; until table
   rendering is activated, the display layer deliberately renders the original
   body verbatim. */
#define MD_MAX_TABLE_COLUMNS 24

typedef enum { MD_ALIGN_LEFT = 0, MD_ALIGN_CENTER, MD_ALIGN_RIGHT } MdAlign;

typedef struct { size_t offset, length; } MdTableCell;   /* content range in text */

typedef struct {
    int first_cell, cell_count;     /* range in cells[], exactly `columns` wide */
} MdTableRow;

typedef struct {
    int first_row, row_count;       /* range in rows[], header row included */
    int first_cell, cell_count;     /* range in cells[], all rows back to back */
    int columns;                    /* 1..MD_MAX_TABLE_COLUMNS */
    unsigned char align[MD_MAX_TABLE_COLUMNS];
    size_t literal_offset, literal_length;  /* normalized source slice */
} MdTable;

/* One valid HTTP(S) link. The displayed label is a contiguous range in the
   document text; the destination it opens lives in the document's target
   arena, addressed by offset and length. Each arena entry is NUL-terminated,
   so a destination of any length can be opened directly. Malformed links and
   links with any other scheme are never recorded. */
typedef struct {
    size_t offset, length;               /* label range in text */
    size_t target_offset, target_length; /* destination in the arena, no NUL */
} MdLink;

typedef struct {
    wchar_t *text;      /* document-owned, NUL-terminated; newlines are LF */
    size_t length;      /* wchar units, excluding the terminator */
    MdRun *runs;
    int run_count, run_capacity;
    MdBlock *blocks;
    int block_count, block_capacity;
    MdLink *links;      /* valid links, in document order */
    int link_count, link_capacity;
    wchar_t *targets;   /* arena: every NUL-terminated destination, back to back */
    size_t targets_length, targets_capacity;
    MdTableCell *cells;
    int cell_count, cell_capacity;
    MdTableRow *rows;
    int row_count, row_capacity;
    MdTable *tables;    /* recognized tables, in document order */
    int table_count, table_capacity;
    wchar_t *literals;  /* arena: normalized source slice of each table */
    size_t literals_length, literals_capacity;
    MdCodeFence *fences; /* rendered fenced blocks, in document order */
    int fence_count, fence_capacity;
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
/* Test-only hook: while enabled only link metadata growth fails, so the link
    path is exercised on its own. */
void markdown_test_fail_links(bool enable);
/* Test-only hook: while enabled only fence-record growth fails, so the fence
    path is exercised on its own. A body with no rendered fence allocates
    nothing here, so the hook leaves it unaffected. */
void markdown_test_fail_fences(bool enable);
/* Test-only hooks for the table arenas: while enabled the matching growth
    fails, so each table path is exercised on its own. */
void markdown_test_fail_cells(bool enable);
void markdown_test_fail_rows(bool enable);
void markdown_test_fail_tables(bool enable);
void markdown_test_fail_literals(bool enable);
/* Test-only seam: the number of times the reusable cell-normalization buffer
    had to grow. A table with many equally wide cells needs few allocations. */
void markdown_test_reset_scratch_allocations(void);
int markdown_test_scratch_allocations(void);

#endif
