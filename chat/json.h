#ifndef DARKCHAT_JSON_H
#define DARKCHAT_JSON_H

/* Focused JSON support for DarkChat: UTF-16 to escaped-UTF-8 request encoding
   and bounded extraction of string values from responses. Pure C with no
   Win32 dependency, so the unit tests run anywhere. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct {
    char *data;       /* always NUL-terminated after every append */
    size_t length, capacity;
    bool oom;         /* sticky failure flag; further appends are no-ops */
} JsonBuf;

void json_buf_init(JsonBuf *buf, size_t capacity);
void json_buf_free(JsonBuf *buf);
bool json_buf_ok(const JsonBuf *buf);
bool json_buf_append_raw(JsonBuf *buf, const char *data, size_t length);
/* Appends text as one quoted, escaped JSON string. \r\n and \r become \n;
   unpaired surrogates become U+FFFD. */
bool json_buf_append_json_string(JsonBuf *buf, const wchar_t *text);

/* Exact number of bytes json_buf_append_json_string writes for `text`,
   including both quotes. Shares the encoder's single traversal and size table,
   so a measured size and an encoded size cannot diverge; a size that cannot be
   represented saturates at SIZE_MAX rather than wrapping. */
size_t json_encoded_string_size(const wchar_t *text);

/* Exact length of the standard-alphabet padded base64 of `raw_bytes`:
   4 * ceil(n / 3). Saturates at SIZE_MAX rather than wrapping. */
size_t json_base64_payload_size(size_t raw_bytes);

/* Exact bytes of a standalone QUOTED base64 JSON string: two quotes plus
   json_base64_payload_size. Use this only where the payload is its own JSON
   string; inside an already-open string (a data URL, whose quotes live in the
   caller's framing literals) charge the payload size alone, or the quotes are
   counted twice. */
size_t json_encoded_base64_size(size_t raw_bytes);

/* Appends the standard-alphabet padded base64 of bytes[0..n) with no quotes
   and no escapes -- for a data-URL interior. Encodes in fixed-size chunks
   with no full-size temporary of the payload. `bytes` may be NULL only when
   n == 0. False on allocation failure (sticky oom). */
bool json_buf_append_base64(JsonBuf *buf, const unsigned char *bytes, size_t n);

/* Decodes a quoted JSON string starting at json into out (UTF-8,
   NUL-terminated). Handles \uXXXX escapes and UTF-16 surrogate pairs; a lone
   surrogate becomes U+FFFD. Returns one past the closing quote, or NULL on
   malformed input or if the decoded text does not fit in capacity. */
const char *json_decode_string(const char *json, char *out, size_t capacity);

/* Navigates a path such as "choices[0].message.content" from a JSON document
   and decodes the string value it names. A buffer of source length plus one
   always suffices; smaller buffers fail rather than truncate. */
bool json_query_string(const char *json, const char *path, char *out,
    size_t capacity);

/* Converts a UTF-8 buffer to a freshly allocated (free()) NUL-terminated
   UTF-16 string. Invalid bytes become U+FFFD. NULL on allocation failure. */
wchar_t *json_utf8_to_utf16(const char *utf8, size_t length);

bool json_validate(const char *json);
bool json_query_number(const char *json, const char *path, double *out);
/* Returns the number of elements in the array named by path. */
bool json_query_array_length(const char *json, const char *path, size_t *out);

/* Classifies the value a path names, so a caller can tell an absent field
   apart from one that is present but not a number. The field is located by
   exact key navigation (never substring matching), so "id" cannot match
   "identifier" and a quoted "5" is not a number. A malformed path (trailing
   dot, empty segment, malformed or unterminated index) returns false; a
   syntactically valid path that names nothing reports JSON_FIELD_ABSENT.
   Whenever `kind` is non-NULL it is always set on return: ABSENT for a
   rejected call, JSON_FIELD_NUMBER with the parsed value when a finite number
   is present, JSON_FIELD_INVALID for a string, bool, null, object or array. */
typedef enum {
    JSON_FIELD_ABSENT,
    JSON_FIELD_NUMBER,
    JSON_FIELD_INVALID
} JsonFieldKind;
bool json_query_field(const char *json, const char *path, JsonFieldKind *kind,
    double *value);

/* --- Value spans, strict decoding and container cursors ------------------- */

/* Strict variant of json_decode_string: in addition to the syntax rules it
    rejects unpaired surrogate escapes and a decoded NUL (U+0000) instead of
    substituting U+FFFD or dropping the byte, so a caller can never silently
    lose or corrupt an imported string. `out` may be NULL to validate and
    measure only (capacity is then ignored); otherwise the decoded text must
    fit, including the terminator. `length`, when non-NULL, receives the
    decoded UTF-8 byte count (excluding the terminator). Returns one past the
    closing quote, or NULL on malformed input or when it does not fit. */
const char *json_decode_string_strict(const char *json, char *out,
    size_t capacity, size_t *length);

/* Strict validation of a raw UTF-8 byte range: rejects overlong encodings,
    surrogate code points, values above U+10FFFF, and truncated sequences. */
bool json_utf8_valid(const char *utf8, size_t length);

/* Classification of the value a span pointer names. The whole value is
    validated, including its closing delimiter, so trailing junk or an
    unterminated container/string is JSON_VALUE_INVALID. */
typedef enum {
    JSON_VALUE_INVALID,
    JSON_VALUE_STRING,
    JSON_VALUE_NUMBER,
    JSON_VALUE_BOOL,
    JSON_VALUE_NULL,
    JSON_VALUE_OBJECT,
    JSON_VALUE_ARRAY
} JsonValueKind;
JsonValueKind json_value_kind(const char *value);

/* One past the end of the value at `value`, or NULL when malformed. */
const char *json_value_end(const char *value);

/* Parses a finite number occupying the value span, and a real true/false
    literal occupying the value span. False for any other type. */
bool json_value_number(const char *value, double *out);
bool json_value_bool(const char *value, bool *out);

/* Linear container iteration: initializing over a validated object or array
    and walking to the end visits every member/element once, without
    re-scanning from the document root. `value` must point at the '{' or '['.
    After a true json_cursor_next, json_cursor_key returns the raw key span
    (points at its opening quote; NULL for arrays) and json_cursor_value
    returns the value span. The document must already be well-formed
    (json_validate); the cursor is defensive but reports malformed structure by
    returning false early. */
typedef struct {
    const char *cursor;
    char close;
    bool first;
    const char *key;
    const char *value;
} JsonCursor;
bool json_cursor_object(JsonCursor *cursor, const char *value);
bool json_cursor_array(JsonCursor *cursor, const char *value);
bool json_cursor_next(JsonCursor *cursor);
const char *json_cursor_key(const JsonCursor *cursor);
const char *json_cursor_value(const JsonCursor *cursor);

#endif
