#include "../chat/storage.h"
#include "../chat/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Allocation seam for the decode-failure tests: the storage test links with
   -Wl,--wrap=malloc and -Wl,--wrap=realloc (chat.bat test does), so every
   message-array reservation is interceptable and fresh array memory is
   poisoned. A successful decode therefore cannot rely on zero-filled memory:
   an uninitialized slot would expose 0x5C5C... garbage. Injection is armed
   immediately before the operation under test and reset right after:
   - pass_mallocs / fail_mallocs: skip that many mallocs, then fail that many.
   - fail_malloc_size / fail_big_mallocs: fail the next N mallocs of at least
     that size (targets one message's UTF-16 expansion mid-decode).
   - fail_realloc_index: fail the nth realloc call (the conversation
     message-array reservations are the only reallocs a load performs). */
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
static long pass_mallocs, fail_mallocs;
static size_t fail_malloc_size;
static long fail_big_mallocs;
static long fail_realloc_index, realloc_seen;
void *__wrap_malloc(size_t size) {
    if (pass_mallocs > 0) { --pass_mallocs; return __real_malloc(size); }
    if (fail_mallocs > 0) { --fail_mallocs; return NULL; }
    if (fail_big_mallocs > 0 && fail_malloc_size && size >= fail_malloc_size) {
        --fail_big_mallocs; return NULL;
    }
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    ++realloc_seen;
    if (fail_realloc_index > 0 && realloc_seen == fail_realloc_index) return NULL;
    void *grown = __real_realloc(pointer, size);
    if (grown && !pointer) memset(grown, 0x5C, size);
    return grown;
}
static void seam_reset(void) {
    pass_mallocs = fail_mallocs = fail_big_mallocs = 0;
    fail_malloc_size = 0;
    fail_realloc_index = 0;
    realloc_seen = 0;
}

/* Compares two generations by every field the format persists. */
static bool same_generation(const ChatGeneration *a, const ChatGeneration *b) {
    return a->state == b->state &&
        a->started_at == b->started_at && a->finished_at == b->finished_at &&
        a->first_token_at == b->first_token_at &&
        a->ttft_ms == b->ttft_ms && a->latency_ms == b->latency_ms &&
        a->prompt_tokens == b->prompt_tokens &&
        a->completion_tokens == b->completion_tokens &&
        a->total_tokens == b->total_tokens && a->cost == b->cost &&
        a->reasoning_ms == b->reasoning_ms &&
        !wcscmp(a->requested_model, b->requested_model) &&
        !wcscmp(a->actual_model, b->actual_model) &&
        !wcscmp(a->finish_reason, b->finish_reason) &&
        !wcscmp(a->error, b->error);
}
static bool same_message(const ChatMessage *a, const ChatMessage *b) {
    return a->role == b->role && a->created_at == b->created_at &&
        a->modified_at == b->modified_at &&
        !wcscmp(chat_message_text(a), chat_message_text(b)) &&
        !wcscmp(chat_message_reasoning(a), chat_message_reasoning(b)) &&
        same_generation(&a->generation, &b->generation);
}
/* Compares every logically persisted field. View bookkeeping (message ids,
   revisions, expansion) and runtime-only state (status, reply counter) are
   deliberately excluded: they are not part of a snapshot. */
static bool same_chat(const Chat *a, const Chat *b) {
    if (a->next_id != b->next_id || a->active != b->active ||
        a->conversation_count != b->conversation_count ||
        a->model_history_count != b->model_history_count ||
        a->window_x != b->window_x || a->window_y != b->window_y ||
        a->window_width != b->window_width ||
        a->window_height != b->window_height ||
        a->maximized != b->maximized || a->sidebar_width != b->sidebar_width ||
        wcscmp(a->model, b->model) ||
        wcscmp(a->system_prompt, b->system_prompt)) return false;
    for (int i = 0; i < a->model_history_count; i++)
        if (wcscmp(a->model_history[i], b->model_history[i])) return false;
    for (int i = 0; i < a->conversation_count; i++) {
        const ChatConversation *ca = &a->conversations[i];
        const ChatConversation *cb = &b->conversations[i];
        if (ca->id != cb->id || ca->created_at != cb->created_at ||
            ca->modified_at != cb->modified_at || ca->renamed != cb->renamed ||
            ca->message_count != cb->message_count ||
            wcscmp(ca->title, cb->title) || wcscmp(ca->draft, cb->draft))
            return false;
        if (ca->message_count && (!ca->messages || !cb->messages)) return false;
        for (size_t j = 0; j < ca->message_count; j++)
            if (!same_message(&ca->messages[j], &cb->messages[j])) return false;
    }
    return true;
}

static void corrupt(const wchar_t *path) {
    FILE *f=_wfopen(path,L"wb");
    if (f) { fputs("{\"version\":1,\"truncated\":",f); fclose(f); }
}
static bool files_equal(const wchar_t *a, const wchar_t *b) {
    FILE *fa=_wfopen(a,L"rb"), *fb=_wfopen(b,L"rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return false; }
    bool equal=true; int ca, cb;
    do { ca=fgetc(fa); cb=fgetc(fb); if (ca!=cb) { equal=ca==cb; break; } } while (ca!=EOF);
    fclose(fa); fclose(fb);
    return equal;
}
static void remove_store(const ChatStorage *store, const wchar_t *dir) {
    DeleteFileW(store->path); DeleteFileW(store->backup); DeleteFileW(store->temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock);
    RemoveDirectoryW(dir);
}
int main(void) {
    wchar_t dir[256]; swprintf(dir,256,L"build\\storage-test-%lu",GetCurrentProcessId());
    Chat *chat=calloc(1,sizeof *chat), *loaded=calloc(1,sizeof *loaded);
    CHECK(chat && loaded);
    chat_init(chat); chat_clear(chat);
    ChatStorage store, other;
    CHECK(storage_open(&store,dir));
    CHECK(!storage_open(&other,dir)); storage_close(&other);
    CHECK(storage_load(&store,loaded)==0);
    uint64_t id=chat_active(chat)->id;
    CHECK(chat_rename(chat,L"Renamed \xd83d\xde80"));
    CHECK(chat_active(chat)->id==id);
    wcscpy(chat->system_prompt,L"Be concise.\nUnicode \x03bb");
    wcscpy(chat->conversations[0].draft,L"Unsent draft");
    chat->sidebar_width=280;
    int n=chat_begin_response(chat,CHAT_SEND,L"First question"); CHECK(n==1);
    ChatMessage *m=&chat->conversations[0].messages[n];
    wcscpy(m->text,L"Partial \xd83d\xde80\n```c\nint x;\n```");
    wcscpy(m->generation.actual_model,L"actual/model");
    m->generation.ttft_ms=123.5; m->generation.prompt_tokens=42;
    m->generation.completion_tokens=8; m->generation.total_tokens=50; m->generation.cost=0.000123;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->conversations[0].id==id);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
    CHECK(loaded->conversations[0].messages[1].generation.cost==0.000123);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].text,m->text));
    CHECK(!wcscmp(loaded->system_prompt,chat->system_prompt));
    CHECK(loaded->sidebar_width==280 && loaded->model_history_count==1);
    CHECK(!wcscmp(loaded->conversations[0].draft,L"Unsent draft"));
    /* A message line without the optional reasoning fields (an older version 1
       snapshot) loads with empty reasoning and unavailable duration. */
    CHECK(!loaded->conversations[0].messages[1].reasoning[0]);
    CHECK(loaded->conversations[0].messages[1].generation.reasoning_ms==-1);
    wcscpy(m->reasoning,L"Checked the options and chose this.");
    m->generation.reasoning_ms=2500.0;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(!wcscmp(loaded->conversations[0].messages[1].reasoning,
        L"Checked the options and chose this."));
    CHECK(loaded->conversations[0].messages[1].generation.reasoning_ms==2500.0);
    wchar_t *large_reasoning=(wchar_t *)malloc(70001*sizeof(wchar_t));
    CHECK(large_reasoning);
    for (int i=0;i<70000;i++) large_reasoning[i]=L'r';
    large_reasoning[70000]=0;
    CHECK(chat_message_set_reasoning(m,large_reasoning));
    free(large_reasoning);
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(wcslen(chat_message_reasoning(
        &loaded->conversations[0].messages[1]))==70000);
    m->generation.state=CHAT_GENERATION_COMPLETE;
    CHECK(storage_save(&store,chat));
    CHECK(storage_load(&store,loaded)==1);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
    HANDLE block=CreateFileW(store.temporary,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,0,NULL);
    CHECK(block!=INVALID_HANDLE_VALUE);
    CHECK(!storage_save(&store,chat)); CloseHandle(block);
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    block=CreateFileW(store.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(block!=INVALID_HANDLE_VALUE);
    CHECK(!storage_save(&store,chat)); CloseHandle(block);
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    corrupt(store.temporary); /* torn temp must not displace valid primary */
    CHECK(storage_load(&store,loaded)==1 && !store.recovered);
    corrupt(store.path);
    CHECK(storage_load(&store,loaded)==1 && store.recovered);
    CHECK(loaded->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
    CHECK(storage_save(&store,loaded)); /* must not rotate corrupt primary over backup */
    corrupt(store.path);
    CHECK(storage_load(&store,loaded)==1 && store.recovered);
    CHECK(storage_save(&store,loaded));
    CHECK(CopyFileW(store.path,store.temporary,FALSE));
    DeleteFileW(store.path); DeleteFileW(store.backup);
    CHECK(storage_load(&store,loaded)==1 && store.recovered); /* first-save temp recovery */
    corrupt(store.temporary); corrupt(store.backup);
    CHECK(storage_load(&store,loaded)==-1);
    CHECK(!storage_save(&store,chat)); /* preserve unreadable files */
    storage_close(&store);
    /* An unknown newer version must not be overwritten by an older backup. */
    CHECK(storage_open(&store,dir));
    CHECK(storage_save(&store,chat)); CHECK(storage_save(&store,chat));
    FILE *future=_wfopen(store.path,L"r+b"); CHECK(future);
    char header[64]={0}; CHECK(fread(header,1,63,future)==63);
    char *version=strstr(header,"\"version\":1"); CHECK(version);
    CHECK(fseek(future,(long)(version-header)+(long)strlen("\"version\":"),SEEK_SET)==0);
    fputc('2',future); fclose(future);
    CHECK(storage_load(&store,loaded)==-1 && !store.writable);
    storage_close(&store);
    DeleteFileW(store.path); DeleteFileW(store.backup); DeleteFileW(store.temporary);
    wchar_t lock[300]; swprintf(lock,300,L"%ls\\writer.lock",dir); DeleteFileW(lock); RemoveDirectoryW(dir);
    /* A version 1 snapshot produced by the previous build of the same format
       loads unchanged, decodes into reserved dynamic message capacity, and
       saving identical logical content reproduces the exact bytes. */
    {
        wchar_t fdir[256]; swprintf(fdir,256,L"build\\storage-fixture-%lu",GetCurrentProcessId());
        ChatStorage fstore;
        CHECK(storage_open(&fstore,fdir));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.path,FALSE));
        Chat *fixture=calloc(1,sizeof *fixture);
        CHECK(fixture);
        CHECK(storage_load(&fstore,fixture)==1);
        CHECK(fixture->conversation_count==2);
        CHECK(!wcscmp(fixture->conversations[0].title,L"Renamed \xd83d\xde80"));
        CHECK(fixture->conversations[0].message_count==2);
        CHECK(!wcscmp(chat_message_text(&fixture->conversations[0].messages[0]),
            L"First question"));
        CHECK(fixture->conversations[0].messages!=NULL &&
            fixture->conversations[0].message_capacity>=2);
        CHECK(fixture->conversations[1].messages!=NULL &&
            fixture->conversations[1].message_capacity>=2);
        CHECK(!wcscmp(chat_message_text(&fixture->conversations[0].messages[1]),
            L"Regenerated answer with \x03bd math."));
        CHECK(fixture->conversations[0].messages[1].generation.state==CHAT_GENERATION_COMPLETE);
        CHECK(fixture->conversations[0].messages[1].generation.cost==-1);
        CHECK(wcslen(chat_message_reasoning(
            &fixture->conversations[0].messages[1]))==25000);
        CHECK(fixture->conversations[0].messages[1].generation.reasoning_ms==1234.5);
        CHECK(!fixture->conversations[1].messages[1].reasoning[0]);
        CHECK(fixture->conversations[1].messages[1].generation.state==CHAT_GENERATION_INTERRUPTED);
        CHECK(!wcscmp(fixture->system_prompt,L"Be concise.\nUnicode \x03bb"));
        CHECK(!wcscmp(fixture->conversations[0].draft,L"Unsent draft"));
        /* Roundtrip identity: loading the fixture and saving it again writes
           the same bytes the previous build produced. */
        CHECK(storage_save(&fstore,fixture));
        CHECK(files_equal(fstore.path,L"tests\\state-v1-fixture.jsonl"));
        chat_dispose(fixture); free(fixture);
        storage_close(&fstore);
        remove_store(&fstore,fdir);
    }
    /* Decode failures under the poisoned allocator. The destination passed to
       storage_load is the object under test: a successful recovery must give
       it the fallback's full content, and a failed load must leave its
       previous logical content untouched. */
    {
        /* Fixture-backed cases: two conversations exercise both reservation
           calls and the per-message decode path. */
        wchar_t fdir2[256]; swprintf(fdir2,256,L"build\\storage-fail-%lu",GetCurrentProcessId());
        ChatStorage fstore;
        CHECK(storage_open(&fstore,fdir2));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.path,FALSE));
        CHECK(CopyFileW(L"tests\\state-v1-fixture.jsonl",fstore.backup,FALSE));
        Chat *reference=calloc(1,sizeof *reference), *dest=calloc(1,sizeof *dest);
        CHECK(reference && dest);
        seam_reset();
        CHECK(storage_load(&fstore,reference)==1 && !fstore.recovered);
        /* 1. The first conversation's message-array reservation (the first
              realloc of a load) fails. The partially decoded store is
              quarantined and the untouched backup is delivered. */
        seam_reset(); fail_realloc_index=1;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        CHECK(fstore.writable);
        /* 2. The second conversation's reservation fails after the first
              conversation's messages were decoded, so the quarantine must
              dispose live messages and their overflow storage. */
        seam_reset(); fail_realloc_index=2;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        /* 3. A failure while decoding the settings line, before any
              conversation is reached, is recovered the same way. */
        seam_reset(); pass_mallocs=1; fail_mallocs=1;
        CHECK(storage_load(&fstore,dest)==1 && fstore.recovered);
        seam_reset();
        CHECK(same_chat(dest,reference));
        /* 4. A failure midway through decoding a message, after earlier
              messages succeeded. The oversized reasoning in the second
              message makes its UTF-16 expansion the first allocation at or
              above the threshold, so the partially constructed message must
              be disposed by the quarantine. */
        {
            Chat *big=calloc(1,sizeof *big);
            CHECK(big);
            chat_init(big); chat_clear(big);
            CHECK(chat_begin_response(big,CHAT_SEND,L"large reasoning")==1);
            wchar_t *wide=(wchar_t *)malloc(40001*sizeof *wide);
            CHECK(wide);
            for (int i=0;i<40000;i++) wide[i]=L'R';
            wide[40000]=0;
            CHECK(chat_message_set_reasoning(&big->conversations[0].messages[1],wide));
            free(wide);
            big->conversations[0].messages[1].generation.state=CHAT_GENERATION_COMPLETE;
            wchar_t bdir[256]; swprintf(bdir,256,L"build\\storage-middecode-%lu",GetCurrentProcessId());
            ChatStorage bstore;
            CHECK(storage_open(&bstore,bdir));
            CHECK(storage_save(&bstore,big));
            CHECK(storage_save(&bstore,big));   /* backup keeps recovery possible */
            seam_reset();
            fail_malloc_size=60000; fail_big_mallocs=1;
            CHECK(storage_load(&bstore,dest)==1 && bstore.recovered);
            seam_reset();
            CHECK(same_chat(dest,big));
            storage_close(&bstore);
            remove_store(&bstore,bdir);
            chat_dispose(big); free(big);
        }
        chat_dispose(reference); free(reference);
        chat_dispose(dest); free(dest);
        storage_close(&fstore);
        remove_store(&fstore,fdir2);
    }

    /* With no valid fallback the load fails: the destination keeps its
       previous logical content, the quarantine is fully disposed (nothing is
       partially adopted), and the store refuses further writes. */
    {
        wchar_t edir[256]; swprintf(edir,256,L"build\\storage-nodecode-%lu",GetCurrentProcessId());
        ChatStorage estore;
        CHECK(storage_open(&estore,edir));
        CHECK(storage_save(&estore,chat));   /* primary only, no backup */
        Chat *before=calloc(1,sizeof *before), *dest=calloc(1,sizeof *dest);
        CHECK(before && dest);
        CHECK(storage_load(&estore,before)==1);   /* destination baseline */
        CHECK(storage_load(&estore,dest)==1);     /* destination under test */
        CHECK(same_chat(dest,before));
        /* The first conversation's reservation fails and there is nothing to
           fall back to. */
        seam_reset(); fail_realloc_index=1;
        CHECK(storage_load(&estore,dest)==-1);
        seam_reset();
        CHECK(same_chat(dest,before));            /* destination preserved */
        CHECK(!estore.writable);
        CHECK(!storage_save(&estore,dest));       /* unreadable files are preserved */
        CHECK(storage_load(&estore,dest)==-1 && !estore.writable);
        CHECK(same_chat(dest,before));
        storage_close(&estore);
        remove_store(&estore,edir);
        chat_dispose(before); free(before);
        chat_dispose(dest); free(dest);
    }

    chat_dispose(chat); chat_dispose(loaded);
    free(chat); free(loaded);
    puts("Storage roundtrip, locking, corruption, backup and interrupted-write recovery passed");
    return 0;
}
