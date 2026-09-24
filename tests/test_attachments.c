#include "chat/persistence/attachments.h"
#include "chat/core/chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* malloc wrap for OOM injection (io_read and get's buffer go through malloc). */
void *__real_malloc(size_t size);
void __real_free(void *pointer);
static long pass_mallocs, fail_mallocs;
static long live_allocs;
void *__wrap_malloc(size_t size) {
    if (pass_mallocs > 0) { --pass_mallocs; goto pass; }
    if (fail_mallocs > 0) { --fail_mallocs; return NULL; }
pass: {
    void *result = __real_malloc(size);
    if (result) ++live_allocs;
    return result;
}
}
void __wrap_free(void *pointer) {
    if (pointer) --live_allocs;
    __real_free(pointer);
}
static void seam_reset(void) { pass_mallocs = fail_mallocs = 0; }

/* Joins dir\digest (sole managed name has no extension). */
static void blob_path(wchar_t *out, size_t cap, const wchar_t *dir,
    const char *digest) {
    size_t pos = 0;
    const wchar_t *p = dir;
    for (; *p && pos + 1 < cap; p++) out[pos++] = *p;
    if (pos + 1 < cap) out[pos++] = L'\\';
    for (; *digest && pos + 1 < cap; digest++) out[pos++] = (wchar_t)*digest;
    out[pos] = 0;
}

static int count_top_managed(const ChatAttachmentStore *store) {
    wchar_t pattern[1100];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    int n = 0;
    swprintf(pattern, 1100, L"%ls\\*", store->dir);
    find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        int i;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        for (i = 0; i < 64; i++) {
            wchar_t c = entry.cFileName[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) break;
        }
        if (i == 64 && entry.cFileName[64] == 0) n++;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return n;
}

static int count_files_in(const wchar_t *subdir) {
    wchar_t pattern[1160];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    int n = 0;
    swprintf(pattern, 1160, L"%ls\\*", subdir);
    find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return n;
}

static int count_tmp_files(const ChatAttachmentStore *store) {
    wchar_t sub[1100];
    swprintf(sub, 1100, L"%ls\\.tmp", store->dir);
    return count_files_in(sub);
}

static bool write_bytes(const wchar_t *path, const void *data, size_t n) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool ok;
    if (file == INVALID_HANDLE_VALUE) return false;
    ok = WriteFile(file, data, (DWORD)n, &written, NULL) && written == n;
    CloseHandle(file);
    return ok;
}

static bool file_has(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void remove_store_tree(ChatAttachmentStore *store, const wchar_t *parent) {
    wchar_t pattern[1160];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    if (!store->dir[0]) return;
    swprintf(pattern, 1160, L"%ls\\*", store->dir);
    find = FindFirstFileW(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[1200];
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
                continue;
            swprintf(full, 1200, L"%ls\\%ls", store->dir, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ChatAttachmentStore sub;
                memset(&sub, 0, sizeof sub);
                wcscpy(sub.dir, full);
                remove_store_tree(&sub, NULL);
                RemoveDirectoryW(full);
            } else {
                DeleteFileW(full);
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(store->dir);
    if (parent) RemoveDirectoryW(parent);
}

/* Per-store fault hooks (not process-global: each store holds its own io). */
static bool fail_write;
static bool fault_write(const wchar_t *path, const void *data, size_t n) {
    (void)path; (void)data; (void)n;
    return !fail_write;
}
static bool fail_remove;
static bool fault_remove(const wchar_t *path) {
    (void)path;
    return !fail_remove;
}

int main(void) {
    wchar_t dir[256];
    uint64_t next_id;
    ChatAttachmentStore store;
    ChatAttachmentMeta meta, meta2;
    bool created;
    unsigned char *data = NULL;
    size_t n = 0;
    char hex[65];
    wchar_t path[1160];
    const char *live1[1];

    /* --- pure SHA-256 vectors (put rejects n==0, so empty lives here).
       Boundary lengths pin the padding path with full digests, not just
       output length. Expected values from an independent implementation
       (.NET SHA256), including the NIST 56-byte multi-block message. --- */
    attachment_digest_hex("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    attachment_digest_hex("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    attachment_digest_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    {
        char buf[64];
        memset(buf, 'a', 55);
        attachment_digest_hex(buf, 55, hex);
        CHECK(!strcmp(hex, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
        memset(buf, 'a', 56);
        attachment_digest_hex(buf, 56, hex);
        CHECK(!strcmp(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
        memset(buf, 'a', 64);
        attachment_digest_hex(buf, 64, hex);
        CHECK(!strcmp(hex, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    }

    swprintf(dir, 256, L"build\\attach-test-%lu", (unsigned long)GetCurrentProcessId());
    next_id = 1000;
    seam_reset();

    /* --- open creates the directory skeleton; next_id is required --- */
    CHECK(attachment_store_open(&store, dir, &next_id));
    {
        wchar_t sub[1100];
        swprintf(sub, 1100, L"%ls\\.tmp", store.dir);
        CHECK(GetFileAttributesW(sub) != INVALID_FILE_ATTRIBUTES);
        swprintf(sub, 1100, L"%ls\\corrupt", store.dir);
        CHECK(GetFileAttributesW(sub) != INVALID_FILE_ATTRIBUTES);
    }
    attachment_store_close(&store);
    CHECK(!attachment_store_open(&store, dir, NULL));
    CHECK(attachment_store_open(&store, dir, &next_id));

    /* --- validation rejects without consuming an id or writing --- */
    {
        int before = count_top_managed(&store);
        wchar_t long_name[65];
        int i;
        for (i = 0; i < 64; i++) long_name[i] = L'a';
        long_name[64] = 0;
        CHECK(!attachment_store_put(&store, NULL, 4, "image/png", NULL, &meta, &created));
        CHECK(!attachment_store_put(&store, "abcd", 0, "image/png", NULL, &meta, &created));
        CHECK(!attachment_store_put(&store, "abcd", 4, "", NULL, &meta, &created));
        CHECK(!attachment_store_put(&store, "abcd", 4, NULL, NULL, &meta, &created));
        CHECK(!attachment_store_put(&store, "abcd", 4, "image/png", long_name,
            &meta, &created));
        CHECK(next_id == 1000);
        CHECK(count_top_managed(&store) == before);
    }

    /* --- put/get round trip; digest stability; dedup reuses one file --- */
    {
        const char payload[] = "hello attachment";
        char hex2[65];
        attachment_digest_hex(payload, sizeof payload - 1, hex2);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", L"photo.png", &meta, &created));
        CHECK(created);
        CHECK(!strcmp(meta.digest, hex2));
        CHECK(meta.id == 1001);
        CHECK(meta.bytes == sizeof payload - 1);
        CHECK(!wcscmp(meta.display_name, L"photo.png"));
        CHECK(next_id == 1001);
        CHECK(count_top_managed(&store) == 1);
        CHECK(count_tmp_files(&store) == 0); /* unique staging cleaned up */
        blob_path(path, 1160, store.dir, meta.digest);
        CHECK(file_has(path)); /* extensionless digest name */

        CHECK(attachment_store_get(&store, meta.digest, &data, &n));
        CHECK(n == sizeof payload - 1 && !memcmp(data, payload, n));
        free(data); data = NULL;

        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", L"copy.png", &meta2, &created));
        CHECK(!created); /* verified reuse, not rewrite */
        CHECK(!strcmp(meta2.digest, meta.digest));
        CHECK(meta2.id == 1002);
        CHECK(next_id == 1002);
        CHECK(count_top_managed(&store) == 1);
    }

    /* --- different MIME, same bytes: one path, reuse (no rename needed) --- */
    {
        const char payload[] = "hello attachment";
        char d[65];
        wchar_t only[1100];
        attachment_digest_hex(payload, sizeof payload - 1, d);
        blob_path(only, 1100, store.dir, d);
        CHECK(file_has(only));
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/jpeg", L"photo.jpg", &meta, &created));
        CHECK(!created);
        CHECK(file_has(only)); /* still the sole extensionless name */
        CHECK(meta.id == 1003);
        CHECK(count_top_managed(&store) == 1);
        CHECK(!strcmp(meta.mime, "image/jpeg"));
    }

    /* --- verify-on-dedup: damaged on-disk blob → quarantine + rewrite --- */
    {
        const char payload[] = "verify me";
        char d[65];
        int managed_before;
        wchar_t quarantined[1200];
        attachment_digest_hex(payload, sizeof payload - 1, d);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta, &created));
        CHECK(created);
        managed_before = count_top_managed(&store);
        blob_path(path, 1160, store.dir, d);
        CHECK(write_bytes(path, "CORRUPT", 7));
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta2, &created));
        CHECK(created); /* rewrite after quarantine, not blind reuse;
                           informational only — rollback uses live-set sweep */
        CHECK(!strcmp(meta2.digest, d));
        CHECK(count_top_managed(&store) == managed_before);
        CHECK(file_has(path));
        swprintf(quarantined, 1200, L"%ls\\corrupt\\", store.dir);
        {
            size_t pos = wcslen(quarantined);
            size_t i;
            for (i = 0; i < 64; i++) quarantined[pos++] = (wchar_t)d[i];
            quarantined[pos] = 0;
        }
        CHECK(file_has(quarantined));
        CHECK(attachment_store_get(&store, d, &data, &n));
        CHECK(n == sizeof payload - 1 && !memcmp(data, payload, n));
        free(data); data = NULL;
    }

    /* --- missing blob and invalid digests --- */
    CHECK(!attachment_store_get(&store,
        "0000000000000000000000000000000000000000000000000000000000000000",
        &data, &n));
    CHECK(data == NULL && n == 0);
    CHECK(!attachment_store_get(&store, "not-a-digest", &data, &n));
    CHECK(!attachment_store_get(&store,
        "000000000000000000000000000000000000000000000000000000000000000A",
        &data, &n));

    /* --- get mismatch → quarantine (file leaves the top level) --- */
    {
        const char payload[] = "will be damaged";
        char d[65];
        attachment_digest_hex(payload, sizeof payload - 1, d);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/gif", NULL, &meta, &created));
        blob_path(path, 1160, store.dir, d);
        CHECK(write_bytes(path, "XX", 2));
        CHECK(!attachment_store_get(&store, d, &data, &n));
        CHECK(data == NULL);
        CHECK(!file_has(path));
    }

    /* --- zero-byte damaged blob: get quarantines it (read must accept
       empty so the digest check runs); put of the correct bytes repairs --- */
    {
        const char payload[] = "zero-byte victim";
        char d[65];
        wchar_t quarantined[1200];
        int managed_before;
        attachment_digest_hex(payload, sizeof payload - 1, d);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta, &created));
        blob_path(path, 1160, store.dir, d);
        CHECK(write_bytes(path, "", 0)); /* truncate to empty */
        CHECK(file_has(path));
        managed_before = count_top_managed(&store);

        CHECK(!attachment_store_get(&store, d, &data, &n));
        CHECK(data == NULL);
        CHECK(!file_has(path)); /* empty file left the top level */
        swprintf(quarantined, 1200, L"%ls\\corrupt\\", store.dir);
        {
            size_t pos = wcslen(quarantined);
            size_t i;
            for (i = 0; i < 64; i++) quarantined[pos++] = (wchar_t)d[i];
            quarantined[pos] = 0;
        }
        CHECK(file_has(quarantined));

        /* Fresh zero-byte file: put must repair, not fail the read path. */
        CHECK(write_bytes(path, "", 0));
        CHECK(count_top_managed(&store) == managed_before);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta2, &created));
        CHECK(created); /* informational: this call wrote bytes — not a
                           delete permission for rollback (see header) */
        CHECK(!strcmp(meta2.digest, d));
        CHECK(attachment_store_get(&store, d, &data, &n));
        CHECK(n == sizeof payload - 1 && !memcmp(data, payload, n));
        free(data); data = NULL;
    }

    /* --- concurrent-style puts with different MIME share one path and
       cannot delete each other's only copy (MIME never enters the name). --- */
    {
        const char payload[] = "dual-mime victim";
        char d[65];
        wchar_t only[1100];
        int managed_before;
        attachment_digest_hex(payload, sizeof payload - 1, d);
        blob_path(only, 1100, store.dir, d);
        managed_before = count_top_managed(&store);

        /* Two puts, different MIME arguments, same bytes → same path. */
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta, &created));
        CHECK(created);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/jpeg", NULL, &meta2, &created));
        CHECK(!created); /* second put reuses the one file */
        CHECK(file_has(only));
        CHECK(count_top_managed(&store) == managed_before + 1);
        CHECK(!strcmp(meta.mime, "image/png"));
        CHECK(!strcmp(meta2.mime, "image/jpeg"));

        /* Interleaved "both write then both finish" cannot leave zero files:
           each write targets the same path with identical content. */
        {
            /* Simulate A write / B write / A verify / B verify by calling
               put twice more after manually rewriting the blob. */
            CHECK(write_bytes(only, payload, sizeof payload - 1));
            CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
                "image/webp", NULL, &meta, &created));
            CHECK(!created);
            CHECK(file_has(only));
            CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
                "image/gif", NULL, &meta2, &created));
            CHECK(!created);
            CHECK(file_has(only));
            CHECK(count_top_managed(&store) == managed_before + 1);
        }

        CHECK(attachment_store_get(&store, d, &data, &n));
        CHECK(n == sizeof payload - 1 && !memcmp(data, payload, n));
        free(data); data = NULL;
        live1[0] = d;
        CHECK(attachment_store_collect(&store, live1, 1));
        CHECK(file_has(only)); /* live sweep keeps the one file */
        CHECK(count_top_managed(&store) == 1);
        DeleteFileW(only);
    }

    /* --- id exhaustion checked before any write --- */
    {
        uint64_t save = next_id;
        int before = count_top_managed(&store);
        next_id = CHAT_MAX_ID;
        CHECK(!attachment_store_put(&store, "exhaust", 7, "image/png", NULL,
            &meta, &created));
        CHECK(next_id == CHAT_MAX_ID);
        CHECK(count_top_managed(&store) == before);
        CHECK(count_tmp_files(&store) == 0);
        next_id = save;
    }

    /* --- write_atomic seam failure: no id burn, no managed file.
       This does NOT enter the real atomic writer (fault_write returns
       before any temp/rename); temporary-file cleanup is proven by the
       real-writer rename-failure test below. --- */
    {
        AttachmentIo saved = store.io;
        uint64_t baseline = next_id;
        int before = count_top_managed(&store);
        fail_write = true;
        store.io.write_atomic = fault_write;
        CHECK(!attachment_store_put(&store, "write-fail", 10, "image/png", NULL,
            &meta, &created));
        CHECK(next_id == baseline);
        CHECK(count_top_managed(&store) == before);
        store.io = saved;
        fail_write = false;
    }

    /* --- real atomic writer: rename onto a directory fails after the
       unique temp is written; staging is cleaned, no id consumed --- */
    {
        const char payload[] = "rename-onto-dir";
        char d[65];
        wchar_t dest[1100];
        uint64_t baseline = next_id;
        int before = count_top_managed(&store);
        attachment_digest_hex(payload, sizeof payload - 1, d);
        blob_path(dest, 1100, store.dir, d);
        CHECK(CreateDirectoryW(dest, NULL)); /* blocks MoveFileExW */
        CHECK(!attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta, &created));
        CHECK(next_id == baseline);
        CHECK(count_top_managed(&store) == before);
        CHECK(count_tmp_files(&store) == 0); /* real writer deleted its temp */
        RemoveDirectoryW(dest);
    }

    /* --- OOM on get's read buffer (relative leak check around the call) --- */
    {
        const char payload[] = "oom target";
        long before;
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", NULL, &meta, &created));
        seam_reset();
        before = live_allocs;
        fail_mallocs = 1;
        CHECK(!attachment_store_get(&store, meta.digest, &data, &n));
        CHECK(data == NULL);
        CHECK(live_allocs == before); /* failed get leaks nothing */
        seam_reset();
        before = live_allocs;
        CHECK(attachment_store_get(&store, meta.digest, &data, &n));
        free(data); data = NULL;
        CHECK(live_allocs == before); /* successful get + free is balanced */
    }

    /* --- path-traversal-free naming: evil display_name never becomes a path --- */
    {
        const char payload[] = "evil name";
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/png", L"..\\..\\evil.exe", &meta, &created));
        CHECK(GetFileAttributesW(L"..\\..\\evil.exe") == INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW(L"evil.exe") == INVALID_FILE_ATTRIBUTES);
        blob_path(path, 1160, store.dir, meta.digest);
        CHECK(file_has(path));
    }

    /* --- sweep: managed candidates only; foreign files survive --- */
    {
        const char a[] = "blob-a", b[] = "blob-b";
        char da[65], db[65];
        wchar_t foreign1[1100], foreign2[1100], foreign3[1100];
        int managed_before;
        attachment_digest_hex(a, sizeof a - 1, da);
        attachment_digest_hex(b, sizeof b - 1, db);
        CHECK(attachment_store_put(&store, a, sizeof a - 1, "image/png", NULL,
            &meta, &created));
        CHECK(attachment_store_put(&store, b, sizeof b - 1, "image/png", NULL,
            &meta2, &created));
        swprintf(foreign1, 1100, L"%ls\\index.jsonl", store.dir);
        swprintf(foreign2, 1100, L"%ls\\notes.txt", store.dir);
        swprintf(foreign3, 1100, L"%ls\\%hs.tmp", store.dir,
            "0000000000000000000000000000000000000000000000000000000000000000");
        CHECK(write_bytes(foreign1, "x", 1));
        CHECK(write_bytes(foreign2, "y", 1));
        CHECK(write_bytes(foreign3, "z", 1));
        managed_before = count_top_managed(&store);
        CHECK(managed_before >= 2);

        /* Malformed live set fails closed before any delete. */
        {
            const char *bad[] = { da, "not-hex" };
            int before = count_top_managed(&store);
            CHECK(!attachment_store_collect(&store, bad, 2));
            CHECK(count_top_managed(&store) == before);
            CHECK(file_has(foreign1));
        }

        /* Digests standing in for backup/temp/pending references survive. */
        live1[0] = da;
        CHECK(attachment_store_collect(&store, live1, 1));
        blob_path(path, 1160, store.dir, da);
        CHECK(file_has(path));
        blob_path(path, 1160, store.dir, db);
        CHECK(!file_has(path)); /* not in the live set → reclaimed */
        CHECK(file_has(foreign1));
        CHECK(file_has(foreign2));
        CHECK(file_has(foreign3));

        /* Crash-between-put-and-reference: unreferenced put is an orphan. */
        {
            const char orphan[] = "orphan-bytes";
            char dorphan[65];
            attachment_digest_hex(orphan, sizeof orphan - 1, dorphan);
            CHECK(attachment_store_put(&store, orphan, sizeof orphan - 1,
                "image/png", NULL, &meta, &created));
            CHECK(created);
            CHECK(attachment_store_collect(&store, NULL, 0));
            CHECK(!attachment_store_get(&store, dorphan, &data, &n));
            CHECK(file_has(foreign1) && file_has(foreign2) && file_has(foreign3));
        }

        /* remove failure during sweep: continues other work, reports false. */
        {
            const char victim[] = "sweep-victim";
            AttachmentIo saved = store.io;
            CHECK(attachment_store_put(&store, victim, sizeof victim - 1,
                "image/png", NULL, &meta, &created));
            fail_remove = true;
            store.io.remove = fault_remove;
            CHECK(!attachment_store_collect(&store, NULL, 0));
            store.io = saved;
            fail_remove = false;
            CHECK(attachment_store_collect(&store, NULL, 0));
            CHECK(count_top_managed(&store) == 0);
            CHECK(file_has(foreign1)); /* still untouched */
        }
    }

    /* --- quarantine API directly --- */
    {
        const char payload[] = "quarantine me";
        char d[65];
        attachment_digest_hex(payload, sizeof payload - 1, d);
        CHECK(attachment_store_put(&store, payload, sizeof payload - 1,
            "image/webp", NULL, &meta, &created));
        CHECK(attachment_store_quarantine(&store, d));
        CHECK(!attachment_store_get(&store, d, &data, &n));
        CHECK(!attachment_store_quarantine(&store, d));
        CHECK(!attachment_store_quarantine(&store, "zz"));
    }

    /* --- per-store seam isolation: one store's fault never reaches another --- */
    {
        ChatAttachmentStore other;
        uint64_t other_id = 50;
        AttachmentIo saved = store.io;
        CHECK(attachment_store_open(&other, dir, &other_id));
        fail_write = true;
        store.io.write_atomic = fault_write;
        CHECK(!attachment_store_put(&store, "isolated", 8, "image/png", NULL,
            &meta, &created));
        CHECK(attachment_store_put(&other, "isolated", 8, "image/png", NULL,
            &meta, &created));
        CHECK(created);
        store.io = saved;
        fail_write = false;
        attachment_store_close(&other);
    }

    /* --- close is idempotent; a closed store fails every operation --- */
    attachment_store_close(&store);
    attachment_store_close(&store);
    CHECK(!attachment_store_put(&store, "x", 1, "image/png", NULL, &meta, &created));
    CHECK(!attachment_store_get(&store,
        "0000000000000000000000000000000000000000000000000000000000000000",
        &data, &n));
    CHECK(!attachment_store_collect(&store, NULL, 0));
    CHECK(!attachment_store_quarantine(&store,
        "0000000000000000000000000000000000000000000000000000000000000000"));

    /* Cleanup: reopen (recreates skeleton), then remove the whole tree. */
    CHECK(attachment_store_open(&store, dir, &next_id));
    remove_store_tree(&store, dir);

    puts("Attachment store digest, dedup verify, unique staging, guarded sweep and id rules passed");
    return 0;
}
