/* Pure table-layout primitive tests; no Win32, like test_markdown. */
#include "chat/transcript/table_layout.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); ++failures; }
}

/* Fake metric adapter: normal glyphs are 8 px, bold glyphs 10 px. Width is
   therefore length * unit and every expectation below is exact. */
static int fake_width(void *user, const TableSpan *span) {
    (void)user;
    int unit = (span->style & MD_STYLE_BOLD) ? 10 : 8;
    return (int)span->length * unit;
}

/* Non-additive metric: a multi-glyph range is kerned, so measuring glyphs one
   at a time overcounts. width(n) = 8n - (n - 1) = 7n + 1. */
static int kerning_width(void *user, const TableSpan *span) {
    (void)user;
    if (span->length == 0) return 0;
    return (int)span->length * 8 - (int)(span->length - 1);
}

/* Senselessly wide metric, to force the apportionment intermediates to need
   more than 64 bits. */
static int huge_width(void *user, const TableSpan *span) {
    (void)user;
    return span->length == 0 ? 0 : INT_MAX;
}

typedef struct {
    TableSpan spans[512];
    int span_count;
    TableLayoutCell cells[64];
    int cell_count;
    TableLayoutRow rows[16];
    int row_count;
    unsigned char align[MD_MAX_TABLE_COLUMNS];
    TableLayoutInput in;
} Fixture;

static void reset(Fixture *f, int columns) {
    memset(f, 0, sizeof *f);
    f->in.spans = f->spans;
    f->in.cells = f->cells;
    f->in.rows = f->rows;
    f->in.align = f->align;
    f->in.columns = columns;
}

static void sync_fixture(Fixture *f) {
    f->in.span_count = f->span_count;
    f->in.cell_count = f->cell_count;
    f->in.row_count = f->row_count;
}

static void add_cell(Fixture *f, const wchar_t *text, unsigned style) {
    TableLayoutCell *cell = &f->cells[f->cell_count++];
    cell->first_span = f->span_count;
    cell->span_count = 1;
    TableSpan *span = &f->spans[f->span_count++];
    span->text = text;
    span->length = wcslen(text);
    span->style = (unsigned char)style;
    span->heading = 0;
}

static void add_row(Fixture *f, int count) {
    TableLayoutRow *row = &f->rows[f->row_count++];
    row->first_cell = f->cell_count - count;
    row->cell_count = count;
}

static void expect_columns(const TableColumns *c, int count, const int *width,
    const int *x, int total, const char *what) {
    check(c->column_count == count, what);
    if (c->column_count != count) return;
    for (int i = 0; i < count; i++) {
        check(c->width[i] == width[i], what);
        check(c->x[i] == x[i], what);
    }
    check(c->total_width == total, what);
}

/* The flat code unit at an offset within a fixture cell. */
static wchar_t flat_at(const Fixture *f, int row, int column, int index) {
    const TableLayoutRow *r = &f->rows[row];
    const TableLayoutCell *c = &f->cells[r->first_cell + column];
    int base = 0;
    for (int k = 0; k < c->span_count; k++) {
        const TableSpan *s = &f->spans[c->first_span + k];
        int length = (int)s->length;
        if (index < base + length) return s->text[index - base];
        base += length;
    }
    return L'\0';
}

/* Concatenating the returned line ranges must tile the cell and reproduce the
   source exactly, with no skipped or duplicated code units. */
static void expect_lossless(Fixture *f, int row, int column, int width_px,
    TableSpanWidth measure, const wchar_t *source, const char *what) {
    TableCellLine lines[32];
    int count = table_layout_cell_breaks(&f->in, row, column, width_px, measure,
        NULL, lines, 32);
    check(count > 0, what);
    if (count <= 0) return;
    wchar_t rebuilt[256] = {0};
    int n = 0;
    bool ok = n < 255;
    for (int i = 0; i < count && ok; i++) {
        if (lines[i].start != n) ok = false;    /* a gap or an overlap */
        for (int j = lines[i].start; j < lines[i].end && ok; j++) {
            if (n >= 255) { ok = false; break; }
            rebuilt[n++] = flat_at(f, row, column, j);
        }
    }
    if (ok) rebuilt[n] = 0;
    check(ok && n == (int)wcslen(source) && !wcscmp(rebuilt, source), what);
}

int main(void) {
    check(TABLE_MIN_COLUMN_DIP == 48, "minimum column DIP is calibrated");
    check(TABLE_GUTTER_DIP == 16, "gutter DIP is calibrated");
    check(TABLE_MAX_PHYSICAL_LINES == 512, "physical-line cap finalized");
    check(MD_MAX_FLATTEN_CHARS == 262144, "flatten char cap finalized");

    { /* Preferred layout fits: slack stays unused, single column. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"abc", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "preferred fit succeeds");
        const int w[1] = {24}, x[1] = {0};
        expect_columns(&cols, 1, w, x, 24, "single column at preferred width");
    }

    { /* Empty and short columns keep the fixed minimum. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"a", 0);
        add_cell(&f, L"", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "short cells lay out");
        const int w[2] = {16, 16}, x[2] = {0, 24};
        expect_columns(&cols, 2, w, x, 40, "short and empty columns use minimum");
    }

    { /* Two columns, gutter reserved between them. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"ab", 0);
        add_cell(&f, L"cdef", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 200, 16, 8, fake_width, NULL, &cols),
            "two columns fit");
        const int w[2] = {16, 32}, x[2] = {0, 24};
        expect_columns(&cols, 2, w, x, 56, "gutter reserved between columns");
    }

    { /* Proportional shrink from natural toward the fixed floor. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"aaaaaaaaaa", 0);     /* natural 80 */
        add_cell(&f, L"aaaaa", 0);          /* natural 40 */
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "shrink path succeeds");
        const int w[2] = {60, 32}, x[2] = {0, 68};
        expect_columns(&cols, 2, w, x, 100, "proportional shrink uses budget");
    }

    { /* A short column sits at the floor while a wide one shrinks. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"aaaaaaaaaa", 0);     /* natural 80 */
        add_cell(&f, L"a", 0);              /* natural 16 (minimum) */
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 56, 16, 8, fake_width, NULL, &cols),
            "mixed natural shrink succeeds");
        const int w[2] = {32, 16}, x[2] = {0, 40};
        expect_columns(&cols, 2, w, x, 56, "short column retains minimum");
    }

    { /* Every column at its floor is the exact fit boundary. */
        Fixture f;
        reset(&f, 3);
        add_cell(&f, L"aaaaaaaaaa", 0);
        add_cell(&f, L"aaaaaaaaaa", 0);
        add_cell(&f, L"aaaaaaaaaa", 0);
        add_row(&f, 3);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 40, 10, 5, fake_width, NULL, &cols),
            "exact floor fit succeeds");
        const int w[3] = {10, 10, 10}, x[3] = {0, 15, 30};
        expect_columns(&cols, 3, w, x, 40, "every column pinned to its floor");
        check(!table_layout_columns(&f.in, 39, 10, 5, fake_width, NULL, &cols),
            "one pixel under the floors fails");
    }

    { /* Minimum floors cannot fit: signal by returning false and zeroing. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"aaaaa", 0);
        add_cell(&f, L"aaaaa", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        cols.column_count = 7;
        check(!table_layout_columns(&f.in, 80, 48, 16, fake_width, NULL, &cols),
            "fixed floors that do not fit are rejected");
        check(cols.column_count == 0 && cols.width[0] == 0,
            "failed layout zeroes the result");
    }

    { /* Row order does not change column widths. */
        Fixture a, b;
        reset(&a, 2);
        add_cell(&a, L"aa", 0);
        add_cell(&a, L"b", 0);
        add_row(&a, 2);
        add_cell(&a, L"c", 0);
        add_cell(&a, L"dddd", 0);
        add_row(&a, 2);
        sync_fixture(&a);
        reset(&b, 2);
        add_cell(&b, L"c", 0);
        add_cell(&b, L"dddd", 0);
        add_row(&b, 2);
        add_cell(&b, L"aa", 0);
        add_cell(&b, L"b", 0);
        add_row(&b, 2);
        sync_fixture(&b);
        TableColumns first, second;
        check(table_layout_columns(&a.in, 60, 16, 8, fake_width, NULL, &first) &&
            table_layout_columns(&b.in, 60, 16, 8, fake_width, NULL, &second),
            "both row orders lay out");
        const int w[2] = {16, 32}, x[2] = {0, 24};
        expect_columns(&first, 2, w, x, 56, "row order A");
        expect_columns(&second, 2, w, x, 56, "row order B is identical");
    }

    { /* Alignment anchors derive from the left anchor and width. */
        Fixture f;
        reset(&f, 3);
        add_cell(&f, L"aaaaaaaaaa", 0);     /* width 80 */
        add_cell(&f, L"aaaaaaaaaaaa", 0);   /* width 96 */
        add_cell(&f, L"aaaaaaaa", 0);       /* width 64 */
        add_row(&f, 3);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 1000, 16, 8, fake_width, NULL, &cols),
            "three columns fit");
        const int w[3] = {80, 96, 64}, x[3] = {0, 88, 192};
        expect_columns(&cols, 3, w, x, 256, "three column anchors");
        check(table_layout_anchor(&cols, 0, MD_ALIGN_LEFT) == 0, "left anchor 0");
        check(table_layout_anchor(&cols, 0, MD_ALIGN_CENTER) == 40, "center anchor 0");
        check(table_layout_anchor(&cols, 0, MD_ALIGN_RIGHT) == 80, "right anchor 0");
        check(table_layout_anchor(&cols, 1, MD_ALIGN_CENTER) == 136, "center anchor 1");
        check(table_layout_anchor(&cols, 1, MD_ALIGN_RIGHT) == 184, "right anchor 1");
        check(table_layout_anchor(&cols, 2, MD_ALIGN_CENTER) == 224, "center anchor 2");
        check(table_layout_anchor(&cols, 2, MD_ALIGN_RIGHT) == 256, "right anchor 2");
        check(table_layout_anchor(&cols, 9, MD_ALIGN_LEFT) == 0, "anchor out of range");
    }

    { /* Hard wrap without spaces splits exactly at the width. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"abcdefghij", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        check(table_layout_cell_lines(&f.in, 0, 0, 40, fake_width, NULL) == 2,
            "ten glyphs wrap to two lines");
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 8) == 2, "hard wrap count");
        check(lines[0].start == 0 && lines[0].end == 5 &&
            lines[1].start == 5 && lines[1].end == 10, "hard wrap ranges");
    }

    { /* Greedy wrap breaks at the last space that fits. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"ab cd", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        check(table_layout_cell_breaks(&f.in, 0, 0, 24, fake_width, NULL,
            lines, 8) == 2, "space wrap count");
        check(lines[0].start == 0 && lines[0].end == 3 &&
            lines[1].start == 3 && lines[1].end == 5, "space wrap ranges");
    }

    { /* An over-long word hard-breaks at the column width. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"abcdef", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        check(table_layout_cell_breaks(&f.in, 0, 0, 24, fake_width, NULL,
            lines, 8) == 2, "over-long word wraps");
        check(lines[0].start == 0 && lines[0].end == 3 &&
            lines[1].start == 3 && lines[1].end == 6, "over-long word hard break");
    }

    { /* Repeated spaces are preserved, not silently skipped. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"a   b", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        check(table_layout_cell_breaks(&f.in, 0, 0, 24, fake_width, NULL,
            lines, 8) == 2, "repeated spaces wrap");
        expect_lossless(&f, 0, 0, 24, fake_width, L"a   b",
            "repeated spaces reconstruct exactly");

        Fixture g;
        reset(&g, 1);
        add_cell(&g, L"one  two   three", 0);
        add_row(&g, 1);
        sync_fixture(&g);
        expect_lossless(&g, 0, 0, 64, fake_width, L"one  two   three",
            "multi-space line reconstructs exactly");
    }

    { /* Fitting uses whole-range measurement, so a non-additive metric is
         honored rather than summing per-character widths. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"abcdef", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        check(table_layout_cell_breaks(&f.in, 0, 0, 23, kerning_width, NULL,
            lines, 8) == 2, "kerning metric wraps");
        check(lines[0].start == 0 && lines[0].end == 3,
            "kerning range measured as a whole");
        expect_lossless(&f, 0, 0, 23, kerning_width, L"abcdef",
            "kerning wrap reconstructs exactly");
    }

    { /* Surrogate pairs are never split by a wrap. */
        wchar_t text[5] = { L'a', (wchar_t)0xD83D, (wchar_t)0xDE00, L'b', 0 };
        Fixture f;
        reset(&f, 1);
        add_cell(&f, text, 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableCellLine lines[8];
        int count = table_layout_cell_breaks(&f.in, 0, 0, 24, fake_width, NULL,
            lines, 8);
        check(count == 2 && lines[0].start == 0 && lines[0].end == 3 &&
            lines[1].start == 3 && lines[1].end == 4,
            "pair stays with the preceding glyph");
        count = table_layout_cell_breaks(&f.in, 0, 0, 16, fake_width, NULL,
            lines, 8);
        check(count == 3 && lines[0].start == 0 && lines[0].end == 1 &&
            lines[1].start == 1 && lines[1].end == 3 &&
            lines[2].start == 3 && lines[2].end == 4,
            "pair starts its own line");
        for (int i = 0; i < count; i++) {
            check(lines[i].start != 2 && lines[i].end != 2, "pair not split");
        }
        count = table_layout_cell_breaks(&f.in, 0, 0, 8, fake_width, NULL,
            lines, 8);
        check(count == 3 && lines[0].end == 1 && lines[1].start == 1 &&
            lines[1].end == 3, "pair taken whole when it alone overflows");
    }

    { /* Maximum column count. */
        Fixture f;
        reset(&f, MD_MAX_TABLE_COLUMNS);
        for (int i = 0; i < MD_MAX_TABLE_COLUMNS; i++)
            add_cell(&f, L"a", 0);
        add_row(&f, MD_MAX_TABLE_COLUMNS);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 1000, 1, 0, fake_width, NULL, &cols),
            "max columns lay out");
        check(cols.column_count == MD_MAX_TABLE_COLUMNS, "max column count");
        bool ok = cols.total_width == MD_MAX_TABLE_COLUMNS * 8;
        for (int i = 0; i < MD_MAX_TABLE_COLUMNS && ok; i++)
            ok = cols.width[i] == 8 && cols.x[i] == i * 8;
        check(ok, "max columns spaced at preferred width");
    }

    { /* Empty cell still occupies one physical line. */
        Fixture f;
        reset(&f, 1);
        add_cell(&f, L"", 0);
        add_row(&f, 1);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "empty cell lays out");
        const int w[1] = {16}, x[1] = {0};
        expect_columns(&cols, 1, w, x, 16, "empty cell keeps the minimum");
        check(table_layout_cell_lines(&f.in, 0, 0, 16, fake_width, NULL) == 1,
            "empty cell is one line");
        TableCellLine lines[2];
        check(table_layout_cell_breaks(&f.in, 0, 0, 16, fake_width, NULL,
            lines, 2) == 1 && lines[0].start == 0 && lines[0].end == 0,
            "empty cell line range is empty");
    }

    { /* Exact minimum-fit boundary, one pixel over fails. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"aaaaa", 0);
        add_cell(&f, L"aaaaa", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, 88, 40, 8, fake_width, NULL, &cols),
            "exact minimum fit succeeds");
        const int w[2] = {40, 40}, x[2] = {0, 48};
        expect_columns(&cols, 2, w, x, 88, "exact minimum layout");
        check(!table_layout_columns(&f.in, 87, 40, 8, fake_width, NULL, &cols),
            "one pixel under minimum fails");
    }

    { /* Malformed ranges, pointers and indices are rejected, never read. */
        Fixture f;
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        TableColumns cols;
        TableCellLine lines[4];
        check(!table_layout_columns(&f.in, 100, 16, 8, NULL, NULL, &cols),
            "missing measure rejected");
        check(table_layout_cell_lines(&f.in, 5, 0, 40, fake_width, NULL) == 0,
            "out-of-range row returns zero");
        check(table_layout_cell_lines(&f.in, 0, 5, 40, fake_width, NULL) == 0,
            "out-of-range column returns zero");

        f.rows[0].first_cell = -1;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "negative row start rejected");
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "malformed row rejects breaks");
        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.rows[0].first_cell = 1;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "row range past the cells rejected");

        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.cells[0].first_span = 1;
        f.cells[0].span_count = 5;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "cell span range past the spans rejected");

        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.spans[0].text = NULL;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "NULL span text rejected");

        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.in.rows = NULL;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "NULL rows with a row count rejected");

        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.in.cells = NULL;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "NULL cells with a cell count rejected");

        f = (Fixture){0};
        reset(&f, 0);
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "zero columns rejected");
        reset(&f, MD_MAX_TABLE_COLUMNS + 1);
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "too many columns rejected");

        /* Negative aggregate counts are rejected before any bounds arithmetic,
           by both the whole-table and the per-cell entry points. */
        f = (Fixture){0};
        reset(&f, 2);
        add_cell(&f, L"aa", 0);
        add_cell(&f, L"bb", 0);
        add_row(&f, 2);
        sync_fixture(&f);
        f.in.row_count = -1;
        check(!table_layout_columns(&f.in, 100, 16, 8, fake_width, NULL, &cols),
            "negative row count rejected by columns");
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "negative row count rejected by breaks");
        check(table_layout_cell_lines(&f.in, 0, 0, 40, fake_width, NULL) == 0,
            "negative row count rejected by lines");
        f.in.row_count = 1;
        f.in.cell_count = -1;
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "negative cell count rejected by breaks");
        f.in.cell_count = 2;
        f.in.span_count = -1;
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "negative span count rejected by breaks");
        check(table_layout_cell_lines(&f.in, 0, 0, 40, fake_width, NULL) == 0,
            "negative span count rejected by lines");
        f.in.span_count = 2;
        f.cells[0].span_count = -1;
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "negative cell span count rejected");
        f.cells[0].span_count = 1;
        f.cells[0].first_span = -1;
        check(table_layout_cell_breaks(&f.in, 0, 0, 40, fake_width, NULL,
            lines, 4) == 0, "negative cell span start rejected");
    }

    { /* Degenerate: huge widths must not overflow the apportionment. */
        Fixture f;
        reset(&f, MD_MAX_TABLE_COLUMNS);
        for (int i = 0; i < MD_MAX_TABLE_COLUMNS; i++)
            add_cell(&f, L"a", 0);
        add_row(&f, MD_MAX_TABLE_COLUMNS);
        sync_fixture(&f);
        TableColumns cols;
        check(table_layout_columns(&f.in, INT_MAX, 16, 8, huge_width, NULL,
            &cols), "huge widths still lay out");
        check(cols.column_count == MD_MAX_TABLE_COLUMNS, "huge width column count");
        long long sum = 0;
        bool floors = true;
        for (int i = 0; i < cols.column_count; i++) {
            sum += cols.width[i];
            if (cols.width[i] < 16) floors = false;
        }
        sum += (long long)8 * (cols.column_count - 1);
        check(floors, "huge widths never fall below the floor");
        check(sum == cols.total_width && sum <= INT_MAX,
            "huge width total stays within the available width");
        check(table_layout_cell_lines(&f.in, 0, 0, INT_MAX, huge_width, NULL) == 1,
            "huge measured glyph fits one line");
    }

    { /* Physical-line cap: exactly the cap succeeds, one over signals -1. */
        static wchar_t at_cap[TABLE_MAX_PHYSICAL_LINES + 1];
        static wchar_t over_cap[TABLE_MAX_PHYSICAL_LINES + 2];
        for (int i = 0; i < TABLE_MAX_PHYSICAL_LINES; i++) at_cap[i] = L'a';
        at_cap[TABLE_MAX_PHYSICAL_LINES] = 0;
        for (int i = 0; i < TABLE_MAX_PHYSICAL_LINES + 1; i++) over_cap[i] = L'a';
        over_cap[TABLE_MAX_PHYSICAL_LINES + 1] = 0;

        Fixture f;
        reset(&f, 1);
        add_cell(&f, at_cap, 0);
        add_row(&f, 1);
        sync_fixture(&f);
        check(table_layout_cell_lines(&f.in, 0, 0, 8, fake_width, NULL) ==
            TABLE_MAX_PHYSICAL_LINES, "line count at the cap is allowed");

        Fixture g;
        reset(&g, 1);
        add_cell(&g, over_cap, 0);
        add_row(&g, 1);
        sync_fixture(&g);
        check(table_layout_cell_lines(&g.in, 0, 0, 8, fake_width, NULL) == -1,
            "one line over the cap signals overflow");
    }

    if (failures) {
        printf("test_table_layout: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_table_layout: fixed-minimum column policy, lossless wrapping, "
        "whole-range metrics, surrogate safety, anchors, validation and caps "
        "passed");
    return 0;
}
