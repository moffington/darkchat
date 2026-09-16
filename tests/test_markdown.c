/* Pure Markdown parser tests; no Win32, like test_json. */
#include "../chat/markdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); ++failures; }
}

/* Finds the run covering the first occurrence of needle in the document. */
static const MdRun *run_over(const MdDocument *d, const wchar_t *needle) {
    const wchar_t *at = d->text ? wcsstr(d->text, needle) : NULL;
    if (!at) return NULL;
    size_t offset = (size_t)(at - d->text);
    size_t end = offset + wcslen(needle);
    for (int i = 0; i < d->run_count; i++) {
        const MdRun *r = &d->runs[i];
        if (r->offset <= offset && end <= r->offset + r->length) return r;
    }
    return NULL;
}

static void text_is(const MdDocument *d, const wchar_t *expect,
    const char *what) {
    check(d->text && !wcscmp(d->text, expect), what);
}

static void render_ok(const wchar_t *src, MdDocument *d, const char *what) {
    check(markdown_render(src, d), what);
}

int main(void) {
    { /* Plain text stays one coalesced run, newlines included. */
        MdDocument d; render_ok(L"Hello world", &d, "plain renders");
        text_is(&d, L"Hello world", "plain text unchanged");
        check(d.run_count == 1, "plain is a single run");
        markdown_dispose(&d);
    }
    { MdDocument d; render_ok(L"a\nb\r\n\rc", &d, "newline forms render");
        text_is(&d, L"a\nb\n\nc", "CRLF/CR normalized to LF");
        check(d.run_count == 1, "breaks coalesce into the plain run");
        markdown_dispose(&d); }
    { /* Empty and NULL sources succeed with an empty document. */
        MdDocument d; render_ok(L"", &d, "empty renders");
        check(d.length == 0 && d.run_count == 0, "empty document");
        markdown_dispose(&d);
        render_ok(NULL, &d, "null renders");
        check(d.length == 0 && d.run_count == 0, "null document");
        markdown_dispose(&d); }
    { /* Headings: 1-3 with a required space after the marker. */
        MdDocument d;
        render_ok(L"# One\n## Two\n### Three\n#### Four\n#NoSpace",
            &d, "headings render");
        text_is(&d, L"One\nTwo\nThree\n#### Four\n#NoSpace",
            "markers removed, deeper or spaceless kept literal");
        const MdRun *h1 = run_over(&d, L"One");
        const MdRun *h3 = run_over(&d, L"Three");
        const MdRun *h4 = run_over(&d, L"Four");
        check(h1 && h1->bold && h1->heading == 1, "h1 bold level 1");
        check(h3 && h3->bold && h3->heading == 3, "h3 bold level 3");
        check(h4 && !h4->bold && h4->heading == 0, "h4 literal");
        markdown_dispose(&d); }
    { /* Emphasis, with word-internal delimiters staying literal. */
        MdDocument d;
        render_ok(L"a **bold** b *it* c _ul_ d snake_case_name e 2*3*4",
            &d, "emphasis renders");
        text_is(&d,
            L"a bold b it c ul d snake_case_name e 2*3*4",
            "markers removed, identifiers and math literal");
        const MdRun *bold = run_over(&d, L"bold");
        const MdRun *it = run_over(&d, L"it");
        const MdRun *ul = run_over(&d, L"ul");
        const MdRun *snake = run_over(&d, L"snake_case_name");
        check(bold && bold->bold && !bold->italic, "bold run");
        check(it && it->italic && !it->bold, "italic run");
        check(ul && ul->italic && !ul->bold, "underscore italic run");
        check(snake && !snake->italic && !snake->bold, "identifier literal");
        markdown_dispose(&d); }
    { /* Malformed emphasis is preserved verbatim. */
        MdDocument d; render_ok(L"a **bold b *open c", &d, "malformed renders");
        text_is(&d, L"a **bold b *open c", "malformed kept literally");
        check(d.run_count == 1, "malformed is one plain run");
        markdown_dispose(&d); }
    { /* Inline code wins over emphasis and keeps its content raw. */
        MdDocument d; render_ok(L"x `*a*` y", &d, "code renders");
        text_is(&d, L"x *a* y", "code content raw");
        const MdRun *code = run_over(&d, L"*a*");
        check(code && code->mono && code->code && !code->italic,
            "code run mono, not italic");
        markdown_dispose(&d); }
    { /* Paired tildes strike non-space content and compose with outer styles. */
        MdDocument d;
        render_ok(L"a ~~old~~ b **~~bold~~** ~~`code`~~ "
                  L"~~[link](https://x.io/a)~~", &d, "strike renders");
        text_is(&d, L"a old b bold code link (https://x.io/a)",
            "strike markers removed");
        const MdRun *old = run_over(&d, L"old");
        const MdRun *bold = run_over(&d, L"bold");
        const MdRun *code = run_over(&d, L"code");
        const MdRun *link = run_over(&d, L"link (https://x.io/a)");
        check(old && old->strike && !old->bold, "plain strike run");
        check(bold && bold->strike && bold->bold, "bold strike run");
        check(code && code->strike && code->mono && code->code,
            "code inside strike keeps styles");
        check(link && link->strike, "link inside strike keeps style");
        markdown_dispose(&d); }
    { /* Unmatched, spaced and escaped paired tildes stay literal. */
        MdDocument d;
        render_ok(L"~one~ ~~ open~~", &d,
            "literal tildes render");
        text_is(&d, L"~one~ ~~ open~~",
            "invalid strike syntax literal");
        check(d.run_count == 1 && !d.runs[0].strike,
            "invalid strike has no style");
        markdown_dispose(&d);
        render_ok(L"~~open ~~", &d, "trailing-space strike renders");
        text_is(&d, L"~~open ~~", "trailing-space strike literal");
        check(d.run_count == 1 && !d.runs[0].strike,
            "trailing-space strike has no style");
        markdown_dispose(&d);
        render_ok(L"\\~~escaped~~", &d, "escaped strike renders");
        text_is(&d, L"~~escaped~~", "escaped strike literal");
        check(d.run_count == 1 && !d.runs[0].strike,
            "escaped strike has no style");
        markdown_dispose(&d); }
    { /* Inline and fenced code shield tildes from strikethrough parsing. */
        MdDocument d;
        render_ok(L"`~~inline~~`\n```\n~~fenced~~\n```", &d,
            "code shields strike");
        text_is(&d, L"~~inline~~\n~~fenced~~", "code tildes retained");
        const MdRun *inline_code = run_over(&d, L"~~inline~~");
        const MdRun *fenced_code = run_over(&d, L"~~fenced~~");
        check(inline_code && inline_code->mono && !inline_code->strike,
            "inline code is not struck");
        check(fenced_code && fenced_code->mono && fenced_code->code &&
            !fenced_code->strike, "fenced code is not struck");
        markdown_dispose(&d); }
    { /* Fenced code: markers and language tag hidden, content literal. */
        MdDocument d; render_ok(L"before\n```c\nint x; // *not*\n```\nafter",
            &d, "fence renders");
        text_is(&d, L"before\nint x; // *not*\nafter",
            "fence lines removed, content raw");
        const MdRun *code = run_over(&d, L"int x;");
        check(code && code->mono && code->code, "fence run mono+code");
        const MdRun *after = run_over(&d, L"after");
        check(after && !after->mono && !after->code, "text after fence plain");
        check(wcsstr(d.text, L"c\n") == NULL, "language tag hidden");
        markdown_dispose(&d); }
    { /* Unterminated fence keeps the rest as literal code. */
        MdDocument d; render_ok(L"```js\nlet x = 1;", &d, "open fence renders");
        text_is(&d, L"let x = 1;", "open fence content kept");
        const MdRun *code = run_over(&d, L"let x = 1;");
        check(code && code->mono && code->code, "open fence run mono+code");
        markdown_dispose(&d); }
    { /* HTTP(S) links become "label (url)"; other schemes stay literal. */
        MdDocument d;
        render_ok(L"[site](https://example.com/x) [f](ftp://y.io/a) "
                  L"[bad](https://open", &d, "links render");
        text_is(&d, L"site (https://example.com/x) [f](ftp://y.io/a) "
                  L"[bad](https://open", "https expanded, others literal");
        markdown_dispose(&d); }
    { /* Flat lists: textual bullets and preserved ordered markers. */
        MdDocument d; render_ok(L"- a\n* b\n1. first\n10. tenth\n5.x",
            &d, "lists render");
        text_is(&d, L"\u2022 a\n\u2022 b\n1. first\n10. tenth\n5.x",
            "bullets synthesized, ordered markers preserved");
        markdown_dispose(&d); }
    { /* Blockquotes: bar prefix, muted content, inline parsing inside. */
        MdDocument d; render_ok(L"> quoted **bold** text", &d, "quote renders");
        text_is(&d, L"\u258C quoted bold text", "quote bar and content");
        const MdRun *q = run_over(&d, L"quoted ");
        check(q && q->muted, "quote run muted");
        const MdRun *bold = run_over(&d, L"bold");
        check(bold && bold->bold && bold->muted, "quote inline styles nest");
        markdown_dispose(&d); }
    { /* Escapes keep punctuation literal. */
        MdDocument d; render_ok(L"\\*not emphasis\\* and \\`code\\`",
            &d, "escapes render");
        text_is(&d, L"*not emphasis* and `code`", "escaped markers literal");
        const MdRun *r = run_over(&d, L"not emphasis");
        check(r && !r->italic, "escaped star not emphasis");
        markdown_dispose(&d); }
    { /* Emphasis nests around code and links. */
        MdDocument d;
        render_ok(L"**bold `code` and [l](https://x.io/a) tail**",
            &d, "nesting renders");
        const MdRun *code = run_over(&d, L"code");
        check(code && code->mono && code->code && code->bold,
            "code inside bold keeps both");
        const MdRun *l = run_over(&d, L"l (https://x.io/a)");
        check(l && l->bold, "link inside bold");
        markdown_dispose(&d); }
    { /* Long input completes with linear work. */
        size_t n = 100000;
        wchar_t *src = (wchar_t *)malloc((n + 8) * sizeof(wchar_t));
        check(src != NULL, "long source allocated");
        if (src) {
            for (size_t i = 0; i < n; i++) src[i] = L'a';
            src[n] = 0;
            wcscat(src, L" **b**");
            MdDocument d; render_ok(src, &d, "long renders");
            check(d.length == n + 2, "long length exact");
            const MdRun *bold = run_over(&d, L"b");
            check(bold && bold->bold, "long tail bold");
            markdown_dispose(&d);
            free(src);
        } }
    { /* Transactional allocation failure leaves a zeroed document. */
        MdDocument d;
        markdown_test_fail_allocations(true);
        check(!markdown_render(L"**x**", &d), "failure reported");
        check(d.text == NULL && d.runs == NULL && d.run_count == 0 &&
            d.length == 0, "failed document zeroed");
        markdown_test_fail_allocations(false);
        render_ok(L"**x**", &d, "render works again after failure");
        text_is(&d, L"x", "post-failure render correct");
        markdown_dispose(&d); }
    markdown_dispose(NULL);
    if (failures) { printf("%d markdown test(s) failed\n", failures); return 1; }
    puts("Markdown parser: headings, emphasis, strikethrough, code, fences, "
        "links, lists, quotes, escapes, CRLF, long input and allocation "
        "fallback passed");
    return 0;
}
