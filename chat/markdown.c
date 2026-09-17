#include "markdown.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* The renderer builds one document buffer (source text plus synthesized
   bullets, quote bars and link URLs) and appends runs linearly, so memory is
   O(input) and adjacent runs with identical style coalesce into one. Every
   failure aborts the whole document; the caller falls back to verbatim text. */

#define MD_NPOS ((size_t)-1)

static bool test_fail_allocations;
static bool test_fail_blocks;

void markdown_test_fail_allocations(bool enable) {
    test_fail_allocations = enable;
}

void markdown_test_fail_blocks(bool enable) {
    test_fail_blocks = enable;
}

void markdown_dispose(MdDocument *doc) {
    if (!doc) return;
    free(doc->text);
    free(doc->runs);
    free(doc->blocks);
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
                    /* The label keeps the current style; the URL follows in
                       parentheses so the display's URL detector opens it. */
                    emit(b, s + i + 1, label_end - i - 1, style);
                    emit(b, L" (", 2, style);
                    emit(b, s + url, url_end - url, style);
                    emit(b, L")", 1, style);
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
    MdFence marker;
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

/* One source line; blocks are recognized line-by-line and fences hide their
   marker lines. Newlines are normalized to LF. */
static void render_line(MdBuilder *b, const wchar_t *line, size_t n,
    MdFence *fence, MdBlocks *blocks) {
    MdStyle plain = {0};
    if (fence->delimiter) {
        if (fence_closes(line, n, fence)) {
            memset(fence, 0, sizeof *fence); /* closing fence is hidden */
        } else {
            MdStyle code = {0};
            code.style = MD_STYLE_MONO | MD_STYLE_CODE;
            size_t from = 0;
            while (from < n && from < fence->indent && line[from] == L' ')
                ++from;
            emit_break(b, code.style);
            size_t start = b->length;
            emit(b, line + from, n - from, code);
            close_block(b, start, MD_BLOCK_CODE, 0, 0, 0, 0, 0);
        }
        return;
    }
    MdFence marker;
    if (fence_marker(line, n, &marker)) {
        *fence = marker;                    /* opening fence is hidden */
        return;
    }
    bool blank = true;
    for (size_t i = 0; i < n; i++) if (!is_space(line[i])) { blank = false; break; }
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
        p.rel <= MD_ROOT_PREFIX_COLS) {
        size_t hashes = 0;
        while (p.content + hashes < n && line[p.content + hashes] == L'#')
            ++hashes;
        if (hashes >= 1 && hashes <= 3) {
            size_t from = p.content + hashes;
            if (from == n || is_space(line[from])) {
                heading = true;
                heading_level = (int)hashes;
                heading_from = from;
                while (heading_from < n && is_space(line[heading_from]))
                    ++heading_from;
            }
        }
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

static void render_document(MdBuilder *b, const wchar_t *source) {
    const wchar_t *line = source ? source : L"";
    MdFence fence = {0};
    MdBlocks blocks = {0};
    for (;;) {
        const wchar_t *end = line;
        while (*end && *end != L'\n' && *end != L'\r') ++end;
        render_line(b, line, (size_t)(end - line), &fence, &blocks);
        if (!*end) break;
        line = end + 1;
        if (*end == L'\r' && *line == L'\n') ++line;
    }
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
        return false;                       /* *doc stays zeroed */
    }
    b.text[b.length] = L'\0';
    doc->text = b.text;
    doc->length = b.length;
    doc->runs = b.runs;
    doc->run_count = b.run_count;
    doc->run_capacity = b.run_capacity;
    doc->blocks = b.blocks;
    doc->block_count = b.block_count;
    doc->block_capacity = b.block_capacity;
    return true;
}
