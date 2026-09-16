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

void markdown_test_fail_allocations(bool enable) {
    test_fail_allocations = enable;
}

void markdown_dispose(MdDocument *doc) {
    if (!doc) return;
    free(doc->text);
    free(doc->runs);
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

/* One source line; blocks are recognized line-by-line and fences hide their
   marker lines. Newlines are normalized to LF. */
static void render_line(MdBuilder *b, const wchar_t *line, size_t n,
    bool *fence) {
    size_t indent = 0;
    while (indent < n && indent < 3 && line[indent] == L' ') ++indent;
    const wchar_t *c = line + indent;
    size_t m = n - indent;
    bool marker = m >= 3 && c[0] == L'`' && c[1] == L'`' && c[2] == L'`';
    MdStyle plain = {0};
    if (*fence) {
        if (marker) {
            *fence = false;                 /* closing fence is hidden */
        } else {
            MdStyle code = {0};
            code.style = MD_STYLE_MONO | MD_STYLE_CODE;
            emit_break(b, code.style);
            emit(b, c, m, code);
        }
        return;
    }
    if (marker) {
        *fence = true;                      /* opening fence is hidden */
        return;
    }
    bool blank = true;
    for (size_t i = 0; i < m; i++) if (!is_space(c[i])) { blank = false; break; }
    if (blank) {
        emit_break(b, 0);
        return;
    }
    if (c[0] == L'#') {
        int level = 0;
        while (level < (int)m && c[level] == L'#') ++level;
        if (level >= 1 && level <= 3 &&
            ((size_t)level == m || is_space(c[level]))) {
            size_t from = (size_t)level;
            while (from < m && is_space(c[from])) ++from;
            emit_break(b, 0);
            MdStyle heading = {0};
            heading.style = MD_STYLE_BOLD;
            heading.heading = level;
            parse_inline(b, c + from, m - from, heading);
            return;
        }
    } else if (c[0] == L'>' && (m == 1 || is_space(c[1]))) {
        size_t from = m == 1 ? m : 1;
        while (from < m && is_space(c[from])) ++from;
        emit_break(b, 0);
        emit(b, L"\u258C ", 2, plain);      /* quote bar, muted content */
        MdStyle quoted = plain;
        quoted.style |= MD_STYLE_MUTED;
        parse_inline(b, c + from, m - from, quoted);
        return;
    } else if ((c[0] == L'-' || c[0] == L'*') && m >= 2 && is_space(c[1])) {
        size_t from = 1;
        while (from < m && is_space(c[from])) ++from;
        emit_break(b, 0);
        bool checked;
        if (task_marker(c + from, m - from, &checked)) {
            from += 3;
            while (from < m && is_space(c[from])) ++from;
            emit(b, checked ? L"\u2611 " : L"\u2610 ", 2, plain);
        } else {
            emit(b, L"\u2022 ", 2, plain);  /* textual bullet, no indent state */
        }
        parse_inline(b, c + from, m - from, plain);
        return;
    } else {
        size_t digits = 0;
        while (digits < m && digits < 9 && c[digits] >= L'0' &&
            c[digits] <= L'9') ++digits;
        if (digits >= 1 && digits < m &&
            (c[digits] == L'.' || c[digits] == L')') &&
            digits + 1 < m && is_space(c[digits + 1])) {
            size_t from = digits + 1;
            while (from < m && is_space(c[from])) ++from;
            emit_break(b, 0);
            emit(b, c, digits + 2, plain);  /* ordered marker kept verbatim */
            parse_inline(b, c + from, m - from, plain);
            return;
        }
    }
    emit_break(b, 0);
    parse_inline(b, c, m, plain);
}

static void render_document(MdBuilder *b, const wchar_t *source) {
    const wchar_t *line = source ? source : L"";
    bool fence = false;
    for (;;) {
        const wchar_t *end = line;
        while (*end && *end != L'\n' && *end != L'\r') ++end;
        render_line(b, line, (size_t)(end - line), &fence);
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
        return false;                       /* *doc stays zeroed */
    }
    b.text[b.length] = L'\0';
    doc->text = b.text;
    doc->length = b.length;
    doc->runs = b.runs;
    doc->run_count = b.run_count;
    doc->run_capacity = b.run_capacity;
    return true;
}
