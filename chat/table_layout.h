#ifndef DARKCHAT_TABLE_LAYOUT_H
#define DARKCHAT_TABLE_LAYOUT_H

/* Pure, platform-independent GFM table-layout primitive (TABLES_PLAN.md
   Phase 2). It resolves deterministic column widths and pixel anchors from
   styled cell content and wraps a cell into lossless physical-line ranges. It
   performs no Win32, DPI or Rich Edit work: the Win32 adapter scales the DIP
   constants and passes device pixels, and only that layer encodes anchors into
   twip tab stops. The module owns no memory and allocates nothing; every
   structure is an index into caller-owned arrays, which are range-validated
   before use. */

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "markdown.h"

/* Calibrated by the Phase 0 spike (tests/test_richedit_tabs.c) and provisional
   until Phase 3 benchmarks the real flattening path. The two DIP values are
   configuration: the adapter scales them before calling this module (I11).
   TABLE_MAX_PHYSICAL_LINES is a per-cell guard only; it is NOT the final table
   resource limit. Phase 3 must enforce the limit across the whole table by
   summing, per row, the maximum cell-line count over that row's columns, and
   render the table literally when the total exceeds the cap. */
#define TABLE_MIN_COLUMN_DIP       48
#define TABLE_GUTTER_DIP           16
#define TABLE_MAX_PHYSICAL_LINES   4096
#define MD_MAX_FLATTEN_CHARS       262144

/* One styled substring of a cell. `text` points into the caller's buffer and
   stays owned by the caller. `style` carries only the metric-relevant
   MdStyleFlags (MONO, BOLD, ITALIC); `heading` is 0..3. */
typedef struct {
    const wchar_t *text;
    size_t length;
    unsigned char style;
    unsigned char heading;
} TableSpan;

/* Device-pixel width of exactly this styled substring, measured as a whole.
   The adapter selects the face matching the span's style and measures it; this
   module never measures text itself and never sums per-character widths (I8). */
typedef int (*TableSpanWidth)(void *user, const TableSpan *span);

/* One cell: a contiguous range of spans in TableLayoutInput.spans. A cell with
   no content has span_count == 0 and still occupies one physical line. */
typedef struct {
    int first_span, span_count;
} TableLayoutCell;

/* One row: a range in cells[], exactly `columns` wide after normalization. */
typedef struct {
    int first_cell, cell_count;
} TableLayoutRow;

/* The whole table in flat, caller-owned arrays. `align` holds one MD_ALIGN_*
   per column. Rows and cells are index ranges, never pointers. Every range is
   validated (bounds, negatives, NULL bases) before any dereference. */
typedef struct {
    const TableSpan *spans;
    int span_count;
    const TableLayoutCell *cells;
    int cell_count;
    const TableLayoutRow *rows;
    int row_count;
    int columns;                    /* 1..MD_MAX_TABLE_COLUMNS */
    const unsigned char *align;     /* MD_ALIGN_* per column */
} TableLayoutInput;

typedef struct {
    int column_count;
    int x[MD_MAX_TABLE_COLUMNS];        /* left-edge pixel anchor per column */
    int width[MD_MAX_TABLE_COLUMNS];    /* resolved content width per column */
    int total_width;                    /* last x + last width */
} TableColumns;

/* One wrapped physical line of a cell: the flat range [start, end) of code
   units it displays. Lines tile the cell with no gaps or overlaps, so
   concatenating the ranges reproduces every code unit exactly, repeated spaces
   included. */
typedef struct {
    int start, end;
} TableCellLine;

/* Resolves column widths and left anchors. `available_px` is the usable body
   width; `minimum_column_px` and `gutter_px` are the DIP constants already
   scaled to device pixels. Each column's natural width is the measured cell
   width raised to at least `minimum_column_px`, and its fixed floor is
   `minimum_column_px`, so empty and short columns keep the minimum. When the
   natural layout does not fit the columns shrink proportionally toward that
   floor; the call fails (zeroing `out`) when every column's fixed floor plus
   the gutters cannot fit. */
bool table_layout_columns(const TableLayoutInput *in, int available_px,
    int minimum_column_px, int gutter_px, TableSpanWidth measure, void *user,
    TableColumns *out);

/* The tab-stop anchor for a column at the given alignment: its left edge,
   center or right edge (TABLES_PLAN.md 5.1). Returns 0 for an out-of-range
   column. The adapter converts the pixel result to twips; no encoding here. */
int table_layout_anchor(const TableColumns *columns, int column,
    unsigned char align);

/* Number of physical lines a cell occupies when wrapped to `width_px`, always
   at least 1 for a valid cell. Returns 0 for an invalid input or an
   out-of-range row or column, and -1 when the wrapped line count would exceed
   TABLE_MAX_PHYSICAL_LINES. */
int table_layout_cell_lines(const TableLayoutInput *in, int row, int column,
    int width_px, TableSpanWidth measure, void *user);

/* Like table_layout_cell_lines but also writes each line's [start, end) flat
   range into `lines` (at most `capacity` entries) so a caller can emit the
   wrapped content. The ranges tile the cell, so concatenating them reproduces
   the cell exactly. The returned line count may exceed `capacity`. */
int table_layout_cell_breaks(const TableLayoutInput *in, int row, int column,
    int width_px, TableSpanWidth measure, void *user, TableCellLine *lines,
    int capacity);

#endif
