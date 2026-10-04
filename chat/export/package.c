#include "chat/export/package.h"

#include <string.h>

/* ---- MIME to extension ---- */

static bool lower_equals(const char *a, const char *b) {
    for (size_t i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}

bool chat_export_sidecar_extension(const char *mime, char out[8]) {
    static const struct { const char *mime; const char *ext; } table[] = {
        { "image/png", "png" },
        { "image/jpeg", "jpg" },
        { "image/webp", "webp" },
        { "image/gif", "gif" },
    };
    if (!mime || !out) return false;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (lower_equals(mime, table[i].mime)) {
            memcpy(out, table[i].ext, strlen(table[i].ext) + 1);
            return true;
        }
    return false;
}

/* ---- filename legality ---- */

/* Characters Windows refuses in a filename. ':' is excluded here because it
   is legal in a directory component (the drive) and this predicate is only
   ever applied to a basename or a sidecar reference component, where it is
   not. */
static bool forbidden_in_name(wchar_t c) {
    return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
           c == L'|' || c == L'?' || c == L'*';
}

static wchar_t lower_unit(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

bool chat_filename_is_reserved_device(const wchar_t *name, size_t length) {
    static const wchar_t *const devices[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
        L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
        L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };
    size_t stem = length;
    if (!name) return false;
    /* Windows refuses a reserved name however it is extended, so the check
       stops at the first dot rather than at the end of the name. */
    for (size_t i = 0; i < length; i++)
        if (name[i] == L'.') { stem = i; break; }
    if (!stem) return false;
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        const wchar_t *device = devices[i];
        size_t j = 0;
        for (; j < stem && device[j]; j++)
            if (lower_unit(name[j]) != lower_unit(device[j])) break;
        if (j == stem && !device[j]) return true;
    }
    return false;
}

static bool ends_with_ci(const wchar_t *name, size_t length,
    const wchar_t *suffix) {
    size_t n = wcslen(suffix);
    if (length < n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower_unit(name[length - n + i]) != suffix[i]) return false;
    return true;
}

/* UTF-8 length of a UTF-16 run, saturating. The stem bound is expressed in
   bytes because that is what the importer measures the folder component in;
   measuring units here instead would let a non-ASCII stem through this side
   and be rejected on the way back in. A lone surrogate is counted as one
   replacement byte rather than trusted to encode. */
static size_t utf8_length(const wchar_t *text, size_t length) {
    size_t bytes = 0;
    for (size_t i = 0; i < length; i++) {
        wchar_t c = text[i];
        if (c < 0x80) bytes += 1;
        else if (c < 0x800) bytes += 2;
        else if (c >= 0xd800 && c <= 0xdfff) bytes += 3;   /* counted as one */
        else bytes += 3;
        if (bytes > (size_t)CHAT_EXPORT_STEM_MAX) return bytes;  /* early out */
    }
    return bytes;
}

bool chat_export_sidecar_stem_from_path(const wchar_t *path,
    wchar_t *out, size_t capacity) {
    const wchar_t *base = path;
    wchar_t trimmed[CHAT_EXPORT_SIDECAR_FOLDER_MAX + 8];
    size_t length;

    if (!path || !out || capacity == 0) return false;
    out[0] = 0;

    /* Basename only: the directory part legitimately contains a drive colon
       and separators, and judging those against the filename rules would
       reject every absolute path. */
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;

    length = wcslen(base);
    if (!length) return false;
    if (ends_with_ci(base, length, L".markdown")) length -= 9;
    else if (ends_with_ci(base, length, L".md")) length -= 3;
    if (!length || length + 1 > sizeof trimmed / sizeof trimmed[0]) return false;
    for (size_t i = 0; i < length; i++) {
        if (forbidden_in_name(base[i])) return false;
        trimmed[i] = base[i];
    }
    trimmed[length] = 0;
    /* "." and ".." are legal directory entries but meaningless as a package
       stem, and either would name a folder the filesystem cannot create. */
    if (!wcscmp(trimmed, L".") || !wcscmp(trimmed, L"..")) return false;
    if (chat_filename_is_reserved_device(trimmed, length)) return false;
    if (utf8_length(trimmed, length) > (size_t)CHAT_EXPORT_STEM_MAX)
        return false;

    if (length + 1 > capacity) return false;
    memcpy(out, trimmed, (length + 1) * sizeof(wchar_t));
    return true;
}

/* ---- import reference validation ---- */

static bool lower_hex_run(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/* True when span[0..n) is exactly the NUL-terminated literal. A length-bounded
   compare, not strcmp: these spans are interior to the reference string and are
   not terminated at their own end, so a strcmp here would run past the
   component and answer a question about the wrong bytes. */
static bool span_is(const char *span, size_t n, const char *literal) {
    return strlen(literal) == n && !memcmp(span, literal, n);
}

/* Rejects anything that could not be a single component of a path, and
   anything Windows could not name. Applied to both halves of a reference.

   Space is deliberately allowed: the export derives the folder name from the
   file the user chose, so "trip 1.attachments" is a name this exporter itself
   produces and this importer must accept. The traversal guarantee comes from
   the component being separator-free and never equal to a dot segment, not
   from refusing benign characters. */
static bool component_ok(const char *p, size_t n) {
    if (!n || n >= CHAT_EXPORT_SIDECAR_FOLDER_MAX) return false;
    if (span_is(p, n, ".") || span_is(p, n, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' ||
            c == '|' || c == '?' || c == '*')
            return false;
    }
    return true;
}

bool chat_import_sidecar_reference(const char *file, size_t file_length,
    char out_folder[80], char out_basename[80]) {
    static const char *const extensions[] = { "png", "jpg", "webp", "gif" };
    const char *slash;
    size_t folder_len, name_len, ext_len;
    bool known = false;

    if (!file || !out_folder || !out_basename) return false;
    out_folder[0] = 0;
    out_basename[0] = 0;
    if (!file_length) return false;

    /* Exactly one separator. A second one would make the basename a path in
       disguise, which is the shape this whole check exists to exclude. */
    slash = memchr(file, '/', file_length);
    if (!slash || memchr(slash + 1, '/', file_length - (size_t)(slash + 1 - file)))
        return false;

    folder_len = (size_t)(slash - file);
    name_len = file_length - folder_len - 1;
    if (!component_ok(file, folder_len)) return false;
    if (!component_ok(slash + 1, name_len)) return false;

    /* The folder is the package folder and nothing else. */
    {
        static const char suffix[] = CHAT_EXPORT_SIDECAR_SUFFIX;
        size_t n = sizeof suffix - 1;
        if (folder_len <= n) return false;
        if (memcmp(file + folder_len - n, suffix, n) != 0) return false;
    }

    /* The basename is a digest, a dot, and a known extension. */
    if (name_len <= 65 || name_len >= CHAT_EXPORT_SIDECAR_FOLDER_MAX)
        return false;
    if (file[folder_len + 1 + 64] != '.') return false;
    if (!lower_hex_run(slash + 1, 64)) return false;
    ext_len = name_len - 65;
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; i++)
        if (ext_len == strlen(extensions[i]) &&
            !memcmp(slash + 1 + 65, extensions[i], ext_len)) {
            known = true;
            break;
        }
    if (!known) return false;

    memcpy(out_folder, file, folder_len);
    out_folder[folder_len] = 0;
    memcpy(out_basename, slash + 1, name_len);
    out_basename[name_len] = 0;
    return true;
}

/* ---- Markdown escaping ---- */

/* True when `unit` is the low half of a surrogate pair whose high half
   precedes it, so a bounded copy must not stop between them. */
static bool is_low_surrogate(wchar_t c) {
    return c >= 0xdc00 && c <= 0xdfff;
}

static bool emit(wchar_t *out, size_t capacity, size_t *used, wchar_t unit) {
    if (*used + 1 >= capacity) return false;
    out[(*used)++] = unit;
    return true;
}

bool chat_export_markdown_alt(const wchar_t *display_name,
    wchar_t *out, size_t capacity) {
    size_t used = 0;
    if (!out || capacity == 0) return false;
    out[0] = 0;
    if (!display_name) return true;
    for (const wchar_t *p = display_name; *p; p++) {
        wchar_t c = *p;
        if (is_low_surrogate(c) && p > display_name) {
            /* A copy that stopped here would leave half a character behind. */
            if (!emit(out, capacity, &used, c)) return false;
            continue;
        }
        if (c == L'\r' || c == L'\n') {
            /* Collapse runs so a name cannot become a line break in the
               rendered output. */
            if (!emit(out, capacity, &used, L' ')) return false;
            while (p[1] == L'\r' || p[1] == L'\n') p++;
            continue;
        }
        if (c == L'\\' || c == L'[' || c == L']')
            if (!emit(out, capacity, &used, L'\\')) return false;
        if (!emit(out, capacity, &used, c)) return false;
    }
    out[used] = 0;
    return true;
}

static bool destination_unit_safe(wchar_t c) {
    if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
        (c >= L'0' && c <= L'9'))
        return true;
    return c == L'-' || c == L'.' || c == L'_' || c == L'~' || c == L'/';
}

bool chat_export_markdown_destination(const wchar_t *path,
    wchar_t *out, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (!out || capacity == 0) return false;
    out[0] = 0;
    if (!path) return true;
    for (const wchar_t *p = path; *p; p++) {
        wchar_t c = *p;
        if (destination_unit_safe(c)) {
            if (!emit(out, capacity, &used, c)) return false;
            continue;
        }
        /* Percent-encode as UTF-8 bytes, so a non-ASCII name produces a link
           a Markdown reader resolves the same way the filesystem resolves the
           file next to the .md. */
        unsigned char utf8[4];
        size_t n = 0;
        if (c < 0x80) {
            utf8[n++] = (unsigned char)c;
        } else if (c < 0x800) {
            utf8[n++] = (unsigned char)(0xc0u | (c >> 6));
            utf8[n++] = (unsigned char)(0x80u | (c & 0x3fu));
        } else if (c >= 0xd800 && c <= 0xdbff && p[1] >= 0xdc00 &&
                   p[1] <= 0xdfff) {
            unsigned long cp = 0x10000ul +
                ((unsigned long)(c - 0xd800) << 10) + (p[1] - 0xdc00);
            utf8[n++] = (unsigned char)(0xf0u | (cp >> 18));
            utf8[n++] = (unsigned char)(0x80u | ((cp >> 12) & 0x3fu));
            utf8[n++] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
            utf8[n++] = (unsigned char)(0x80u | (cp & 0x3fu));
            p++;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return false;   /* a lone low surrogate names no file */
        } else {
            utf8[n++] = (unsigned char)(0xe0u | ((c >> 12) & 0x0fu));
            utf8[n++] = (unsigned char)(0x80u | ((c >> 6) & 0x3fu));
            utf8[n++] = (unsigned char)(0x80u | (c & 0x3fu));
        }
        for (size_t i = 0; i < n; i++) {
            if (!emit(out, capacity, &used, L'%')) return false;
            if (!emit(out, capacity, &used, (wchar_t)hex[utf8[i] >> 4]))
                return false;
            if (!emit(out, capacity, &used, (wchar_t)hex[utf8[i] & 15]))
                return false;
        }
    }
    out[used] = 0;
    return true;
}