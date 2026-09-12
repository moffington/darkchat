#ifndef DARKCHAT_STORAGE_H
#define DARKCHAT_STORAGE_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "chat.h"
/* One writer per directory. A failed load disables writes to preserve evidence. */
typedef struct {
    wchar_t path[1024], backup[1024], temporary[1024];
    HANDLE lock;
    bool writable, primary_valid, recovered;
} ChatStorage;
bool storage_open(ChatStorage *store, const wchar_t *directory);
/* 1 loaded, 0 new store, -1 unreadable/unsupported (writes disabled). */
int storage_load(ChatStorage *store, Chat *chat);
bool storage_save(ChatStorage *store, const Chat *chat);
void storage_close(ChatStorage *store);
#endif
