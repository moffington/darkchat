#include "chat/transcript/table_layout.h"
#include <limits.h>
#include <string.h>

/* Deterministic, allocation-free table layout (TABLES_PLAN.md 5.3).
   A column's natural width is the widest unwrapped cell raised to at least the
   configured minimum; its fixed floor is that minimum. When the natural layout
   does not fit, columns shrink proportionally toward the floor and never below
   it. Integer apportionment uses a widened intermediate so it cannot overflow,
   and picks the same widths for the same inputs, independent of row order.
   Wrapping is greedy at spaces with surrogate-pair-safe hard breaks for
   over-long words, and every wrapped range is lossless. */

static bool is_high_surrogate(wchar_t c) {
    return c >= (wchar_t)0xD800 && c <= (wchar_t)0xDBFF;
}

static bool is_low_surrogate(wchar_t c) {
    return c >= (wchar_t)0xDC00 && c <= (wchar_t)0xDFFF;
}

/* floor(a * b / m) for non-negative inputs, without overflow. */
static long long muldiv_floor(long long a, long long b, long long m) {
    if (a <= 0 || b <= 0 || m <= 0) return 0;
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 wide;
    return (long long)(((wide)a * (wide)b) / (wide)m);
#else
    return (long long)((long double)a * (long double)b / (long double)m);
#endif
}

/* (a * b) % m for non-negative inputs, without overflow. */
static unsigned long long mulmod_u64(unsigned long long a,
    unsigned long long b, unsigned long long m) {
    if (m == 0 || a == 0 || b == 0) return 0;
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 wide;
    return (unsigned long long)(((wide)a * (wide)b) % (wide)m);
#else
    unsigned long long result = 0;
    a %= m;
    while (b) {
        if (b & 1u) {
            result += a;
            if (result >= m) result -= m;
        }
        a <<= 1;
        if (a >= m) a -= m;
        b >>= 1;
    }
    return result;
#endif
}

static int span_length(const TableSpan *span) {
    return span->length > (size_t)INT_MAX ? INT_MAX : (int)span->length;
}

/* Validates every pointer and index range before any dereference. */
static bool input_valid(const TableLayoutInput *in) {
    if (!in || !in->align) return false;
    if (in->columns < 1 || in->columns > MD_MAX_TABLE_COLUMNS) return false;
    if (in->row_count < 0 || in->cell_count < 0 || in->span_count < 0)
        return false;
    if (in->row_count > 0 && !in->rows) return false;
    if (in->cell_count > 0 && !in->cells) return false;
    if (in->span_count > 0 && !in->spans) return false;
    for (int r = 0; r < in->row_count; r++) {
        const TableLayoutRow *row = &in->rows[r];
        if (row->first_cell < 0 || row->cell_count < 0) return false;
        if (row->first_cell > in->cell_count - row->cell_count) return false;
    }
    for (int c = 0; c < in->cell_count; c++) {
        const TableLayoutCell *cell = &in->cells[c];
        if (cell->first_span < 0 || cell->span_count < 0) return false;
        if (cell->first_span > in->span_count - cell->span_count) return false;
    }
    for (int s = 0; s < in->span_count; s++)
        if (in->spans[s].length > 0 && !in->spans[s].text) return false;
    return true;
}

/* Flat character length of a cell: the sum of its span lengths, saturated. */
static int cell_length(const TableLayoutInput *in,
    const TableLayoutCell *cell) {
    long long length = 0;
    for (int k = 0; k < cell->span_count; k++) {
        length += span_length(&in->spans[cell->first_span + k]);
        if (length > INT_MAX) return INT_MAX;
    }
    return (int)length;
}

/* The character at a flat offset in the cell, or NUL past the end. */
static wchar_t flat_char(const TableLayoutInput *in,
    const TableLayoutCell *cell, int index) {
    int base = 0;
    for (int k = 0; k < cell->span_count; k++) {
        const TableSpan *span = &in->spans[cell->first_span + k];
        int length = span_length(span);
        if (index < base + length) return span->text[index - base];
        base += length;
    }
    return L'\0';
}

/* Width of the whole flat sub-range [begin, end) of a cell. The range is split
   at span boundaries so each call to `measure` sees one complete styled
   substring, never a single code unit. */
static int range_width(const TableLayoutInput *in,
    const TableLayoutCell *cell, int begin, int end, TableSpanWidth measure,
    void *user) {
    if (end <= begin) return 0;
    long long total = 0;
    int base = 0;
    for (int k = 0; k < cell->span_count; k++) {
        const TableSpan *span = &in->spans[cell->first_span + k];
        int length = span_length(span);
        long long low = begin > base ? begin : base;
        long long high = end < base + length ? end : base + length;
        if (low < high) {
            TableSpan piece = *span;
            piece.text = span->text + (size_t)(low - base);
            piece.length = (size_t)(high - low);
            int width = measure(user, &piece);
            if (width < 0) width = 0;
            total += width;
            if (total > INT_MAX) return INT_MAX;
        }
        base += length;
    }
    return (int)total;
}

/* Largest end in [start, length] whose whole-range width fits; assumes the
   metric is monotonic in length. */
static int fitting_end(const TableLayoutInput *in,
    const TableLayoutCell *cell, int start, int length, int width_px,
    TableSpanWidth measure, void *user) {
    int lo = start, hi = length, best = start;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (range_width(in, cell, start, mid, measure, user) <= width_px) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

bool table_layout_columns(const TableLayoutInput *in, int available_px,
    int minimum_column_px, int gutter_px, TableSpanWidth measure, void *user,
    TableColumns *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!out || !measure || !input_valid(in)) return false;
    int columns = in->columns;
    int minimum = minimum_column_px < 1 ? 1 : minimum_column_px;
    int gutter = gutter_px < 0 ? 0 : gutter_px;

    /* Natural width: widest cell, raised to the fixed minimum. */
    int natural[MD_MAX_TABLE_COLUMNS];
    for (int c = 0; c < columns; c++) natural[c] = minimum;
    for (int r = 0; r < in->row_count; r++) {
        const TableLayoutRow *row = &in->rows[r];
        int count = row->cell_count < columns ? row->cell_count : columns;
        for (int c = 0; c < count; c++) {
            const TableLayoutCell *cell = &in->cells[row->first_cell + c];
            int width = range_width(in, cell, 0, cell_length(in, cell),
                measure, user);
            if (width > natural[c]) natural[c] = width;
        }
    }

    long long natural_sum = 0;
    for (int c = 0; c < columns; c++) natural_sum += natural[c];
    long long gutter_total = (long long)gutter * (columns - 1);
    long long available = available_px > 0 ? available_px : 0;
    long long floor_total = (long long)minimum * columns;
    if (floor_total + gutter_total > available) return false;

    int width[MD_MAX_TABLE_COLUMNS];
    if (natural_sum + gutter_total <= available) {
        /* Compact: slack stays unused, every column keeps its natural width. */
        for (int c = 0; c < columns; c++) width[c] = natural[c];
    } else {
        /* Shrink proportionally from natural toward the fixed floor. */
        long long budget = available - gutter_total;      /* >= floor_total */
        long long capacity_sum = natural_sum - floor_total;
        long long excess = natural_sum - budget;
        int reduction[MD_MAX_TABLE_COLUMNS];
        unsigned long long remainder[MD_MAX_TABLE_COLUMNS];
        long long applied = 0;
        for (int c = 0; c < columns; c++) {
            long long capacity = natural[c] - minimum;
            if (capacity <= 0) {
                reduction[c] = 0;
                remainder[c] = 0;
                continue;
            }
            reduction[c] = (int)muldiv_floor(capacity, excess, capacity_sum);
            remainder[c] = mulmod_u64((unsigned long long)capacity,
                (unsigned long long)excess, (unsigned long long)capacity_sum);
            applied += reduction[c];
        }
        /* Largest-remainder rounding, lowest column index winning ties. */
        long long leftover = excess - applied;
        while (leftover > 0) {
            int best = -1;
            for (int c = 0; c < columns; c++) {
                if (natural[c] - minimum - reduction[c] <= 0) continue;
                if (best < 0 || remainder[c] > remainder[best]) best = c;
            }
            if (best < 0) break;
            reduction[best]++;
            remainder[best] = 0;
            leftover--;
        }
        for (int c = 0; c < columns; c++) width[c] = natural[c] - reduction[c];
    }

    long long anchor = 0;
    for (int c = 0; c < columns; c++) {
        out->width[c] = width[c];
        out->x[c] = (int)anchor;
        anchor += width[c];
        if (c + 1 < columns) anchor += gutter;
    }
    out->column_count = columns;
    out->total_width = (int)anchor;
    return true;
}

int table_layout_anchor(const TableColumns *columns, int column,
    unsigned char align) {
    if (!columns || column < 0 || column >= columns->column_count ||
        columns->column_count > MD_MAX_TABLE_COLUMNS) return 0;
    int x = columns->x[column];
    int width = columns->width[column];
    switch (align) {
    case MD_ALIGN_CENTER: return x + width / 2;
    case MD_ALIGN_RIGHT:  return x + width;
    default:              return x;
    }
}

/* O(requested cell) validation for the per-cell entry points. Validating the
   whole table on every cell would make a caller that walks every cell
   quadratic in the table size; this checks only the ranges the requested row
   and column actually dereference, with the same rejections. */
static bool cell_input_valid(const TableLayoutInput *in, int row, int column) {
    if (!in || !in->align) return false;
    if (in->columns < 1 || in->columns > MD_MAX_TABLE_COLUMNS) return false;
    /* Reject negative aggregate counts before any bounds arithmetic. */
    if (in->row_count < 0 || in->cell_count < 0 || in->span_count < 0)
        return false;
    if (!in->rows || row < 0 || row >= in->row_count) return false;
    const TableLayoutRow *entry = &in->rows[row];
    if (entry->first_cell < 0 || entry->cell_count < 0) return false;
    if (!in->cells || entry->first_cell > in->cell_count - entry->cell_count)
        return false;
    if (column < 0 || column >= in->columns || column >= entry->cell_count)
        return false;
    const TableLayoutCell *cell = &in->cells[entry->first_cell + column];
    if (cell->first_span < 0 || cell->span_count < 0) return false;
    if (cell->span_count > 0 && !in->spans) return false;
    if (cell->first_span > in->span_count - cell->span_count) return false;
    for (int k = 0; k < cell->span_count; k++) {
        const TableSpan *span = &in->spans[cell->first_span + k];
        if (span->length > 0 && !span->text) return false;
    }
    return true;
}

int table_layout_cell_breaks(const TableLayoutInput *in, int row, int column,
    int width_px, TableSpanWidth measure, void *user, TableCellLine *lines,
    int capacity) {
    if (!measure || !cell_input_valid(in, row, column)) return 0;
    const TableLayoutRow *entry = &in->rows[row];
    const TableLayoutCell *cell = &in->cells[entry->first_cell + column];
    int length = cell_length(in, cell);
    if (length == 0) {
        if (lines && capacity > 0) {
            lines[0].start = 0;
            lines[0].end = 0;
        }
        return 1;
    }
    if (width_px < 0) width_px = 0;

    int start = 0;
    int count = 0;
    while (start < length) {
        if (count == TABLE_MAX_PHYSICAL_LINES) return -1;
        int fit = fitting_end(in, cell, start, length, width_px, measure, user);
        int next;
        if (fit >= length) {
            next = length;
        } else {
            /* Never cut between a high and low surrogate. */
            if (fit > start && is_high_surrogate(flat_char(in, cell, fit - 1)) &&
                is_low_surrogate(flat_char(in, cell, fit)))
                fit--;
            int last_space = -1;
            for (int j = start; j < fit; j++)
                if (flat_char(in, cell, j) == L' ') last_space = j;
            if (last_space >= start) {
                next = last_space + 1;
            } else {
                if (fit <= start) {
                    /* Not even one character fits: take one whole character. */
                    fit = start + 1;
                    if (is_high_surrogate(flat_char(in, cell, start)) &&
                        fit < length && is_low_surrogate(flat_char(in, cell, fit)))
                        fit++;
                }
                next = fit;
            }
            if (next <= start) next = start + 1;   /* always make progress */
        }
        if (lines && count < capacity) {
            lines[count].start = start;
            lines[count].end = next;
        }
        count++;
        start = next;
    }
    return count;
}

int table_layout_cell_lines(const TableLayoutInput *in, int row, int column,
    int width_px, TableSpanWidth measure, void *user) {
    return table_layout_cell_breaks(in, row, column, width_px, measure, user,
        NULL, 0);
}
