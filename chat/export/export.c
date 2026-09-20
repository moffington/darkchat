#include "chat/export/export.h"

#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Bounded writer. Every append is preflighted against `limit` before it can
   allocate, so an over-limit export fails at the crossing point rather than
   after the payload is already resident. `failed` is sticky and mirrors the
   buffer's own sticky allocation-failure flag (`JsonBuf.oom`). */
typedef struct {
    JsonBuf *out;
    size_t limit;
    bool failed;
} Export;

static bool export_fail(Export *e) {
    e->failed = true;
    e->out->oom = true;
    return false;
}

static bool export_room(const Export *e, size_t extra) {
    /* Short-circuit keeps the subtraction in range. */
    return extra <= e->limit && e->out->length <= e->limit - extra;
}

static bool export_raw(Export *e, const char *data, size_t length) {
    if (e->failed) return false;
    if (!length) return true;
    if (!export_room(e, length)) return export_fail(e);
    if (!json_buf_append_raw(e->out, data, length)) return export_fail(e);
    return true;
}

static bool export_cstr(Export *e, const char *text) {
    return export_raw(e, text, strlen(text));
}

static bool export_json_string(Export *e, const wchar_t *text) {
    if (e->failed) return false;
    size_t size = json_encoded_string_size(text);
    if (!export_room(e, size)) return export_fail(e);
    if (!json_buf_append_json_string(e->out, text)) return export_fail(e);
    return true;
}

static bool export_pad(Export *e, int indent) {
    for (int i = 0; i < indent; i++)
        if (!export_raw(e, " ", 1)) return false;
    return true;
}

static bool export_key(Export *e, int indent, const char *name) {
    if (!export_pad(e, indent)) return false;
    if (!export_raw(e, "\"", 1)) return false;
    if (!export_raw(e, name, strlen(name))) return false;
    return export_raw(e, "\": ", 3);
}

static bool export_i64(Export *e, int indent, const char *name, int64_t value) {
    if (!export_key(e, indent, name)) return false;
    char scratch[32];
    int written = snprintf(scratch, sizeof scratch, "%" PRId64, value);
    if (written < 0 || (size_t)written >= sizeof scratch) return export_fail(e);
    return export_raw(e, scratch, (size_t)written);
}

static bool export_u64(Export *e, int indent, const char *name, uint64_t value) {
    if (!export_key(e, indent, name)) return false;
    char scratch[32];
    int written = snprintf(scratch, sizeof scratch, "%" PRIu64, value);
    if (written < 0 || (size_t)written >= sizeof scratch) return export_fail(e);
    return export_raw(e, scratch, (size_t)written);
}

/* Rewrites the active locale's decimal separator to '.', then rejects any
   remaining non-JSON-number byte. %g observes LC_NUMERIC, so without this a
   caller's setlocale (for example a comma separator) would emit invalid JSON
   and break deterministic output. `localeconv` is read-only and handles a
   multi-byte separator, so no temporary locale or global state is touched. The
   formatted string only shrinks, so the caller's buffer stays valid. */
static bool normalize_number(char *text) {
    const struct lconv *lc = localeconv();
    const char *point = lc && lc->decimal_point && lc->decimal_point[0]
        ? lc->decimal_point : ".";
    if (!(point[0] == '.' && point[1] == 0)) {
        char *found = strstr(text, point);
        if (found) {
            size_t length = strlen(point);
            size_t tail = strlen(found + length);
            *found = '.';
            memmove(found + 1, found + length, tail + 1);
        }
    }
    for (const char *p = text; *p; p++)
        if (!strchr("-+0123456789.eE", *p)) return false;
    return true;
}

static bool export_double(Export *e, int indent, const char *name,
    double value) {
    /* %.17g of NaN/Infinity is not valid JSON, so a non-finite metric is
       treated as corruption of the live state and fails the export. */
    if (!isfinite(value)) return export_fail(e);
    char scratch[64];
    int written = snprintf(scratch, sizeof scratch, "%.17g", value);
    if (written < 0 || (size_t)written >= sizeof scratch) return export_fail(e);
    if (!normalize_number(scratch)) return export_fail(e);
    if (!export_key(e, indent, name)) return false;
    return export_raw(e, scratch, strlen(scratch));
}

static bool export_string(Export *e, int indent, const char *name,
    const wchar_t *value) {
    if (!export_key(e, indent, name)) return false;
    return export_json_string(e, value);
}

static bool export_token(Export *e, int indent, const char *name,
    const char *token) {
    if (!token) return export_fail(e);
    if (!export_key(e, indent, name)) return false;
    if (!export_raw(e, "\"", 1)) return false;
    if (!export_raw(e, token, strlen(token))) return false;
    return export_raw(e, "\"", 1);
}

static bool export_bool_true(Export *e, int indent, const char *name) {
    if (!export_key(e, indent, name)) return false;
    return export_raw(e, "true", 4);
}

/* Closes a field or element: `,\n` between members, `\n` after the last. */
static bool export_sep(Export *e, bool last) {
    return export_raw(e, last ? "\n" : ",\n", last ? 1 : 2);
}

/* Checked enum stringification: an unknown value fails rather than emitting a
   silently wrong token. The switches cover every enumerator, so a future role
   or backend is a compile-time prompt to extend them. */
static const char *role_token(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_USER: return "user";
    case CHAT_ROLE_ASSISTANT: return "assistant";
    case CHAT_ROLE_SYSTEM: return "system";
    case CHAT_ROLE_ERROR: return "error";
    }
    return NULL;
}

static const char *role_heading(ChatRole role) {
    switch (role) {
    case CHAT_ROLE_USER: return "User";
    case CHAT_ROLE_ASSISTANT: return "Assistant";
    case CHAT_ROLE_SYSTEM: return "System";
    case CHAT_ROLE_ERROR: return "Error";
    }
    return NULL;
}

static const char *backend_token(ChatBackend backend) {
    switch (backend) {
    case CHAT_BACKEND_OPENROUTER: return "openrouter";
    case CHAT_BACKEND_OLLAMA: return "ollama";
    }
    return NULL;
}

static bool export_message(Export *e, const ChatMessage *m, int indent,
    bool last) {
    const char *role = role_token(m->role);
    const char *backend = backend_token(m->generation.backend);
    if (!role || !backend) return export_fail(e);
    int inner = indent + 2;
    const ChatGeneration *g = &m->generation;
    if (!export_pad(e, indent)) return false;
    if (!export_raw(e, "{\n", 2)) return false;
    if (!export_token(e, inner, "role", role)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, inner, "created_at", m->created_at)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, inner, "modified_at", m->modified_at)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_string(e, inner, "text", chat_message_text(m))) return false;
    if (!export_sep(e, false)) return false;
    if (chat_message_reasoning(m)[0]) {
        if (!export_string(e, inner, "reasoning",
                chat_message_reasoning(m))) return false;
        if (!export_sep(e, false)) return false;
    }
    if (!export_key(e, inner, "generation")) return false;
    if (!export_raw(e, "{\n", 2)) return false;
    {
        int gin = inner + 2;
        if (!export_string(e, gin, "requested_model", g->requested_model))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_string(e, gin, "actual_model", g->actual_model))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_string(e, gin, "finish_reason", g->finish_reason))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_double(e, gin, "prompt_tokens", g->prompt_tokens))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_double(e, gin, "completion_tokens", g->completion_tokens))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_double(e, gin, "total_tokens", g->total_tokens))
            return false;
        if (!export_sep(e, false)) return false;
        if (!export_double(e, gin, "cost", g->cost)) return false;
        if (!export_sep(e, false)) return false;
        if (!export_token(e, gin, "backend", backend)) return false;
        if (!export_sep(e, false)) return false;
        if (!export_double(e, gin, "reasoning_ms", g->reasoning_ms))
            return false;
    }
    if (!export_raw(e, "\n", 1)) return false;
    if (!export_pad(e, inner)) return false;
    if (!export_raw(e, "}\n", 2)) return false;
    if (!export_pad(e, indent)) return false;
    return export_raw(e, last ? "}\n" : "},\n", last ? 2 : 3);
}

static bool export_conversation(Export *e, const ChatConversation *c,
    int indent, bool last) {
    /* The plan's schema carries only domain-valid ids: the persisted counter
       ceiling is the same bound storage enforces, so an out-of-range value is
       corruption, not an opaque pass-through. */
    if (c->id < 1 || c->id > CHAT_MAX_ID) return export_fail(e);
    int inner = indent + 2;
    if (!export_pad(e, indent)) return false;
    if (!export_raw(e, "{\n", 2)) return false;
    if (!export_u64(e, inner, "id", c->id)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_string(e, inner, "title", c->title)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, inner, "created_at", c->created_at)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, inner, "modified_at", c->modified_at)) return false;
    if (!export_sep(e, false)) return false;
    /* Both per-backend overrides, independently and exactly as storage emits
       them, so an empty conversation carrying either or both round-trips. */
    if (c->model[0]) {
        if (!export_string(e, inner, "model", c->model)) return false;
        if (!export_sep(e, false)) return false;
    }
    if (c->ollama_model[0]) {
        if (!export_string(e, inner, "ollama_model", c->ollama_model))
            return false;
        if (!export_sep(e, false)) return false;
    }
    if (c->system_prompt.data) {
        if (!export_string(e, inner, "system_prompt",
                chat_text_value(&c->system_prompt))) return false;
        if (!export_sep(e, false)) return false;
    }
    if (c->system_prompt_present) {
        if (!export_bool_true(e, inner, "system_prompt_present")) return false;
        if (!export_sep(e, false)) return false;
    }
    if (!export_key(e, inner, "messages")) return false;
    if (!c->message_count) {
        if (!export_raw(e, "[]\n", 3)) return false;
    } else {
        if (!export_raw(e, "[\n", 2)) return false;
        for (size_t j = 0; j < c->message_count; j++)
            if (!export_message(e, &c->messages[j], inner + 2,
                    j + 1 == c->message_count)) return false;
        if (!export_pad(e, inner)) return false;
        if (!export_raw(e, "]\n", 2)) return false;
    }
    if (!export_pad(e, indent)) return false;
    return export_raw(e, last ? "}\n" : "},\n", last ? 2 : 3);
}

/* The one authoritative payload shared by both formats. */
static bool export_build_payload(Export *e, const Chat *chat, int conversation,
    bool all, int64_t exported_at) {
    if (!chat) return export_fail(e);
    int first = 0, count = 0;
    if (all) {
        first = 0;
        count = chat->conversation_count;
    } else {
        if (conversation < 0 || conversation >= chat->conversation_count)
            return export_fail(e);
        first = conversation;
        count = 1;
    }
    if (!export_raw(e, "{\n", 2)) return false;
    if (!export_string(e, 2, "format", L"darkchat.export")) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, 2, "version", 1)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_i64(e, 2, "exported_at", exported_at)) return false;
    if (!export_sep(e, false)) return false;
    if (!export_key(e, 2, "conversations")) return false;
    if (!count) {
        if (!export_raw(e, "[]\n", 3)) return false;
    } else {
        if (!export_raw(e, "[\n", 2)) return false;
        for (int i = 0; i < count; i++)
            if (!export_conversation(e, &chat->conversations[first + i], 4,
                    i + 1 == count)) return false;
        if (!export_pad(e, 2)) return false;
        if (!export_raw(e, "]\n", 2)) return false;
    }
    return export_raw(e, "}\n", 2);
}

/* --- UTF-8 presentation helpers ----------------------------------------- */

static size_t utf8_encode(char *out, uint32_t cp) {
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

/* Next normalized code point: CRLF and CR collapse to LF; a surrogate pair
   combines; a lone surrogate becomes U+FFFD. Mirrors the JSON encoder's
   text rules so the presentation and the payload agree byte for byte. */
static uint32_t next_codepoint(const wchar_t **p) {
    wchar_t c = *(*p)++;
    if (!c) return 0;
    if (c == L'\r') {
        if (**p == L'\n') ++*p;
        return '\n';
    }
    if (c >= 0xd800 && c <= 0xdbff && **p >= 0xdc00 && **p <= 0xdfff) {
        uint32_t cp = 0x10000 + ((uint32_t)(c - 0xd800) << 10) +
            (uint32_t)(**p - 0xdc00);
        ++*p;
        return cp;
    }
    if (c >= 0xd800 && c <= 0xdfff) return 0xfffd;
    return (uint32_t)c;
}

static size_t utf8_measure(const wchar_t *text) {
    const wchar_t *p = text ? text : L"";
    size_t total = 0;
    for (;;) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) break;
        total += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    }
    return total;
}

static bool export_utf8(Export *e, const wchar_t *text) {
    if (e->failed) return false;
    size_t total = utf8_measure(text);
    if (!export_room(e, total)) return export_fail(e);
    const wchar_t *p = text ? text : L"";
    for (;;) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) break;
        char scratch[4];
        size_t n = utf8_encode(scratch, cp);
        if (!json_buf_append_raw(e->out, scratch, n)) return export_fail(e);
    }
    return true;
}

/* Trailing LF run of the normalized text. */
static size_t trailing_newlines(const wchar_t *text) {
    const wchar_t *p = text ? text : L"";
    size_t run = 0;
    for (;;) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) break;
        run = cp == '\n' ? run + 1 : 0;
    }
    return run;
}

/* Preserves the text's own trailing LF run, appending only enough LFs to
   reach the two-LF section boundary. */
static bool ensure_section_break(Export *e, size_t trailing) {
    for (size_t i = trailing; i < 2; i++)
        if (!export_raw(e, "\n", 1)) return false;
    return true;
}

/* Explicit, locale-independent whitespace: never iswspace(). CR/LF flatten to
   spaces first, then the ends are trimmed. Returns the trimmed length. */
static bool explicit_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
        c == L'\v' || c == L'\f';
}

static size_t normalize_title(const wchar_t *title, wchar_t *out,
    size_t capacity) {
    size_t n = 0;
    for (const wchar_t *p = title; *p && n + 1 < capacity; p++) {
        wchar_t c = *p;
        /* A CRLF pair is one line break, so consume the LF and emit a single
           space; lone CR and lone LF each flatten to one space too. Working on
           raw UTF-16 units keeps surrogate pairs intact in the heading. */
        if (c == L'\r') {
            if (p[1] == L'\n') p++;
            c = L' ';
        } else if (c == L'\n') {
            c = L' ';
        }
        out[n++] = c;
    }
    out[n] = 0;
    size_t start = 0;
    while (start < n && explicit_space(out[start])) start++;
    size_t end = n;
    while (end > start && explicit_space(out[end - 1])) end--;
    size_t length = end - start;
    memmove(out, out + start, length * sizeof(wchar_t));
    out[length] = 0;
    return length;
}

static bool export_reasoning_block(Export *e, const wchar_t *reasoning) {
    const wchar_t *p = reasoning ? reasoning : L"";
    bool line_start = true;
    if (!export_raw(e, ">", 1)) return false;
    for (;;) {
        uint32_t cp = next_codepoint(&p);
        if (!cp) break;
        if (cp == '\n') {
            if (!export_raw(e, "\n>", 2)) return false;
            line_start = true;
            continue;
        }
        if (line_start) {
            if (!export_raw(e, " ", 1)) return false;
            line_start = false;
        }
        char scratch[4];
        size_t n = utf8_encode(scratch, cp);
        if (!export_raw(e, scratch, n)) return false;
    }
    return export_raw(e, "\n", 1);
}

/* Presentation only: the exact title/text/reasoning stay in the payload. */
static bool export_markdown_body(Export *e, const Chat *chat, int first,
    int count) {
    for (int i = 0; i < count; i++) {
        const ChatConversation *c = &chat->conversations[first + i];
        wchar_t title[CHAT_TITLE_TEXT];
        size_t title_length = normalize_title(c->title, title, CHAT_TITLE_TEXT);
        if (title_length) {
            if (!export_raw(e, "# ", 2)) return false;
            if (!export_utf8(e, title)) return false;
            if (!export_raw(e, "\n", 1)) return false;
            if (!ensure_section_break(e, 1)) return false;
        }
        for (size_t j = 0; j < c->message_count; j++) {
            const ChatMessage *m = &c->messages[j];
            const char *heading = role_heading(m->role);
            if (!heading) return export_fail(e);
            if (!export_raw(e, "## ", 3)) return false;
            if (!export_cstr(e, heading)) return false;
            if (!export_raw(e, "\n\n", 2)) return false;
            const wchar_t *text = chat_message_text(m);
            if (text[0]) {
                if (!export_utf8(e, text)) return false;
                if (!ensure_section_break(e, trailing_newlines(text)))
                    return false;
            }
            const wchar_t *reasoning = chat_message_reasoning(m);
            if (reasoning[0]) {
                if (!export_reasoning_block(e, reasoning)) return false;
                if (!ensure_section_break(e, 1)) return false;
            }
        }
    }
    if (e->out->length && e->out->data[e->out->length - 1] != '\n')
        if (!export_raw(e, "\n", 1)) return false;
    return true;
}

bool chat_export_json_limited(const Chat *chat, int conversation, bool all,
    int64_t exported_at, size_t limit, JsonBuf *out) {
    if (!out) return false;
    *out = (JsonBuf){0};
    Export e = { out, limit, false };
    if (!export_build_payload(&e, chat, conversation, all, exported_at)) {
        json_buf_free(out);
        *out = (JsonBuf){0};
        return false;
    }
    return true;
}

bool chat_export_json(const Chat *chat, int conversation, bool all,
    int64_t exported_at, JsonBuf *out) {
    return chat_export_json_limited(chat, conversation, all, exported_at,
        CHAT_EXPORT_LIMIT, out);
}

bool chat_export_markdown_limited(const Chat *chat, int conversation, bool all,
    int64_t exported_at, size_t limit, JsonBuf *out) {
    if (!out) return false;
    *out = (JsonBuf){0};
    JsonBuf payload = {0};
    Export pe = { &payload, limit, false };
    if (!export_build_payload(&pe, chat, conversation, all, exported_at)) {
        json_buf_free(&payload);
        return false;
    }
    /* The range was validated while building the payload; chat is non-NULL. */
    int first = 0, count = 0;
    if (all) {
        first = 0;
        count = chat->conversation_count;
    } else {
        first = conversation;
        count = 1;
    }
    Export e = { out, limit, false };
    bool ok = export_cstr(&e, "<!-- darkchat.export:\n");
    /* Escape `<`/`>` in the payload. These characters can only occur inside
       JSON strings, where \uXXXX is legal, and escaping them guarantees the
       first `-->` after the opener is the real terminator. */
    for (size_t i = 0; ok && i < payload.length; ) {
        size_t j = i;
        while (j < payload.length && payload.data[j] != '<' &&
                payload.data[j] != '>') j++;
        ok = export_raw(&e, payload.data + i, j - i);
        if (ok && j < payload.length) {
            const char *escape = payload.data[j] == '<' ? "\\u003c" : "\\u003e";
            ok = export_raw(&e, escape, 6);
            j++;
        }
        i = j;
    }
    if (ok) ok = export_cstr(&e, "-->\n");
    if (ok && count)
        ok = export_markdown_body(&e, chat, first, count);
    json_buf_free(&payload);
    if (!ok || e.failed) {
        json_buf_free(out);
        *out = (JsonBuf){0};
        return false;
    }
    return true;
}

bool chat_export_markdown(const Chat *chat, int conversation, bool all,
    int64_t exported_at, JsonBuf *out) {
    return chat_export_markdown_limited(chat, conversation, all, exported_at,
        CHAT_EXPORT_LIMIT, out);
}
