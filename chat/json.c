#include "chat/json.h"
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- UTF-8 conversion --------------------------------------------------- */

static size_t encode_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static size_t decode_utf8(const char *s, size_t left, uint32_t *cp) {
    unsigned char b0 = (unsigned char)s[0];
    if (b0 < 0x80) { *cp = b0; return 1; }
    unsigned need;
    uint32_t value;
    if ((b0 & 0xe0) == 0xc0) { need = 1; value = b0 & 0x1fu; }
    else if ((b0 & 0xf0) == 0xe0) { need = 2; value = b0 & 0x0fu; }
    else if ((b0 & 0xf8) == 0xf0) { need = 3; value = b0 & 0x07u; }
    else return 0;
    if (left < (size_t)need + 1) return 0;
    for (unsigned k = 1; k <= need; k++) {
        unsigned char b = (unsigned char)s[k];
        if ((b & 0xc0) != 0x80) return 0;
        value = (value << 6) | (b & 0x3fu);
    }
    if ((need == 1 && value < 0x80) || (need == 2 && value < 0x800) ||
        (need == 3 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return 0;
    *cp = value;
    return (size_t)need + 1;
}

wchar_t *json_utf8_to_utf16(const char *utf8, size_t length) {
    if (!utf8) return NULL;
    size_t units = 0, i = 0;
    while (i < length) {
        uint32_t cp;
        size_t n = decode_utf8(utf8 + i, length - i, &cp);
        if (!n) { cp = 0xfffd; n = 1; }
        units += cp >= 0x10000 ? 2 : 1;
        i += n;
    }
    wchar_t *out = (wchar_t *)malloc((units + 1) * sizeof *out);
    if (!out) return NULL;
    size_t o = 0;
    i = 0;
    while (i < length) {
        uint32_t cp;
        size_t n = decode_utf8(utf8 + i, length - i, &cp);
        if (!n) { cp = 0xfffd; n = 1; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (wchar_t)(0xd800 + (cp >> 10));
            out[o++] = (wchar_t)(0xdc00 + (cp & 0x3ff));
        } else if (cp) {
            out[o++] = (wchar_t)cp;
        }
    }
    out[o] = 0;
    return out;
}

/* --- Encoding ----------------------------------------------------------- */

void json_buf_init(JsonBuf *buf, size_t capacity) {
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
    buf->oom = false;
    if (capacity) {
        buf->data = (char *)malloc(capacity);
        if (buf->data) buf->capacity = capacity;
        else buf->oom = true;
    }
}

void json_buf_free(JsonBuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

bool json_buf_ok(const JsonBuf *buf) { return buf->data && !buf->oom; }

static bool reserve(JsonBuf *buf, size_t extra) {
    if (buf->oom) return false;
    if (buf->length + extra + 1 <= buf->capacity) return true;
    size_t capacity = buf->capacity ? buf->capacity : 1024;
    while (capacity < buf->length + extra + 1) capacity *= 2;
    char *data = (char *)realloc(buf->data, capacity);
    if (!data) { buf->oom = true; return false; }
    buf->data = data;
    buf->capacity = capacity;
    return true;
}

bool json_buf_append_raw(JsonBuf *buf, const char *data, size_t length) {
    if (!length) return json_buf_ok(buf);
    if (!reserve(buf, length)) return false;
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    buf->data[buf->length] = 0;
    return true;
}

/* Escape sink: writes into a buffer, or only counts bytes when `buf` is NULL.
   The encoder and the size query therefore share one traversal and one set of
   size rules, so an encoded size and a measured size cannot diverge. */
typedef struct { JsonBuf *buf; size_t bytes; } EscapeSink;

static bool escape_emit(EscapeSink *sink, const char *data, size_t length) {
    /* Saturate instead of wrapping: an encoded size that cannot be represented
       is reported as SIZE_MAX, never as a small value. */
    sink->bytes = sink->bytes > SIZE_MAX - length ? SIZE_MAX : sink->bytes + length;
    return !sink->buf || json_buf_append_raw(sink->buf, data, length);
}

/* Writes one escaped code point and reports its length; the caller emits the
   surrounding quotes. */
static bool append_escaped_codepoint(EscapeSink *sink, uint32_t cp) {
    char scratch[8];
    const char *chunk = scratch;
    size_t length;
    if (cp < 0x20) {
        const char *named = NULL;
        switch (cp) {
        case 0x08: named = "\\b"; break;
        case 0x09: named = "\\t"; break;
        case 0x0a: named = "\\n"; break;
        case 0x0c: named = "\\f"; break;
        default: break;
        }
        if (named) { chunk = named; length = 2; }
        else {
            static const char hex[] = "0123456789abcdef";
            scratch[0] = '\\'; scratch[1] = 'u'; scratch[2] = '0'; scratch[3] = '0';
            scratch[4] = hex[(cp >> 4) & 0xf];
            scratch[5] = hex[cp & 0xf];
            length = 6;
        }
    } else if (cp == '"' || cp == '\\') {
        scratch[0] = '\\';
        scratch[1] = (char)cp;
        length = 2;
    } else {
        length = encode_utf8(scratch, cp);
    }
    return escape_emit(sink, chunk, length);
}

/* One traversal of a UTF-16 string body, without the surrounding quotes.
   Returns false only when writing into a real buffer failed. */
static bool escape_text(EscapeSink *sink, const wchar_t *text) {
    if (!text) text = L"";
    const wchar_t *p = text;
    for (;;) {
        wchar_t c = *p++;
        if (!c) break;
        if (c == L'\r') {
            /* Composer text arrives with CRLF; JSON and the model both want
               plain newlines. */
            if (*p == L'\n') ++p;
            if (!append_escaped_codepoint(sink, '\n')) return false;
            continue;
        }
        if (c >= 0xd800 && c <= 0xdbff && *p >= 0xdc00 && *p <= 0xdfff) {
            uint32_t cp = 0x10000 + ((c - 0xd800) << 10) + (*p - 0xdc00);
            ++p;
            if (!append_escaped_codepoint(sink, cp)) return false;
            continue;
        }
        if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;  /* lone surrogate */
        if (!append_escaped_codepoint(sink, (uint32_t)c)) return false;
    }
    return true;
}

bool json_buf_append_json_string(JsonBuf *buf, const wchar_t *text) {
    if (!json_buf_append_raw(buf, "\"", 1)) return false;
    EscapeSink sink = { buf, 0 };
    if (!escape_text(&sink, text)) return false;
    return json_buf_append_raw(buf, "\"", 1);
}

size_t json_encoded_string_size(const wchar_t *text) {
    EscapeSink sink = { NULL, 2 };  /* both quotes, then the escaped body */
    escape_text(&sink, text);
    return sink.bytes;
}

/* --- Base64 ------------------------------------------------------------- */

/* Standard alphabet with '=' padding. Pure ASCII, so the payload needs no
   JSON escapes wherever it lands. */
static const char BASE64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t json_base64_payload_size(size_t raw_bytes) {
    /* ceil(n / 3) groups of four characters, saturating. The quotient is
       formed without adding to n, so n == SIZE_MAX cannot wrap first. */
    size_t groups = raw_bytes / 3 + (raw_bytes % 3 ? 1 : 0);
    return groups > SIZE_MAX / 4 ? SIZE_MAX : groups * 4;
}

size_t json_encoded_base64_size(size_t raw_bytes) {
    size_t payload = json_base64_payload_size(raw_bytes);
    return payload > SIZE_MAX - 2 ? SIZE_MAX : payload + 2;
}

bool json_buf_append_base64(JsonBuf *buf, const unsigned char *bytes, size_t n) {
    if (!n) return json_buf_ok(buf);
    if (!bytes) return false;
    char chunk[512];   /* a multiple of the 4-character group */
    size_t out = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned b0 = bytes[i];
        unsigned b1 = i + 1 < n ? bytes[i + 1] : 0;
        unsigned b2 = i + 2 < n ? bytes[i + 2] : 0;
        chunk[out++] = BASE64_ALPHABET[b0 >> 2];
        chunk[out++] = BASE64_ALPHABET[((b0 & 0x03) << 4) | (b1 >> 4)];
        chunk[out++] = i + 1 < n
            ? BASE64_ALPHABET[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
        chunk[out++] = i + 2 < n ? BASE64_ALPHABET[b2 & 0x3f] : '=';
        if (out == sizeof chunk) {
            if (!json_buf_append_raw(buf, chunk, out)) return false;
            out = 0;
        }
    }
    return out ? json_buf_append_raw(buf, chunk, out) : json_buf_ok(buf);
}

/* --- Decoding ----------------------------------------------------------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

static int hex4(const char *p) {
    int value = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= c - '0';
        else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
        else return -1;
    }
    return value;
}

/* Shared decoder for json_decode_string (lenient) and
   json_decode_string_strict. `out` may be NULL to measure only. `strict`
   rejects unpaired surrogate escapes and a decoded NUL (U+0000) rather than
   substituting U+FFFD or dropping the byte. Returns one past the closing
   quote, NULL on malformed input or when the text does not fit capacity
   (capacity is ignored when measuring). `length`, when non-NULL, receives the
   decoded byte count. */
static const char *decode_string_impl(const char *json, char *out,
    size_t capacity, bool strict, size_t *length) {
    if (!json || *json != '"') return NULL;
    if (out && capacity == 0) return NULL;
    const char *p = json + 1;
    size_t used = 0;
    for (;;) {
        char c = *p;
        if (!c) return NULL;
        if (c == '"') {
            if (out) out[used] = 0;
            if (length) *length = used;
            return p + 1;
        }
        if (c != '\\') {
            /* Unescaped control characters are not valid JSON. The lenient
               decoder tolerates them for callers that did not validate, but
               the strict contract rejects them. */
            if (strict && (unsigned char)c < 0x20) return NULL;
            if (out) {
                if (used + 1 >= capacity) return NULL;
                out[used] = c;
            }
            used++;
            ++p;
            continue;
        }
        ++p;
        char e = *p++;
        uint32_t cp;
        switch (e) {
        case '"': case '\\': case '/': cp = (unsigned char)e; break;
        case 'b': cp = 0x08; break;
        case 'f': cp = 0x0c; break;
        case 'n': cp = 0x0a; break;
        case 'r': cp = 0x0d; break;
        case 't': cp = 0x09; break;
        case 'u': {
            int value = hex4(p);
            if (value < 0) return NULL;
            p += 4;
            cp = (uint32_t)value;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (p[0] == '\\' && p[1] == 'u') {
                    int low = hex4(p + 2);
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) +
                            (uint32_t)(low - 0xdc00);
                        p += 6;
                    } else if (strict) return NULL;  /* unpaired low escape */
                    else cp = 0xfffd;
                } else if (strict) return NULL;      /* unpaired high surrogate */
                else cp = 0xfffd;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                if (strict) return NULL;
                cp = 0xfffd;
            }
            if (!cp) {
                if (strict) return NULL;
                continue;  /* keep NUL bytes out of the C string */
            }
            break;
        }
        default: return NULL;
        }
        char scratch[4];
        size_t n = encode_utf8(scratch, cp);
        if (out) {
            if (used + n >= capacity) return NULL;
            memcpy(out + used, scratch, n);
        }
        used += n;
    }
}

const char *json_decode_string(const char *json, char *out, size_t capacity) {
    if (!out) return NULL;
    return decode_string_impl(json, out, capacity, false, NULL);
}

const char *json_decode_string_strict(const char *json, char *out,
    size_t capacity, size_t *length) {
    return decode_string_impl(json, out, capacity, true, length);
}

/* --- Structure skipping -------------------------------------------------- */

static const char *skip_string(const char *p) {
    ++p;
    while (*p) {
        if ((unsigned char)*p < 0x20) return NULL;
        if (*p == '"') return p + 1;
        if (*p == '\\') {
            ++p;
            if (!*p) return NULL;
            if (*p == 'u') {
                if (hex4(p + 1) < 0) return NULL;
                p += 5;
                continue;
            }
            if (!strchr("\"\\/bfnrt", *p)) return NULL;
        }
        ++p;
    }
    return NULL;
}

static const char *skip_number(const char *p) {
    if (*p == '-') ++p;
    if (*p == '0') ++p;
    else {
        if (*p < '1' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') ++p;
    }
    return p;
}

static const char *skip_value(const char *p, int depth);

static const char *skip_container(const char *p, int depth) {
    char open = *p, close = open == '{' ? '}' : ']';
    ++p;
    p = skip_ws(p);
    if (*p == close) return p + 1;
    for (;;) {
        if (open == '{') {
            if (*p != '"') return NULL;
            p = skip_string(p);
            if (!p) return NULL;
            p = skip_ws(p);
            if (*p != ':') return NULL;
            p = skip_ws(p + 1);
        }
        p = skip_value(p, depth + 1);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') { ++p; p = skip_ws(p); continue; }
        if (*p == close) return p + 1;
        return NULL;
    }
}

static const char *skip_value(const char *p, int depth) {
    if (depth > 64) return NULL;
    p = skip_ws(p);
    switch (*p) {
    case '"': return skip_string(p);
    case '{': case '[': return skip_container(p, depth);
    case 't': return strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
    case 'f': return strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
    case 'n': return strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
    default:
        return skip_number(p);
    }
}

/* --- Path navigation ----------------------------------------------------- */

/* Moves p to the value a path segment names, or NULL when it is not there. */
static const char *navigate(const char *p, const char *name, int index) {
    p = skip_ws(p);
    if (index >= 0) {
        if (*p != '[') return NULL;
        ++p;
        int seen = 0;
        for (;;) {
            p = skip_ws(p);
            if (*p == ']') return NULL;
            if (seen == index) return p;
            p = skip_value(p, 0);
            if (!p) return NULL;
            ++seen;
            p = skip_ws(p);
            if (*p != ',') return NULL;
            ++p;
        }
    }
    if (*p != '{') return NULL;
    ++p;
    for (;;) {
        p = skip_ws(p);
        if (*p != '"') return NULL;
        char key[256];
        const char *after = json_decode_string(p, key, sizeof key);
        if (!after) return NULL;
        p = skip_ws(after);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if (key[0] && strcmp(key, name) == 0) return p;
        p = skip_value(p, 0);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ',') return NULL;
        ++p;
    }
}

/* Parses one path segment: either a bare name or a [n] index. Advances past a
   separating dot; trailing dots and empty names are rejected. */
static bool next_segment(const char **cursor, char *name, size_t capacity,
    int *index) {
    const char *p = *cursor;
    *index = -1;
    if (*p == '[') {
        ++p;
        if (*p < '0' || *p > '9') return false;
        long value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (value > 1000000) return false;
            ++p;
        }
        if (*p != ']') return false;
        ++p;
        *index = (int)value;
    } else {
        size_t used = 0;
        while (*p && *p != '.' && *p != '[') {
            if (used + 1 >= capacity) return false;
            name[used++] = *p++;
        }
        if (!used) return false;
        name[used] = 0;
    }
    if (*p == '.') {
        if (!p[1]) return false;
        ++p;
    }
    *cursor = p;
    return true;
}

/* Resolves a dot/index path from a document to the exact value position, or
   false when the path names nothing. Shared by every query. */
static bool find_path(const char *json, const char *path, const char **out) {
    if (!json || !path) return false;
    const char *p = skip_ws(json);
    if (*p != '{' && *p != '[') return false;
    const char *cursor = path;
    while (*cursor) {
        char name[128];
        int index;
        if (!next_segment(&cursor, name, sizeof name, &index)) return false;
        p = navigate(p, name, index);
        if (!p) return false;
        p = skip_ws(p);
    }
    *out = p;
    return true;
}

bool json_query_string(const char *json, const char *path, char *out,
    size_t capacity) {
    if (!json || !path || !out || capacity == 0) return false;
    const char *p;
    if (!find_path(json, path, &p)) return false;
    return *p == '"' && json_decode_string(p, out, capacity) != NULL;
}

bool json_validate(const char *json) {
    if (!json) return false;
    const char *end = skip_value(json, 0);
    return end && !*skip_ws(end);
}

bool json_query_number(const char *json, const char *path, double *out) {
    if (!json || !path || !out) return false;
    const char *p;
    if (!find_path(json, path, &p)) return false;
    const char *end = skip_number(p);
    if (!end || (*end && !strchr(",}] \t\r\n", *end))) return false;
    char *parsed;
    errno = 0;
    double value = strtod(p, &parsed);
    if (errno || parsed != end || !isfinite(value)) return false;
    *out = value;
    return true;
}

bool json_query_array_length(const char *json, const char *path, size_t *out) {
    if (!json || !path || !out) return false;
    const char *p;
    if (!find_path(json, path, &p)) return false;
    if (*p != '[') return false;
    p = skip_ws(p + 1);
    size_t count = 0;
    if (*p == ']') { *out = 0; return true; }
    for (;;) {
        p = skip_value(p, 0);
        if (!p) return false;
        ++count;
        p = skip_ws(p);
        if (*p == ']') { *out = count; return true; }
        if (*p != ',') return false;
        p = skip_ws(p + 1);
    }
}

/* Validates a path's syntax without touching the document: every segment
   must parse cleanly through the end of the path. Trailing dots, empty
   segments and malformed indexes make a path malformed, which is distinct
   from a valid path that names nothing. */
static bool path_valid(const char *path) {
    const char *cursor = path;
    while (*cursor) {
        char name[128];
        int index;
        if (!next_segment(&cursor, name, sizeof name, &index)) return false;
    }
    return true;
}

bool json_query_field(const char *json, const char *path, JsonFieldKind *kind,
    double *value) {
    /* Initialize whenever possible: even a rejected call reports ABSENT. */
    if (kind) *kind = JSON_FIELD_ABSENT;
    if (!kind || !json || !path || !*path || !path_valid(path)) return false;
    const char *p;
    /* The path is syntactically valid, so a navigation miss means the field
       is genuinely absent, never that the path was malformed. */
    if (!find_path(json, path, &p)) return true;
    p = skip_ws(p);
    /* In a validated document a value can only start one of these ways; a
       number never starts with a letter or punctuation like these. */
    switch (*p) {
    case '"': case '{': case '[':
    case 't': case 'f': case 'n':
        *kind = JSON_FIELD_INVALID;
        return true;
    default: break;
    }
    const char *end = skip_number(p);
    if (!end || (*end && !strchr(",}] \t\r\n", *end))) {
        *kind = JSON_FIELD_INVALID;
        return true;
    }
    char *parsed;
    errno = 0;
    double number = strtod(p, &parsed);
    if (errno || parsed != end || !isfinite(number)) {
        *kind = JSON_FIELD_INVALID;
        return true;
    }
    *kind = JSON_FIELD_NUMBER;
    if (value) *value = number;
    return true;
}

/* --- Value spans, strict decoding and container cursors ------------------- */

bool json_utf8_valid(const char *utf8, size_t length) {
    if (!utf8 && length) return false;
    size_t i = 0;
    while (i < length) {
        unsigned char b = (unsigned char)utf8[i];
        if (b < 0x80) { ++i; continue; }
        unsigned need;
        uint32_t cp;
        if ((b & 0xe0) == 0xc0) { need = 1; cp = b & 0x1fu; }
        else if ((b & 0xf0) == 0xe0) { need = 2; cp = b & 0x0fu; }
        else if ((b & 0xf8) == 0xf0) { need = 3; cp = b & 0x07u; }
        else return false;
        if (i + need >= length) return false;
        for (unsigned k = 1; k <= need; k++) {
            unsigned char c = (unsigned char)utf8[i + k];
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3fu);
        }
        if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += (size_t)need + 1;
    }
    return true;
}

/* True when `end` (one past a value) is followed only by whitespace and then
   either end-of-input or a structural delimiter. Whitespace is skipped so
   trailing junk after a space is still rejected. */
static bool value_terminated(const char *end) {
    if (!end) return false;
    const char *p = skip_ws(end);
    return *p == 0 || *p == ',' || *p == '}' || *p == ']';
}

JsonValueKind json_value_kind(const char *value) {
    if (!value) return JSON_VALUE_INVALID;
    const char *p = skip_ws(value);
    switch (*p) {
    case '"':
        return value_terminated(skip_value(p, 0))
            ? JSON_VALUE_STRING : JSON_VALUE_INVALID;
    case '{':
        return value_terminated(skip_value(p, 0))
            ? JSON_VALUE_OBJECT : JSON_VALUE_INVALID;
    case '[':
        return value_terminated(skip_value(p, 0))
            ? JSON_VALUE_ARRAY : JSON_VALUE_INVALID;
    case 't':
        return strncmp(p, "true", 4) == 0 && value_terminated(p + 4)
            ? JSON_VALUE_BOOL : JSON_VALUE_INVALID;
    case 'f':
        return strncmp(p, "false", 5) == 0 && value_terminated(p + 5)
            ? JSON_VALUE_BOOL : JSON_VALUE_INVALID;
    case 'n':
        return strncmp(p, "null", 4) == 0 && value_terminated(p + 4)
            ? JSON_VALUE_NULL : JSON_VALUE_INVALID;
    default:
        return value_terminated(skip_number(p))
            ? JSON_VALUE_NUMBER : JSON_VALUE_INVALID;
    }
}

const char *json_value_end(const char *value) {
    return value ? skip_value(value, 0) : NULL;
}

bool json_value_number(const char *value, double *out) {
    if (!value || !out) return false;
    const char *p = skip_ws(value);
    const char *end = skip_number(p);
    if (!value_terminated(end)) return false;
    char *parsed;
    errno = 0;
    double number = strtod(p, &parsed);
    if (errno || parsed != end || !isfinite(number)) return false;
    *out = number;
    return true;
}

bool json_value_bool(const char *value, bool *out) {
    if (!value || !out) return false;
    const char *p = skip_ws(value);
    if (strncmp(p, "true", 4) == 0 && value_terminated(p + 4)) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0 && value_terminated(p + 5)) {
        *out = false;
        return true;
    }
    return false;
}

bool json_cursor_object(JsonCursor *cursor, const char *value) {
    if (!cursor || !value) return false;
    const char *p = skip_ws(value);
    if (*p != '{') return false;
    cursor->cursor = p + 1;
    cursor->close = '}';
    cursor->first = true;
    cursor->key = NULL;
    cursor->value = NULL;
    return true;
}

bool json_cursor_array(JsonCursor *cursor, const char *value) {
    if (!cursor || !value) return false;
    const char *p = skip_ws(value);
    if (*p != '[') return false;
    cursor->cursor = p + 1;
    cursor->close = ']';
    cursor->first = true;
    cursor->key = NULL;
    cursor->value = NULL;
    return true;
}

bool json_cursor_next(JsonCursor *cursor) {
    if (!cursor) return false;
    bool object = cursor->close == '}';
    const char *p = skip_ws(cursor->cursor);
    if (cursor->first) {
        if (*p == cursor->close) { cursor->cursor = p; return false; }
        cursor->first = false;
    } else if (*p == ',') {
        p = skip_ws(p + 1);
    } else if (*p == cursor->close) {
        cursor->cursor = p;
        return false;
    } else {
        return false;
    }
    if (object) {
        if (*p != '"') return false;
        const char *after = skip_string(p);
        if (!after) return false;
        cursor->key = p;
        p = skip_ws(after);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
    } else {
        cursor->key = NULL;
    }
    cursor->value = p;
    const char *end = skip_value(p, 0);
    if (!end) return false;
    cursor->cursor = end;
    return true;
}

const char *json_cursor_key(const JsonCursor *cursor) {
    return cursor ? cursor->key : NULL;
}

const char *json_cursor_value(const JsonCursor *cursor) {
    return cursor ? cursor->value : NULL;
}
