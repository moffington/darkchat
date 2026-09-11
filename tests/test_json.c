#include "../chat/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pure-C JSON encoding/decoding tests for the OpenRouter client. No Windows
   APIs, so they run anywhere. Built and run by `chat.bat test`. */

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

/* Decodes a UTF-8 string literal into a UTF-16 buffer for comparisons. */
static int utf16_of(const char *utf8, wchar_t *out, size_t capacity) {
    wchar_t *wide = json_utf8_to_utf16(utf8, strlen(utf8));
    if (!wide) return 0;
    size_t length = wcslen(wide);
    int ok = length + 1 <= capacity;
    if (ok) wcscpy(out, wide);
    free(wide);
    return ok;
}

static void test_round_trip(void) {
    JsonBuf buf;
    json_buf_init(&buf, 16);
    const wchar_t *text =
        L"He said \"hi\"\nBack\\slash\tTab\r\nCRLF\rCR \u00e9 \U0001f600 end";
    check(json_buf_append_json_string(&buf, text), "append json string");
    check(json_buf_ok(&buf) && buf.data && buf.length > 0,
        "json buffer holds the string");

    char decoded[512];
    check(json_decode_string(buf.data, decoded, sizeof decoded) != NULL,
        "decode accepts what we encoded");
    wchar_t wide[256];
    check(utf16_of(decoded, wide, 256), "decoded utf8 converts to utf16");
    check(wcscmp(wide, L"He said \"hi\"\nBack\\slash\tTab\nCRLF\nCR "
        L"\u00e9 \U0001f600 end") == 0, "round trip preserves text");
    json_buf_free(&buf);

    /* Invalid JSON strings are rejected. */
    check(json_decode_string("\"unterminated", decoded, sizeof decoded) == NULL,
        "missing closing quote rejected");
    check(json_decode_string("\"bad \\q escape\"", decoded,
        sizeof decoded) == NULL, "unknown escape rejected");
    check(json_decode_string("\"trunc \\u12", decoded,
        sizeof decoded) == NULL, "truncated \\u rejected");
    check(json_decode_string("no quotes", decoded, sizeof decoded) == NULL,
        "non-string rejected");
}

static void test_escapes(void) {
    char out[64];
    check(json_decode_string("\"\\u00e9\"", out, sizeof out) != NULL,
        "\\u00e9 decodes");
    check((unsigned char)out[0] == 0xc3 && (unsigned char)out[1] == 0xa9 &&
        out[2] == 0, "\\u00e9 is UTF-8 e-acute");
    check(json_decode_string("\"\\ud83d\\ude00\"", out, sizeof out) != NULL,
        "surrogate pair decodes");
    check((unsigned char)out[0] == 0xf0 && (unsigned char)out[1] == 0x9f &&
        (unsigned char)out[2] == 0x98 && (unsigned char)out[3] == 0x80 &&
        out[4] == 0, "surrogate pair encodes the emoji");
    check(json_decode_string("\"\\ud83d\"", out, sizeof out) != NULL,
        "lone high surrogate tolerated");
    check((unsigned char)out[0] == 0xef && (unsigned char)out[1] == 0xbf &&
        (unsigned char)out[2] == 0xbd && out[3] == 0,
        "lone surrogate becomes U+FFFD");
    check(json_decode_string("\"\\uXXXX\"", out, sizeof out) == NULL,
        "non-hex escape rejected");
    check(json_decode_string("\"\\ud83d\\u0041\"", out, sizeof out) != NULL,
        "high surrogate without a low pair tolerated");
    check(json_decode_string("\"\\/\"", out, sizeof out) != NULL &&
        out[0] == '/', "escaped slash decodes");
    check(json_decode_string("\"\\u0000tail\"", out, sizeof out) != NULL,
        "\\u0000 does not abort decoding");
}

static void test_queries(void) {
    const char *response =
        "{\n  \"id\": \"gen-1\",\n  \"choices\": [\n"
        "    { \"message\": { \"role\": \"assistant\",\n"
        "      \"content\": \"Hello \\\"world\\\"\\nLine 2 \\u00e9\\ud83d\\ude00\" },\n"
        "      \"finish_reason\": \"stop\" }\n  ],\n"
        "  \"usage\": { \"total_tokens\": 42 }\n}";
    char out[256];
    check(json_query_string(response, "choices[0].message.content", out,
        sizeof out), "content extracted");
    wchar_t wide[128];
    check(utf16_of(out, wide, 128), "content converts");
    check(wcscmp(wide, L"Hello \"world\"\nLine 2 \u00e9\U0001f600") == 0,
        "content survives escapes and unicode");

    check(json_query_string(response, "id", out, sizeof out),
        "top-level string found");
    check(json_query_string(response, "choices[0].finish_reason", out,
        sizeof out) && strcmp(out, "stop") == 0, "sibling key found");
    check(json_query_string(response, "usage.total_tokens", out,
        sizeof out) == false, "number is not a string");
    check(json_query_string(response, "choices[1].message.content", out,
        sizeof out) == false, "out-of-range index fails");
    check(json_query_string(response, "choices[0].missing", out,
        sizeof out) == false, "missing key fails");
    check(json_query_string(response, "", out, sizeof out) == false,
        "empty path fails");
    check(json_query_string("{", "id", out, sizeof out) == false,
        "truncated document fails");
    check(json_query_string(response, "choices[0].message.content", out, 8)
        == false, "small buffer fails instead of truncating");

    const char *error =
        "{\"error\": { \"message\": \"Rate limited\", \"code\": 429 }}";
    check(json_query_string(error, "error.message", out, sizeof out) &&
        strcmp(out, "Rate limited") == 0, "API error message extracted");
}

static void test_buffers(void) {
    JsonBuf buf;
    json_buf_init(&buf, 0);
    wchar_t *long_text = (wchar_t *)malloc(70000 * sizeof *long_text);
    char *decoded = (char *)malloc(300000);
    if (long_text && decoded) {
        for (int i = 0; i < 69999; i++) long_text[i] = (wchar_t)(L'a' + i % 26);
        long_text[69999] = 0;
        check(json_buf_append_json_string(&buf, long_text),
            "long string survives growth");
        check(json_decode_string(buf.data, decoded, 300000) != NULL,
            "long string decodes");
        wchar_t *converted =
            json_utf8_to_utf16(decoded, strlen(decoded));
        check(converted && wcscmp(converted, long_text) == 0,
            "decoded long string matches");
        free(converted);
    } else check(0, "test allocation succeeded");
    free(long_text);
    free(decoded);
    json_buf_free(&buf);

    json_buf_init(&buf, 8);
    check(json_buf_append_raw(&buf, "hello", 5), "raw append");
    check(json_buf_append_raw(&buf, " world", 6), "raw append grows");
    check(buf.data && strcmp(buf.data, "hello world") == 0,
        "raw buffer content matches");
    json_buf_free(&buf);
}

static void test_utf16_conversion(void) {
    char out[64];
    check(json_decode_string("\"plain\"", out, sizeof out) != NULL,
        "ascii decodes");
    wchar_t wide[16];
    check(utf16_of(out, wide, 16) && wcscmp(wide, L"plain") == 0,
        "ascii converts");
    /* Invalid UTF-8 becomes U+FFFD rather than garbage. */
    wchar_t *converted = json_utf8_to_utf16("\xff\xfe bad", 7);
    check(converted && converted[0] == 0xfffd && converted[1] == 0xfffd,
        "invalid bytes become U+FFFD");
    free(converted);
}

int main(void) {
    test_round_trip();
    test_escapes();
    test_queries();
    test_buffers();
    test_utf16_conversion();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall json checks passed\n");
    return 0;
}
