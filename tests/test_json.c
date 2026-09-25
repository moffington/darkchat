#include "chat/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Pure-C JSON encoding/decoding tests for the OpenRouter client. No Windows
   APIs, so they run anywhere. Built and run by `chat.bat test`. */

static int failures;

static void check(int condition, const char *what) {
    if (!condition) { printf("FAIL: %s\n", what); ++failures; }
    else printf("ok: %s\n", what);
}

/* Allocation seam for the base64 append's growth path: pass `pass_allocs`
   wrapped allocations, fail the next `fail_allocs`, then resume. Mirrors
   tests/test_export.c. */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
static long pass_allocs, fail_allocs;
void *__wrap_malloc(size_t size) {
    if (pass_allocs > 0) { --pass_allocs; return __real_malloc(size); }
    if (fail_allocs > 0) { --fail_allocs; return NULL; }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (pass_allocs > 0) { --pass_allocs; return __real_realloc(pointer, size); }
    if (fail_allocs > 0) { --fail_allocs; return NULL; }
    return __real_realloc(pointer, size);
}
void __wrap_free(void *pointer) {
    __real_free(pointer);
}
static void seam_reset(void) {
    pass_allocs = fail_allocs = 0;
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
    check(json_encoded_string_size(text) == buf.length,
        "the measured size equals the encoded size");

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

    size_t count = 99;
    check(json_query_array_length(response, "choices", &count) && count == 1,
        "array length extracted");
    check(json_query_array_length("{\"items\":[]}", "items", &count) &&
        count == 0, "empty array length extracted");
    check(!json_query_array_length(response, "usage", &count) &&
        !json_query_array_length(response, "missing", &count),
        "non-array and missing array rejected");
}

static void test_encoded_size(void) {
    JsonBuf buf;
    json_buf_init(&buf, 0);
    /* Escaping rules: \r\n and \r fold to \n (2 bytes), \b\t\n\f are 2 bytes,
       other controls are \u00xx (6), quote and backslash are 2, and text is
       UTF-8 (1-4 bytes). The measured size must track every one of those. */
    struct { const wchar_t *text; size_t expected; const char *what; } cases[] = {
        { L"", 2, "an empty string is two quotes" },
        { L"abc", 5, "ascii is one byte per unit" },
        { L"\n\t\b\f", 10, "named escapes are two bytes each" },
        { L"\r\n", 4, "crlf folds to one newline" },
        { L"\r", 4, "a lone carriage return folds to one newline" },
        { L"\x0001", 8, "other controls use a six-byte escape" },
        { L"\"\\", 6, "quote and backslash are escaped" },
        { L"\u00e9", 4, "two-byte utf8" },
        { L"\u2014", 5, "three-byte utf8" },
        { L"\U0001f600", 6, "a surrogate pair is four-byte utf8" },
        { L"\xd83d", 5, "a lone surrogate becomes U+FFFD" }
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        json_buf_free(&buf);
        json_buf_init(&buf, 0);
        bool written = json_buf_append_json_string(&buf, cases[i].text);
        check(written && json_encoded_string_size(cases[i].text) == buf.length,
            cases[i].what);
        check(json_encoded_string_size(cases[i].text) == cases[i].expected,
            "the size matches the documented escape cost");
    }
    /* A mixed string over the growth boundary keeps the two in step. */
    json_buf_free(&buf);
    json_buf_init(&buf, 16);
    wchar_t mixed[600];
    for (int i = 0; i < 599; i++) mixed[i] = (wchar_t)(i % 7 == 0 ? L'\t' : L'a' + i % 26);
    mixed[599] = 0;
    check(json_buf_append_json_string(&buf, mixed) &&
        json_encoded_string_size(mixed) == buf.length,
        "a long mixed string measures what it encodes");
    json_buf_free(&buf);
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

static void test_field_kind(void) {
    JsonFieldKind kind;
    double value = -1;
    /* Absent fields are distinguished from present-but-invalid values. */
    check(json_query_field("{\"a\":1}", "b", &kind, &value) &&
        kind == JSON_FIELD_ABSENT, "absent field reports ABSENT");
    check(json_query_field("{\"id\":5}", "id", &kind, &value) &&
        kind == JSON_FIELD_NUMBER && value == 5,
        "present number reports NUMBER");
    check(json_query_field("{\"id\":2.5}", "id", &kind, &value) &&
        kind == JSON_FIELD_NUMBER && value == 2.5,
        "fractional number reports NUMBER with its value");
    check(json_query_field("{\"id\":-3}", "id", &kind, &value) &&
        kind == JSON_FIELD_NUMBER && value == -3,
        "negative number reports NUMBER with its value");
    /* Present but not numeric: a quoted "5" is a string, never parsed as the
       number 5 (no substring matching anywhere in the query path). */
    check(json_query_field("{\"id\":\"5\"}", "id", &kind, &value) &&
        kind == JSON_FIELD_INVALID, "quoted number reports INVALID");
    check(json_query_field("{\"id\":true}", "id", &kind, &value) &&
        kind == JSON_FIELD_INVALID, "bool reports INVALID");
    check(json_query_field("{\"id\":null}", "id", &kind, &value) &&
        kind == JSON_FIELD_INVALID, "null reports INVALID");
    check(json_query_field("{\"id\":{}}", "id", &kind, &value) &&
        kind == JSON_FIELD_INVALID, "object reports INVALID");
    check(json_query_field("{\"id\":[]}", "id", &kind, &value) &&
        kind == JSON_FIELD_INVALID, "array reports INVALID");
    /* Navigation and boundary behavior. */
    check(json_query_field("{\"a\":{\"id\":7}}", "a.id", &kind, &value) &&
        kind == JSON_FIELD_NUMBER && value == 7, "nested field found");
    check(json_query_field("[{\"id\":9}]", "[0].id", &kind, &value) &&
        kind == JSON_FIELD_NUMBER && value == 9, "array element found");
    check(json_query_field("{\"identifier\":3}", "id", &kind, &value) &&
        kind == JSON_FIELD_ABSENT, "a longer key never substring-matches");
    check(json_query_field("{\"id\":5}", "", &kind, &value) == false,
        "empty path rejected");
    check(json_query_field(NULL, "id", &kind, &value) == false &&
        json_query_field("{}", "id", NULL, &value) == false,
        "invalid arguments rejected");
    check(json_query_field("{\"a\":1}", "b", &kind, NULL) &&
        kind == JSON_FIELD_ABSENT, "value pointer optional");
    /* Malformed paths are rejected as errors, not reported absent, and the
       kind is still initialized for the caller. */
    JsonFieldKind preset = JSON_FIELD_INVALID;
    check(json_query_field("{\"a\":{\"b\":1}}", "a.", &kind, &value) == false &&
        kind == JSON_FIELD_ABSENT, "trailing dot rejected");
    kind = preset;
    check(json_query_field("{\"a\":1}", ".a", &kind, &value) == false &&
        kind == JSON_FIELD_ABSENT, "leading dot rejected");
    kind = preset;
    check(json_query_field("{\"a\":{\"b\":1}}", "a..b", &kind, &value) == false &&
        kind == JSON_FIELD_ABSENT, "empty middle segment rejected");
    kind = preset;
    check(json_query_field("{\"[x]\":1}", "[x]", &kind, &value) == false &&
        kind == JSON_FIELD_ABSENT, "non-numeric index rejected");
    kind = preset;
    check(json_query_field("{\"a[3\":1}", "a[3", &kind, &value) == false &&
        kind == JSON_FIELD_ABSENT, "unterminated index rejected");
    /* A valid path that names nothing stays ABSENT, distinct from malformed. */
    check(json_query_field("{\"a\":{\"b\":1}}", "a.c", &kind, &value) &&
        kind == JSON_FIELD_ABSENT, "missing key under valid path is absent");
}

static void test_spans_and_cursors(void) {
    /* Strict UTF-8 validation. */
    check(json_utf8_valid("plain", 5), "ascii is valid utf-8");
    check(json_utf8_valid("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 9),
        "2/3/4-byte sequences are valid");
    check(!json_utf8_valid("\xc0\x80", 2), "overlong encoding rejected");
    check(!json_utf8_valid("\xed\xa0\x80", 3), "surrogate encoding rejected");
    check(!json_utf8_valid("\xf4\x90\x80\x80", 4), "above U+10FFFF rejected");
    check(!json_utf8_valid("\xe2\x82", 2), "truncated sequence rejected");
    check(!json_utf8_valid("\x80", 1), "lone continuation byte rejected");
    check(!json_utf8_valid("\xff", 1), "0xFF start byte rejected");
    check(json_utf8_valid(NULL, 0) && !json_utf8_valid(NULL, 1),
        "null range is only valid when empty");

    /* Value classification and spans. */
    check(json_value_kind("\"s\"") == JSON_VALUE_STRING, "string classified");
    check(json_value_kind("  -1.5e2 ") == JSON_VALUE_NUMBER,
        "number classified with leading space");
    check(json_value_kind("true") == JSON_VALUE_BOOL &&
        json_value_kind("false") == JSON_VALUE_BOOL, "booleans classified");
    check(json_value_kind("null") == JSON_VALUE_NULL, "null classified");
    check(json_value_kind("{\"a\":1}") == JSON_VALUE_OBJECT, "object classified");
    check(json_value_kind("[1,2]") == JSON_VALUE_ARRAY, "array classified");
    check(json_value_kind("truex") == JSON_VALUE_INVALID &&
        json_value_kind("nul") == JSON_VALUE_INVALID &&
        json_value_kind("01") == JSON_VALUE_INVALID, "malformed values rejected");
    /* The whole value is validated, so trailing junk is never a match. */
    check(json_value_kind("{\"a\":1}junk") == JSON_VALUE_INVALID &&
        json_value_kind("\"x\"junk") == JSON_VALUE_INVALID &&
        json_value_kind("[1]junk") == JSON_VALUE_INVALID,
        "trailing junk after a value is rejected");
    check(json_value_kind("\"x\" junk") == JSON_VALUE_INVALID &&
        json_value_kind("{\"a\":1} junk") == JSON_VALUE_INVALID &&
        json_value_kind("[1] junk") == JSON_VALUE_INVALID &&
        json_value_kind("true junk") == JSON_VALUE_INVALID,
        "whitespace does not hide trailing junk");
    check(json_value_kind("{\"a\":1},{\"b\":2}") == JSON_VALUE_OBJECT &&
        json_value_kind("\"x\",") == JSON_VALUE_STRING,
        "a delimiter after a value is accepted");
    {
        const char *doc = "{\"a\":1}tail";
        const char *end = json_value_end(doc);
        check(end == doc + 7, "value end stops at the value");
    }
    {
        double number = 0;
        check(json_value_number(" -1.25e+2 ,", &number) && number == -125,
            "span number parses");
        check(!json_value_number("01", &number) &&
            !json_value_number("\"1\"", &number) &&
            !json_value_number("1 junk", &number),
            "bad span numbers rejected");
    }
    {
        bool flag = false;
        check(json_value_bool(" true ", &flag) && flag,
            "span bool parses true");
        check(json_value_bool("false}", &flag) && !flag,
            "span bool parses false");
        check(!json_value_bool("1", &flag) && !json_value_bool("truex", &flag) &&
            !json_value_bool("true junk", &flag),
            "non-bool spans rejected");
    }

    /* Strict string decoding: reject lossy escapes. */
    {
        char out[32];
        size_t length = 0;
        check(json_decode_string_strict("\"ok\"", out, sizeof out, &length) &&
            !strcmp(out, "ok") && length == 2, "strict decode of plain text");
        check(json_decode_string_strict("\"\\uD83D\\uDE80\"", out, sizeof out,
            &length) && length == 4, "strict decode of a surrogate pair");
        check(json_decode_string_strict("\"\\uD800\"", NULL, 0, &length) == NULL,
            "unpaired high surrogate escape rejected");
        check(json_decode_string_strict("\"\\uDC00\"", NULL, 0, &length) == NULL,
            "lone low surrogate escape rejected");
        check(json_decode_string_strict("\"a\\u0000b\"", NULL, 0,
            &length) == NULL, "escaped NUL rejected");
        check(json_decode_string_strict("\"a\nb\"", NULL, 0,
            &length) == NULL, "literal newline rejected");
        check(json_decode_string_strict("\"a\tb\"", NULL, 0,
            &length) == NULL, "literal tab rejected");
        check(json_decode_string_strict("\"smol\"", out, sizeof out, &length) &&
            length == 4, "literal space is not a control character");
        check(json_decode_string_strict("\"toolong\"", out, 4, &length) == NULL,
            "strict decode rejects a too-small buffer");
        check(json_decode_string_strict("\"measure\"", NULL, 0,
            &length) && length == 7, "measure-only decode reports length");
    }

    /* Container cursors walk without root re-scanning. */
    {
        const char *doc = "{ \"format\": \"x\", \"version\": 1, "
            "\"conversations\": [ {\"a\":true}, {} ] }";
        JsonCursor root;
        check(json_cursor_object(&root, doc), "root cursor opens");
        char key[32];
        int seen = 0;
        while (json_cursor_next(&root)) {
            const char *raw = json_cursor_key(&root);
            check(raw != NULL && json_decode_string_strict(raw, key,
                sizeof key, NULL) != NULL, "cursor key decodes");
            if (!strcmp(key, "format"))
                check(json_value_kind(json_cursor_value(&root)) ==
                    JSON_VALUE_STRING, "format value is a string");
            if (!strcmp(key, "version"))
                check(json_value_kind(json_cursor_value(&root)) ==
                    JSON_VALUE_NUMBER, "version value is a number");
            if (!strcmp(key, "conversations")) {
                JsonCursor array;
                check(json_cursor_array(&array, json_cursor_value(&root)),
                    "conversations cursor opens");
                int elements = 0;
                while (json_cursor_next(&array)) {
                    check(json_value_kind(json_cursor_value(&array)) ==
                        JSON_VALUE_OBJECT, "element is an object");
                    ++elements;
                }
                check(elements == 2, "array cursor visits every element");
            }
            ++seen;
        }
        check(seen == 3, "root cursor visits every member");
    }
    /* Empty containers yield no iterations. */
    {
        JsonCursor cursor;
        check(json_cursor_object(&cursor, "{}") && !json_cursor_next(&cursor),
            "empty object visits nothing");
        check(json_cursor_array(&cursor, "[]") && !json_cursor_next(&cursor),
            "empty array visits nothing");
        check(!json_cursor_object(&cursor, "[]") &&
            !json_cursor_array(&cursor, "{}"), "wrong container type rejected");
    }
}

/* Base64: payload size is the closed form 4*ceil(n/3), the quoted size adds
   exactly the two string quotes, and the chunked encoder writes exactly the
   measured bytes. The quote split is load-bearing: an image part's literals
   carry the URL's quotes, so they must charge the payload size alone. */
static void test_base64(void) {
    struct { const unsigned char *bytes; size_t n; const char *expected; }
    vectors[] = {
        { (const unsigned char *)"", 0, "" },
        { (const unsigned char *)"M", 1, "TQ==" },
        { (const unsigned char *)"Ma", 2, "TWE=" },
        { (const unsigned char *)"Man", 3, "TWFu" },
        { (const unsigned char *)"Mana", 4, "TWFuYQ==" },
        { (const unsigned char *)"Manan", 5, "TWFuYW4=" },
        { (const unsigned char *)"\x00\x00\x00", 3, "AAAA" },
        { (const unsigned char *)"\xfb\xff\xbf", 3, "+/+/" }
    };
    JsonBuf buf;
    json_buf_init(&buf, 0);
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        json_buf_free(&buf);
        /* Capacity up front: a zero-length append on a buffer with no storage
           reports its state exactly like json_buf_append_raw does. */
        json_buf_init(&buf, 8);
        check(json_buf_append_base64(&buf, vectors[i].bytes, vectors[i].n),
            "base64 append succeeds");
        check(buf.length == json_base64_payload_size(vectors[i].n),
            "the measured payload size equals the encoded length");
        check(buf.length == strlen(vectors[i].expected) &&
            !memcmp(buf.data, vectors[i].expected, buf.length),
            "the encoding matches the standard alphabet and padding");
    }
    /* Closed form at the group boundaries and beyond one chunk. */
    size_t sizes[] = { 0, 1, 2, 3, 4, 5, 6, 1000, (1u << 20) - 1, 1u << 20,
        (1u << 20) + 1 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t n = sizes[i];
        check(json_base64_payload_size(n) == 4 * ((n + 2) / 3),
            "the payload size is 4*ceil(n/3)");
        check(json_encoded_base64_size(n) == json_base64_payload_size(n) + 2,
            "the quoted size adds exactly the two quotes");
    }
    /* The quoted size is what json_encoded_string_size measures for the same
       ASCII payload -- base64 never needs an escape. */
    json_buf_free(&buf);
    json_buf_init(&buf, 0);
    check(json_buf_append_base64(&buf, (const unsigned char *)"Ma", 2),
        "fixture: a small payload encodes");
    wchar_t wide[8];
    for (size_t i = 0; i < buf.length; i++) wide[i] = (wchar_t)buf.data[i];
    wide[buf.length] = 0;
    check(json_encoded_base64_size(2) == json_encoded_string_size(wide),
        "the quoted base64 size agrees with the string encoder");
    /* A payload spanning several internal chunks still measures right. */
    unsigned char *big = (unsigned char *)malloc(2000);
    if (!big) { check(0, "fixture: big payload allocated"); return; }
    for (size_t i = 0; i < 2000; i++) big[i] = (unsigned char)(i & 0xff);
    json_buf_free(&buf);
    json_buf_init(&buf, 0);
    check(json_buf_append_base64(&buf, big, 2000) &&
        buf.length == json_base64_payload_size(2000),
        "a multi-chunk payload measures what it encodes");
    /* Saturation: an unrepresentable size never wraps to a small number. */
    check(json_base64_payload_size(SIZE_MAX) == SIZE_MAX &&
        json_encoded_base64_size(SIZE_MAX) == SIZE_MAX,
        "an unrepresentable base64 size saturates");
    /* Growth failure at every allocation point of the multi-chunk append:
       the call fails with the sticky oom flag and a length that holds only
       whole groups it actually wrote. */
    for (int k = 0; k < 4; k++) {
        json_buf_free(&buf);
        json_buf_init(&buf, 8);
        pass_allocs = k;
        fail_allocs = 1;
        bool ok = json_buf_append_base64(&buf, big, 2000);
        seam_reset();
        if (ok) {
            check(buf.length == json_base64_payload_size(2000),
                "an unfailed append writes every group");
        } else {
            check(!json_buf_ok(&buf) && buf.length % 4 == 0 &&
                buf.length < json_base64_payload_size(2000),
                "a failed base64 append reports oom and only whole groups");
        }
    }
    free(big);
    json_buf_free(&buf);
}

int main(void) {
    double value;
    check(json_validate("{\"n\":-1.25e+2,\"s\":\"\\uD83D\\uDE80\"}"), "strict document validates");
    check(!json_validate("{\"n\":01}") && !json_validate("{\"n\":1e}") &&
        !json_validate("{\"n\":1,}") && !json_validate("{} trailing"), "malformed numbers and trailing data rejected");
    check(!json_validate("{\"s\":\"\\q\"}") && !json_validate("{\"s\":\"a\nb\"}"), "invalid string escapes and controls rejected");
    check(json_query_number("{\"usage\":{\"cost\":0.000012}}","usage.cost",&value) &&
        value==0.000012, "nested fractional cost decoded");
    check(!json_query_number("{\"n\":null}","n",&value) &&
        !json_query_number("{\"n\":1e9999}","n",&value), "unknown and overflowing numbers rejected");
    test_round_trip();
    test_escapes();
    test_queries();
    test_encoded_size();
    test_base64();
    test_buffers();
    test_utf16_conversion();
    test_field_kind();
    test_spans_and_cursors();
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall json checks passed\n");
    return 0;
}
