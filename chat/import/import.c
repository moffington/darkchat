#include "chat/import/import.h"

#include "chat/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Largest timestamp/id the storage format accepts as an exact double. */
#define IMPORT_INT_MAX 9007199254740991.0

/* Shared decode state. `scratch` is one reusable UTF-8 buffer sized to the
   whole input copy; decoding a string never needs more than its raw span. */
typedef struct {
    char *scratch;
    size_t scratch_cap;
    int dest_count;
    int messages_added;
} ImportCtx;

/* Decodes the JSON string at `raw` strictly into ctx->scratch (NUL-terminated)
   and reports its byte length. Rejects unpaired surrogate escapes, escaped
   NUL, malformed escapes, and non-strings. */
static ChatImportStatus ctx_utf8(ImportCtx *ctx, const char *raw,
    size_t *bytes) {
    size_t length = 0;
    if (!json_decode_string_strict(raw, NULL, 0, &length))
        return CHAT_IMPORT_MALFORMED;
    if (length + 1 > ctx->scratch_cap) return CHAT_IMPORT_TOO_LARGE;
    if (!json_decode_string_strict(raw, ctx->scratch, ctx->scratch_cap, NULL))
        return CHAT_IMPORT_MALFORMED;
    if (bytes) *bytes = length;
    return CHAT_IMPORT_OK;
}

/* Strictly decodes the string at `raw` to a freshly allocated UTF-16 string.
   `max_units` is an exclusive upper bound on the decoded length, or 0 for no
   bound (the input cap still applies). The caller owns the result. */
static ChatImportStatus ctx_wide(ImportCtx *ctx, const char *raw,
    size_t max_units, wchar_t **out) {
    *out = NULL;
    size_t bytes = 0;
    ChatImportStatus status = ctx_utf8(ctx, raw, &bytes);
    if (status != CHAT_IMPORT_OK) return status;
    wchar_t *wide = json_utf8_to_utf16(ctx->scratch, bytes);
    if (!wide) return CHAT_IMPORT_OOM;
    if (max_units && wcslen(wide) >= max_units) {
        free(wide);
        return CHAT_IMPORT_TOO_LARGE;
    }
    *out = wide;
    return CHAT_IMPORT_OK;
}

/* Decodes an object key strictly into `key`. A key longer than the small
   comparison buffer is reported as unknown (`*known` false) rather than
   malformed; a syntactically invalid key is malformed. */
static ChatImportStatus ctx_key(ImportCtx *ctx, const char *raw, char *key,
    size_t key_capacity, bool *known) {
    size_t length = 0;
    if (!json_decode_string_strict(raw, NULL, 0, &length))
        return CHAT_IMPORT_MALFORMED;
    if (length + 1 > key_capacity) {
        *known = false;
        return CHAT_IMPORT_OK;
    }
    if (!json_decode_string_strict(raw, key, key_capacity, NULL))
        return CHAT_IMPORT_MALFORMED;
    *known = true;
    (void)ctx;
    return CHAT_IMPORT_OK;
}

static bool parse_integer(const char *value, double min, double max,
    int64_t *out) {
    double number = 0;
    if (json_value_kind(value) != JSON_VALUE_NUMBER ||
        !json_value_number(value, &number) ||
        number < min || number > max || floor(number) != number) return false;
    *out = (int64_t)number;
    return true;
}

static ChatImportStatus parse_timestamp(const char *value, int64_t *out) {
    return parse_integer(value, 1.0, IMPORT_INT_MAX, out)
        ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
}

enum {
    MSG_ROLE = 1u << 0,
    MSG_CREATED = 1u << 1,
    MSG_MODIFIED = 1u << 2,
    MSG_TEXT = 1u << 3,
    MSG_REASONING = 1u << 4,
    MSG_GENERATION = 1u << 5
};

static ChatImportStatus parse_role(ImportCtx *ctx, const char *value,
    ChatRole *role) {
    ChatImportStatus status = ctx_utf8(ctx, value, NULL);
    if (status != CHAT_IMPORT_OK) return status;
    if (!strcmp(ctx->scratch, "user")) *role = CHAT_ROLE_USER;
    else if (!strcmp(ctx->scratch, "assistant")) *role = CHAT_ROLE_ASSISTANT;
    else if (!strcmp(ctx->scratch, "system")) *role = CHAT_ROLE_SYSTEM;
    else if (!strcmp(ctx->scratch, "error")) *role = CHAT_ROLE_ERROR;
    else return CHAT_IMPORT_MALFORMED;
    return CHAT_IMPORT_OK;
}

#define MSG_REQUIRED (MSG_ROLE | MSG_CREATED | MSG_MODIFIED | MSG_TEXT | \
    MSG_GENERATION)

/* The loop records failures in `status` and never returns early, so the single
   tail free releases a decoded text/reasoning buffer on every path, including
   a duplicate-field or malformed return. */
static ChatImportStatus parse_message(ImportCtx *ctx, const char *object,
    Chat *staging, int conversation) {
    /* Capacity and id demand are known before any field is decoded; checking
       here keeps a doomed entry from allocating text it would then strand. */
    if (staging->conversations[conversation].message_count >= CHAT_MAX_MESSAGES)
        return CHAT_IMPORT_CAPACITY;
    if (staging->next_id >= CHAT_MAX_ID) return CHAT_IMPORT_CAPACITY;
    JsonCursor cursor;
    if (!json_cursor_object(&cursor, object)) return CHAT_IMPORT_MALFORMED;
    unsigned seen = 0;
    ChatRole role = CHAT_ROLE_USER;
    wchar_t *text = NULL, *reasoning = NULL;
    int64_t created_at = 0, modified_at = 0;
    ChatImportStatus status = CHAT_IMPORT_OK;
    while (status == CHAT_IMPORT_OK && json_cursor_next(&cursor)) {
        char key[32];
        bool known = false;
        status = ctx_key(ctx, json_cursor_key(&cursor), key, sizeof key,
            &known);
        if (status != CHAT_IMPORT_OK || !known) continue;
        const char *value = json_cursor_value(&cursor);
        if (!strcmp(key, "role")) {
            if (seen & MSG_ROLE) status = CHAT_IMPORT_MALFORMED;
            else { seen |= MSG_ROLE; status = parse_role(ctx, value, &role); }
        } else if (!strcmp(key, "created_at")) {
            if (seen & MSG_CREATED) status = CHAT_IMPORT_MALFORMED;
            else { seen |= MSG_CREATED;
                   status = parse_timestamp(value, &created_at); }
        } else if (!strcmp(key, "modified_at")) {
            if (seen & MSG_MODIFIED) status = CHAT_IMPORT_MALFORMED;
            else { seen |= MSG_MODIFIED;
                   status = parse_timestamp(value, &modified_at); }
        } else if (!strcmp(key, "text")) {
            if (seen & MSG_TEXT) status = CHAT_IMPORT_MALFORMED;
            else { seen |= MSG_TEXT; status = ctx_wide(ctx, value, 0, &text); }
        } else if (!strcmp(key, "reasoning")) {
            if (seen & MSG_REASONING) status = CHAT_IMPORT_MALFORMED;
            else { seen |= MSG_REASONING;
                   status = ctx_wide(ctx, value, 0, &reasoning); }
        } else if (!strcmp(key, "generation")) {
            if (seen & MSG_GENERATION) status = CHAT_IMPORT_MALFORMED;
            else {
                seen |= MSG_GENERATION;
                /* Required, and sanitized away: only the object shape of the
                   exported audit block is checked. */
                status = json_value_kind(value) == JSON_VALUE_OBJECT
                    ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
            }
        }
    }
    if (status == CHAT_IMPORT_OK && (seen & MSG_REQUIRED) != MSG_REQUIRED)
        status = CHAT_IMPORT_MALFORMED;
    if (status == CHAT_IMPORT_OK) {
        int index = chat_append_at(staging, conversation, role,
            text ? text : L"");
        if (index < 0) status = CHAT_IMPORT_OOM;
        else {
            ChatMessage *message =
                &staging->conversations[conversation].messages[index];
            if (reasoning && !chat_message_set_reasoning(message, reasoning))
                status = CHAT_IMPORT_OOM;
            else {
                message->created_at = created_at;
                message->modified_at = modified_at;
                if (role == CHAT_ROLE_ASSISTANT)
                    message->generation.state = CHAT_GENERATION_COMPLETE;
            }
        }
    }
    free(text);
    free(reasoning);
    return status;
}

enum {
    CONV_ID = 1u << 0,
    CONV_TITLE = 1u << 1,
    CONV_CREATED = 1u << 2,
    CONV_MODIFIED = 1u << 3,
    CONV_MESSAGES = 1u << 4,
    CONV_MODEL = 1u << 5,
    CONV_OLLAMA = 1u << 6,
    CONV_PROMPT = 1u << 7,
    CONV_PRESENT = 1u << 8
};

static ChatImportStatus parse_conversation(ImportCtx *ctx, const char *object,
    Chat *staging) {
    if (staging->conversation_count >=
        CHAT_MAX_CONVERSATIONS - ctx->dest_count)
        return CHAT_IMPORT_CAPACITY;
    int index = chat_new_conversation(staging);
    if (index < 0) return CHAT_IMPORT_CAPACITY;
    ChatConversation *conversation = &staging->conversations[index];
    /* Imported titles are authoritative: derivation from the first user
       message must never overwrite one. */
    conversation->renamed = true;

    JsonCursor cursor;
    if (!json_cursor_object(&cursor, object)) return CHAT_IMPORT_MALFORMED;
    unsigned seen = 0;
    int64_t created_at = 0, modified_at = 0;
    bool have_prompt_text = false, have_prompt_present = false;
    while (json_cursor_next(&cursor)) {
        char key[32];
        bool known = false;
        ChatImportStatus status = ctx_key(ctx, json_cursor_key(&cursor), key,
            sizeof key, &known);
        if (status != CHAT_IMPORT_OK) return status;
        if (!known) continue;
        const char *value = json_cursor_value(&cursor);
        if (!strcmp(key, "title")) {
            if (seen & CONV_TITLE) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_TITLE;
            wchar_t *wide = NULL;
            status = ctx_wide(ctx, value, CHAT_TITLE_TEXT, &wide);
            if (status == CHAT_IMPORT_OK && !wide[0])
                status = CHAT_IMPORT_MALFORMED;   /* empty title */
            if (status == CHAT_IMPORT_OK) wcscpy(conversation->title, wide);
            free(wide);
        } else if (!strcmp(key, "created_at")) {
            if (seen & CONV_CREATED) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_CREATED;
            status = parse_timestamp(value, &created_at);
        } else if (!strcmp(key, "modified_at")) {
            if (seen & CONV_MODIFIED) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_MODIFIED;
            status = parse_timestamp(value, &modified_at);
        } else if (!strcmp(key, "id")) {
            if (seen & CONV_ID) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_ID;
            int64_t ignored = 0;
            /* Shape-validated in the exported id domain, then ignored: every
               imported conversation gets a fresh id. */
            status = parse_integer(value, 1.0, (double)CHAT_MAX_ID, &ignored)
                ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
        } else if (!strcmp(key, "model") || !strcmp(key, "ollama_model")) {
            unsigned bit = key[0] == 'm' ? CONV_MODEL : CONV_OLLAMA;
            if (seen & bit) return CHAT_IMPORT_MALFORMED;
            seen |= bit;
            wchar_t *wide = NULL;
            status = ctx_wide(ctx, value, CHAT_MODEL_TEXT, &wide);
            if (status == CHAT_IMPORT_OK)
                wcscpy(bit == CONV_MODEL ? conversation->model
                                         : conversation->ollama_model, wide);
            free(wide);
        } else if (!strcmp(key, "system_prompt")) {
            if (seen & CONV_PROMPT) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_PROMPT;
            wchar_t *wide = NULL;
            status = ctx_wide(ctx, value, CHAT_COMPOSER_TEXT, &wide);
            if (status == CHAT_IMPORT_OK && !wide[0])
                status = CHAT_IMPORT_MALFORMED;   /* export emits non-empty */
            if (status == CHAT_IMPORT_OK &&
                !chat_conversation_set_system_prompt(staging, index, wide))
                status = CHAT_IMPORT_OOM;
            free(wide);
            have_prompt_text = status == CHAT_IMPORT_OK;
        } else if (!strcmp(key, "system_prompt_present")) {
            if (seen & CONV_PRESENT) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_PRESENT;
            bool present = false;
            status = json_value_bool(value, &present) && present
                ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
            if (status == CHAT_IMPORT_OK)
                conversation->system_prompt_present = true;
            have_prompt_present = status == CHAT_IMPORT_OK;
        } else if (!strcmp(key, "messages")) {
            if (seen & CONV_MESSAGES) return CHAT_IMPORT_MALFORMED;
            seen |= CONV_MESSAGES;
            if (json_value_kind(value) != JSON_VALUE_ARRAY)
                return CHAT_IMPORT_MALFORMED;
            JsonCursor array;
            if (!json_cursor_array(&array, value)) return CHAT_IMPORT_MALFORMED;
            while (json_cursor_next(&array)) {
                const char *element = json_cursor_value(&array);
                if (json_value_kind(element) != JSON_VALUE_OBJECT)
                    return CHAT_IMPORT_MALFORMED;
                status = parse_message(ctx, element, staging, index);
                if (status != CHAT_IMPORT_OK) return status;
                ++ctx->messages_added;
            }
            continue;
        } else {
            continue;   /* unknown field: ignored */
        }
        if (status != CHAT_IMPORT_OK) return status;
    }
    if ((seen & (CONV_ID | CONV_TITLE | CONV_CREATED | CONV_MODIFIED |
            CONV_MESSAGES)) !=
        (CONV_ID | CONV_TITLE | CONV_CREATED | CONV_MODIFIED | CONV_MESSAGES))
        return CHAT_IMPORT_MALFORMED;
    if (have_prompt_text && have_prompt_present) return CHAT_IMPORT_MALFORMED;
    conversation->created_at = created_at;
    conversation->modified_at = modified_at;
    return CHAT_IMPORT_OK;
}

enum {
    ROOT_FORMAT = 1u << 0,
    ROOT_VERSION = 1u << 1,
    ROOT_CONVERSATIONS = 1u << 2
};

static ChatImportStatus parse_document(ImportCtx *ctx, const char *document,
    Chat *staging) {
    JsonCursor root;
    if (!json_cursor_object(&root, document)) return CHAT_IMPORT_MALFORMED;
    unsigned seen = 0;
    const char *conversations = NULL;
    while (json_cursor_next(&root)) {
        char key[32];
        bool known = false;
        ChatImportStatus status = ctx_key(ctx, json_cursor_key(&root), key,
            sizeof key, &known);
        if (status != CHAT_IMPORT_OK) return status;
        if (!known) continue;
        const char *value = json_cursor_value(&root);
        if (!strcmp(key, "format")) {
            if (seen & ROOT_FORMAT) return CHAT_IMPORT_MALFORMED;
            seen |= ROOT_FORMAT;
            status = ctx_utf8(ctx, value, NULL);
            if (status == CHAT_IMPORT_OK &&
                strcmp(ctx->scratch, "darkchat.export") != 0)
                status = CHAT_IMPORT_MALFORMED;
        } else if (!strcmp(key, "version")) {
            if (seen & ROOT_VERSION) return CHAT_IMPORT_MALFORMED;
            seen |= ROOT_VERSION;
            double version = 0;
            status = json_value_kind(value) == JSON_VALUE_NUMBER &&
                json_value_number(value, &version) && version == 1.0
                ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
        } else if (!strcmp(key, "conversations")) {
            if (seen & ROOT_CONVERSATIONS) return CHAT_IMPORT_MALFORMED;
            seen |= ROOT_CONVERSATIONS;
            if (json_value_kind(value) != JSON_VALUE_ARRAY)
                return CHAT_IMPORT_MALFORMED;
            conversations = value;
            continue;
        } else {
            continue;   /* unknown field: ignored */
        }
        if (status != CHAT_IMPORT_OK) return status;
    }
    if ((seen & (ROOT_FORMAT | ROOT_VERSION | ROOT_CONVERSATIONS)) !=
        (ROOT_FORMAT | ROOT_VERSION | ROOT_CONVERSATIONS))
        return CHAT_IMPORT_MALFORMED;
    JsonCursor array;
    if (!json_cursor_array(&array, conversations)) return CHAT_IMPORT_MALFORMED;
    bool any = false;
    while (json_cursor_next(&array)) {
        const char *element = json_cursor_value(&array);
        if (json_value_kind(element) != JSON_VALUE_OBJECT)
            return CHAT_IMPORT_MALFORMED;
        any = true;
        ChatImportStatus status = parse_conversation(ctx, element, staging);
        if (status != CHAT_IMPORT_OK) return status;
    }
    return any ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
}

/* Infallible ownership transfer: moves each staged conversation into the
   destination tail and zeroes the staging slots so the temporary dispose
   cannot free the moved allocations twice. The destination's active selection
   and status are deliberately left untouched. */
static void transfer_conversations(Chat *chat, Chat *staging) {
    int added = staging->conversation_count;
    int start = chat->conversation_count;
    for (int i = 0; i < added; i++) {
        chat->conversations[start + i] = staging->conversations[i];
        memset(&staging->conversations[i], 0, sizeof staging->conversations[i]);
    }
    chat->conversation_count = start + added;
    chat->next_id = staging->next_id;
}

ChatImportStatus chat_import_json(Chat *chat, const char *json, size_t length,
    ChatImportStats *stats) {
    if (stats) *stats = (ChatImportStats){0};
    if (!chat || !json || length == 0) return CHAT_IMPORT_MALFORMED;
    if (length > CHAT_IMPORT_LIMIT) return CHAT_IMPORT_TOO_LARGE;
    /* Never trust the caller's buffer: copy exactly `length` bytes into an
       owned terminated buffer and reject an embedded NUL, which would
       otherwise silently truncate every C-string scan. */
    char *copy = (char *)malloc(length + 1);
    if (!copy) return CHAT_IMPORT_OOM;
    memcpy(copy, json, length);
    copy[length] = 0;
    if (memchr(copy, 0, length) != NULL || !json_utf8_valid(copy, length) ||
        !json_validate(copy)) {
        free(copy);
        return CHAT_IMPORT_MALFORMED;
    }
    char *scratch = (char *)malloc(length + 1);
    if (!scratch) { free(copy); return CHAT_IMPORT_OOM; }

    Chat *staging = (Chat *)malloc(sizeof *staging);
    if (!staging) { free(scratch); free(copy); return CHAT_IMPORT_OOM; }
    memset(staging, 0, sizeof *staging);
    staging->next_id = chat->next_id;

    ImportCtx ctx = { scratch, length + 1, chat->conversation_count, 0 };
    ChatImportStatus status = parse_document(&ctx, copy, staging);
    free(scratch);
    free(copy);
    if (status != CHAT_IMPORT_OK) {
        chat_dispose(staging);
        free(staging);
        return status;
    }
    if (chat->conversation_count >
        CHAT_MAX_CONVERSATIONS - staging->conversation_count) {
        chat_dispose(staging);
        free(staging);
        return CHAT_IMPORT_CAPACITY;
    }
    transfer_conversations(chat, staging);
    if (stats) {
        stats->conversations_added = staging->conversation_count;
        stats->messages_added = ctx.messages_added;
    }
    chat_dispose(staging);
    free(staging);
    return CHAT_IMPORT_OK;
}
