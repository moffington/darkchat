#include "chat/transcript/markdown.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* The renderer builds one document buffer (source text plus synthesized
   bullets and quote bars), a side arena of link destinations and runs, so
   memory is O(input) and adjacent runs with identical style coalesce into one.
   Every failure aborts the whole document; the caller falls back to verbatim
   text. */

#define MD_NPOS ((size_t)-1)

static bool test_fail_allocations;
static bool test_fail_blocks;
static bool test_fail_links;
static bool test_fail_cells;
static bool test_fail_rows;
static bool test_fail_tables;
static bool test_fail_literals;
static bool test_fail_fences;
static int test_scratch_allocations;

void markdown_test_fail_allocations(bool enable) {
    test_fail_allocations = enable;
}

void markdown_test_fail_blocks(bool enable) {
    test_fail_blocks = enable;
}

void markdown_test_fail_links(bool enable) {
    test_fail_links = enable;
}

void markdown_test_fail_cells(bool enable) {
    test_fail_cells = enable;
}

void markdown_test_fail_rows(bool enable) {
    test_fail_rows = enable;
}

void markdown_test_fail_tables(bool enable) {
    test_fail_tables = enable;
}

void markdown_test_fail_literals(bool enable) {
    test_fail_literals = enable;
}

void markdown_test_fail_fences(bool enable) {
    test_fail_fences = enable;
}

/* Test-only: how many times the per-cell normalization scratch buffer had to
   grow since the last reset. Reuse means one render needs only a handful of
   allocations, never one per cell. */
void markdown_test_reset_scratch_allocations(void) {
    test_scratch_allocations = 0;
}

int markdown_test_scratch_allocations(void) {
    return test_scratch_allocations;
}

void markdown_dispose(MdDocument *doc) {
    if (!doc) return;
    free(doc->text);
    free(doc->runs);
    free(doc->blocks);
    free(doc->links);
    free(doc->targets);
    free(doc->cells);
    free(doc->rows);
    free(doc->tables);
    free(doc->literals);
    free(doc->fences);
    memset(doc, 0, sizeof *doc);
}

typedef struct {
    unsigned style;
    int heading;
} MdStyle;

typedef struct {
    wchar_t *text;
    size_t length, capacity;
    MdRun *runs;
    int run_count, run_capacity;
    MdBlock *blocks;
    int block_count, block_capacity;
    MdLink *links;
    int link_count, link_capacity;
    wchar_t *targets;
    size_t targets_length, targets_capacity;
    MdTableCell *cells;
    int cell_count, cell_capacity;
    MdTableRow *rows;
    int row_count, row_capacity;
    MdTable *tables;
    int table_count, table_capacity;
    wchar_t *literals;
    size_t literals_length, literals_capacity;
    MdCodeFence *fences;
    int fence_count, fence_capacity;
    wchar_t *scratch;                   /* reused per-cell normalization buffer */
    size_t scratch_capacity;
    bool failed;
} MdBuilder;

static bool grow_text(MdBuilder *b, size_t extra) {
    if (b->failed) return false;
    if (test_fail_allocations) { b->failed = true; return false; }
    if (b->length + extra + 1 <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 256;
    while (capacity < b->length + extra + 1) capacity *= 2;
    wchar_t *grown = (wchar_t *)realloc(b->text, capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->text = grown;
    b->capacity = capacity;
    return true;
}

static bool grow_runs(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations) { b->failed = true; return false; }
    if (b->run_count < b->run_capacity) return true;
    int capacity = b->run_capacity ? b->run_capacity * 2 : 32;
    MdRun *grown = (MdRun *)realloc(b->runs, (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->runs = grown;
    b->run_capacity = capacity;
    return true;
}

static bool grow_blocks(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_blocks) { b->failed = true; return false; }
    if (b->block_count < b->block_capacity) return true;
    int capacity = b->block_capacity ? b->block_capacity * 2 : 32;
    MdBlock *grown = (MdBlock *)realloc(b->blocks, (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->blocks = grown;
    b->block_capacity = capacity;
    return true;
}

static bool grow_links(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_links) { b->failed = true; return false; }
    if (b->link_count < b->link_capacity) return true;
    int capacity = b->link_capacity ? b->link_capacity * 2 : 16;
    MdLink *grown = (MdLink *)realloc(b->links, (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->links = grown;
    b->link_capacity = capacity;
    return true;
}

static bool grow_targets(MdBuilder *b, size_t extra) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_links) { b->failed = true; return false; }
    if (b->targets_length + extra <= b->targets_capacity) return true;
    size_t capacity = b->targets_capacity ? b->targets_capacity : 64;
    while (capacity < b->targets_length + extra) capacity *= 2;
    wchar_t *grown = (wchar_t *)realloc(b->targets, capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->targets = grown;
    b->targets_capacity = capacity;
    return true;
}

static bool grow_cells(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_cells) { b->failed = true; return false; }
    if (b->cell_count < b->cell_capacity) return true;
    int capacity = b->cell_capacity ? b->cell_capacity * 2 : 32;
    MdTableCell *grown = (MdTableCell *)realloc(b->cells,
        (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->cells = grown;
    b->cell_capacity = capacity;
    return true;
}

static bool grow_rows(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_rows) { b->failed = true; return false; }
    if (b->row_count < b->row_capacity) return true;
    int capacity = b->row_capacity ? b->row_capacity * 2 : 16;
    MdTableRow *grown = (MdTableRow *)realloc(b->rows,
        (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->rows = grown;
    b->row_capacity = capacity;
    return true;
}

static bool grow_tables(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_tables) { b->failed = true; return false; }
    if (b->table_count < b->table_capacity) return true;
    int capacity = b->table_capacity ? b->table_capacity * 2 : 8;
    MdTable *grown = (MdTable *)realloc(b->tables,
        (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->tables = grown;
    b->table_capacity = capacity;
    return true;
}

static bool grow_fences(MdBuilder *b) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_fences) { b->failed = true; return false; }
    if (b->fence_count < b->fence_capacity) return true;
    int capacity = b->fence_capacity ? b->fence_capacity * 2 : 8;
    MdCodeFence *grown = (MdCodeFence *)realloc(b->fences,
        (size_t)capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->fences = grown;
    b->fence_capacity = capacity;
    return true;
}

static bool grow_literals(MdBuilder *b, size_t extra) {
    if (b->failed) return false;
    if (test_fail_allocations || test_fail_literals) { b->failed = true; return false; }
    if (b->literals_length + extra <= b->literals_capacity) return true;
    size_t capacity = b->literals_capacity ? b->literals_capacity : 128;
    while (capacity < b->literals_length + extra) capacity *= 2;
    wchar_t *grown = (wchar_t *)realloc(b->literals, capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->literals = grown;
    b->literals_capacity = capacity;
    return true;
}

/* Grows the builder's single reusable cell-normalization buffer. One buffer is
   reused across every cell of every table, so a reparse costs a handful of
   allocations rather than rows x columns. Growth honors the allocation-failure
   hook; an empty cell needs no buffer at all. */
static bool grow_scratch(MdBuilder *b, size_t needed) {
    if (b->failed) return false;
    if (b->scratch_capacity >= needed) return true;
    if (test_fail_allocations) { b->failed = true; return false; }
    size_t capacity = b->scratch_capacity ? b->scratch_capacity : 128;
    while (capacity < needed) capacity *= 2;
    wchar_t *grown = (wchar_t *)realloc(b->scratch, capacity * sizeof *grown);
    if (!grown) { b->failed = true; return false; }
    b->scratch = grown;
    b->scratch_capacity = capacity;
    ++test_scratch_allocations;
    return true;
}

/* Records one valid link whose label has just been emitted. The destination,
   NUL-terminated, is copied into the arena first, so a link entry never
   references missing text and the display can hand it to the shell directly
   without a length limit. */
static void emit_link(MdBuilder *b, size_t offset, size_t length,
    const wchar_t *target, size_t target_length) {
    size_t target_offset = b->targets_length;
    if (!grow_targets(b, target_length + 1)) return;
    if (target_length) wmemcpy(b->targets + b->targets_length, target, target_length);
    b->targets_length += target_length;
    b->targets[b->targets_length++] = L'\0';
    if (!grow_links(b)) return;
    MdLink *link = &b->links[b->link_count++];
    link->offset = offset;
    link->length = length;
    link->target_offset = target_offset;
    link->target_length = target_length;
}

/* Appends text carrying one style, extending the trailing run when it is
   identical and directly adjacent (runs are written linearly, so the trailing
   run ends at the current write position whenever it can extend). */
static void emit(MdBuilder *b, const wchar_t *text, size_t length, MdStyle s) {
    if (b->failed || !length || !grow_text(b, length)) return;
    if (b->run_count) {
        MdRun *last = &b->runs[b->run_count - 1];
        if (last->offset + last->length == b->length &&
            last->style == s.style && last->heading == s.heading) {
            wmemcpy(b->text + b->length, text, length);
            b->length += length;
            last->length += length;
            return;
        }
    }
    if (!grow_runs(b)) return;
    wmemcpy(b->text + b->length, text, length);
    MdRun *run = &b->runs[b->run_count++];
    run->offset = b->length;
    run->length = length;
    run->style = s.style;
    run->heading = s.heading;
    b->length += length;
}

/* One paragraph break, styled with the surrounding context so code blocks
   coalesce across their lines. Skipped at the start of the document. */
static void emit_break(MdBuilder *b, unsigned style) {
    if (!b->length) return;
    MdStyle s = {0};
    s.style = style;
    emit(b, L"\n", 1, s);
}

/* Records the paragraph written since `start`. Empty paragraphs (blank lines,
    hidden fence markers) are not recorded: they hold no character and need no
    layout. */
static void close_block(MdBuilder *b, size_t start, int kind, unsigned char quote,
    unsigned char list, unsigned char flags, unsigned char first,
    unsigned char continuation) {
    if (b->failed || b->length == start) return;
    if (!grow_blocks(b)) return;
    MdBlock *block = &b->blocks[b->block_count++];
    block->offset = start;
    block->length = b->length - start;
    block->kind = (unsigned char)kind;
    block->quote_depth = quote;
    block->list_depth = list;
    block->flags = flags;
    block->first_indent = first;
    block->continuation_indent = continuation;
    block->table_index = -1;
}

/* Records one table block unconditionally, including a zero-length range: an
   all-empty header still names its table. Ordinary blocks keep close_block's
   non-empty rule. */
static void record_table_block(MdBuilder *b, size_t start, int table_index) {
    if (b->failed) return;
    if (!grow_blocks(b)) return;
    MdBlock *block = &b->blocks[b->block_count++];
    block->offset = start;
    block->length = b->length - start;
    block->kind = MD_BLOCK_TABLE;
    block->quote_depth = 0;
    block->list_depth = 0;
    block->flags = 0;
    block->first_indent = 0;
    block->continuation_indent = 0;
    block->table_index = table_index;
}

static bool is_space(wchar_t c) { return c == L' ' || c == L'\t'; }

/* Task markers are only valid immediately after an otherwise-valid unordered
   list marker, and must end at whitespace or the item boundary. */
static bool task_marker(const wchar_t *s, size_t n, bool *checked) {
    if (n < 3 || s[0] != L'[' || s[2] != L']' ||
        (s[1] != L' ' && s[1] != L'x' && s[1] != L'X') ||
        (n > 3 && !is_space(s[3]))) return false;
    *checked = s[1] != L' ';
    return true;
}

static bool is_punct(wchar_t c) {
    static const wchar_t *set = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    return c && wcschr(set, c) != NULL;
}

static size_t find_char(const wchar_t *s, size_t n, size_t from, wchar_t c) {
    for (size_t i = from; i < n; i++) if (s[i] == c) return i;
    return MD_NPOS;
}

/* Backticks are maximal delimiter runs: a different-length run stays in the
   code content and cannot partially close the opener. */
static size_t backtick_run(const wchar_t *s, size_t n, size_t from) {
    size_t end = from;
    while (end < n && s[end] == L'`') ++end;
    return end - from;
}

static size_t find_close_code(const wchar_t *s, size_t n, size_t from,
    size_t length) {
    for (size_t i = from; i < n;) {
        if (s[i] != L'`') { ++i; continue; }
        size_t run = backtick_run(s, n, i);
        if (run == length) return i;
        i += run;
    }
    return MD_NPOS;
}

/* Only http:// and https:// targets become clickable link output. */
static bool is_http(const wchar_t *s, size_t n) {
    static const wchar_t *schemes[2] = {L"https://", L"http://"};
    for (int k = 0; k < 2; k++) {
        size_t length = wcslen(schemes[k]);
        if (n < length) continue;
        size_t i = 0;
        for (; i < length; i++) {
            wchar_t c = s[i];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (c != schemes[k][i]) break;
        }
        if (i == length) return true;
    }
    return false;
}

/* An emphasis opener needs content and a boundary: start of the span,
   whitespace or punctuation before it. Word-internal underscores (identifiers
   like snake_case_name) and asterisks (2*3*4) stay literal. */
static bool opens_emphasis(const wchar_t *s, size_t n, size_t i, wchar_t c) {
    if (i + 1 >= n || is_space(s[i + 1])) return false;
    if (i > 0 && !is_space(s[i - 1]) && !is_punct(s[i - 1])) return false;
    if (c == L'_' && i > 0 && iswalnum((wint_t)s[i - 1])) return false;
    return true;
}

/* The closing delimiter must be preceded by non-space and followed by a
   boundary; underscores additionally refuse an alphanumeric tail. */
static size_t find_close_emphasis(const wchar_t *s, size_t n, size_t open,
    wchar_t c) {
    for (size_t j = open + 2; j < n; j++) {
        if (s[j] != c || is_space(s[j - 1])) continue;
        if (j + 1 < n && !is_space(s[j + 1]) && !is_punct(s[j + 1])) continue;
        if (c == L'_' && j + 1 < n && iswalnum((wint_t)s[j + 1])) continue;
        return j;
    }
    return MD_NPOS;
}

static size_t find_close_bold(const wchar_t *s, size_t n, size_t open) {
    for (size_t j = open + 3; j + 1 < n; j++) {
        if (s[j] != L'*' || s[j + 1] != L'*' || is_space(s[j - 1])) continue;
        if (j + 2 < n && !is_space(s[j + 2]) && !is_punct(s[j + 2])) continue;
        return j;
    }
    return MD_NPOS;
}

static size_t find_close_bold_italic(const wchar_t *s, size_t n, size_t open) {
    for (size_t j = open + 4; j + 2 < n; j++) {
        if (s[j] != L'*' || s[j + 1] != L'*' || s[j + 2] != L'*') continue;
        if (is_space(s[j - 1])) continue;
        if (j + 3 < n && !is_space(s[j + 3]) && !is_punct(s[j + 3])) continue;
        return j;
    }
    return MD_NPOS;
}

/* A conservative paired-tilde span needs non-space content at both edges.
   Other delimiter rules intentionally stay simple, matching this subset's
   treatment of emphasis rather than attempting full GFM delimiter parsing. */
static size_t find_close_strike(const wchar_t *s, size_t n, size_t open) {
    for (size_t j = open + 3; j + 1 < n; j++) {
        if (s[j] == L'~' && s[j + 1] == L'~' && !is_space(s[j - 1])) return j;
    }
    return MD_NPOS;
}

/* Inline pass, in precedence order: backslash escapes, inline code, links,
   then strikethrough and emphasis. Anything unmatched is kept literally. */
static void parse_inline(MdBuilder *b, const wchar_t *s, size_t n,
    MdStyle style) {
    size_t i = 0, plain = 0;
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < n && is_punct(s[i + 1])) {
            if (i > plain) emit(b, s + plain, i - plain, style);
            emit(b, s + i + 1, 1, style);
            i += 2;
            plain = i;
            continue;
        }
        if (c == L'`') {
            size_t opening = backtick_run(s, n, i);
            size_t close = find_close_code(s, n, i + opening, opening);
            if (close != MD_NPOS) {
                if (i > plain) emit(b, s + plain, i - plain, style);
                MdStyle code = style;
                code.style |= MD_STYLE_MONO | MD_STYLE_CODE;
                emit(b, s + i + opening, close - i - opening, code);
                i = close + opening;
                plain = i;
                continue;
            }
            /* Keep the entire unmatched delimiter run literal; its individual
               backticks cannot become smaller openers on this pass. */
            i += opening;
            continue;
        }
        if (c == L'[') {
            size_t label_end = find_char(s, n, i + 1, L']');
            if (label_end != MD_NPOS && label_end > i + 1 &&
                label_end + 1 < n && s[label_end + 1] == L'(') {
                size_t url = label_end + 2;
                size_t url_end = find_char(s, n, url, L')');
                if (url_end != MD_NPOS && url_end > url &&
                    is_http(s + url, url_end - url)) {
                    if (i > plain) emit(b, s + plain, i - plain, style);
                    /* The label keeps the current style and is the only text
                       shown; the display layer opens the recorded target. */
                    size_t label_length = label_end - i - 1;
                    size_t label_offset = b->length;
                    emit(b, s + i + 1, label_length, style);
                    if (!b->failed)
                        emit_link(b, label_offset, label_length, s + url,
                            url_end - url);
                    i = url_end + 1;
                    plain = i;
                    continue;
                }
            }
            ++i;
            continue;
        }
        if (c == L'~' && i + 2 < n && s[i + 1] == L'~' &&
            !is_space(s[i + 2])) {
            size_t close = find_close_strike(s, n, i);
            if (close != MD_NPOS) {
                if (i > plain) emit(b, s + plain, i - plain, style);
                MdStyle inner = style;
                inner.style |= MD_STYLE_STRIKE;
                parse_inline(b, s + i + 2, close - i - 2, inner);
                i = close + 2;
                plain = i;
                continue;
            }
            ++i;
            continue;
        }
        if ((c == L'*' || c == L'_') && opens_emphasis(s, n, i, c)) {
            if (c == L'*' && i + 1 < n && s[i + 1] == L'*') {
                bool triple = i + 2 < n && s[i + 2] == L'*';
                size_t skip = triple ? 3 : 2;
                size_t close = triple ? find_close_bold_italic(s, n, i) :
                    find_close_bold(s, n, i);
                if (close != MD_NPOS) {
                    if (i > plain) emit(b, s + plain, i - plain, style);
                    MdStyle inner = style;
                    inner.style |= MD_STYLE_BOLD;
                    if (triple) inner.style |= MD_STYLE_ITALIC;
                    parse_inline(b, s + i + skip, close - i - skip, inner);
                    i = close + skip;
                    plain = i;
                    continue;
                }
            } else {
                size_t close = find_close_emphasis(s, n, i, c);
                if (close != MD_NPOS) {
                    if (i > plain) emit(b, s + plain, i - plain, style);
                    MdStyle inner = style;
                    inner.style |= MD_STYLE_ITALIC;
                    parse_inline(b, s + i + 1, close - i - 1, inner);
                    i = close + 1;
                    plain = i;
                    continue;
                }
            }
            ++i;
            continue;
        }
        ++i;
    }
    if (n > plain) emit(b, s + plain, n - plain, style);
}

typedef struct {
    wchar_t delimiter;
    size_t length, indent;
    /* Render recording: an open fence accumulates the emitted range of its
       content lines (including interior newlines) so a completed fence can be
       recorded as one MdCodeFence. `start` is fixed after the separator before
       the first content line; `end` advances after every content line. */
    bool recording;
    size_t start, end;
} MdFence;

/* Openers keep the subset's permissive hidden info strings. Closing validation
   is separate because only horizontal whitespace may follow a closing run. */
static bool fence_marker(const wchar_t *line, size_t n, MdFence *marker) {
    size_t indent = 0;
    while (indent < n && indent < 3 && line[indent] == L' ') ++indent;
    size_t from = indent;
    if (n - from < 3 || (line[from] != L'`' && line[from] != L'~'))
        return false;
    wchar_t delimiter = line[from];
    while (from < n && line[from] == delimiter) ++from;
    if (from - indent < 3) return false;
    marker->delimiter = delimiter;
    marker->length = from - indent;
    marker->indent = indent;
    return true;
}

static bool fence_closes(const wchar_t *line, size_t n, const MdFence *fence) {
    MdFence marker = {0};
    if (!fence_marker(line, n, &marker) || marker.delimiter != fence->delimiter ||
        marker.length < fence->length) return false;
    for (size_t i = marker.indent + marker.length; i < n; i++)
        if (!is_space(line[i])) return false;
    return true;
}

/* One active list level: its marker column and glyph width in quote-relative
   columns, plus the layout pair its content was written with, so a following
   continuation can inherit both. */
typedef struct {
    unsigned char column, glyph, first, continuation;
} MdLevel;

typedef struct {
    unsigned char quote_depth;
    MdLevel level[MD_MAX_DEPTH];
    int depth;
} MdBlocks;

/* A parsed block prefix. Nothing is emitted while it is filled in, so a line
   that is rejected (too deep, unsupported indentation) stays entirely
   literal. `quote_depth` saturates one past MD_MAX_DEPTH: the source may hold
   any number of markers, and a deeper line than the cap renders literally. */
typedef struct {
    size_t rel;                 /* indentation columns after the prefix */
    size_t base_column;         /* column `rel` is measured from */
    size_t ws_begin, ws_end;    /* the whitespace run that produced rel */
    size_t content;             /* index where content starts */
    size_t marker;              /* index of the list marker, when present */
    unsigned char quote_depth, digits, glyph;
    bool has_marker, ordered, task, checked;
} MdPrefix;

static size_t column_step(size_t column, wchar_t c) {
    return c == L'\t' ? (column / 4 + 1) * 4 : column + 1;
}

static unsigned char clamp_columns(size_t columns) {
    return columns > 255u ? (unsigned char)255 : (unsigned char)columns;
}

/* Scans a line's block prefix. Up to three root spaces may precede a quote
   marker (the legacy tolerance); indentation is then measured against the real
   source cursor after the quote prefix, so ">> x" and "> > x" both start
   their content at relative column 0 even though their source widths differ,
   while the rendered bars are counted separately from the source. */
static void scan_prefix(const wchar_t *line, size_t n, MdPrefix *p) {
    memset(p, 0, sizeof *p);
    size_t skip = 0;
    while (skip < n && skip < MD_ROOT_PREFIX_COLS && line[skip] == L' ') ++skip;
    size_t i = skip, column = skip;
    size_t depth = 0;
    for (;;) {
        if (i >= n || line[i] != L'>') break;
        size_t after = i + 1;                   /* past the marker */
        bool spaced = after < n && is_space(line[after]);
        size_t end = spaced ? after + 1 : after;/* past the optional space */
        bool interior = end < n && line[end] == L'>';
        /* The last marker needs a space or the end of the line after it. */
        if (!interior && !spaced && end != n) break;
        /* Every consumed character counts: a tab after the markers lands on
           the tab stop of the real source column. */
        column = column_step(column, L'>');
        if (spaced) column = column_step(column, line[after]);
        i = end;
        ++depth;
        if (!interior) break;
    }
    p->quote_depth = depth > MD_MAX_DEPTH ? (unsigned char)(MD_MAX_DEPTH + 1)
        : (unsigned char)depth;
    if (depth) {
        p->base_column = column;
        p->ws_begin = i;
    } else {
        /* Unquoted indentation is measured from the line start, so the whole
           leading run belongs to the indentation the layout may have to cut. */
        p->base_column = 0;
        p->ws_begin = 0;
        i = 0;
        column = 0;
    }
    while (i < n && is_space(line[i])) { column = column_step(column, line[i]); ++i; }
    p->ws_end = i;
    p->rel = column - p->base_column;
    p->content = i;
    if (i < n && (line[i] == L'-' || line[i] == L'*') && i + 1 < n &&
        is_space(line[i + 1])) {
        size_t from = i + 1;
        while (from < n && is_space(line[from])) ++from;
        bool checked = false;
        if (task_marker(line + from, n - from, &checked)) {
            p->task = true;
            p->checked = checked;
            from += 3;
            while (from < n && is_space(line[from])) ++from;
        }
        p->has_marker = true;
        p->glyph = 2;
        p->marker = i;
        p->content = from;
    } else {
        size_t digits = 0;
        while (i + digits < n && digits < 9 && line[i + digits] >= L'0' &&
            line[i + digits] <= L'9') ++digits;
        if (digits >= 1 && i + digits < n &&
            (line[i + digits] == L'.' || line[i + digits] == L')') &&
            i + digits + 1 < n && is_space(line[i + digits + 1])) {
            size_t from = i + digits + 1;
            while (from < n && is_space(line[from])) ++from;
            p->has_marker = true;
            p->ordered = true;
            p->digits = (unsigned char)digits;
            p->glyph = (unsigned char)(digits + 2);
            p->marker = i;
            p->content = from;
        }
    }
}

/* Index in [begin, end) where the column budget runs out. The remaining source
   whitespace is kept verbatim, so a tab is never rewritten as spaces. */
static size_t indent_cut(const wchar_t *line, size_t begin, size_t end,
    size_t from_column, size_t budget) {
    size_t column = from_column;
    for (size_t i = begin; i < end; i++) {
        column = column_step(column, line[i]);
        if (column - from_column > budget) return i;
    }
    return end;
}

static bool push_level(MdBlocks *blocks, const MdPrefix *p) {
    if (blocks->depth >= MD_MAX_DEPTH) return false;
    MdLevel *level = &blocks->level[blocks->depth++];
    level->column = clamp_columns(p->rel);
    level->glyph = p->glyph;
    level->first = 0;
    level->continuation = 0;
    return true;
}

/* True when a line is entirely whitespace. */
static bool line_is_blank(const wchar_t *line, size_t n) {
    for (size_t i = 0; i < n; i++) if (!is_space(line[i])) return false;
    return true;
}

/* A fence opener (up to three leading spaces). */
static bool line_fence_open(const wchar_t *line, size_t n) {
    MdFence marker = {0};
    return fence_marker(line, n, &marker);
}

/* A root-level ATX heading of level 1-3 with its required following space.
   `from` receives the first content column. */
static bool line_heading_root(const wchar_t *line, size_t n, int *level,
    size_t *from) {
    MdPrefix p;
    scan_prefix(line, n, &p);
    if (p.quote_depth || p.has_marker || p.rel > MD_ROOT_PREFIX_COLS)
        return false;
    size_t hashes = 0;
    while (p.content + hashes < n && line[p.content + hashes] == L'#') ++hashes;
    if (hashes < 1 || hashes > 3) return false;
    size_t start = p.content + hashes;
    if (start != n && !is_space(line[start])) return false;
    while (start < n && is_space(line[start])) ++start;
    if (level) *level = (int)hashes;
    if (from) *from = start;
    return true;
}

/* True when a line begins a block-level construct that ends an open table:
   blank, a fence opener, a heading, or a quote/list prefix. Shared by
   render_line's predicates and the table scanner so the two cannot drift. */
static bool line_starts_block(const wchar_t *line, size_t n) {
    if (line_is_blank(line, n)) return true;
    if (line_fence_open(line, n)) return true;
    if (line_heading_root(line, n, NULL, NULL)) return true;
    MdPrefix p;
    scan_prefix(line, n, &p);
    return p.quote_depth != 0 || p.has_marker;
}

/* Records the open fence's accumulated range when it emitted any character,
   then clears the fence state. The separator newline before the first content
   line is never included; interior newlines (including those of blank content
   lines) are. A fence that emitted nothing records nothing. */
static void close_fence_record(MdBuilder *b, MdFence *fence) {
    if (fence->recording && fence->end > fence->start && grow_fences(b)) {
        MdCodeFence *record = &b->fences[b->fence_count++];
        record->offset = fence->start;
        record->length = fence->end - fence->start;
    }
    memset(fence, 0, sizeof *fence);
}

/* One source line; blocks are recognized line-by-line and fences hide their
   marker lines. Newlines are normalized to LF. */
static void render_line(MdBuilder *b, const wchar_t *line, size_t n,
    MdFence *fence, MdBlocks *blocks) {
    MdStyle plain = {0};
    if (fence->delimiter) {
        if (fence_closes(line, n, fence)) {
            close_fence_record(b, fence);   /* closing fence is hidden */
        } else {
            MdStyle code = {0};
            code.style = MD_STYLE_MONO | MD_STYLE_CODE;
            size_t from = 0;
            while (from < n && from < fence->indent && line[from] == L' ')
                ++from;
            emit_break(b, code.style);
            if (!fence->recording) {
                /* Start after the separator so the break before the fence is
                   never copied, even when this first content line is empty. */
                fence->recording = true;
                fence->start = b->length;
            }
            size_t start = b->length;
            emit(b, line + from, n - from, code);
            fence->end = b->length;
            close_block(b, start, MD_BLOCK_CODE, 0, 0, 0, 0, 0);
        }
        return;
    }
    MdFence marker = {0};
    if (fence_marker(line, n, &marker)) {
        *fence = marker;                    /* opening fence is hidden */
        return;
    }
    bool blank = line_is_blank(line, n);
    if (blank) {
        emit_break(b, 0);                   /* blank lines keep list state */
        return;
    }
    MdPrefix p;
    scan_prefix(line, n, &p);
    /* A quote boundary starts a fresh list: "- a" followed by "> - b" must
       not make the quoted item a second level of the unquoted list. */
    if (p.quote_depth != blocks->quote_depth) blocks->depth = 0;
    blocks->quote_depth = p.quote_depth;

    bool nested = blocks->depth > 0;
    bool literal = p.quote_depth > MD_MAX_DEPTH;
    bool continuation = false;
    if (!literal && p.has_marker) {
        if (!nested && p.quote_depth == 0 && p.rel > MD_ROOT_PREFIX_COLS) {
            literal = true;                 /* four-space indented marker */
        } else {
            while (blocks->depth > 0 &&
                p.rel < blocks->level[blocks->depth - 1].column)
                --blocks->depth;
            if (blocks->depth == 0) {
                if (!push_level(blocks, &p)) literal = true;
            } else if (p.rel < blocks->level[blocks->depth - 1].column +
                blocks->level[blocks->depth - 1].glyph) {
                /* Within the parent's marker tolerance: a sibling item, whose
                   own column anchors the level from here on. */
                blocks->level[blocks->depth - 1].column = clamp_columns(p.rel);
                blocks->level[blocks->depth - 1].glyph = p.glyph;
            } else if (!push_level(blocks, &p)) {
                literal = true;
            }
        }
    } else if (!literal && blocks->depth > 0 &&
        p.rel > blocks->level[blocks->depth - 1].column) {
        continuation = true;                /* indented under an open item */
    }

    unsigned char first = 0, continuation_indent = 0, flags = 0;
    int kind = MD_BLOCK_PARAGRAPH;
    size_t retain_from = p.ws_end, retain_to = p.ws_end;
    if (!literal) {
        /* Only an unindented ordinary paragraph loses its leading columns (the
           legacy root tolerance); a recognized marker, a live list or a quote
           makes the source indentation structural. */
        size_t base = (p.quote_depth || nested || p.has_marker) ? p.rel : 0;
        if (base > MD_MAX_INDENT) {
            retain_from = indent_cut(line, p.ws_begin, p.ws_end, p.base_column,
                MD_MAX_INDENT);
            retain_to = p.ws_end;
            base = MD_MAX_INDENT;
        }
        first = clamp_columns(base);
        if (p.has_marker) {
            kind = MD_BLOCK_ITEM;
            flags = p.ordered ? (unsigned char)MD_FLAG_ORDERED : (unsigned char)0;
            if (p.task)
                flags |= (unsigned char)(MD_FLAG_TASK |
                    (p.checked ? MD_FLAG_CHECKED : 0));
            continuation_indent = (unsigned char)(first +
                p.quote_depth * MD_QUOTE_GLYPH_COLS + p.glyph);
            MdLevel *level = &blocks->level[blocks->depth - 1];
            level->first = first;
            level->continuation = continuation_indent;
        } else if (continuation) {
            kind = MD_BLOCK_ITEM;
            flags = MD_FLAG_CONTINUATION;
            const MdLevel *level = &blocks->level[blocks->depth - 1];
            first = level->first;           /* bars and content stay aligned */
            continuation_indent = level->continuation;
        } else if (p.quote_depth) {
            kind = MD_BLOCK_QUOTE;
            continuation_indent = (unsigned char)(first +
                p.quote_depth * MD_QUOTE_GLYPH_COLS);
        }
    }

    bool heading = false;
    int heading_level = 0;
    size_t heading_from = 0;
    if (!literal && !p.has_marker && !continuation && !p.quote_depth &&
        p.rel <= MD_ROOT_PREFIX_COLS &&
        line_heading_root(line, n, &heading_level, &heading_from)) {
        heading = true;
    }

    emit_break(b, 0);
    size_t start = b->length;
    if (literal || (!p.has_marker && !continuation && !p.quote_depth &&
        !heading)) {
        /* Plain, or unsupported: the whole line stays verbatim, whitespace
           included, and any open list ends here. */
        parse_inline(b, line, n, plain);
        close_block(b, start, MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0);
        blocks->depth = 0;
        return;
    }
    if (heading) {
        MdStyle style = {0};
        style.style = MD_STYLE_BOLD;
        style.heading = heading_level;
        parse_inline(b, line + heading_from, n - heading_from, style);
        close_block(b, start, MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0);
        blocks->depth = 0;
        return;
    }
    /* Recorded before an ordinary paragraph closes the list it interrupts. */
    unsigned char list_depth = (unsigned char)blocks->depth;
    if (!p.has_marker) blocks->depth = 0;   /* a quoted paragraph ends a list */
    MdStyle style = plain;
    if (p.quote_depth) style.style |= MD_STYLE_MUTED;
    for (unsigned q = 0; q < p.quote_depth; q++)
        emit(b, L"\u258C ", 2, plain);      /* one bar per quote level */
    if (p.has_marker) {
        if (p.task) emit(b, p.checked ? L"\u2611 " : L"\u2610 ", 2, plain);
        else if (p.ordered) emit(b, line + p.marker, p.digits + 2, plain);
        else emit(b, L"\u2022 ", 2, plain);
    } else if (continuation) {
        /* Spaces stand in for the parent's marker so both the bars and the
           content line up with the item they continue. */
        size_t pad = (size_t)continuation_indent - first -
            (size_t)p.quote_depth * MD_QUOTE_GLYPH_COLS;
        for (size_t k = 0; k < pad; k++) emit(b, L" ", 1, plain);
    }
    if (retain_to > retain_from)
        emit(b, line + retain_from, retain_to - retain_from, plain);
    parse_inline(b, line + p.content, n - p.content, style);
    close_block(b, start, kind, p.quote_depth,
        p.has_marker || continuation ? list_depth : 0,
        flags, first, continuation_indent);
}

/* --- GFM tables ------------------------------------------------------------
   Tables are recognized only at the root. A candidate header is confirmed by
   the very next line, so a header that has not yet received its delimiter
   keeps rendering as an ordinary paragraph (streaming-safe). Recognition
   records metadata; the emitted text holds the cell contents and the display
   layer keeps the whole message verbatim until table rendering is activated. */

typedef struct { size_t begin, end; } MdCellRange;

/* A `|` is a cell separator unless escaped: an odd run of backslashes right
   before it makes it content. */
static bool pipe_separator(const wchar_t *s, size_t index) {
    size_t run = 0;
    while (index > run && s[index - 1 - run] == L'\\') ++run;
    return (run % 2) == 0;
}

static bool has_separator_pipe(const wchar_t *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == L'|' && pipe_separator(s, i)) return true;
    return false;
}

/* Splits one row into cells, stripping optional edge pipes (surrounding
   whitespace included). At most `capacity` ranges are stored; `overflow` is
   set when the row holds more cells than that. */
static int split_row(const wchar_t *s, size_t n, MdCellRange *out, int capacity,
    bool *overflow) {
    if (overflow) *overflow = false;
    size_t begin = 0, end = n;
    size_t i = 0;
    while (i < n && is_space(s[i])) ++i;
    if (i < n && s[i] == L'|' && pipe_separator(s, i)) begin = i + 1;
    size_t j = n;
    while (j > begin && is_space(s[j - 1])) --j;
    if (j > begin && s[j - 1] == L'|' && pipe_separator(s, j - 1)) end = j - 1;
    int count = 0;
    size_t seg = begin;
    for (size_t k = begin; k < end; k++) {
        if (s[k] != L'|' || !pipe_separator(s, k)) continue;
        if (count < capacity) {
            out[count].begin = seg;
            out[count].end = k;
            count++;
        } else if (overflow) {
            *overflow = true;
        }
        seg = k + 1;
    }
    if (count < capacity) {
        out[count].begin = seg;
        out[count].end = end;
        count++;
    } else if (overflow) {
        *overflow = true;
    }
    return count;
}

/* One delimiter cell: `:?-+:?` with at least one dash, trimmed. Alignment
   comes from the colons. */
static bool delimiter_cell(const wchar_t *s, size_t n, unsigned char *align) {
    size_t i = 0, j = n;
    while (i < j && is_space(s[i])) ++i;
    while (j > i && is_space(s[j - 1])) --j;
    bool left = false, right = false;
    if (i < j && s[i] == L':') { left = true; ++i; }
    if (j > i && s[j - 1] == L':') { right = true; --j; }
    if (i >= j) return false;
    for (size_t k = i; k < j; k++) if (s[k] != L'-') return false;
    *align = (unsigned char)(left && right ? MD_ALIGN_CENTER :
        right ? MD_ALIGN_RIGHT : MD_ALIGN_LEFT);
    return true;
}

/* Confirms a header/delimiter pair and returns its column count, or 0 when the
   pair is not a table. Both lines must be root-level; at least one unescaped
   pipe must appear in the pair, so "a" / "-" never becomes a one-column
   table. */
static int recognize_table(const wchar_t *header, size_t hn,
    const wchar_t *delim, size_t dn, MdCellRange *cells,
    unsigned char *align) {
    MdPrefix hp, dp;
    scan_prefix(header, hn, &hp);
    scan_prefix(delim, dn, &dp);
    if (hp.quote_depth || hp.has_marker || hp.rel > MD_ROOT_PREFIX_COLS)
        return 0;
    if (dp.quote_depth || dp.has_marker || dp.rel > MD_ROOT_PREFIX_COLS)
        return 0;
    if (line_is_blank(header, hn) || line_fence_open(header, hn) ||
        line_heading_root(header, hn, NULL, NULL)) return 0;
    if (!has_separator_pipe(header, hn) && !has_separator_pipe(delim, dn))
        return 0;
    bool h_overflow = false, d_overflow = false;
    int hc = split_row(header, hn, cells, MD_MAX_TABLE_COLUMNS, &h_overflow);
    MdCellRange dcells[MD_MAX_TABLE_COLUMNS];
    int dc = split_row(delim, dn, dcells, MD_MAX_TABLE_COLUMNS, &d_overflow);
    if (h_overflow || d_overflow || hc < 1 || hc != dc) return 0;
    for (int i = 0; i < hc; i++) {
        if (!delimiter_cell(delim + dcells[i].begin,
            dcells[i].end - dcells[i].begin, &align[i])) return 0;
    }
    return hc;
}

/* Emits one cell's content. Escaped pipes are resolved (one backslash is
   consumed) before the inline pass, so `\|` displays `|` even inside a code
   span; other backslashes are left for parse_inline. Whitespace is trimmed and
   the cell range is recorded even when empty. */
static void emit_table_cell(MdBuilder *b, const wchar_t *line, size_t begin,
    size_t end, unsigned style) {
    if (b->failed) return;
    size_t span = end > begin ? end - begin : 0;
    if (!grow_scratch(b, span)) return;
    wchar_t *buffer = b->scratch;
    size_t out = 0;
    for (size_t i = begin; i < end;) {
        if (line[i] != L'\\') { buffer[out++] = line[i++]; continue; }
        size_t j = i;
        while (j < end && line[j] == L'\\') ++j;
        size_t run = j - i;
        if (j < end && line[j] == L'|' && (run % 2) == 1) {
            for (size_t k = 1; k < run; k++) buffer[out++] = L'\\';
            buffer[out++] = L'|';
            i = j + 1;
        } else {
            for (size_t k = 0; k < run; k++) buffer[out++] = L'\\';
            i = j;
        }
    }
    size_t from = 0, to = out;
    while (from < to && is_space(buffer[from])) ++from;
    while (to > from && is_space(buffer[to - 1])) --to;
    size_t offset = b->length;
    if (to > from) {
        MdStyle s = {0};
        s.style = style;
        parse_inline(b, buffer + from, to - from, s);
    }
    if (!grow_cells(b)) return;
    b->cells[b->cell_count].offset = offset;
    b->cells[b->cell_count].length = b->length - offset;
    b->cell_count++;
}

/* Records one row and emits exactly `columns` cells, padding missing cells with
   zero-length ranges and truncating extra ones. */
static void emit_table_row(MdBuilder *b, const wchar_t *line,
    const MdCellRange *cells, int cell_count, int columns, unsigned style) {
    if (b->failed) return;
    if (!grow_rows(b)) return;
    int row = b->row_count++;
    b->rows[row].first_cell = b->cell_count;
    b->rows[row].cell_count = columns;
    for (int col = 0; col < columns; col++) {
        if (col < cell_count)
            emit_table_cell(b, line, cells[col].begin, cells[col].end, style);
        else
            emit_table_cell(b, line, 0, 0, style);
    }
}

static void append_literal(MdBuilder *b, const wchar_t *text, size_t length,
    bool newline) {
    if (b->failed) return;
    if (!grow_literals(b, length + (newline ? 1u : 0u))) return;
    if (newline) b->literals[b->literals_length++] = L'\n';
    if (length) wmemcpy(b->literals + b->literals_length, text, length);
    b->literals_length += length;
}

/* The first character of the next line, or NULL when `end` terminates the
   source. An empty final line yields NULL as well: it can never be a valid
   delimiter. */
static const wchar_t *line_after(const wchar_t *end) {
    if (!*end) return NULL;
    const wchar_t *p = end + 1;
    if (*end == L'\r' && *p == L'\n') ++p;
    return *p ? p : NULL;
}

/* True when `p` continues the currently open list item: no marker, the same
   quote depth (a quote boundary starts a fresh list), and indented past the
   open level's own column. Mirrors render_line's continuation branch, so a
   root-only table cannot hijack an indented list continuation. */
static bool line_continues_list(const MdBlocks *blocks, const MdPrefix *p) {
    if (blocks->depth <= 0) return false;
    if (p->quote_depth != blocks->quote_depth) return false;
    if (p->has_marker) return false;
    return p->rel > blocks->level[blocks->depth - 1].column;
}

/* Recognizes one table beginning at `start`. Returns the number of source
   wchars consumed (the table lines, terminators excluded), or 0 when `start`
   does not open a table; the terminating line is left to render_line. A
   recognized root table ends any open list, exactly as an ordinary root
   paragraph would. */
static size_t table_scan(MdBuilder *b, const wchar_t *start,
    MdBlocks *blocks) {
    const wchar_t *hend = start;
    while (*hend && *hend != L'\n' && *hend != L'\r') ++hend;
    MdPrefix hp;
    scan_prefix(start, (size_t)(hend - start), &hp);
    if (line_continues_list(blocks, &hp)) return 0;
    const wchar_t *dline = line_after(hend);
    if (!dline) return 0;
    const wchar_t *dend = dline;
    while (*dend && *dend != L'\n' && *dend != L'\r') ++dend;
    MdCellRange hcells[MD_MAX_TABLE_COLUMNS];
    unsigned char align[MD_MAX_TABLE_COLUMNS];
    int columns = recognize_table(start, (size_t)(hend - start), dline,
        (size_t)(dend - dline), hcells, align);
    if (columns <= 0) return 0;

    blocks->depth = 0;
    blocks->quote_depth = 0;

    int table_index = b->table_count;
    if (!grow_tables(b)) return 0;
    MdTable *table = &b->tables[b->table_count++];
    table->first_row = b->row_count;
    table->first_cell = b->cell_count;
    table->columns = columns;
    for (int i = 0; i < columns; i++) table->align[i] = align[i];
    table->literal_offset = b->literals_length;
    table->literal_length = 0;

    emit_break(b, 0);
    size_t block_start = b->length;
    emit_table_row(b, start, hcells, columns, columns, MD_STYLE_BOLD);
    append_literal(b, start, (size_t)(hend - start), false);
    append_literal(b, dline, (size_t)(dend - dline), true);

    const wchar_t *last_end = dend;
    const wchar_t *row = line_after(dend);
    while (row) {
        size_t rn = 0;
        while (row[rn] && row[rn] != L'\n' && row[rn] != L'\r') ++rn;
        if (line_starts_block(row, rn)) break;
        MdCellRange rcells[MD_MAX_TABLE_COLUMNS];
        int rcount = split_row(row, rn, rcells, columns, NULL);
        emit_table_row(b, row, rcells, rcount, columns, 0);
        append_literal(b, row, rn, true);
        last_end = row + rn;
        row = line_after(row + rn);
    }

    record_table_block(b, block_start, table_index);
    table->row_count = b->row_count - table->first_row;
    table->cell_count = b->cell_count - table->first_cell;
    table->literal_length = b->literals_length - table->literal_offset;
    return (size_t)(last_end - start);
}

static void render_document(MdBuilder *b, const wchar_t *source) {
    const wchar_t *line = source ? source : L"";
    MdFence fence = {0};
    MdBlocks blocks = {0};
    for (;;) {
        const wchar_t *end = line;
        while (*end && *end != L'\n' && *end != L'\r') ++end;
        if (!fence.delimiter) {
            size_t consumed = table_scan(b, line, &blocks);
            if (consumed) {
                line += consumed;
                if (!*line) break;
                if (*line == L'\r' && line[1] == L'\n') line += 2;
                else if (*line == L'\r' || *line == L'\n') line += 1;
                continue;
            }
        }
        render_line(b, line, (size_t)(end - line), &fence, &blocks);
        if (!*end) break;
        line = end + 1;
        if (*end == L'\r' && *line == L'\n') ++line;
    }
    /* An unterminated fence still records what it emitted. */
    close_fence_record(b, &fence);
}

bool markdown_render(const wchar_t *source, MdDocument *doc) {
    if (!doc) return false;
    memset(doc, 0, sizeof *doc);
    MdBuilder b;
    memset(&b, 0, sizeof b);
    render_document(&b, source);
    /* Reserve the terminator even for an empty document. */
    if (!b.failed) grow_text(&b, 0);
    if (b.failed) {
        free(b.text);
        free(b.runs);
        free(b.blocks);
        free(b.links);
        free(b.targets);
        free(b.cells);
        free(b.rows);
        free(b.tables);
        free(b.literals);
        free(b.fences);
        free(b.scratch);
        return false;                       /* *doc stays zeroed */
    }
    free(b.scratch);
    b.text[b.length] = L'\0';
    doc->text = b.text;
    doc->length = b.length;
    doc->runs = b.runs;
    doc->run_count = b.run_count;
    doc->run_capacity = b.run_capacity;
    doc->blocks = b.blocks;
    doc->block_count = b.block_count;
    doc->block_capacity = b.block_capacity;
    doc->links = b.links;
    doc->link_count = b.link_count;
    doc->link_capacity = b.link_capacity;
    doc->targets = b.targets;
    doc->targets_length = b.targets_length;
    doc->targets_capacity = b.targets_capacity;
    doc->cells = b.cells;
    doc->cell_count = b.cell_count;
    doc->cell_capacity = b.cell_capacity;
    doc->rows = b.rows;
    doc->row_count = b.row_count;
    doc->row_capacity = b.row_capacity;
    doc->tables = b.tables;
    doc->table_count = b.table_count;
    doc->table_capacity = b.table_capacity;
    doc->literals = b.literals;
    doc->literals_length = b.literals_length;
    doc->literals_capacity = b.literals_capacity;
    doc->fences = b.fences;
    doc->fence_count = b.fence_count;
    doc->fence_capacity = b.fence_capacity;
    return true;
}
