#include "chat/core/commands.h"
#include <string.h>

/* Presentation order: the menu builder walks this table group by group, so the
   order here is the order the commands appear in. */
static const ChatActionInfo table[] = {
    { ACTION_NEW, L"&New conversation", NULL,
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_RENAME, L"&Rename...", L"F2",
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_DELETE, L"&Delete...", L"Del",
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_DELETE_ALL, L"Delete &all...", NULL,
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_CLEAR, L"&Clear messages...", NULL,
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_SEARCH, L"&Search conversations", L"Ctrl+F",
        CHAT_ACTION_GROUP_CONVERSATION, CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_RETRY, L"&Retry unsuccessful response", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_NONE },
    { ACTION_REGENERATE, L"Re&generate last response", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_NONE },
    { ACTION_EDIT, L"&Edit latest user message...", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_NONE },
    { ACTION_CANCEL_EDIT, L"Cancel edit mode", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_NONE },
    { ACTION_COPY, L"&Copy response", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_SELECTION, L"Copy transcript &selection", NULL,
        CHAT_ACTION_GROUP_RESPONSE, CHAT_ACTION_FLAG_NONE },
    { ACTION_SYSTEM, L"&System prompt...", NULL,
        CHAT_ACTION_GROUP_SETTINGS, CHAT_ACTION_FLAG_NONE },
    { ACTION_SIDEBAR, L"Sidebar &width...", NULL,
        CHAT_ACTION_GROUP_SETTINGS, CHAT_ACTION_FLAG_NONE },
    { ACTION_MODELS, L"&Choose model...", L"Ctrl+Space",
        CHAT_ACTION_GROUP_SETTINGS, CHAT_ACTION_FLAG_NONE },
    { ACTION_BACKEND_OPENROUTER, L"&OpenRouter", NULL,
        CHAT_ACTION_GROUP_BACKEND, CHAT_ACTION_FLAG_NONE },
    { ACTION_BACKEND_OLLAMA, L"&Ollama (local)", NULL,
        CHAT_ACTION_GROUP_BACKEND, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_SORT_DEFAULT, L"Sort: &Default (balanced)", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_SORT_PRICE, L"Sort: Prefer lowest &price", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_SORT_THROUGHPUT, L"Sort: Prefer highest t&hroughput", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_SORT_LATENCY, L"Sort: Prefer lowest &latency", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_ALLOW_FALLBACKS, L"Allow fallback &providers", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_ROUTING_DATA_COLLECTION, L"Allow providers that may store &data", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_ROUTING_ZDR, L"Require &zero data retention", NULL,
        CHAT_ACTION_GROUP_ROUTING, CHAT_ACTION_FLAG_NONE },
    { ACTION_MODEL_USE_HERE, L"Use model for &this chat", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_MODEL_CLEAR_HERE, L"&Clear conversation model", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_SYSTEM_HERE, L"System prompt for this con&versation...", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION,
        CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_SYSTEM_CLEAR_HERE, L"Use &global system prompt", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_PROFILE_APPLY, L"&Apply prompt profile", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION,
        CHAT_ACTION_FLAG_SEPARATOR_BEFORE | CHAT_ACTION_FLAG_SUBMENU_ONLY },
    { ACTION_PROFILE_SAVE, L"&Save current prompt as profile...", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION, CHAT_ACTION_FLAG_NONE },
    { ACTION_PROFILE_EDIT, L"&Edit profile...", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION,
        CHAT_ACTION_FLAG_SEPARATOR_BEFORE | CHAT_ACTION_FLAG_SUBMENU_ONLY },
    { ACTION_PROFILE_DELETE, L"&Delete profile...", NULL,
        CHAT_ACTION_GROUP_CUSTOMIZATION, CHAT_ACTION_FLAG_SUBMENU_ONLY },
    { ACTION_EXPORT_MARKDOWN, L"Export this conversation as &Markdown...", NULL,
        CHAT_ACTION_GROUP_DATA, CHAT_ACTION_FLAG_NONE },
    { ACTION_EXPORT_JSON, L"Export this conversation as &JSON...", NULL,
        CHAT_ACTION_GROUP_DATA, CHAT_ACTION_FLAG_NONE },
    { ACTION_EXPORT_ALL, L"Export &all conversations as JSON...", NULL,
        CHAT_ACTION_GROUP_DATA, CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_IMPORT_JSON, L"&Import conversations from JSON...", NULL,
        CHAT_ACTION_GROUP_DATA, CHAT_ACTION_FLAG_SEPARATOR_BEFORE },
    { ACTION_IMPORT_MARKDOWN, L"Import conversations from &Markdown...", NULL,
        CHAT_ACTION_GROUP_DATA, CHAT_ACTION_FLAG_NONE },
};

int chat_action_dynamic_profile_index(int id) {
    if (id < CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE ||
        id >= CHAT_ACTION_DYNAMIC_END)
        return -1;
    for (int range = 0; range < 4; range++) {
        int base = CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE +
            range * CHAT_MAX_PROMPT_PROFILES;
        if (id < base + CHAT_MAX_PROMPT_PROFILES) return id - base;
    }
    return -1;
}

const ChatActionInfo *chat_action_table(size_t *count) {
    if (count) *count = sizeof table / sizeof table[0];
    return table;
}

const ChatActionInfo *chat_action_info(int id) {
    size_t count;
    const ChatActionInfo *all = chat_action_table(&count);
    for (size_t i = 0; i < count; i++)
        if (all[i].id == id) return &all[i];
    return NULL;
}

const wchar_t *chat_action_menu_label(int id) {
    const ChatActionInfo *info = chat_action_info(id);
    return info ? info->menu_label : NULL;
}

const wchar_t *chat_action_shortcut(int id) {
    const ChatActionInfo *info = chat_action_info(id);
    return info ? info->shortcut : NULL;
}

bool chat_action_plain_label(int id, wchar_t *out, size_t capacity) {
    const ChatActionInfo *info = chat_action_info(id);
    if (!info || !out || capacity == 0) return false;
    out[0] = 0;
    size_t used = 0;
    for (const wchar_t *p = info->menu_label; *p; p++) {
        if (*p == L'&') continue;
        if (used + 1 >= capacity) return false;
        out[used++] = *p;
    }
    out[used] = 0;
    return true;
}

static bool response_is_replaceable(const ChatConversation *c, int user) {
    if (user < 0) return false;
    if ((size_t)user + 1 >= c->message_count) return true;
    ChatGenerationState state = c->messages[user + 1].generation.state;
    return state == CHAT_GENERATION_FAILED ||
        state == CHAT_GENERATION_CANCELLED ||
        state == CHAT_GENERATION_INTERRUPTED;
}

void chat_action_context_init(ChatActionContext *context, const Chat *chat) {
    if (!context) return;
    memset(context, 0, sizeof *context);
    context->chat = chat;
    if (!chat) return;
    context->backend_openrouter = chat->backend != CHAT_BACKEND_OLLAMA;
    const ChatConversation *c = chat_active(chat);
    if (!c) return;
    int user = chat_latest_user(c);
    context->has_latest_user = user >= 0;
    for (size_t i = 0; i < c->message_count; i++)
        if (c->messages[i].role == CHAT_ROLE_ASSISTANT) {
            context->has_latest_response = true;
            break;
        }
    context->response_replaceable = response_is_replaceable(c, user);
    /* Override state of the active conversation, resolved for the active
        backend, and the profile census the Customization group needs. */
    context->backend_openrouter = chat->backend != CHAT_BACKEND_OLLAMA;
    context->has_model_override = context->backend_openrouter
        ? c->model[0] != 0 : c->ollama_model[0] != 0;
    context->has_prompt_override =
        c->system_prompt.data != NULL || c->system_prompt_present;
    context->has_global_model =
        (context->backend_openrouter ? chat->model : chat->ollama_model)[0] != 0;
    context->profile_count = chat->profile_count;
}

bool chat_action_available(int id, const ChatActionContext *context) {
    if (!context) return false;
    /* Dynamic profile submenu items: a valid live index, gated like their
        submenu headers. Dispatch re-validates the index against the live
        count, so a stale item can never act on a removed profile. */
    if (id >= CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE &&
        id < CHAT_ACTION_DYNAMIC_END) {
        int index = chat_action_dynamic_profile_index(id);
        return !context->generating && context->profile_count > 0 &&
            index >= 0 && index < context->profile_count;
    }
    switch (id) {
    case ACTION_NEW:
    case ACTION_SEARCH:
        return true;
    case ACTION_COPY:
        return context->has_latest_response;
    case ACTION_SELECTION:
        return context->has_transcript_selection;
    case ACTION_RETRY:
        return !context->generating && context->response_replaceable;
    case ACTION_REGENERATE:
    case ACTION_EDIT:
        return !context->generating && context->has_latest_user;
    case ACTION_CANCEL_EDIT:
        return !context->generating && context->editing;
    case ACTION_RENAME:
    case ACTION_DELETE:
    case ACTION_DELETE_ALL:
    case ACTION_CLEAR:
    case ACTION_SYSTEM:
    case ACTION_SIDEBAR:
    case ACTION_MODELS:
    case ACTION_BACKEND_OPENROUTER:
    case ACTION_BACKEND_OLLAMA:
        return !context->generating;
    case ACTION_ROUTING_SORT_DEFAULT:
    case ACTION_ROUTING_SORT_PRICE:
    case ACTION_ROUTING_SORT_THROUGHPUT:
    case ACTION_ROUTING_SORT_LATENCY:
    case ACTION_ROUTING_ALLOW_FALLBACKS:
    case ACTION_ROUTING_DATA_COLLECTION:
    case ACTION_ROUTING_ZDR:
        return !context->generating && context->backend_openrouter;
    case ACTION_MODEL_USE_HERE:
        /* Copying the global model into the override is meaningful only
            when a global model exists and the override is not already
            active (an active override already shows this exact state). */
        return !context->generating && context->has_global_model &&
            !context->has_model_override;
    case ACTION_MODEL_CLEAR_HERE:
        return !context->generating && context->has_model_override;
    case ACTION_SYSTEM_HERE:
        return !context->generating;
    case ACTION_SYSTEM_CLEAR_HERE:
        return !context->generating && context->has_prompt_override;
    case ACTION_PROFILE_APPLY:
    case ACTION_PROFILE_EDIT:
    case ACTION_PROFILE_DELETE:
        return !context->generating && context->profile_count > 0;
    case ACTION_PROFILE_SAVE:
        return !context->generating &&
            context->profile_count < CHAT_MAX_PROMPT_PROFILES;
    case ACTION_EXPORT_MARKDOWN:
    case ACTION_EXPORT_JSON:
    case ACTION_EXPORT_ALL:
    case ACTION_IMPORT_JSON:
    case ACTION_IMPORT_MARKDOWN:
        return !context->generating;
    default:
        return false;
    }
}
