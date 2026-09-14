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

#endif
