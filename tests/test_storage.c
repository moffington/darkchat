#include "../chat/storage.h"
#include "../chat/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void corrupt(const wchar_t *path) {
    FILE *f=_wfopen(path,L"wb");
    if (f) { fputs("{\"version\":1,\"truncated\":",f); fclose(f); }
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
    chat_dispose(chat); chat_dispose(loaded);
    free(chat); free(loaded);
    puts("Storage roundtrip, locking, corruption, backup and interrupted-write recovery passed");
    return 0;
}
