#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "chat/models/model_catalog.h"
#include "chat/json.h"
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void chat_model_catalog_init(ChatModelCatalog *catalog) {
    if (catalog) memset(catalog, 0, sizeof *catalog);
}

void chat_model_catalog_dispose(ChatModelCatalog *catalog) {
    if (!catalog) return;
    free(catalog->items);
    memset(catalog, 0, sizeof *catalog);
}

static bool ensure_capacity(ChatModelCatalog *catalog) {
    if (catalog->count < catalog->capacity) return true;
    size_t capacity = catalog->capacity ? catalog->capacity * 2 : 64;
    if (capacity > CHAT_MODEL_CATALOG_MAX) capacity = CHAT_MODEL_CATALOG_MAX;
    if (capacity <= catalog->count) return false;
    ChatModelInfo *items = (ChatModelInfo *)realloc(catalog->items,
        capacity * sizeof *items);
    if (!items) return false;
    catalog->items = items;
    catalog->capacity = capacity;
    return true;
}

bool chat_model_catalog_contains(const ChatModelCatalog *catalog,
    const wchar_t *id) {
    if (!catalog || !id) return false;
    for (size_t i = 0; i < catalog->count; i++)
        if (!wcscmp(catalog->items[i].id, id)) return true;
    return false;
}

bool chat_model_catalog_append(ChatModelCatalog *catalog,
    const ChatModelInfo *info) {
    if (!catalog || !info || !info->id[0]) return false;
    if (!ensure_capacity(catalog)) return false;
    catalog->items[catalog->count++] = *info;
    return true;
}

/* Copies a decoded UTF-8 field into a fixed wide buffer, or leaves it empty
   on absence/decoding failure. `full_length` receives the decoded length
   before truncation so a too-long id is detected, not silently shortened.
   Returns false only on allocation failure. */
static bool decode_into(const char *json, const char *path, char *scratch,
    size_t scratch_size, wchar_t *out, size_t out_size, bool *present,
    size_t *full_length) {
    out[0] = 0;
    if (present) *present = false;
    if (full_length) *full_length = 0;
    if (!json_query_string(json, path, scratch, scratch_size)) return true;
    if (!scratch[0]) return true;
    wchar_t *wide = json_utf8_to_utf16(scratch, strlen(scratch));
    if (!wide) return false;
    if (present) *present = true;
    if (full_length) *full_length = wcslen(wide);
    wcsncpy(out, wide, out_size - 1);
    out[out_size - 1] = 0;
    free(wide);
    return true;
}

bool chat_model_catalog_parse(ChatModelCatalog *catalog, const char *json,
    ChatModelParseStats *stats) {
    ChatModelParseStats local = {0};
    if (stats) *stats = local;
    if (!catalog || !json) return false;
    size_t entries = 0;
    if (!json_query_array_length(json, "data", &entries)) return false;
    size_t scratch_size = strlen(json) + 1;
    char *scratch = (char *)malloc(scratch_size);
    if (!scratch) return false;
    ChatModelCatalog temp;
    chat_model_catalog_init(&temp);
    bool ok = true;
    for (size_t i = 0; i < entries && ok; i++) {
        char path[48];
        ChatModelInfo info;
        memset(&info, 0, sizeof info);
        info.context_length = -1;
        snprintf(path, sizeof path, "data[%lu].id", (unsigned long)i);
        bool present = false;
        size_t id_length = 0;
        if (!decode_into(json, path, scratch, scratch_size, info.id,
                CHAT_MODEL_TEXT, &present, &id_length)) { ok = false; break; }
        if (!present) { ++local.invalid; continue; }
        if (id_length >= CHAT_MODEL_TEXT) { ++local.too_long; continue; }
        snprintf(path, sizeof path, "data[%lu].name", (unsigned long)i);
        if (!decode_into(json, path, scratch, scratch_size, info.name,
                CHAT_MODEL_NAME_TEXT, NULL, NULL)) { ok = false; break; }
        snprintf(path, sizeof path, "data[%lu].context_length",
            (unsigned long)i);
        double value = 0;
        if (json_query_number(json, path, &value) && value >= 0 &&
            value < 2147483647.0)
            info.context_length = (int)value;
        if (chat_model_catalog_contains(&temp, info.id)) {
            ++local.duplicate;
            continue;
        }
        if (temp.count >= CHAT_MODEL_CATALOG_MAX) {
            ++local.truncated;
            continue;
        }
        if (!chat_model_catalog_append(&temp, &info)) { ok = false; break; }
        ++local.parsed;
    }
    free(scratch);
    if (!ok) {
        chat_model_catalog_dispose(&temp);
        return false;
    }
    chat_model_catalog_dispose(catalog);
    *catalog = temp;
    if (stats) *stats = local;
    return true;
}

static bool add_id(ChatModelCatalog *out, const wchar_t *id) {
    if (!id || !id[0] || wcslen(id) >= CHAT_MODEL_TEXT) return true;
    if (chat_model_catalog_contains(out, id)) return true;
    ChatModelInfo info;
    memset(&info, 0, sizeof info);
    info.context_length = -1;
    wcsncpy(info.id, id, CHAT_MODEL_TEXT - 1);
    return chat_model_catalog_append(out, &info);
}

bool chat_model_catalog_merged(const ChatModelCatalog *catalog,
    const wchar_t *current,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    ChatModelCatalog *out) {
    if (!out) return false;
    if (!add_id(out, current)) goto fail;
    for (int i = 0; i < history_count; i++)
        if (!add_id(out, history[i])) goto fail;
    if (catalog) {
        for (size_t i = 0; i < catalog->count; i++) {
            const ChatModelInfo *info = &catalog->items[i];
            if (chat_model_catalog_contains(out, info->id)) continue;
            if (!chat_model_catalog_append(out, info)) goto fail;
        }
    }
    return true;
fail:
    /* The contract is "left empty on allocation failure": release the items
       appended before the failing growth instead of leaking them. dispose
       leaves the initialized-empty state. */
    chat_model_catalog_dispose(out);
    return false;
}

static bool substring_fold(const wchar_t *text, const wchar_t *query,
    size_t query_length) {
    size_t length = wcslen(text);
    if (!query_length) return true;
    if (query_length > length || query_length > INT_MAX) return false;
    for (size_t i = 0; i + query_length <= length; i++)
        if (CompareStringOrdinal(text + i, (int)query_length, query,
                (int)query_length, TRUE) == CSTR_EQUAL) return true;
    return false;
}

size_t chat_model_catalog_filter(const ChatModelCatalog *catalog,
    const wchar_t *query, const ChatModelInfo **out, size_t max) {
    if (!catalog || !out) return 0;
    if (!query) query = L"";
    size_t query_length = wcslen(query);
    size_t matched = 0;
    for (size_t i = 0; i < catalog->count; i++) {
        const ChatModelInfo *info = &catalog->items[i];
        if (!substring_fold(info->id, query, query_length) &&
            !substring_fold(info->name, query, query_length)) continue;
        if (matched < max) out[matched] = info;
        ++matched;
    }
    return matched;
}
