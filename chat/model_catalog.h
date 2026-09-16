#ifndef DARKCHAT_MODEL_CATALOG_H
#define DARKCHAT_MODEL_CATALOG_H

/* OpenRouter model catalog: a bounded, transient list of selectable models
   parsed from GET /api/v1/models. This module owns no Win32 handle and no
   network state; the worker and the picker build on it. The catalog is never
   persisted: a missing or stale catalog falls back to the model history. */
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "chat.h"

/* Display name is optional and may be longer than the routable id. */
#define CHAT_MODEL_NAME_TEXT 128
/* Hard ceiling on retained entries; the parser reports the excess as
   truncated rather than growing without bound. */
#define CHAT_MODEL_CATALOG_MAX 4096

typedef struct {
    /* Routable id (what a request sends); required and shorter than
       CHAT_MODEL_TEXT. `name` is a display label; empty means display `id`. */
    wchar_t id[CHAT_MODEL_TEXT];
    wchar_t name[CHAT_MODEL_NAME_TEXT];
    /* -1 when the provider omitted context_length or gave a non-number. */
    int context_length;
} ChatModelInfo;

/* Why a raw entry was not retained. `parsed` counts retained entries. */
typedef struct {
    size_t parsed;
    size_t invalid;     /* id missing, empty, or not a string */
    size_t duplicate;   /* id already retained */
    size_t too_long;    /* id would not fit CHAT_MODEL_TEXT */
    size_t truncated;   /* valid entry beyond CHAT_MODEL_CATALOG_MAX */
} ChatModelParseStats;

typedef struct {
    ChatModelInfo *items;
    size_t count, capacity;
} ChatModelCatalog;

void chat_model_catalog_init(ChatModelCatalog *catalog);
void chat_model_catalog_dispose(ChatModelCatalog *catalog);

/* Parses the `data` array of an OpenRouter models response. The destination is
   replaced transactionally: on any allocation failure or a malformed root it
   is left exactly as it was and false is returned. `stats` is always filled
   when non-NULL. */
bool chat_model_catalog_parse(ChatModelCatalog *catalog, const char *json,
    ChatModelParseStats *stats);

/* Appends when the id is non-empty, fits and is not already present. Returns
   false only on allocation failure. */
bool chat_model_catalog_append(ChatModelCatalog *catalog,
    const ChatModelInfo *info);
bool chat_model_catalog_contains(const ChatModelCatalog *catalog,
    const wchar_t *id);

/* Builds the picker's merged view: the current model first when it is set and
   absent, then the model history in MRU order, then the catalog in API order.
   Entries are deduplicated by exact id, first occurrence wins, and every id
   that does not fit CHAT_MODEL_TEXT is skipped. `out` must be initialized and
   is left empty on allocation failure (which returns false). */
bool chat_model_catalog_merged(const ChatModelCatalog *catalog,
    const wchar_t *current,
    wchar_t history[][CHAT_MODEL_TEXT], int history_count,
    ChatModelCatalog *out);

/* Number of entries matching `query`, written as pointers to `out` (up to
   `max`). An empty query matches every entry. Matching is an ordinal,
   locale-independent Unicode case-insensitive substring over both the id and
   the display name. Returns the full match count even when it exceeds `max`
   (only the first `max` are written). */
size_t chat_model_catalog_filter(const ChatModelCatalog *catalog,
    const wchar_t *query, const ChatModelInfo **out, size_t max);

#endif
