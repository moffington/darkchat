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

static bool style_is(const MdRun *run, unsigned style) {
    return run && run->style == style;
}

int main(void) {
    check((MD_STYLE_MONO & MD_STYLE_CODE) == 0,
        "mono and code flags do not overlap");
    { /* Plain text stays one coalesced run, newlines included. */
        MdDocument d; render_ok(L"Hello world", &d, "plain renders");
        text_is(&d, L"Hello world", "plain text unchanged");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "plain is a single unstyled run");
        markdown_dispose(&d);
    }
    { MdDocument d; render_ok(L"a\nb\r\n\rc", &d, "newline forms render");
        text_is(&d, L"a\nb\n\nc", "CRLF/CR normalized to LF");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "breaks coalesce into the plain run");
        markdown_dispose(&d); }
    { /* Identical adjacent styles coalesce; differing complete styles do not. */
        MdDocument d; render_ok(L"**a****b**~~c~~", &d,
            "adjacent style runs render");
        text_is(&d, L"abc", "adjacent markers removed");
        check(d.run_count == 2, "only identical adjacent styles coalesce");
        check(d.run_count == 2 && d.runs[0].length == 2 &&
            d.runs[0].style == MD_STYLE_BOLD,
            "adjacent bold runs coalesce exactly");
        check(d.run_count == 2 && d.runs[1].length == 1 &&
            d.runs[1].style == MD_STYLE_STRIKE,
            "different style value starts a new run");
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
        check(style_is(h1, MD_STYLE_BOLD) && h1->heading == 1,
            "h1 bold level 1");
        check(style_is(h3, MD_STYLE_BOLD) && h3->heading == 3,
            "h3 bold level 3");
        check(style_is(h4, 0) && h4->heading == 0, "h4 literal");
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
        check(style_is(bold, MD_STYLE_BOLD), "bold run");
        check(style_is(it, MD_STYLE_ITALIC), "italic run");
        check(style_is(ul, MD_STYLE_ITALIC), "underscore italic run");
        check(style_is(snake, 0), "identifier literal");
        markdown_dispose(&d); }
    { /* Malformed emphasis is preserved verbatim. */
        MdDocument d; render_ok(L"a **bold b *open c", &d, "malformed renders");
        text_is(&d, L"a **bold b *open c", "malformed kept literally");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "malformed is one plain run");
        markdown_dispose(&d); }
    { /* Inline code wins over emphasis and keeps its content raw. */
        MdDocument d; render_ok(L"x `*a*` y", &d, "code renders");
        text_is(&d, L"x *a* y", "code content raw");
        const MdRun *code = run_over(&d, L"*a*");
        check(style_is(code, MD_STYLE_MONO | MD_STYLE_CODE),
            "code run is exactly mono and code");
        markdown_dispose(&d); }
    { /* Inline code closes only on a later equal-length backtick run. */
        MdDocument d;
        render_ok(L"``code with ` inside`` and ``one ``` two``", &d,
            "variable backtick code renders");
        text_is(&d, L"code with ` inside and one ``` two",
            "outer variable delimiters removed exactly");
        check(style_is(run_over(&d, L"code with ` inside"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "shorter embedded run stays raw code content");
        check(style_is(run_over(&d, L"one ``` two"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "longer embedded run stays raw code content");
        markdown_dispose(&d); }
    { /* Triple backticks are inline delimiters away from the line start. */
        MdDocument d;
        render_ok(L"a ```code with `` inside``` b", &d,
            "triple backtick code renders");
        text_is(&d, L"a code with `` inside b",
            "triple delimiters removed exactly");
        check(style_is(run_over(&d, L"code with `` inside"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "triple code keeps shorter run as content");
        markdown_dispose(&d); }
    { /* An unmatched maximal run stays literal and is not split into openers. */
        MdDocument d;
        render_ok(L"``open` and ```x", &d, "mismatched backticks render");
        text_is(&d, L"``open` and ```x",
            "mismatched backtick runs stay literal");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "mismatched backticks have no code style");
        markdown_dispose(&d);
        render_ok(L"\\``escaped``", &d, "escaped backtick run renders");
        text_is(&d, L"``escaped``", "escaped opening backtick stays literal");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "escaped backtick run has no code style");
        markdown_dispose(&d);
        render_ok(L"\\``code`", &d, "escaped backtick has one-character scope");
        text_is(&d, L"`code", "only the escaped backtick stays literal");
        check(style_is(run_over(&d, L"code"), MD_STYLE_MONO | MD_STYLE_CODE),
            "following backtick can still open code");
        markdown_dispose(&d);
        render_ok(L"``open\r\nclose``", &d, "multiline code stays separate");
        text_is(&d, L"``open\nclose``",
            "inline code does not cross normalized source lines");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "cross-line delimiters have no code style");
        markdown_dispose(&d); }
    { /* Variable-length inline code remains raw and composes with outer style. */
        MdDocument d;
        render_ok(L"**``*raw* ~~old~~ [x](https://x.io)``**", &d,
            "nested variable backtick code renders");
        text_is(&d, L"*raw* ~~old~~ [x](https://x.io)",
            "variable code shields inline syntax");
        check(style_is(run_over(&d, L"*raw* ~~old~~ [x](https://x.io)"),
            MD_STYLE_BOLD | MD_STYLE_MONO | MD_STYLE_CODE),
            "variable code composes with outer bold only");
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
        check(style_is(old, MD_STYLE_STRIKE), "plain strike run");
        check(style_is(bold, MD_STYLE_BOLD | MD_STYLE_STRIKE),
            "bold strike run");
        check(style_is(code, MD_STYLE_STRIKE | MD_STYLE_MONO | MD_STYLE_CODE),
            "code inside strike keeps styles");
        check(style_is(link, MD_STYLE_STRIKE), "link inside strike keeps style");
        markdown_dispose(&d); }
    { /* Unmatched, spaced and escaped paired tildes stay literal. */
        MdDocument d;
        render_ok(L"~one~ ~~ open~~", &d,
            "literal tildes render");
        text_is(&d, L"~one~ ~~ open~~",
            "invalid strike syntax literal");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "invalid strike has no style");
        markdown_dispose(&d);
        render_ok(L"~~open ~~", &d, "trailing-space strike renders");
        text_is(&d, L"~~open ~~", "trailing-space strike literal");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "trailing-space strike has no style");
        markdown_dispose(&d);
        render_ok(L"\\~~escaped~~", &d, "escaped strike renders");
        text_is(&d, L"~~escaped~~", "escaped strike literal");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "escaped strike has no style");
        markdown_dispose(&d); }
    { /* Inline and fenced code shield tildes from strikethrough parsing. */
        MdDocument d;
        render_ok(L"`~~inline~~`\n```\n~~fenced~~\n```", &d,
            "code shields strike");
        text_is(&d, L"~~inline~~\n~~fenced~~", "code tildes retained");
        const MdRun *inline_code = run_over(&d, L"~~inline~~");
        const MdRun *fenced_code = run_over(&d, L"~~fenced~~");
        check(style_is(inline_code, MD_STYLE_MONO | MD_STYLE_CODE),
            "inline code is not struck");
        check(style_is(fenced_code, MD_STYLE_MONO | MD_STYLE_CODE),
            "fenced code is not struck");
        markdown_dispose(&d); }
    { /* Fenced code: markers and language tag hidden, content literal. */
        MdDocument d; render_ok(L"before\n```c\nint x; // *not*\n```\nafter",
            &d, "fence renders");
        text_is(&d, L"before\nint x; // *not*\nafter",
            "fence lines removed, content raw");
        const MdRun *code = run_over(&d, L"int x;");
        check(style_is(code, MD_STYLE_MONO | MD_STYLE_CODE),
            "fence run mono+code");
        const MdRun *after = run_over(&d, L"after");
        check(style_is(after, 0), "text after fence plain");
        check(wcsstr(d.text, L"c\n") == NULL, "language tag hidden");
        markdown_dispose(&d); }
    { /* Unterminated fence keeps the rest as literal code. */
        MdDocument d; render_ok(L"```js\nlet x = 1;", &d, "open fence renders");
        text_is(&d, L"let x = 1;", "open fence content kept");
        const MdRun *code = run_over(&d, L"let x = 1;");
        check(style_is(code, MD_STYLE_MONO | MD_STYLE_CODE),
            "open fence run mono+code");
        markdown_dispose(&d); }
    { /* Fences match their delimiter and opener length; longer closers work. */
        MdDocument d;
        render_ok(L"before\n````c\n```\n~~~~\n```` text\n``````\nafter", &d,
            "strict backtick fence renders");
        text_is(&d, L"before\n```\n~~~~\n```` text\nafter",
            "invalid fence closers remain code content");
        check(style_is(run_over(&d, L"```\n~~~~\n```` text"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "short wrong and text-suffixed closers are code");
        check(style_is(run_over(&d, L"after"), 0),
            "longer closing fence restores plain text");
        markdown_dispose(&d); }
    { /* Tilde fences hide info strings and shield all inline syntax. */
        MdDocument d;
        render_ok(L"~~~lang`accepted\n~~old~~\n- [x] task\n~~~", &d,
            "tilde fence renders");
        text_is(&d, L"~~old~~\n- [x] task", "tilde content stays raw");
        check(style_is(run_over(&d, L"~~old~~\n- [x] task"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "tilde fence shields strike and task syntax");
        markdown_dispose(&d); }
    { /* Fenced content removes only the recorded opener indentation. */
        MdDocument d;
        render_ok(L"  ````\n  zero\n    two\n\t tab\n   ````\n  plain\n    deep",
            &d, "fence indentation renders");
        text_is(&d, L"zero\n  two\n\t tab\nplain\n deep",
            "fence indentation is distinct from ordinary lines");
        check(style_is(run_over(&d, L"  two"), MD_STYLE_MONO | MD_STYLE_CODE),
            "remaining fenced indentation is code");
        check(style_is(run_over(&d, L"plain"), 0),
            "ordinary indentation behavior remains unchanged");
        markdown_dispose(&d); }
    { /* Four leading spaces never open a fence; CRLF fences normalize normally. */
        MdDocument d;
        render_ok(L"    ```\nplain", &d, "four-space fence marker renders");
        text_is(&d, L" ```\nplain", "four-space marker stays ordinary text");
        check(d.run_count == 1 && d.runs[0].style == 0,
            "four-space marker has no code style");
        markdown_dispose(&d);
        render_ok(L"~~~info\r\n**raw**\r\n~~~~\t\r\nafter", &d,
            "CRLF tilde fence renders");
        text_is(&d, L"**raw**\nafter", "CRLF fence lines normalize and hide");
        check(style_is(run_over(&d, L"**raw**"),
            MD_STYLE_MONO | MD_STYLE_CODE), "CRLF fenced content is code");
        markdown_dispose(&d); }
    { /* An unterminated fence keeps invalid closer candidates as code to EOF. */
        MdDocument d;
        render_ok(L"````\n```\n~~~~\n```` text", &d,
            "unterminated strict fence renders");
        text_is(&d, L"```\n~~~~\n```` text",
            "unterminated fence preserves invalid closers");
        check(style_is(run_over(&d, L"```\n~~~~\n```` text"),
            MD_STYLE_MONO | MD_STYLE_CODE), "unterminated strict fence is code");
        markdown_dispose(&d); }
    { /* HTTP(S) links become "label (url)"; other schemes stay literal. */
        MdDocument d;
        render_ok(L"[site](https://example.com/x) [f](ftp://y.io/a) "
                  L"[bad](https://open", &d, "links render");
        text_is(&d, L"site (https://example.com/x) [f](ftp://y.io/a) "
                  L"[bad](https://open", "https expanded, others literal");
        markdown_dispose(&d); }
    { /* Flat lists: textual bullets and preserved ordered markers. */
        MdDocument d; render_ok(L"- a\n* b\n1. first\n10. [x] tenth\n5.x",
            &d, "lists render");
        text_is(&d, L"\u2022 a\n\u2022 b\n1. first\n10. [x] tenth\n5.x",
            "bullets synthesized, ordered task-looking markers preserved");
        markdown_dispose(&d); }
    { /* Valid unordered task markers replace bullets, including empty items. */
        MdDocument d;
        render_ok(L"- [ ] todo\n* [x] done\n- [X] complete\n* [ ]\n- ",
            &d, "task lists render");
        text_is(&d, L"\u2610 todo\n\u2611 done\n\u2611 complete\n\u2610 \n\u2022 ",
            "task markers replace bullets and empty items retain marker space");
        check(style_is(run_over(&d, L"\u2610"), 0),
            "unchecked marker is plain");
        check(style_is(run_over(&d, L"\u2611"), 0),
            "checked marker is plain");
        markdown_dispose(&d); }
    { /* Task item content still uses the normal inline parser. */
        MdDocument d;
        render_ok(L"- [x] **bold** *italic* ~~strike~~ `code` "
                   L"[link](https://x.io/a)", &d, "formatted task renders");
        text_is(&d, L"\u2611 bold italic strike code link (https://x.io/a)",
            "task marker removed before formatted content");
        check(style_is(run_over(&d, L"\u2611"), 0),
            "formatted task marker stays plain");
        check(style_is(run_over(&d, L"bold"), MD_STYLE_BOLD),
            "task bold content");
        check(style_is(run_over(&d, L"italic"), MD_STYLE_ITALIC),
            "task italic content");
        check(style_is(run_over(&d, L"strike"), MD_STYLE_STRIKE),
            "task strike content");
        check(style_is(run_over(&d, L"code"), MD_STYLE_MONO | MD_STYLE_CODE),
            "task code content");
        check(style_is(run_over(&d, L"link (https://x.io/a)"), 0),
            "task link content");
        markdown_dispose(&d); }
    { /* Invalid and misplaced task forms remain ordinary list content. */
        MdDocument d;
        render_ok(L"- [y] invalid\n* [x]text\n- [ X ] spaced\n"
                   L"- later [x]\n[x] paragraph\n+ [x] plus", &d,
            "invalid task forms render");
        text_is(&d, L"\u2022 [y] invalid\n\u2022 [x]text\n\u2022 [ X ] spaced\n"
            L"\u2022 later [x]\n[x] paragraph\n+ [x] plus",
            "invalid task forms remain literal list content");
        check(wcsstr(d.text, L"\u2610") == NULL && wcsstr(d.text, L"\u2611") == NULL,
            "invalid task forms do not synthesize checkboxes");
        markdown_dispose(&d); }
    { /* Inline and fenced code shield task-looking text. */
        MdDocument d;
        render_ok(L"- `[x]` inline\n```\n- [x] fenced\n```\n- [ ] real",
            &d, "code shields task markers");
        text_is(&d, L"\u2022 [x] inline\n- [x] fenced\n\u2610 real",
            "code task-looking text stays literal");
        check(style_is(run_over(&d, L"[x]"), MD_STYLE_MONO | MD_STYLE_CODE),
            "inline task-looking text is code");
        check(style_is(run_over(&d, L"- [x] fenced"),
            MD_STYLE_MONO | MD_STYLE_CODE),
            "fenced task-looking text is code");
        markdown_dispose(&d); }
    { /* Blockquotes: bar prefix, muted content, inline parsing inside. */
        MdDocument d; render_ok(L"> quoted **bold** text", &d, "quote renders");
        text_is(&d, L"\u258C quoted bold text", "quote bar and content");
        const MdRun *q = run_over(&d, L"quoted ");
        check(style_is(q, MD_STYLE_MUTED), "quote run muted");
        const MdRun *bold = run_over(&d, L"bold");
        check(style_is(bold, MD_STYLE_BOLD | MD_STYLE_MUTED),
            "quote inline styles nest");
        markdown_dispose(&d); }
    { /* Escapes keep punctuation literal. */
        MdDocument d; render_ok(L"\\*not emphasis\\* and \\`code\\`",
            &d, "escapes render");
        text_is(&d, L"*not emphasis* and `code`", "escaped markers literal");
        const MdRun *r = run_over(&d, L"not emphasis");
        check(style_is(r, 0), "escaped star not emphasis");
        markdown_dispose(&d); }
    { /* Emphasis nests around code and links. */
        MdDocument d;
        render_ok(L"**bold `code` and [l](https://x.io/a) tail**",
            &d, "nesting renders");
        const MdRun *code = run_over(&d, L"code");
        check(style_is(code, MD_STYLE_BOLD | MD_STYLE_MONO | MD_STYLE_CODE),
            "code inside bold keeps both");
        const MdRun *l = run_over(&d, L"l (https://x.io/a)");
        check(style_is(l, MD_STYLE_BOLD), "link inside bold");
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
            check(style_is(bold, MD_STYLE_BOLD), "long tail bold");
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
    puts("Markdown parser: flags, coalescing, headings, emphasis, "
        "strikethrough, variable code, fences, links, task lists, quotes, escapes, CRLF, "
        "long input and allocation fallback passed");
    return 0;
}
