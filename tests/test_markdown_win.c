/* Rich Edit integration of the Markdown body renderer (hidden windows). */
#include "../chat/rich_text_win32.h"
#include "../chat/markdown.h"
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Character format at one character position. */
static CHARFORMAT2W format_at(const RichTextControl *c, int cp) {
    CHARFORMAT2W f;
    memset(&f, 0, sizeof f);
    f.cbSize = sizeof f;
    SendMessageW(c->window, EM_SETSEL, (WPARAM)cp, (LPARAM)(cp + 1));
    SendMessageW(c->window, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&f);
    return f;
}

/* Paragraph format at one character position. */
static PARAFORMAT2 paragraph_at(const RichTextControl *c, int cp) {
    PARAFORMAT2 f;
    memset(&f, 0, sizeof f);
    f.cbSize = sizeof f;
    SendMessageW(c->window, EM_SETSEL, (WPARAM)cp, (LPARAM)(cp + 1));
    SendMessageW(c->window, EM_GETPARAFORMAT, 0, (LPARAM)&f);
    return f;
}

/* Layout columns in twips, the unit the renderer budgets them in. */
static LONG columns_twips(const RichTextControl *c, int columns) {
    return (LONG)(columns * c->theme.ui_size * 15.0f * 0.5f + 0.5f);
}

static void read_text(const RichTextControl *c, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (c->window) rich_text_get_text(c, out, cap);
}

int main(void) {
    CHECK(rich_text_library_open());
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"DarkChat.Markdown.Test";
    RegisterClassW(&cls);
    HWND parent = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP,
        0, 0, 800, 600, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent);
    RichTextTheme theme;
    memset(&theme, 0, sizeof theme);
    theme.background = RGB(20, 20, 20);
    theme.text = RGB(224, 224, 224);
    theme.muted = RGB(128, 128, 128);
    theme.code_background = RGB(43, 43, 43);
    theme.code_text = RGB(140, 190, 140);
    theme.ui_family = L"Segoe UI";
    theme.mono_family = L"Consolas";
    theme.ui_size = 10.0f;
    theme.mono_size = 9.0f;
    theme.small_size = 8.0f;
    RichTextControl probe;
    CHECK(rich_text_create_block(&probe, parent, 1, &theme, 96));
    SetWindowPos(probe.window, NULL, 0, 0, 600, 300,
        SWP_NOZORDER | SWP_NOACTIVATE);
    wchar_t text[512];

    /* Style ranges: bold, mono face/size, code color/background, and italic. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"Intro **bold** `code`");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"Intro bold code"));
    CHARFORMAT2W f = format_at(&probe, 0);
    CHECK((f.dwMask & CFM_BOLD) && !(f.dwEffects & CFE_BOLD));
    CHECK((f.dwMask & CFM_ITALIC) && !(f.dwEffects & CFE_ITALIC));
    f = format_at(&probe, 6);                       /* "bold" */
    CHECK((f.dwEffects & CFE_BOLD) && !(f.dwEffects & CFE_ITALIC));
    CHECK(!wcscmp(f.szFaceName, L"Segoe UI"));
    f = format_at(&probe, 12);                      /* "code" */
    CHECK(!wcscmp(f.szFaceName, L"Consolas"));
    CHECK(f.yHeight == (LONG)(theme.mono_size * 15.0f + 0.5f));
    CHECK(f.crTextColor == theme.code_text);
    CHECK((f.dwMask & CFM_BACKCOLOR) && f.crBackColor == theme.code_background);

    /* Strikethrough is set on its exact run and cleared after it. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"a ~~old~~ b");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"a old b"));
    f = format_at(&probe, 2);                       /* "old" */
    CHECK((f.dwMask & CFM_STRIKEOUT) && (f.dwEffects & CFE_STRIKEOUT));
    f = format_at(&probe, 0);                       /* plain prefix */
    CHECK((f.dwMask & CFM_STRIKEOUT) && !(f.dwEffects & CFE_STRIKEOUT));
    f = format_at(&probe, 6);                       /* plain suffix */
    CHECK((f.dwMask & CFM_STRIKEOUT) && !(f.dwEffects & CFE_STRIKEOUT));

    /* Strikethrough composes with inline-code formatting. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"~~`code`~~ tail");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"code tail"));
    f = format_at(&probe, 0);
    CHECK((f.dwEffects & CFE_STRIKEOUT) &&
        !wcscmp(f.szFaceName, L"Consolas") &&
        f.crBackColor == theme.code_background);
    f = format_at(&probe, 5);                       /* plain suffix */
    CHECK(!(f.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_STRIKEOUT)) &&
        !wcscmp(f.szFaceName, L"Segoe UI") &&
        f.crBackColor == theme.background && f.crTextColor == theme.text);

    /* Background reset: text after a code run restores the surface. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"a `c` b");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"a c b"));
    f = format_at(&probe, 2);                       /* inside code span */
    CHECK(f.crBackColor == theme.code_background &&
        !wcscmp(f.szFaceName, L"Consolas"));
    f = format_at(&probe, 4);                       /* plain text after it */
    CHECK(f.crBackColor == theme.background &&
        !wcscmp(f.szFaceName, L"Segoe UI") && !(f.dwEffects & CFE_BOLD));

    /* Equal-length multi-backtick spans receive the complete code format. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"before **``code with ` inside``** after");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"before code with ` inside after"));
    f = format_at(&probe, 7);                       /* variable-length code */
    CHECK((f.dwEffects & CFE_BOLD) && !wcscmp(f.szFaceName, L"Consolas") &&
        f.yHeight == (LONG)(theme.mono_size * 15.0f + 0.5f) &&
        f.crTextColor == theme.code_text &&
        f.crBackColor == theme.code_background);
    f = format_at(&probe, (int)wcslen(L"before code with ` inside "));
    CHECK(!(f.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_STRIKEOUT)) &&
        !wcscmp(f.szFaceName, L"Segoe UI") &&
        f.crTextColor == theme.text && f.crBackColor == theme.background);

    /* Tilde-fenced code hides its markers and receives the complete code format. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"before\n~~~~lang\n**raw**\n~~~~\nafter");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"before\r\n**raw**\r\nafter"));
    f = format_at(&probe, (int)wcslen(L"before\r\n"));
    CHECK(!(f.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_STRIKEOUT)) &&
        !wcscmp(f.szFaceName, L"Consolas") &&
        f.yHeight == (LONG)(theme.mono_size * 15.0f + 0.5f) &&
        f.crTextColor == theme.code_text && f.crBackColor == theme.code_background);
    f = format_at(&probe, (int)wcslen(L"before\r\n**raw**\r\n"));
    CHECK(!(f.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_STRIKEOUT)) &&
        !wcscmp(f.szFaceName, L"Segoe UI") &&
        f.crTextColor == theme.text && f.crBackColor == theme.background);

    /* Italic and heading sizes. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"a *em* b");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"a em b"));
    f = format_at(&probe, 2);
    CHECK((f.dwEffects & CFE_ITALIC) && !(f.dwEffects & CFE_BOLD));
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"# Head");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"Head"));
    f = format_at(&probe, 0);
    CHECK((f.dwEffects & CFE_BOLD) &&
        f.yHeight == (LONG)(13.0f * 15.0f + 0.5f)); /* h1 = ui_size + 3 */

    /* Markdown muted content changes color without affecting the quote bar. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"> quote");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"\u258C quote"));
    f = format_at(&probe, 0);
    CHECK(f.crTextColor == theme.text);
    f = format_at(&probe, 2);
    CHECK(f.crTextColor == theme.muted);

    /* CRLF source: one paragraph break per pair, not two. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"one\r\n\r\ntwo");
    CHECK(SendMessageW(probe.window, EM_GETLINECOUNT, 0, 0) == 3);
    read_text(&probe, text, 512);
    CHECK(wcsstr(text, L"two") != NULL && wcsstr(text, L"one") != NULL);

    /* Literal streaming appends verbatim; the terminal pass renders Markdown.
       A live turn's control is empty when its first token arrives, so start
       from an empty surface here too. */
    rich_text_set_body(&probe, CHAT_ROLE_ASSISTANT, L"");
    rich_text_append_body(&probe, L"**raw **stream");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"**raw **stream"));
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"**done** final");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"done final"));
    f = format_at(&probe, 0);
    CHECK((f.dwEffects & CFE_BOLD));
    f = format_at(&probe, 5);
    CHECK(!(f.dwEffects & CFE_BOLD));

    /* Repeated rendering is stable. */
    for (int i = 0; i < 3; i++) {
        rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
            L"- item one\n- item **two**");
        read_text(&probe, text, 512);
        CHECK(!wcscmp(text, L"\u2022 item one\r\n\u2022 item two"));
    }

    /* Task markers are Unicode text, with formatting limited to item content. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"- [x] **done**\n* [ ] todo");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"\u2611 done\r\n\u2610 todo"));
    f = format_at(&probe, 0);
    CHECK(!(f.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_STRIKEOUT)));
    f = format_at(&probe, 2);                       /* "done" */
    CHECK(f.dwEffects & CFE_BOLD);

    /* User/system/error bodies are fully literal, fence markers included. */
    rich_text_set_body(&probe, CHAT_ROLE_USER, L"```c\nint x;\n```");
    read_text(&probe, text, 512);
    CHECK(wcsstr(text, L"```c") != NULL && wcsstr(text, L"```") != NULL);

    /* Malformed and long Markdown stay literal and complete. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"a **b *c");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"a **b *c"));
    size_t n = 150000;
    wchar_t *long_text = (wchar_t *)malloc((n + 16) * sizeof(wchar_t));
    CHECK(long_text);
    for (size_t i = 0; i < n; i++) long_text[i] = L'x';
    long_text[n] = 0;
    wcscat(long_text, L" **tail**");
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, long_text);
    CHECK(SendMessageW(probe.window, WM_GETTEXTLENGTH, 0, 0) == (LRESULT)(n + 5));
    free(long_text);

    /* Paragraph layout: plain text is flush, and every field is reset. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"plain `code` plain");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"plain code plain"));
    PARAFORMAT2 pf = paragraph_at(&probe, 0);
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == 0);
    pf = paragraph_at(&probe, (int)wcslen(L"plain code "));
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == 0);

    /* A list item hangs its wrapped lines at the content column. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"- a\n  - b");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"\u2022 a\r\n\u2022 b"));
    pf = paragraph_at(&probe, 0);
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == columns_twips(&probe, 2));
    pf = paragraph_at(&probe, 5);                   /* nested item */
    CHECK(pf.dxStartIndent == columns_twips(&probe, 2) &&
        pf.dxOffset == columns_twips(&probe, 2));

    /* A quoted item indents by its bars and its bullet together. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"> - item");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"\u258C \u2022 item"));
    pf = paragraph_at(&probe, 0);
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == columns_twips(&probe, 4));

    /* A nested quoted item keeps the four-character prefix as base indent. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"> - item\n>   - nested");
    pf = paragraph_at(&probe, (int)wcslen(L"\u258C \u2022 item\r\n"));
    CHECK(pf.dxStartIndent == columns_twips(&probe, 2) &&
        pf.dxOffset == columns_twips(&probe, 4));

    /* A quoted continuation aligns with the item it continues. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"> - item\n>   more");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"\u258C \u2022 item\r\n\u258C   more"));
    pf = paragraph_at(&probe, (int)wcslen(L"\u258C \u2022 item\r\n"));
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == columns_twips(&probe, 4));

    /* Nested rendering is stable across repeats and keeps its text. */
    for (int i = 0; i < 3; i++) {
        rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
            L"- one\n  - two\n    - three");
        read_text(&probe, text, 512);
        CHECK(!wcscmp(text, L"\u2022 one\r\n\u2022 two\r\n\u2022 three"));
        pf = paragraph_at(&probe, (int)wcslen(L"\u2022 one\r\n\u2022 two\r\n"));
        CHECK(pf.dxStartIndent == columns_twips(&probe, 4) &&
            pf.dxOffset == columns_twips(&probe, 2));
    }

    /* Behavioral: a wrapped item line starts at the content column, not at the
       margin where its bullet sits. */
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT,
        L"- aaaaaaaa bbbbbbbb cccccccc dddddddd eeeeeeee ffffffff "
        L"gggggggg hhhhhhhh iiiiiiii jjjjjjjj kkkkkkkk llllllll "
        L"mmmmmmmm nnnnnnnn oooooooo pppppppp");
    CHECK(SendMessageW(probe.window, EM_GETLINECOUNT, 0, 0) > 1);
    int wrap_cp = -1;
    for (int cp = 0; cp < 200 && wrap_cp < 0; cp++)
        if ((int)SendMessageW(probe.window, EM_EXLINEFROMCHAR, 0,
            (LPARAM)cp) > 0) wrap_cp = cp;
    CHECK(wrap_cp > 0);
    POINTL first, wrapped, content;
    memset(&first, 0, sizeof first);
    memset(&wrapped, 0, sizeof wrapped);
    memset(&content, 0, sizeof content);
    SendMessageW(probe.window, EM_POSFROMCHAR, (WPARAM)&first, (LPARAM)0);
    SendMessageW(probe.window, EM_POSFROMCHAR, (WPARAM)&wrapped,
        (LPARAM)wrap_cp);
    SendMessageW(probe.window, EM_POSFROMCHAR, (WPARAM)&content, (LPARAM)2);
    CHECK(wrapped.x > first.x);
    CHECK(abs(wrapped.x - content.x) <= 6);

    /* Allocation failure falls back to verbatim text, then recovers. */
    markdown_test_fail_allocations(true);
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"- **x** tail");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"- **x** tail"));
    pf = paragraph_at(&probe, 0);
    CHECK(pf.dxStartIndent == 0 && pf.dxOffset == 0);  /* fallback is flush */
    markdown_test_fail_allocations(false);
    rich_text_set_markdown(&probe, CHAT_ROLE_ASSISTANT, L"**x** tail");
    read_text(&probe, text, 512);
    CHECK(!wcscmp(text, L"x tail"));

    DestroyWindow(probe.window);
    DestroyWindow(parent);
    rich_text_library_close();
    puts("Markdown Rich Edit integration: flags, style ranges, strikethrough, "
        "background reset, muted text, task lists, CRLF, streaming-to-terminal, "
        "repeats, literal user text, malformed/long input and allocation fallback passed");
    return 0;
}
