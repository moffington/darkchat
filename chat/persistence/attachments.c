#include "chat/persistence/attachments.h"
#include "chat/core/chat.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- SHA-256 (FIPS 180-4), pure; no Win32, no third-party code ---- */

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    unsigned char block[64];
    size_t block_fill;
} Sha256;

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(Sha256 *h) {
    static const uint32_t init[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(h->state, init, sizeof init);
    h->total_bits = 0;
    h->block_fill = 0;
}

static void sha256_compress(Sha256 *h, const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h->state[0]; b = h->state[1]; c = h->state[2]; d = h->state[3];
    e = h->state[4]; f = h->state[5]; g = h->state[6]; hh = h->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h->state[0] += a; h->state[1] += b; h->state[2] += c; h->state[3] += d;
    h->state[4] += e; h->state[5] += f; h->state[6] += g; h->state[7] += hh;
}

static void sha256_update(Sha256 *h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    h->total_bits += (uint64_t)n * 8u;
    while (n) {
        size_t take = 64 - h->block_fill;
        if (take > n) take = n;
        memcpy(h->block + h->block_fill, p, take);
        h->block_fill += take;
        p += take;
        n -= take;
        if (h->block_fill == 64) {
            sha256_compress(h, h->block);
            h->block_fill = 0;
        }
    }
}

static void sha256_final(Sha256 *h, unsigned char out[32]) {
    uint64_t bits = h->total_bits;
    unsigned char pad = 0x80, zero = 0;
    int i;
    sha256_update(h, &pad, 1);
    h->total_bits = bits; /* length field counts only the message */
    while (h->block_fill != 56) {
        sha256_update(h, &zero, 1);
        h->total_bits = bits;
    }
    {
        unsigned char len[8];
        for (i = 0; i < 8; i++)
            len[7 - i] = (unsigned char)(bits >> (8 * (unsigned)i));
        sha256_update(h, len, 8);
    }
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(h->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h->state[i];
    }
}

void attachment_digest_hex(const void *data, size_t n, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    Sha256 h;
    unsigned char raw[32];
    int i;
    sha256_init(&h);
    if (n) sha256_update(&h, data, n);
    sha256_final(&h, raw);
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[64] = 0;
}

/* ---- naming, validation, path building ---- */

static bool digest_valid(const char *d) {
    int i;
    if (!d) return false;
    for (i = 0; i < 64; i++) {
        char c = d[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return d[64] == 0;
}

/* Top-level managed blob only: exactly 64 lowercase hex, no extension. */
static bool managed_filename(const wchar_t *name, char digest[65]) {
    int i;
    for (i = 0; i < 64; i++) {
        wchar_t c = name[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
        digest[i] = (char)c;
    }
    digest[64] = 0;
    return name[64] == 0;
}

static bool append_narrow(wchar_t *out, size_t cap, size_t *pos, const char *s) {
    size_t i = *pos;
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) return false; /* paths are ASCII only */
        if (i + 1 >= cap) return false;
        out[i++] = (wchar_t)(unsigned char)*s;
    }
    out[i] = 0;
    *pos = i;
    return true;
}

/* dir\digest — the sole managed name has no extension. */
static bool path_join_digest(wchar_t *out, size_t cap, const wchar_t *dir,
    const char *digest) {
    size_t pos = 0;
    out[0] = 0;
    for (; *dir; dir++) {
        if (pos + 1 >= cap) return false;
        out[pos++] = *dir;
    }
    if (pos + 1 >= cap) return false;
    out[pos++] = L'\\';
    out[pos] = 0;
    return append_narrow(out, cap, &pos, digest);
}

static bool path_join_name(wchar_t *out, size_t cap, const wchar_t *dir,
    const wchar_t *name) {
    int n = swprintf(out, cap, L"%ls\\%ls", dir, name);
    return n >= 0 && (size_t)n < cap;
}

static bool blob_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- default per-store I/O (Win32) ---- */

static unsigned long temp_seq; /* UI-thread put only; uniqueness also uses pid+tick */

static bool io_mkdir(const wchar_t *path) {
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool io_write_atomic(const wchar_t *path, const void *data, size_t n) {
    wchar_t blob_dir[1024], temp[1100];
    const wchar_t *slash;
    size_t dir_len;
    int attempt;
    if (!path || n == 0 || n > 0x7fffffffu) return false;
    slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    dir_len = (size_t)(slash - path);
    if (dir_len >= 1024) return false;
    memcpy(blob_dir, path, dir_len * sizeof(wchar_t));
    blob_dir[dir_len] = 0;
    for (attempt = 0; attempt < 32; attempt++) {
        HANDLE file;
        DWORD written = 0;
        unsigned long seq = ++temp_seq;
        int tn = swprintf(temp, 1100, L"%ls\\.tmp\\%lu-%llu-%lu.tmp", blob_dir,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(), seq);
        if (tn < 0 || (size_t)tn >= 1100) return false;
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        if (!WriteFile(file, data, (DWORD)n, &written, NULL) || written != n ||
            !FlushFileBuffers(file)) {
            CloseHandle(file);
            DeleteFileW(temp);
            return false;
        }
        CloseHandle(file);
        if (!MoveFileExW(temp, path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp);
            return false;
        }
        return true;
    }
    return false;
}

static bool io_read(const wchar_t *path, unsigned char **out, size_t *out_n) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *data;
    DWORD got = 0;
    *out = NULL;
    *out_n = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 0x7fffffff) {
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart == 0) {
        /* Empty file is a successful read of zero bytes: the digest check
           downstream decides whether that is corruption (a stored blob is
           never empty — put rejects n == 0 — so a zero-byte file always
           mismatches and is quarantined/repaired). Rejecting it here would
           skip quarantine and brick put on that path. */
        CloseHandle(file);
        return true;
    }
    data = (unsigned char *)malloc((size_t)size.QuadPart);
    if (!data) {
        CloseHandle(file);
        return false;
    }
    if (!ReadFile(file, data, (DWORD)size.QuadPart, &got, NULL) ||
        got != (DWORD)size.QuadPart) {
        free(data);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    *out = data;
    *out_n = got;
    return true;
}

static bool io_remove(const wchar_t *path) {
    return DeleteFileW(path) != 0;
}

static bool io_rename_file(const wchar_t *from, const wchar_t *to) {
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool io_find_blob(const wchar_t *dir, const char *digest,
    wchar_t *out_path, size_t out_cap) {
    if (!path_join_digest(out_path, out_cap, dir, digest)) return false;
    return blob_exists(out_path);
}

static AttachmentIo default_io(void) {
    AttachmentIo io;
    io.mkdir = io_mkdir;
    io.write_atomic = io_write_atomic;
    io.read = io_read;
    io.remove = io_remove;
    io.rename_file = io_rename_file;
    io.find_blob = io_find_blob;
    return io;
}

/* ---- store lifecycle ---- */

bool attachment_store_open(ChatAttachmentStore *store,
    const wchar_t *directory, uint64_t *next_id) {
    wchar_t base[1024], sub[1100];
    if (!store || !next_id) return false;
    memset(store, 0, sizeof *store);
    if (directory) {
        if (wcslen(directory) > 900) return false;
        wcscpy(base, directory);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 900);
        if (!n || n >= 900) return false;
        wcscat(base, L"\\DarkChat");
    }
    if (swprintf(store->dir, 1024, L"%ls\\attachments", base) < 0) return false;
    store->next_id = next_id;
    store->io = default_io();
    /* Parent state directory first (CreateDirectoryW is not recursive). */
    if (!store->io.mkdir(base)) return false;
    if (!store->io.mkdir(store->dir)) return false;
    if (swprintf(sub, 1100, L"%ls\\.tmp", store->dir) < 0) return false;
    if (!store->io.mkdir(sub)) return false;
    if (swprintf(sub, 1100, L"%ls\\corrupt", store->dir) < 0) return false;
    if (!store->io.mkdir(sub)) return false;
    store->open = true;
    return true;
}

void attachment_store_close(ChatAttachmentStore *store) {
    if (store) memset(store, 0, sizeof *store);
}

static bool store_ready(const ChatAttachmentStore *store) {
    return store && store->open && store->next_id;
}

/* ---- quarantine ---- */

/* Moves one specific top-level blob path into corrupt\ (keeps the filename). */
static bool quarantine_blob_path(const ChatAttachmentStore *store,
    const wchar_t *path) {
    const wchar_t *name;
    wchar_t target[1160];
    if (!store_ready(store) || !path || !path[0]) return false;
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (!name[0]) return false;
    if (swprintf(target, 1160, L"%ls\\corrupt\\%ls", store->dir, name) < 0)
        return false;
    return store->io.rename_file(path, target);
}

bool attachment_store_quarantine(const ChatAttachmentStore *store,
    const char *digest) {
    wchar_t found[1100];
    if (!store_ready(store) || !digest_valid(digest)) return false;
    if (!store->io.find_blob(store->dir, digest, found, 1100)) return false;
    return quarantine_blob_path(store, found);
}

/* ---- put ---- */

bool attachment_store_put(ChatAttachmentStore *store,
    const void *bytes, size_t n, const char *mime,
    const wchar_t *display_name, ChatAttachmentMeta *out_meta,
    bool *out_created) {
    char digest[65];
    wchar_t expected[1100];
    bool created;
    size_t mime_len;
    uint64_t id;

    if (out_created) *out_created = false;
    if (!store_ready(store) || !bytes || n == 0 || n > 0x7fffffffu ||
        !out_meta || !mime)
        return false;
    mime_len = strlen(mime);
    if (mime_len == 0 || mime_len >= sizeof out_meta->mime) return false;
    if (display_name && wcslen(display_name) >= 64) return false;
    /* Capacity first: an exhausted counter must not leave a new blob. */
    if (*store->next_id >= CHAT_MAX_ID) return false;

    attachment_digest_hex(bytes, n, digest);
    /* Sole path for this digest — MIME never enters the filename, so every
       put of these bytes (any MIME) targets the same file. */
    if (!path_join_digest(expected, 1100, store->dir, digest)) return false;

    if (blob_exists(expected)) {
        unsigned char *on_disk = NULL;
        size_t on_n = 0;
        char actual[65];
        if (!store->io.read(expected, &on_disk, &on_n)) return false;
        attachment_digest_hex(on_disk, on_n, actual);
        free(on_disk);
        if (strcmp(actual, digest) != 0) {
            /* Damaged on disk (including a zero-byte file): quarantine, then
               fall through to rewrite the same path. */
            if (!quarantine_blob_path(store, expected)) return false;
            if (!store->io.write_atomic(expected, bytes, n)) return false;
            created = true;
        } else {
            created = false;
        }
    } else {
        if (!store->io.write_atomic(expected, bytes, n)) return false;
        created = true;
    }

    /* Durable (or verified) before the counter moves. */
    id = ++(*store->next_id);
    memset(out_meta, 0, sizeof *out_meta);
    out_meta->id = id;
    memcpy(out_meta->digest, digest, 65);
    memcpy(out_meta->mime, mime, mime_len + 1);
    out_meta->bytes = n;
    out_meta->created_at = (int64_t)time(NULL) * 1000;
    if (display_name)
        wcsncpy(out_meta->display_name, display_name, 63);
    out_meta->display_name[63] = 0;
    if (out_created) *out_created = created;
    return true;
}

/* ---- get ---- */

bool attachment_store_get(const ChatAttachmentStore *store,
    const char *digest, unsigned char **out_data, size_t *out_n) {
    wchar_t found[1100];
    unsigned char *data = NULL;
    size_t n = 0;
    char actual[65];
    if (out_data) *out_data = NULL;
    if (out_n) *out_n = 0;
    if (!store_ready(store) || !digest_valid(digest) || !out_data || !out_n)
        return false;
    if (!store->io.find_blob(store->dir, digest, found, 1100)) return false;
    if (!store->io.read(found, &data, &n)) return false;
    attachment_digest_hex(data, n, actual);
    if (strcmp(actual, digest) != 0) {
        free(data);
        quarantine_blob_path(store, found);
        return false;
    }
    *out_data = data;
    *out_n = n;
    return true;
}

/* ---- collect ---- */

static bool live_contains(const char *const *live, size_t live_count,
    const char digest[65]) {
    size_t i;
    for (i = 0; i < live_count; i++)
        if (!memcmp(live[i], digest, 64)) return true;
    return false;
}

bool attachment_store_collect(ChatAttachmentStore *store,
    const char *const *live, size_t live_count) {
    wchar_t pattern[1100];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    bool ok = true;
    size_t i;

    if (!store_ready(store)) return false;
    if (live_count && !live) return false;
    /* Fail closed on a malformed live set before any delete. */
    for (i = 0; i < live_count; i++)
        if (!digest_valid(live[i])) return false;

    if (swprintf(pattern, 1100, L"%ls\\*", store->dir) < 0) return false;
    find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char digest[65];
        wchar_t full[1100];
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!managed_filename(entry.cFileName, digest)) continue;
        if (live_contains(live, live_count, digest)) continue;
        if (!path_join_name(full, 1100, store->dir, entry.cFileName)) {
            ok = false;
            continue;
        }
        if (!store->io.remove(full)) ok = false;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return ok;
}
