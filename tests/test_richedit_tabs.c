/* Phase 0 spike (TABLES_PLAN.md): pin the Rich Edit tab-stop behavior the
   table design depends on. Throwaway harness; links no product code. Proves:
     - PFM_TABSTOPS rgxTabs encoding round-trips through EM_GETPARAFORMAT:
       MulDiv(px, 1440, dpi) | ((LONG)align << 24), align 0/1/2.
     - leading-tab left/center/right column zero, three columns, empty cells,
       overlong cell overflow.
     - PFM_TABSTOPS with cTabCount = 0 clears stale stops.
     - LF-only insertion keeps char positions equal to source offsets across
       three or more paragraphs.
     - device metrics for the provisional TABLE_MIN_COLUMN_DIP/TABLE_GUTTER_DIP.
   No chat/transcript/table_layout.h, no parser metadata, no product behavior change. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); \
    return 1; } } while (0)

/* Provisional caps recorded by Phase 0; Phase 3 benchmarks the real path. */
#define TABLE_MAX_PHYSICAL_LINES 4096
#define MD_MAX_FLATTEN_CHARS     262144
#define TABLE_MIN_COLUMN_DIP     48
#define TABLE_GUTTER_DIP         16

/* Alignment nibble values in rgxTabs bits 24-27. */
#define SPIKE_ALIGN_LEFT   0
#define SPIKE_ALIGN_CENTER 1
#define SPIKE_ALIGN_RIGHT  2

static int control_dpi(HWND window) {
    HDC dc = GetDC(window);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi > 0 ? dpi : 96;
}

static LONG px_to_twips(int px, int dpi) {
    return (LONG)MulDiv(px, 1440, dpi);
}

static void set_tab_stops(HWND window, const int *anchors_px,
    const unsigned char *align, int count, int dpi) {
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_TABSTOPS;
    pf.cTabCount = (SHORT)count;
    for (int i = 0; i < count; i++)
        pf.rgxTabs[i] = px_to_twips(anchors_px[i], dpi) |
            ((LONG)align[i] << 24);
    SendMessageW(window, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

static PARAFORMAT2 paragraph_at(HWND window, int cp) {
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    SendMessageW(window, EM_SETSEL, (WPARAM)cp, (LPARAM)(cp + 1));
    SendMessageW(window, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    return pf;
}

static int pos_x(HWND window, int cp) {
    POINTL pt;
    memset(&pt, 0, sizeof pt);
    SendMessageW(window, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)cp);
    return (int)pt.x;
}

static void write_text(HWND window, const wchar_t *text) {
    SetWindowTextW(window, L"");
    SendMessageW(window, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static bool approx(int a, int b, int tolerance) {
    int diff = a - b;
    if (diff < 0) diff = -diff;
    return diff <= tolerance;
}

/* DIP-normalized width of a run at 10pt Segoe UI, for constant calibration. */
static int width_dip(const wchar_t *text, int dpi) {
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    HFONT font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT previous = (HFONT)SelectObject(dc, font);
    SIZE size;
    memset(&size, 0, sizeof size);
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    SelectObject(dc, previous);
    DeleteObject(font);
    ReleaseDC(NULL, dc);
    return (int)((LONG64)size.cx * 96 / dpi);
}

int main(void) {
    CHECK(LoadLibraryW(L"Msftedit.dll") != NULL);
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"DarkChat.RichEditSpike";
    RegisterClassW(&cls);
    HWND parent = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 800, 600, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent);
    HWND window = CreateWindowExW(0, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE,
        0, 0, 600, 400, parent, (HMENU)(INT_PTR)1, GetModuleHandleW(NULL), NULL);
    CHECK(window);
    SendMessageW(window, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(0, 0, 0));
    SendMessageW(window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(0, 0));
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.yHeight = 200;                       /* 10pt */
    cf.bCharSet = DEFAULT_CHARSET;
    wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(window, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    const int dpi = control_dpi(window);
    printf("spike dpi = %d\n", dpi);

    /* 1. rgxTabs encoding round-trips through EM_GETPARAFORMAT. */
    {
        const int anchors[3] = {0, 140, 260};
        const unsigned char align[3] = {SPIKE_ALIGN_LEFT, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"col0\tcol1\tcol2");
        set_tab_stops(window, anchors, align, 3, dpi);
        PARAFORMAT2 pf = paragraph_at(window, 0);
        CHECK((pf.dwMask & PFM_TABSTOPS) != 0);
        CHECK(pf.cTabCount == 3);
        for (int i = 0; i < 3; i++) {
            CHECK((pf.rgxTabs[i] & 0x00FFFFFF) ==
                (px_to_twips(anchors[i], dpi) & 0x00FFFFFF));
            CHECK(((pf.rgxTabs[i] >> 24) & 0x0F) == align[i]);
        }
        /* Left tabs land the following run exactly on its anchor. */
        CHECK(approx(pos_x(window, 0), 0, 4));
        CHECK(approx(pos_x(window, 5), anchors[1], 6));
        CHECK(approx(pos_x(window, 10), anchors[2], 6));
    }

    /* 2a. Centered column zero uses a leading tab at its center anchor. */
    {
        const int anchors[3] = {60, 140, 260};
        const unsigned char align[3] = {SPIKE_ALIGN_CENTER, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"\tMM\tbb\tcc");
        set_tab_stops(window, anchors, align, 3, dpi);
        int x_mm = pos_x(window, 1);
        CHECK(x_mm > 20 && x_mm < anchors[0]);   /* centered around 60 */
        CHECK(approx(pos_x(window, 4), anchors[1], 6));
        CHECK(approx(pos_x(window, 7), anchors[2], 6));
    }

    /* 2b. Right-aligned column zero: its run's right edge meets the anchor. */
    {
        const int anchors[3] = {120, 140, 260};
        const unsigned char align[3] = {SPIKE_ALIGN_RIGHT, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"\tRR\tbb\tcc");
        set_tab_stops(window, anchors, align, 3, dpi);
        int x_rr = pos_x(window, 1);
        int rr_width = width_dip(L"RR", dpi) * dpi / 96;
        CHECK(x_rr > 60);                        /* right of the center anchor */
        CHECK(approx(x_rr + rr_width, anchors[0], 8));
        CHECK(approx(pos_x(window, 4), anchors[1], 6));
    }

    /* 2c. An empty cell still advances through its tab stop. */
    {
        const int anchors[3] = {0, 140, 260};
        const unsigned char align[3] = {SPIKE_ALIGN_LEFT, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"a\t\tccc");
        set_tab_stops(window, anchors, align, 3, dpi);
        CHECK(pos_x(window, 0) < 20);
        CHECK(approx(pos_x(window, 3), anchors[2], 6));
    }

    /* 2d. Overlong content is not clipped: it overruns the stop and the next
       tab advances past the current pen, so the primitive must wrap itself. */
    {
        const int anchors[3] = {0, 60, 130};
        const unsigned char align[3] = {SPIKE_ALIGN_LEFT, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"a\tWWWWWWWWWWWW\tccc");
        set_tab_stops(window, anchors, align, 3, dpi);
        int x_word = pos_x(window, 2);
        int x_next = pos_x(window, 15);
        CHECK(approx(x_word, anchors[1], 6));
        CHECK(x_next > anchors[2]);              /* stop 130 was overrun */
        CHECK(x_next > x_word);
    }

    /* 3. PFM_TABSTOPS with cTabCount = 0 clears stale stops. */
    {
        const int anchors[3] = {0, 140, 260};
        const unsigned char align[3] = {SPIKE_ALIGN_LEFT, SPIKE_ALIGN_LEFT,
            SPIKE_ALIGN_LEFT};
        write_text(window, L"plain");
        set_tab_stops(window, anchors, align, 3, dpi);
        CHECK(paragraph_at(window, 0).cTabCount == 3);
        set_tab_stops(window, anchors, align, 0, dpi);
        PARAFORMAT2 pf = paragraph_at(window, 0);
        CHECK((pf.dwMask & PFM_TABSTOPS) != 0 && pf.cTabCount == 0);
    }

    /* 4. LF-only insertion keeps char positions equal to source offsets. */
    {
        write_text(window, L"one\ntwo\nthree");
        CHECK(SendMessageW(window, EM_GETLINECOUNT, 0, 0) == 3);
        CHECK(SendMessageW(window, EM_LINEINDEX, 0, 0) == 0);
        CHECK(SendMessageW(window, EM_LINEINDEX, 1, 0) == 4);
        CHECK(SendMessageW(window, EM_LINEINDEX, 2, 0) == 8);
        CHECK(SendMessageW(window, EM_EXLINEFROMCHAR, 0, 4) == 1);
        CHECK(SendMessageW(window, EM_EXLINEFROMCHAR, 0, 8) == 2);
        SendMessageW(window, EM_SETSEL, (WPARAM)4, (LPARAM)7);
        wchar_t buffer[16];
        buffer[0] = 0;
        SendMessageW(window, EM_GETSELTEXT, 0, (LPARAM)buffer);
        CHECK(!wcscmp(buffer, L"two"));          /* offset 4 == control cp 4 */
        wchar_t whole[32];
        whole[0] = 0;
        GetWindowTextW(window, whole, 32);
        CHECK(!wcscmp(whole, L"one\r\ntwo\r\nthree"));
    }

    /* 5. Calibrate the two table dimension constants in DIPs. */
    {
        int avg_dip = width_dip(L"abcdefghijklmnopqrstuvwxyz", dpi) / 26;
        int space_dip = width_dip(L" ", dpi);
        if (avg_dip < 1) avg_dip = 1;
        printf("metrics: avg lowercase = %d DIP, space = %d DIP\n",
            avg_dip, space_dip);
        /* Minimum column fits at least four average glyphs; gutter at least
           two spaces. */
        CHECK(TABLE_MIN_COLUMN_DIP >= 4 * avg_dip);
        CHECK(TABLE_GUTTER_DIP >= 2 * space_dip);
    }

    printf("constants: TABLE_MIN_COLUMN_DIP=%d TABLE_GUTTER_DIP=%d "
        "TABLE_MAX_PHYSICAL_LINES=%d MD_MAX_FLATTEN_CHARS=%d\n",
        TABLE_MIN_COLUMN_DIP, TABLE_GUTTER_DIP, TABLE_MAX_PHYSICAL_LINES,
        MD_MAX_FLATTEN_CHARS);

    DestroyWindow(window);
    DestroyWindow(parent);
    puts("Rich Edit tab-stop spike: rgxTabs encoding, leading tab "
        "left/center/right, empty and overlong cells, cTabCount=0 clear, "
        "LF offset equality across three paragraphs, dimension calibration "
        "passed");
    return 0;
}
