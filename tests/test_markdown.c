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

/* Finds the block covering the first occurrence of needle in the document. */
static const MdBlock *block_over(const MdDocument *d, const wchar_t *needle) {
    const wchar_t *at = d->text ? wcsstr(d->text, needle) : NULL;
    if (!at) return NULL;
    size_t offset = (size_t)(at - d->text);
    size_t end = offset + wcslen(needle);
    for (int i = 0; i < d->block_count; i++) {
        const MdBlock *k = &d->blocks[i];
        if (k->offset <= offset && end <= k->offset + k->length) return k;
    }
    return NULL;
}

static void block_is(const MdBlock *k, int kind, unsigned char quote,
    unsigned char list, unsigned char flags, unsigned char first,
    unsigned char continuation, const char *what) {
    check(k != NULL, what);
    if (!k) return;
    check(k->kind == (unsigned char)kind && k->quote_depth == quote &&
        k->list_depth == list && k->flags == flags &&
        k->first_indent == first && k->continuation_indent == continuation,
        what);
}

/* The destination of the stored link whose label covers needle, or NULL. */
static const wchar_t *link_over(const MdDocument *d, const wchar_t *needle,
    size_t *length) {
    const wchar_t *at = d->text ? wcsstr(d->text, needle) : NULL;
    if (!at) return NULL;
    size_t offset = (size_t)(at - d->text);
    size_t end = offset + wcslen(needle);
    for (int i = 0; i < d->link_count; i++) {
        const MdLink *l = &d->links[i];
        if (l->offset <= offset && end <= l->offset + l->length) {
            if (length) *length = l->target_length;
            return d->targets + l->target_offset;
        }
    }
    return NULL;
}

static void link_is(const MdDocument *d, const wchar_t *label,
    const wchar_t *target, const char *what) {
    size_t length = 0;
    const wchar_t *found = link_over(d, label, &length);
    check(found != NULL, what);
    if (!found) return;
    check(length == wcslen(target) && !wmemcmp(found, target, length), what);
    check(found[length] == L'\0', what);   /* arena entry is NUL-terminated */
}

/* Blocks are ordered, disjoint, non-empty, hold no paragraph separator and
    never indent content before the line's first column. */
static void blocks_sane(const MdDocument *d, const char *what) {
    size_t reached = 0;
    for (int i = 0; i < d->block_count; i++) {
        const MdBlock *k = &d->blocks[i];
        check(k->offset >= reached, what);
        check(k->length > 0, what);
        check(k->continuation_indent >= k->first_indent, what);
        for (size_t j = k->offset; j < k->offset + k->length; j++)
            if (d->text[j] == L'\n') { check(false, what); return; }
        reached = k->offset + k->length;
    }
    check(reached <= d->length, what);
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
        check(d.link_count == 0, "code shields links from metadata");
        markdown_dispose(&d); }
    { /* Paired tildes strike non-space content and compose with outer styles. */
        MdDocument d;
        render_ok(L"a ~~old~~ b **~~bold~~** ~~`code`~~ "
                  L"~~[link](https://x.io/a)~~", &d, "strike renders");
        text_is(&d, L"a old b bold code link",
            "strike markers and link target removed");
        const MdRun *old = run_over(&d, L"old");
        const MdRun *bold = run_over(&d, L"bold");
        const MdRun *code = run_over(&d, L"code");
        const MdRun *link = run_over(&d, L"link");
        check(style_is(old, MD_STYLE_STRIKE), "plain strike run");
        check(style_is(bold, MD_STYLE_BOLD | MD_STYLE_STRIKE),
            "bold strike run");
        check(style_is(code, MD_STYLE_STRIKE | MD_STYLE_MONO | MD_STYLE_CODE),
            "code inside strike keeps styles");
        check(style_is(link, MD_STYLE_STRIKE), "link inside strike keeps style");
        link_is(&d, L"link", L"https://x.io/a", "struck link target recorded");
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
        text_is(&d, L"zero\n  two\n\t tab\n  plain\n    deep",
            "fence indentation is distinct from ordinary lines");
        check(style_is(run_over(&d, L"  two"), MD_STYLE_MONO | MD_STYLE_CODE),
            "remaining fenced indentation is code");
        check(style_is(run_over(&d, L"plain"), 0),
            "ordinary indentation behavior remains unchanged");
        markdown_dispose(&d); }
    { /* Four leading spaces never open a fence; CRLF fences normalize normally. */
        MdDocument d;
        render_ok(L"    ```\nplain", &d, "four-space fence marker renders");
        text_is(&d, L"    ```\nplain", "four-space marker stays ordinary text");
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
    { /* HTTP(S) links render as their label with the target recorded; other
         schemes and malformed links stay literal and record nothing. */
        MdDocument d;
        render_ok(L"[site](https://example.com/x) [f](ftp://y.io/a) "
                  L"[bad](https://open", &d, "links render");
        text_is(&d, L"site [f](ftp://y.io/a) [bad](https://open",
            "https label only, others literal");
        check(d.link_count == 1, "only the HTTP(S) link is recorded");
        link_is(&d, L"site", L"https://example.com/x", "link target recorded");
        markdown_dispose(&d);
        render_ok(L"[up](HTTP://Upper.example) and [x](mailto:a@b.c)", &d,
            "case-insensitive scheme renders");
        text_is(&d, L"up and [x](mailto:a@b.c)",
            "uppercase HTTP(S) label only, other scheme literal");
        check(d.link_count == 1, "non-HTTP(S) scheme records no link");
        link_is(&d, L"up", L"HTTP://Upper.example", "target keeps original case");
        markdown_dispose(&d); }
    { /* Multiple links keep order and exact label ranges; identical adjacent
         styles still coalesce into one run across them. */
        MdDocument d;
        render_ok(L"[one](https://a.io/1) & [two](http://b.io/2)", &d,
            "multiple links render");
        text_is(&d, L"one & two", "both labels shown, targets hidden");
        check(d.link_count == 2, "both links recorded");
        link_is(&d, L"one", L"https://a.io/1", "first target");
        link_is(&d, L"two", L"http://b.io/2", "second target");
        check(d.run_count == 1 && d.runs[0].length == d.length,
            "identical adjacent styles coalesce across links");
        markdown_dispose(&d); }
    { /* An empty label does not qualify; an empty link set leaves the arena
         empty. */
        MdDocument d;
        render_ok(L"[](https://x.io) and [](notaurl)", &d, "empty labels render");
        text_is(&d, L"[](https://x.io) and [](notaurl)",
            "empty labels stay literal");
        check(d.link_count == 0 && d.targets == NULL,
            "empty labels record no link or arena");
        markdown_dispose(&d); }
    { /* A quoted list-item link composes block layout, run styling and link
         metadata over the same synthesized prefix. */
        MdDocument d;
        render_ok(L"> - [site](https://example.com)", &d, "quoted link renders");
        text_is(&d, L"\u258C \u2022 site", "quoted link shows label only");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 1, 1, 0, 0, 4,
            "quoted link block layout");
        check(style_is(run_over(&d, L"site"), MD_STYLE_MUTED),
            "quoted link keeps the quote style");
        link_is(&d, L"site", L"https://example.com", "quoted link target");
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
        text_is(&d, L"\u2611 bold italic strike code link",
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
        check(style_is(run_over(&d, L"link"), 0), "task link content");
        link_is(&d, L"link", L"https://x.io/a", "task link target recorded");
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
        render_ok(L"**bold `code` and [link](https://x.io/a) tail**",
            &d, "nesting renders");
        const MdRun *code = run_over(&d, L"code");
        check(style_is(code, MD_STYLE_BOLD | MD_STYLE_MONO | MD_STYLE_CODE),
            "code inside bold keeps both");
        const MdRun *l = run_over(&d, L"link");
        check(style_is(l, MD_STYLE_BOLD), "link inside bold");
        link_is(&d, L"link", L"https://x.io/a", "bold link target recorded");
        markdown_dispose(&d); }
    { /* Flat paragraphs describe their own layout; nesting arrives later. */
        MdDocument d;
        render_ok(L"- a\n* [x] t\n1. one\n10. ten\n> quote\ntext", &d,
            "flat blocks render");
        text_is(&d, L"\u2022 a\n\u2611 t\n1. one\n10. ten\n"
            L"\u258C quote\ntext", "flat block text unchanged");
        blocks_sane(&d, "flat blocks are sane");
        check(d.block_count == 6, "one block per non-empty paragraph");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 0, 1, 0, 0, 2, "bullet block");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 1,
            MD_FLAG_TASK | MD_FLAG_CHECKED, 0, 2, "checked task block");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 0, 1, MD_FLAG_ORDERED, 0, 3,
            "ordered block keeps its marker width");
        block_is(&d.blocks[3], MD_BLOCK_ITEM, 0, 1, MD_FLAG_ORDERED, 0, 4,
            "two-digit ordered block");
        block_is(&d.blocks[4], MD_BLOCK_QUOTE, 1, 0, 0, 0,
            MD_QUOTE_GLYPH_COLS, "quote block");
        block_is(&d.blocks[5], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "plain block");
        block_is(block_over(&d, L"quote"), MD_BLOCK_QUOTE, 1, 0, 0, 0,
            MD_QUOTE_GLYPH_COLS, "quote block found by text");
        markdown_dispose(&d); }
    { /* Headings, blank lines and fences record only their own paragraphs. */
        MdDocument d;
        render_ok(L"# H\n\nbefore\n```\nx\ny\n```\nafter", &d,
            "mixed blocks render");
        blocks_sane(&d, "mixed blocks are sane");
        check(d.block_count == 5, "a blank line records no block");
        block_is(&d.blocks[0], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "heading block");
        block_is(&d.blocks[1], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "prose block");
        block_is(&d.blocks[2], MD_BLOCK_CODE, 0, 0, 0, 0, 0, "first code line");
        block_is(&d.blocks[3], MD_BLOCK_CODE, 0, 0, 0, 0, 0,
            "second code line");
        block_is(&d.blocks[4], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "trailing block");
        markdown_dispose(&d); }
    { /* Runs and blocks are independent layers: one code run, two code blocks. */
        MdDocument d; render_ok(L"```\nx\ny\n```", &d, "fence layers render");
        text_is(&d, L"x\ny", "fenced content unchanged");
        check(d.run_count == 1, "fenced lines coalesce into one run");
        check(d.block_count == 2, "one code block per fenced line");
        blocks_sane(&d, "fence blocks are sane");
        markdown_dispose(&d); }
    { /* Nested unordered lists: depth, indentation and wrapped-line column. */
        MdDocument d; render_ok(L"- a\n  - b\n    - c", &d, "nesting renders");
        text_is(&d, L"\u2022 a\n\u2022 b\n\u2022 c", "bullets synthesized");
        blocks_sane(&d, "nested blocks are sane");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 0, 1, 0, 0, 2, "depth 1 item");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 2, 0, 2, 4, "depth 2 item");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 0, 3, 0, 4, 6, "depth 3 item");
        markdown_dispose(&d); }
    { /* A child needs the parent's content column; one stray space does not
         open a level, it continues the item. */
        MdDocument d; render_ok(L"- a\n - b", &d, "stray space renders");
        text_is(&d, L"\u2022 a\n\u2022 b", "one space is a sibling item");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 1, 0, 1, 3,
            "sibling stays at depth 1");
        markdown_dispose(&d);
        render_ok(L"- a\n  - b", &d, "child indent renders");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 2, 0, 2, 4,
            "child at the content column");
        markdown_dispose(&d); }
    { /* Mixed nesting keeps each level's own marker and geometry. */
        MdDocument d; render_ok(L"1. a\n   - b\n1) c", &d, "mixed renders");
        text_is(&d, L"1. a\n\u2022 b\n1) c", "mixed markers kept");
        blocks_sane(&d, "mixed blocks are sane");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 0, 1, MD_FLAG_ORDERED, 0, 3,
            "ordered parent");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 2, 0, 3, 5,
            "bullet child of an ordered item");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 0, 1, MD_FLAG_ORDERED, 0, 3,
            "popping back to depth 1");
        markdown_dispose(&d); }
    { /* Continuation lines inherit the item's layout; a flush line does not. */
        MdDocument d; render_ok(L"- a\n b", &d, "continuation renders");
        text_is(&d, L"\u2022 a\n  b", "continuation pads the absent marker");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 1, MD_FLAG_CONTINUATION, 0, 2,
            "continuation inherits the item layout");
        markdown_dispose(&d);
        render_ok(L"- a\nb", &d, "flush line renders");
        block_is(&d.blocks[1], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "a flush line is an ordinary paragraph");
        markdown_dispose(&d); }
    { /* Quote prefixes: source width and rendered bars are counted separately,
         and only the last marker may end without a space. */
        MdDocument d; render_ok(L"> x\n> > x\n>> x", &d, "quote depths render");
        text_is(&d, L"\u258C x\n\u258C \u258C x\n\u258C \u258C x",
            "one bar per level, compact and spaced alike");
        block_is(&d.blocks[0], MD_BLOCK_QUOTE, 1, 0, 0, 0, 2, "depth 1 quote");
        block_is(&d.blocks[1], MD_BLOCK_QUOTE, 2, 0, 0, 0, 4, "depth 2 quote");
        block_is(&d.blocks[2], MD_BLOCK_QUOTE, 2, 0, 0, 0, 4,
            "compact depth 2 quote");
        markdown_dispose(&d);
        render_ok(L"> >x\n>>x\n>x", &d, "unterminated quotes render");
        text_is(&d, L"\u258C >x\n\u258C >x\n>x",
            "a marker without its space stays literal content");
        block_is(&d.blocks[0], MD_BLOCK_QUOTE, 1, 0, 0, 0, 2,
            "unterminated quote is depth 1");
        block_is(&d.blocks[2], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "bare marker is not a quote");
        markdown_dispose(&d); }
    { /* The legacy root tolerance still finds a quote after three spaces;
         indentation inside a quote is structural. */
        MdDocument d; render_ok(L"> quote\n   > quote\n    > x\n>   x", &d,
            "quote indentation renders");
        text_is(&d, L"\u258C quote\n\u258C quote\n    > x\n\u258C x",
            "recognized quotes render, unsupported ones stay literal");
        block_is(&d.blocks[0], MD_BLOCK_QUOTE, 1, 0, 0, 0, 2, "plain quote");
        block_is(&d.blocks[1], MD_BLOCK_QUOTE, 1, 0, 0, 0, 2,
            "three leading spaces still open a quote");
        block_is(&d.blocks[2], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "four leading spaces leave the line literal");
        block_is(&d.blocks[3], MD_BLOCK_QUOTE, 1, 0, 0, 2, 4,
            "indentation inside a quote is base indentation");
        markdown_dispose(&d); }
    { /* Quoted lists: bars at the left, content aligned with the item text. */
        MdDocument d; render_ok(L"> - item\n>   more", &d,
            "quoted continuation renders");
        text_is(&d, L"\u258C \u2022 item\n\u258C   more",
            "bars, bullet and padded continuation");
        blocks_sane(&d, "quoted list blocks are sane");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 1, 1, 0, 0, 4, "quoted item");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 1, 1, MD_FLAG_CONTINUATION, 0, 4,
            "quoted continuation keeps bars and content aligned");
        markdown_dispose(&d);
        render_ok(L"> - item\n>   - nested\n>     deeper", &d,
            "nested quoted list renders");
        text_is(&d, L"\u258C \u2022 item\n\u258C \u2022 nested\n"
            L"\u258C   deeper", "nested continuation aligns with its item");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 1, 2, 0, 2, 6,
            "nested quoted item keeps its four-character prefix");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 1, 2, MD_FLAG_CONTINUATION, 2, 6,
            "a deeper continuation follows the nested item");
        markdown_dispose(&d); }
    { /* A quote boundary starts a fresh list. */
        MdDocument d; render_ok(L"- a\n> - b\n  - c", &d,
            "quote boundary renders");
        text_is(&d, L"\u2022 a\n\u258C \u2022 b\n\u2022 c",
            "quoted item renders as a quoted item");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 1, 1, 0, 0, 4,
            "quoted item is depth 1 in its own quote, not depth 2");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 0, 1, 0, 2, 4,
            "the quote ended the unquoted list, which restarts at depth 1");
        markdown_dispose(&d); }
    { /* Task markers work at any supported depth. */
        MdDocument d; render_ok(L"> - [x] deep\n>   - [ ] deeper", &d,
            "nested tasks render");
        text_is(&d, L"\u258C \u2611 deep\n\u258C \u2610 deeper",
            "task markers replace the bullet at depth");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 1, 1,
            MD_FLAG_TASK | MD_FLAG_CHECKED, 0, 4, "checked nested task");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 1, 2, MD_FLAG_TASK, 2, 6,
            "unchecked deeper task");
        markdown_dispose(&d); }
    { /* Nesting past the supported depth stays literal and ends the list. */
        MdDocument d;
        render_ok(L"- a\n  - b\n    - c\n      - d\n        - e\n"
            L"          - f\n            - g\n              - h\n"
            L"                - i\n  - j", &d, "deep nesting renders");
        blocks_sane(&d, "deep blocks are sane");
        block_is(&d.blocks[7], MD_BLOCK_ITEM, 0, 8, 0, 14, 16,
            "depth 8 is the last supported level");
        block_is(&d.blocks[8], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "deeper line stays literal");
        block_is(&d.blocks[9], MD_BLOCK_ITEM, 0, 1, 0, 2, 4,
            "the literal line cleared the list stack");
        markdown_dispose(&d);
        render_ok(L"> > > > > > > > > x", &d, "deep quote renders");
        check(d.block_count == 1 && d.blocks[0].kind == MD_BLOCK_PARAGRAPH &&
            d.blocks[0].first_indent == 0,
            "quote depth past the limit stays literal");
        markdown_dispose(&d); }
    { /* Indentation past the layout cap is kept verbatim, tabs included. */
        MdDocument d;
        render_ok(L"- a\n                                        - b", &d,
            "capped indentation renders");
        text_is(&d, L"\u2022 a\n\u2022         b",
            "excess columns stay in the text");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 2, 0, MD_MAX_INDENT,
            MD_MAX_INDENT + 2, "layout clamped, text retained");
        markdown_dispose(&d);
        render_ok(L"- a\n                                        \tb", &d,
            "capped tab renders");
        text_is(&d, L"\u2022 a\n          \tb", "retained tab is not spaces");
        markdown_dispose(&d); }
    { /* Ordinary leading whitespace is preserved as text, not as layout. */
        MdDocument d; render_ok(L"  plain\n    deep", &d, "indent renders");
        text_is(&d, L"  plain\n    deep", "ordinary whitespace preserved");
        block_is(&d.blocks[0], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "leading whitespace is not layout");
        markdown_dispose(&d);
        render_ok(L"    - a", &d, "root cap renders");
        text_is(&d, L"    - a", "four-space marker stays literal");
        block_is(&d.blocks[0], MD_BLOCK_PARAGRAPH, 0, 0, 0, 0, 0,
            "root cap line is a plain paragraph");
        markdown_dispose(&d); }
    { /* Every character of a quote prefix counts toward the source column, so
         a tab after the markers lands on the right tab stop. */
        MdDocument d; render_ok(L"> \tx", &d, "quoted tab renders");
        text_is(&d, L"\u258C x", "quoted tab content unchanged");
        block_is(&d.blocks[0], MD_BLOCK_QUOTE, 1, 0, 0, 2, 4,
            "tab stops count the consumed markers");
        markdown_dispose(&d); }
    { /* A shifted sibling re-anchors its level, so the next line is classified
         against the sibling's column rather than the original marker's. */
        MdDocument d; render_ok(L"- a\n - b\n  - c", &d,
            "shifted sibling renders");
        text_is(&d, L"\u2022 a\n\u2022 b\n\u2022 c", "all three are items");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 1, 0, 1, 3,
            "sibling anchors the level at its own column");
        block_is(&d.blocks[2], MD_BLOCK_ITEM, 0, 1, 0, 2, 4,
            "one more column is still a sibling of the shifted level");
        markdown_dispose(&d); }
    { /* A recognized marker keeps its source indentation, even at the root. */
        MdDocument d; render_ok(L"   - a\n- b", &d, "root indent renders");
        text_is(&d, L"\u2022 a\n\u2022 b", "bullets synthesized");
        block_is(&d.blocks[0], MD_BLOCK_ITEM, 0, 1, 0, 3, 5,
            "three-space root item keeps three columns");
        block_is(&d.blocks[1], MD_BLOCK_ITEM, 0, 1, 0, 0, 2,
            "flush root item stays flush");
        markdown_dispose(&d); }
    { /* Hundreds of quote markers cannot wrap the depth count into a valid
         quote: the line stays literal. */
        wchar_t deep[320];
        for (int i = 0; i < 256 + MD_MAX_DEPTH; i++) deep[i] = L'>';
        wcscpy(deep + 256 + MD_MAX_DEPTH, L" x");
        MdDocument d; render_ok(deep, &d, "overflowing quote renders");
        check(d.block_count == 1 && d.blocks[0].kind == MD_BLOCK_PARAGRAPH &&
            d.blocks[0].quote_depth == 0,
            "an unbounded marker run never becomes a shallow quote");
        markdown_dispose(&d); }
    { /* Fences keep precedence and shield nested syntax. */
        MdDocument d; render_ok(L"~~~\n- [x] raw\n> - quoted\n~~~", &d,
            "shielded nesting renders");
        text_is(&d, L"- [x] raw\n> - quoted", "fenced nesting stays raw");
        check(d.run_count == 1, "shielded lines are one code run");
        check(d.block_count == 2, "one code block per fenced line");
        block_is(&d.blocks[0], MD_BLOCK_CODE, 0, 0, 0, 0, 0,
            "fenced line is code");
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
        check(d.text == NULL && d.runs == NULL && d.blocks == NULL &&
            d.run_count == 0 && d.block_count == 0 && d.length == 0,
            "failed document zeroed");
        markdown_test_fail_allocations(false);
        render_ok(L"**x**", &d, "render works again after failure");
        text_is(&d, L"x", "post-failure render correct");
        markdown_dispose(&d); }
    { /* Block growth alone is transactional: an empty document needs no block
         array, while any recorded paragraph discards the whole document. */
        MdDocument d;
        markdown_test_fail_blocks(true);
        render_ok(L"", &d, "empty document allocates no blocks");
        check(d.block_count == 0, "empty document has no blocks");
        markdown_dispose(&d);
        check(!markdown_render(L"- a\n> b", &d), "block failure reported");
        check(d.text == NULL && d.runs == NULL && d.blocks == NULL &&
            d.run_count == 0 && d.block_count == 0 && d.length == 0,
            "block failure document zeroed");
        markdown_test_fail_blocks(false);
        render_ok(L"- a\n> b", &d, "render works after block failure");
        check(d.block_count == 2, "post-failure blocks recorded");
        blocks_sane(&d, "post-failure blocks are sane");
        markdown_dispose(&d); }
    { /* Link metadata growth alone is transactional: unlinked text needs no
         link allocation, while any valid link discards the whole document. */
        MdDocument d;
        markdown_test_fail_links(true);
        render_ok(L"", &d, "empty document needs no link metadata");
        check(d.link_count == 0, "empty document has no links");
        markdown_dispose(&d);
        check(!markdown_render(L"see [x](https://x.io)", &d),
            "link failure reported");
        check(d.text == NULL && d.runs == NULL && d.blocks == NULL &&
            d.links == NULL && d.targets == NULL && d.run_count == 0 &&
            d.block_count == 0 && d.link_count == 0 && d.length == 0,
            "link failure document zeroed");
        markdown_test_fail_links(false);
        render_ok(L"see [x](https://x.io)", &d, "render works after link failure");
        text_is(&d, L"see x", "post-failure link render correct");
        link_is(&d, L"x", L"https://x.io", "post-failure link recorded");
        markdown_dispose(&d); }
    markdown_dispose(NULL);
    if (failures) { printf("%d markdown test(s) failed\n", failures); return 1; }
    puts("Markdown parser: flags, coalescing, headings, emphasis, "
        "strikethrough, variable code, fences, link labels/targets, task lists, "
        "quotes, escapes, CRLF, long input and allocation fallback passed");
    return 0;
}
