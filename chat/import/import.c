#include "chat/import/import.h"

#include "chat/core/image_policy.h"
#include "chat/core/sha256.h"
#include "chat/export/package.h"
#include "chat/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Largest timestamp/id the storage format accepts as an exact double. */
#define IMPORT_INT_MAX 9007199254740991.0

/* One attachment a document described, decoded and digest-verified but not yet
   durable. Held only for the length of the parse, which is why the write pass
   runs after every document has validated: a malformed document costs no
   bytes on disk at all. */
typedef struct {
    unsigned char *bytes;
    size_t length;
    uint64_t id;
    char digest[65];
    char mime[32];
    /* Display hints. These belong on the RECORD, not only on the part: the
       exporter and the transcript read the record, so leaving them off it would
       drop the dimensions the document carried the moment anything was
       re-exported or the snapshot was reloaded. */
    uint32_t pixel_width, pixel_height;
    wchar_t display_name[64];
} StagedBlob;

/* Shared decode state. `scratch` is one reusable UTF-8 buffer sized to the
   whole input copy; decoding a string never needs more than its raw span. */
typedef struct {
    char *scratch;
    size_t scratch_cap;
    int dest_count;
    int messages_added;
    /* Declared for the current document: every image part found, in order. */
    const ChatImportBlobs *blobs;
    /* Schema version as declared. Version 1 cannot carry a `content` array by
       construction, so a version 1 document that carries one contradicts
       itself. */
    int version;
    /* JSON exports embed bytes, so a `file` reference in a JSON document is a
       format error rather than something to resolve. Markdown is the only
       format whose payload points at sidecars. */
    bool allow_file;
    StagedBlob *staged;
    size_t staged_count, staged_capacity;
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
    MSG_GENERATION = 1u << 5,
    MSG_CONTENT = 1u << 6
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

/* --- message content parts ------------------------------------------------ */

/* Appends one verified attachment to the staging list. Ownership of `bytes`
   transfers on success only; on failure the caller keeps it.

   The id is drawn from the staging counter here, during parsing, rather than
   in the write pass. That is safe and simpler than deferring it: the staging
   Chat is discarded on every failure, so an id consumed by a rejected document
   is simply never published and the destination's counter does not move. It
   also means the message's parts are complete as soon as parsing finishes, so
   no rebuild pass is needed. */
static ChatImportStatus stage_append(ImportCtx *ctx, Chat *staging,
    unsigned char *bytes, size_t length, const char *mime,
    const wchar_t *display_name, uint32_t width, uint32_t height,
    uint64_t *out_id) {
    if (staging->next_id >= CHAT_MAX_ID) return CHAT_IMPORT_CAPACITY;
    if (ctx->staged_count == ctx->staged_capacity) {
        size_t capacity = ctx->staged_capacity ? ctx->staged_capacity * 2 : 8;
        StagedBlob *grown = (StagedBlob *)realloc(ctx->staged,
            capacity * sizeof *grown);
        if (!grown) return CHAT_IMPORT_OOM;
        ctx->staged = grown;
        ctx->staged_capacity = capacity;
    }
    StagedBlob *slot = &ctx->staged[ctx->staged_count];
    memset(slot, 0, sizeof *slot);
    slot->bytes = bytes;
    slot->length = length;
    /* The digest is computed from the bytes the document carried, so a
       mismatch against what it declared is caught here -- before anything is
       durable -- rather than after a write that would have to be undone. */
    sha256_hex(bytes, length, slot->digest);
    size_t mime_length = strlen(mime);
    memcpy(slot->mime, mime, mime_length + 1);
    slot->pixel_width = width;
    slot->pixel_height = height;
    wcsncpy(slot->display_name, display_name, 63);
    slot->display_name[63] = 0;
    slot->id = ++staging->next_id;
    *out_id = slot->id;
    ctx->staged_count++;
    return CHAT_IMPORT_OK;
}

/* The MIME shape an attachment record must have, checked here rather than at
   chat_attachment_add time. Rejecting it during parsing means a document that
   could never produce a valid record is refused before a byte is written. */
static bool mime_shape_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= 32) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return false;
    }
    return true;
}

static void staged_release(ImportCtx *ctx) {
    for (size_t i = 0; i < ctx->staged_count; i++) free(ctx->staged[i].bytes);
    free(ctx->staged);
    ctx->staged = NULL;
    ctx->staged_count = 0;
    ctx->staged_capacity = 0;
}

/* Reads an image part's bytes from whichever place the document names. The
   declared digest is verified here, before staging, so no write ever happens
   for content that does not match what the document promised. */
static ChatImportStatus resolve_image_bytes(ImportCtx *ctx, const char *sha256,
    const char *data, const char *file, unsigned char **out, size_t *out_n) {
    unsigned char *bytes = NULL;
    size_t length = 0;
    char folder[80], basename[80], actual[65];

    *out = NULL;
    *out_n = 0;
    if (data) {
        /* Base64 is pure ASCII, so it is read through the strict string
           decoder and never widened: a 27 MB payload widened to UTF-16 would
           cost twice the memory for no gain. */
        size_t payload = 0;
        if (!json_decode_string_strict(data, NULL, 0, &payload))
            return CHAT_IMPORT_MALFORMED;
        char *encoded = (char *)malloc(payload + 1);
        if (!encoded) return CHAT_IMPORT_OOM;
        if (!json_decode_string_strict(data, encoded, payload + 1, NULL)) {
            free(encoded);
            return CHAT_IMPORT_MALFORMED;
        }
        bytes = json_base64_decode(encoded, payload, &length);
        free(encoded);
        if (!bytes || !length) {
            free(bytes);
            return bytes ? CHAT_IMPORT_MALFORMED : CHAT_IMPORT_OOM;
        }
    } else {
        if (!ctx->allow_file || !ctx->blobs || !ctx->blobs->read_file)
            return CHAT_IMPORT_MALFORMED;
        if (!chat_import_sidecar_reference(file, strlen(file), folder, basename))
            return CHAT_IMPORT_MALFORMED;
        if (!ctx->blobs->read_file(ctx->blobs->user, folder, basename,
                &bytes, &length))
            return CHAT_IMPORT_ATTACHMENT_IO;
        if (!bytes || !length) {
            if (bytes && ctx->blobs->discard)
                ctx->blobs->discard(ctx->blobs->user, bytes, length);
            return CHAT_IMPORT_ATTACHMENT_IO;
        }
    }
    if (length > CHAT_ATTACHMENT_MAX_BYTES) {
        if (ctx->blobs && ctx->blobs->discard)
            ctx->blobs->discard(ctx->blobs->user, bytes, length);
        else
            free(bytes);
        return CHAT_IMPORT_TOO_LARGE;
    }
    sha256_hex(bytes, length, actual);
    if (strcmp(actual, sha256) != 0) {
        if (ctx->blobs && ctx->blobs->discard)
            ctx->blobs->discard(ctx->blobs->user, bytes, length);
        else
            free(bytes);
        return CHAT_IMPORT_MALFORMED;
    }
    *out = bytes;
    *out_n = length;
    return CHAT_IMPORT_OK;
}

/* The `content` array. Present means authoritative: the parts are the message,
   and `text` must be exactly their plain-text projection, or the document
   contradicts itself. The v1 shape is [TEXT?, IMAGE...], so the single leading
   TEXT part is the whole message text and every image follows it -- the same
   rule the snapshot decoder enforces, which is what keeps one reconstruction
   routine serving both. */
static ChatImportStatus parse_content(ImportCtx *ctx, const char *array,
    ChatMessage *message, Chat *staging) {
    JsonCursor parts;
    bool has_text_part = false;
    size_t images = 0;
    if (!json_cursor_array(&parts, array)) return CHAT_IMPORT_MALFORMED;
    while (json_cursor_next(&parts)) {
        const char *object = json_cursor_value(&parts);
        char type[16] = {0}, mime[32] = {0}, sha256[65] = {0};
        wchar_t display_name[64];
        /* A `file` reference is decoded here: the raw span still carries its
           quotes and escapes, so handing it to the shape check as-is would
           test the wrong bytes. */
        char file_reference[192] = {0};
        const char *data = NULL;
        int64_t width = 0, height = 0;
        uint8_t flags = 0;
        bool seen_type = false, seen_flags = false, seen_mime = false;
        bool seen_width = false, seen_height = false, seen_name = false;
        bool seen_sha = false, seen_data = false, seen_file = false;
        bool seen_text = false;
        unsigned char *bytes = NULL;
        size_t length = 0;
        ChatImportStatus status;
        ChatImagePart image;

        if (json_value_kind(object) != JSON_VALUE_OBJECT)
            return CHAT_IMPORT_MALFORMED;
        JsonCursor cursor;
        if (!json_cursor_object(&cursor, object)) return CHAT_IMPORT_MALFORMED;
        while (json_cursor_next(&cursor)) {
            char key[24];
            bool known = false;
            status = ctx_key(ctx, json_cursor_key(&cursor), key, sizeof key,
                &known);
            if (status != CHAT_IMPORT_OK || !known) continue;
            const char *value = json_cursor_value(&cursor);
            if (!strcmp(key, "type")) {
                size_t n = 0;
                if (seen_type || ctx_utf8(ctx, value, &n) != CHAT_IMPORT_OK ||
                    n + 1 > sizeof type) return CHAT_IMPORT_MALFORMED;
                if (!json_decode_string_strict(value, type, sizeof type, NULL))
                    return CHAT_IMPORT_MALFORMED;
                seen_type = true;
            } else if (!strcmp(key, "flags")) {
                int64_t v = 0;
                if (seen_flags || !parse_integer(value, 0,
                        (double)CHAT_PART_FLAG_MASK, &v))
                    return CHAT_IMPORT_MALFORMED;
                flags = (uint8_t)v;
                seen_flags = true;
            } else if (!strcmp(key, "mime")) {
                size_t n = 0;
                if (seen_mime || ctx_utf8(ctx, value, &n) != CHAT_IMPORT_OK ||
                    n + 1 > sizeof mime) return CHAT_IMPORT_MALFORMED;
                if (!json_decode_string_strict(value, mime, sizeof mime, NULL))
                    return CHAT_IMPORT_MALFORMED;
                seen_mime = true;
            } else if (!strcmp(key, "sha256")) {
                size_t n = 0;
                if (seen_sha || ctx_utf8(ctx, value, &n) != CHAT_IMPORT_OK ||
                    n + 1 > sizeof sha256) return CHAT_IMPORT_MALFORMED;
                if (!json_decode_string_strict(value, sha256,
                        sizeof sha256, NULL))
                    return CHAT_IMPORT_MALFORMED;
                seen_sha = true;
            } else if (!strcmp(key, "pixel_width")) {
                if (seen_width ||
                    !parse_integer(value, 0, (double)UINT32_MAX, &width))
                    return CHAT_IMPORT_MALFORMED;
                seen_width = true;
            } else if (!strcmp(key, "pixel_height")) {
                if (seen_height ||
                    !parse_integer(value, 0, (double)UINT32_MAX, &height))
                    return CHAT_IMPORT_MALFORMED;
                seen_height = true;
            } else if (!strcmp(key, "display_name")) {
                wchar_t *wide = NULL;
                status = ctx_wide(ctx, value, 64, &wide);
                if (status != CHAT_IMPORT_OK) return status;
                wcscpy(display_name, wide);
                free(wide);
                seen_name = true;
            } else if (!strcmp(key, "data")) {
                if (seen_data || json_value_kind(value) != JSON_VALUE_STRING)
                    return CHAT_IMPORT_MALFORMED;
                data = value;
                seen_data = true;
            } else if (!strcmp(key, "file")) {
                size_t n = 0;
                if (seen_file || json_value_kind(value) != JSON_VALUE_STRING)
                    return CHAT_IMPORT_MALFORMED;
                if (ctx_utf8(ctx, value, &n) != CHAT_IMPORT_OK ||
                    n + 1 > sizeof file_reference)
                    return CHAT_IMPORT_MALFORMED;
                if (!json_decode_string_strict(value, file_reference,
                        sizeof file_reference, NULL))
                    return CHAT_IMPORT_MALFORMED;
                seen_file = true;
            } else if (!strcmp(key, "text")) {
                wchar_t *wide = NULL;
                /* The projection must agree with the message's own text, or
                   the document describes two different messages. */
                status = ctx_wide(ctx, value, 0, &wide);
                if (status != CHAT_IMPORT_OK) return status;
                bool same = !wcscmp(wide, chat_message_text(message));
                free(wide);
                if (seen_text || !same) return CHAT_IMPORT_MALFORMED;
                seen_text = true;
            }
        }
        if (!seen_type) return CHAT_IMPORT_MALFORMED;
        if (flags && strcmp(type, "image")) return CHAT_IMPORT_MALFORMED;

        if (!strcmp(type, "text")) {
            /* Only one leading text part, and only over a non-empty
               projection: a text-only array is not a shape this exporter
               produces, so accepting one would mean two ways to say the same
               thing. */
            if (has_text_part || images || flags || !seen_text)
                return CHAT_IMPORT_MALFORMED;
            if (!chat_message_text(message)[0]) return CHAT_IMPORT_MALFORMED;
            has_text_part = true;
            continue;
        }
        if (strcmp(type, "image")) return CHAT_IMPORT_MALFORMED;
        /* An image may not lead a message whose text it is not part of. */
        if (!has_text_part && chat_message_text(message)[0])
            return CHAT_IMPORT_MALFORMED;
        if (seen_data == seen_file) return CHAT_IMPORT_MALFORMED;  /* neither or both */
        if (!seen_sha || !seen_mime) return CHAT_IMPORT_MALFORMED;
        if (!mime_shape_ok(mime)) return CHAT_IMPORT_MALFORMED;

        status = resolve_image_bytes(ctx, sha256, data,
            seen_file ? file_reference : NULL, &bytes, &length);
        if (status != CHAT_IMPORT_OK) return status;
        uint64_t id = 0;
        status = stage_append(ctx, staging, bytes, length, mime,
            seen_name ? display_name : L"", (uint32_t)width, (uint32_t)height,
            &id);
        if (status != CHAT_IMPORT_OK) return status;

        /* The part carries the same values the attachment record will, so the
           two agree by construction: the record is what the exporter and the
           request path read, and this is only the message's copy. */
        memset(&image, 0, sizeof image);
        image.attachment_id = id;
        image.pixel_width = (uint32_t)width;
        image.pixel_height = (uint32_t)height;
        memcpy(image.mime, mime, strlen(mime) + 1);
        if (seen_name) wcsncpy(image.display_name, display_name, 63);
        /* Over the part cap is a malformed document, not an allocation
           failure: add_image rejects at CHAT_MAX_PARTS. */
        if (!chat_message_add_image(message, &image, flags))
            return images + 1 > CHAT_MAX_PARTS ? CHAT_IMPORT_MALFORMED
                                               : CHAT_IMPORT_OOM;
        images++;
    }
    /* An array of only text parts carries no information the message does not
       already have, and the exporter never emits one. */
    return images ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
}

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
    const char *content = NULL;
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
        } else if (!strcmp(key, "content")) {
            if (seen & MSG_CONTENT) status = CHAT_IMPORT_MALFORMED;
            else if (ctx->version < 2) status = CHAT_IMPORT_MALFORMED;
            else {
                seen |= MSG_CONTENT;
                content = value;
            }
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
            else if (content)
                status = parse_content(ctx, content, message, staging);
            if (status == CHAT_IMPORT_OK) {
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
            /* Exactly 1 or 2, the versions this build emits. A higher version
               is a document a later build wrote, and rejecting it here is what
               keeps an old build from importing content it would silently drop
               on its next save. */
            status = json_value_kind(value) == JSON_VALUE_NUMBER &&
                json_value_number(value, &version) &&
                (version == 1.0 || version == 2.0)
                ? CHAT_IMPORT_OK : CHAT_IMPORT_MALFORMED;
            if (status == CHAT_IMPORT_OK) ctx->version = (int)version;
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
   and status are deliberately left untouched.

   Attachment records move separately, in merge_attachments, which runs BEFORE
   this and can still fail. Keeping the only fallible step ahead of the commit
   is what lets the destination stay untouched on every failure path. */
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

/* Appends the staged attachment records to the destination and releases the
   staging array, so the dispose that follows cannot free records the
   destination now owns. Ids come from the staging counter and no existing
   record can hold one, so an append fails only on allocation -- which is why
   this is attempted before the conversation transfer, while a failure can
   still be reported with the destination untouched. */
static ChatImportStatus merge_attachments(Chat *chat, Chat *staging) {
    /* Room for every record is taken BEFORE the first one is appended. Adding
       them one at a time would let an allocation failure partway through leave
       the destination holding some of this import's records and not the rest,
       with a message naming an id the table does not have. Reserving once
       makes each add below unable to fail on allocation, so the destination
       either gains the whole set or none of it. */
    if (staging->attachment_count &&
        !chat_attachment_reserve(chat,
            chat->attachment_count + staging->attachment_count))
        return CHAT_IMPORT_OOM;
    for (size_t i = 0; i < staging->attachment_count; i++)
        if (!chat_attachment_add(chat, &staging->attachments[i]))
            return CHAT_IMPORT_OOM;   /* unreachable once reserved */
    free(staging->attachments);
    staging->attachments = NULL;
    staging->attachment_count = 0;
    staging->attachment_capacity = 0;
    return CHAT_IMPORT_OK;
}

/* Makes every staged attachment durable and publishes its metadata record.
   Reached only after the whole document validated, so a malformed document
   writes nothing at all and needs no reclamation. A failure partway through is
   the only path that leaves bytes behind, and the caller's live-set sweep is
   what reclaims them. */
static ChatImportStatus write_staged_blobs(ImportCtx *ctx, Chat *staging) {
    if (!ctx->staged_count) return CHAT_IMPORT_OK;
    if (!ctx->blobs || !ctx->blobs->stage) return CHAT_IMPORT_ATTACHMENT_IO;
    for (size_t i = 0; i < ctx->staged_count; i++) {
        const StagedBlob *blob = &ctx->staged[i];
        char stored[65] = {0};
        bool created = false;
        ChatAttachmentMeta meta;
        if (!ctx->blobs->stage(ctx->blobs->user, blob->bytes, blob->length,
                stored, &created))
            return CHAT_IMPORT_ATTACHMENT_IO;
        /* The store hashes exactly the bytes it was handed, so this can only
           disagree if a callback ignored its input. Checked rather than
           assumed, because a record naming a digest it does not hold is a
           dangling reference the snapshot would happily encode. */
        if (strcmp(stored, blob->digest) != 0)
            return CHAT_IMPORT_ATTACHMENT_IO;
        memset(&meta, 0, sizeof meta);
        meta.id = blob->id;
        memcpy(meta.digest, blob->digest, 65);
        memcpy(meta.mime, blob->mime, strlen(blob->mime) + 1);
        meta.bytes = blob->length;
        meta.pixel_width = blob->pixel_width;
        meta.pixel_height = blob->pixel_height;
        wmemcpy(meta.display_name, blob->display_name,
            wcslen(blob->display_name) + 1);
        if (!chat_attachment_add(staging, &meta))
            return CHAT_IMPORT_OOM;
    }
    return CHAT_IMPORT_OK;
}

/* Allocates the detached staging Chat shared by every import format. Its id
   counter starts where the destination's does so ids are fresh but the
   destination is not touched until the transfer commits. */
static Chat *import_staging_new(const Chat *chat) {
    Chat *staging = (Chat *)malloc(sizeof *staging);
    if (!staging) return NULL;
    memset(staging, 0, sizeof *staging);
    staging->next_id = chat->next_id;
    return staging;
}

static void import_staging_abort(Chat *staging) {
    chat_dispose(staging);
    free(staging);
}

/* Infallible commit: capacity was preflighted by the format parser, the
   conversations move into the destination tail and `stats` reports the work.
   Ownership transfer zeroes the staging slots so disposing the staging Chat
   cannot free the moved allocations twice. */
static ChatImportStatus import_staging_finish(Chat *chat, Chat *staging,
    int messages_added, int attachments_added, ChatImportStats *stats) {
    if (chat->conversation_count >
        CHAT_MAX_CONVERSATIONS - staging->conversation_count) {
        import_staging_abort(staging);
        return CHAT_IMPORT_CAPACITY;
    }
    /* Records first: this is the last step that can fail, and until it
       succeeds nothing has moved into the destination. */
    ChatImportStatus status = merge_attachments(chat, staging);
    if (status != CHAT_IMPORT_OK) {
        import_staging_abort(staging);
        return status;
    }
    transfer_conversations(chat, staging);
    if (stats) {
        stats->conversations_added = staging->conversation_count;
        stats->messages_added = messages_added;
        stats->attachments_added = attachments_added;
    }
    import_staging_abort(staging);
    return CHAT_IMPORT_OK;
}

/* Shared entry point for both formats. `allow_file` is the only difference: a
   Markdown package's payload names sidecars beside its .md, while a JSON
   export carries its bytes inline. */
static ChatImportStatus chat_import_payload(Chat *chat, const char *json,
    size_t length, const ChatImportBlobs *blobs, ChatImportStats *stats,
    bool allow_file) {
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

    Chat *staging = import_staging_new(chat);
    if (!staging) { free(scratch); free(copy); return CHAT_IMPORT_OOM; }

    ImportCtx ctx = { scratch, length + 1, chat->conversation_count, 0,
        blobs, 1, allow_file, NULL, 0, 0 };
    ChatImportStatus status = parse_document(&ctx, copy, staging);
    int attachments = (int)ctx.staged_count;
    free(scratch);
    free(copy);
    if (status == CHAT_IMPORT_OK)
        status = write_staged_blobs(&ctx, staging);
    /* Every decoded buffer is released here regardless of outcome; anything
       already handed to the store is the caller's to reclaim. */
    staged_release(&ctx);
    if (status != CHAT_IMPORT_OK) {
        import_staging_abort(staging);
        return status;
    }
    return import_staging_finish(chat, staging, ctx.messages_added,
        attachments, stats);
}

ChatImportStatus chat_import_json(Chat *chat, const char *json, size_t length,
    const ChatImportBlobs *blobs, ChatImportStats *stats) {
    return chat_import_payload(chat, json, length, blobs, stats, false);
}

/* --- Markdown import ----------------------------------------------------- */

static bool title_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r';
}

/* Copies at most capacity-1 units of source[0..length), dropping a trailing
   high surrogate left by a truncation that fell inside a surrogate pair. */
static void title_copy_span(wchar_t *out, size_t capacity, const wchar_t *src,
    size_t length) {
    if (!out || capacity == 0) return;
    if (!src) length = 0;
    size_t n = length;
    if (n > capacity - 1) n = capacity - 1;
    if (n > 0 && src[n - 1] >= 0xd800 && src[n - 1] <= 0xdbff) n--;
    for (size_t i = 0; i < n; i++) out[i] = src[i];
    out[n] = 0;
}

void chat_import_copy_title(wchar_t *out, size_t capacity,
    const wchar_t *source, size_t length) {
    title_copy_span(out, capacity, source, length);
}

/* First line beginning exactly "# ": its text is copied (trailing CR/space/tab
   removed) into `out`, or `out` is left empty when no non-empty heading
   exists. This is a deliberately simple, fence-unaware scan. */
static void markdown_heading_title(const wchar_t *text, wchar_t *out,
    size_t capacity) {
    if (out && capacity) out[0] = 0;
    if (!text || !out || capacity == 0) return;
    const wchar_t *p = text;
    while (*p) {
        const wchar_t *line = p;
        const wchar_t *eol = line;
        while (*eol && *eol != L'\n') eol++;
        if (line[0] == L'#' && line[1] == L' ') {
            const wchar_t *start = line + 2;
            const wchar_t *end = eol;
            while (end > start && title_space(end[-1])) end--;
            if (end > start) {
                title_copy_span(out, capacity, start, (size_t)(end - start));
                return;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
}

/* The payload-less path: one conversation with one verbatim user message. */
static ChatImportStatus import_markdown_fallback(Chat *chat,
    const char *markdown, size_t length, const wchar_t *fallback_title,
    ChatImportStats *stats) {
    if (memchr(markdown, 0, length) != NULL ||
        !json_utf8_valid(markdown, length))
        return CHAT_IMPORT_MALFORMED;
    /* Capacity is decided before any allocation or staging mutation: the
       fallback consumes one conversation id and one message id, so with both
       reserved a later append failure can only be an allocation failure. */
    if (chat->conversation_count >= CHAT_MAX_CONVERSATIONS)
        return CHAT_IMPORT_CAPACITY;
    if (chat->next_id > CHAT_MAX_ID - 2)
        return CHAT_IMPORT_CAPACITY;
    wchar_t *text = json_utf8_to_utf16(markdown, length);
    if (!text) return CHAT_IMPORT_OOM;

    Chat *staging = import_staging_new(chat);
    if (!staging) { free(text); return CHAT_IMPORT_OOM; }
    int index = chat_new_conversation(staging);
    if (index < 0) {   /* unreachable after the capacity preflight */
        import_staging_abort(staging);
        free(text);
        return CHAT_IMPORT_OOM;
    }
    ChatConversation *conversation = &staging->conversations[index];
    /* Imported titles are authoritative: deriving from the first user message
       must never overwrite one. */
    conversation->renamed = true;
    wchar_t title[CHAT_TITLE_TEXT];
    markdown_heading_title(text, title, CHAT_TITLE_TEXT);
    if (!title[0]) {
        chat_import_copy_title(title, CHAT_TITLE_TEXT,
            fallback_title, fallback_title ? wcslen(fallback_title) : 0);
        if (!title[0]) wcscpy(title, L"Imported conversation");
    }
    wcscpy(conversation->title, title);
    if (chat_append_at(staging, index, CHAT_ROLE_USER, text) < 0) {
        import_staging_abort(staging);
        free(text);
        return CHAT_IMPORT_OOM;
    }
    free(text);
    return import_staging_finish(chat, staging, 1, 0, stats);
}

ChatImportStatus chat_import_markdown(Chat *chat, const char *markdown,
    size_t length, const wchar_t *fallback_title,
    const ChatImportBlobs *blobs, ChatImportStats *stats) {
    if (stats) *stats = (ChatImportStats){0};
    if (!chat || !markdown || length == 0) return CHAT_IMPORT_MALFORMED;
    if (length > CHAT_IMPORT_LIMIT) return CHAT_IMPORT_TOO_LARGE;
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    if (length >= sizeof bom && (unsigned char)markdown[0] == bom[0] &&
        (unsigned char)markdown[1] == bom[1] &&
        (unsigned char)markdown[2] == bom[2]) {
        markdown += sizeof bom;
        length -= sizeof bom;
    }
    if (length == 0) return CHAT_IMPORT_MALFORMED;   /* BOM-only file */
    static const char opener[] = "<!-- darkchat.export:";
    const size_t opener_length = sizeof opener - 1;
    if (length >= opener_length &&
        memcmp(markdown, opener, opener_length) == 0) {
        const char *begin = markdown + opener_length;
        const char *end = markdown + length;
        const char *close = NULL;
        for (const char *p = begin; p + 3 <= end; p++)
            if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
                close = p;
                break;
            }
        if (!close) return CHAT_IMPORT_MALFORMED;
        /* Trim only ASCII JSON whitespace around the payload. */
        while (begin < close && (*begin == ' ' || *begin == '\t' ||
                *begin == '\r' || *begin == '\n')) begin++;
        const char *json_end = close;
        while (json_end > begin && (json_end[-1] == ' ' ||
                json_end[-1] == '\t' || json_end[-1] == '\r' ||
                json_end[-1] == '\n')) json_end--;
        if (json_end == begin) return CHAT_IMPORT_MALFORMED;
        /* The payload is the same document the JSON path parses; only this
           format's payload may name sidecars, so `allow_file` is the single
           difference between the two entry points. */
        return chat_import_payload(chat, begin, (size_t)(json_end - begin),
            blobs, stats, true);
    }
    return import_markdown_fallback(chat, markdown, length, fallback_title,
        stats);
}
