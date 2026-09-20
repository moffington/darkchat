#ifndef DARKCHAT_COMMANDS_H
#define DARKCHAT_COMMANDS_H

#include "chat/core/chat.h"

/* Every addressable command. The values are menu command ids and are part of
   the existing wiring, so they must not be renumbered or reordered. ACTION_FIRST
   and ACTION_LAST alias the first and last command so coverage tests can walk
   the whole id range without a second hand-maintained list. New commands are
   inserted before ACTION_LAST with a fresh id. */
enum {
    ACTION_NEW=100, ACTION_RENAME, ACTION_DELETE, ACTION_DELETE_ALL, ACTION_CLEAR,
    ACTION_RETRY, ACTION_REGENERATE, ACTION_EDIT, ACTION_COPY, ACTION_SELECTION,
    ACTION_SYSTEM, ACTION_SIDEBAR, ACTION_MODELS, ACTION_CANCEL_EDIT,
    ACTION_SEARCH,
    ACTION_BACKEND_OPENROUTER, ACTION_BACKEND_OLLAMA,
    ACTION_ROUTING_SORT_DEFAULT, ACTION_ROUTING_SORT_PRICE,
    ACTION_ROUTING_SORT_THROUGHPUT, ACTION_ROUTING_SORT_LATENCY,
    ACTION_ROUTING_ALLOW_FALLBACKS, ACTION_ROUTING_DATA_COLLECTION,
    ACTION_ROUTING_ZDR,
    ACTION_MODEL_USE_HERE, ACTION_MODEL_CLEAR_HERE,
    ACTION_SYSTEM_HERE, ACTION_SYSTEM_CLEAR_HERE,
    ACTION_PROFILE_APPLY, ACTION_PROFILE_SAVE,
    ACTION_PROFILE_EDIT, ACTION_PROFILE_DELETE,
    ACTION_FIRST = ACTION_NEW,
    ACTION_LAST = ACTION_PROFILE_DELETE
};

/* Dynamic profile submenu ids. Unlike the registry above these are not
    static menu entries: each profile submenu holds one item per live
    profile, carrying base + profile index. They are dispatched only
    through TrackPopupMenu (which returns the full id, never truncated) and
    stay below 0x10000 so the WM_COMMAND LOWORD path can never misroute a
    stale value into another command. The ranges are deliberately outside
    ACTION_FIRST..ACTION_LAST: coverage walks of the static registry never
    see them, and they are tested separately. */
enum {
    /* "Apply profile to global prompt" items, one per profile. */
    CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE = 0x2000,
    /* "Apply profile to this conversation" items, one per profile. */
    CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE =
        CHAT_ACTION_DYNAMIC_APPLY_GLOBAL_BASE + CHAT_MAX_PROMPT_PROFILES,
    /* "Edit profile" items, one per profile. */
    CHAT_ACTION_DYNAMIC_EDIT_BASE =
        CHAT_ACTION_DYNAMIC_APPLY_HERE_BASE + CHAT_MAX_PROMPT_PROFILES,
    /* "Delete profile" items, one per profile. */
    CHAT_ACTION_DYNAMIC_DELETE_BASE =
        CHAT_ACTION_DYNAMIC_EDIT_BASE + CHAT_MAX_PROMPT_PROFILES,
    /* Exclusive end of every dynamic range. */
    CHAT_ACTION_DYNAMIC_END =
        CHAT_ACTION_DYNAMIC_DELETE_BASE + CHAT_MAX_PROMPT_PROFILES
};
_Static_assert(CHAT_ACTION_DYNAMIC_END <= 0xFFFF,
    "dynamic submenu ids must stay below the WM_COMMAND LOWORD ceiling");

/* Converts a dynamic submenu id into its profile index, or -1 when the id
    is outside every dynamic range. */
int chat_action_dynamic_profile_index(int id);

#define CHAT_ACTION_LABEL_TEXT 128

/* Menu/palette sections. BACKEND and ROUTING are the nested Settings submenus
    today and keep their own groups so a palette can section them separately.
    CUSTOMIZATION is the per-conversation override and prompt-profile group. */
typedef enum {
    CHAT_ACTION_GROUP_CONVERSATION = 0,
    CHAT_ACTION_GROUP_RESPONSE,
    CHAT_ACTION_GROUP_SETTINGS,
    CHAT_ACTION_GROUP_BACKEND,
    CHAT_ACTION_GROUP_ROUTING,
    CHAT_ACTION_GROUP_CUSTOMIZATION,
    CHAT_ACTION_GROUP_COUNT
} ChatActionGroup;

enum {
    CHAT_ACTION_FLAG_NONE = 0,
    /* A separator is emitted before this item, reproducing the current menu
        layout without hard-coding it in the menu builder. */
    CHAT_ACTION_FLAG_SEPARATOR_BEFORE = 1u << 0,
    /* The item is a submenu header whose contents are dynamic (one item per
        prompt profile). The menu builder emits MF_POPUP for it and populates
        the child items from the live profile library; the palette skips it
        (a submenu header has no single effect to invoke). */
    CHAT_ACTION_FLAG_SUBMENU_ONLY = 1u << 1
};

typedef struct {
    int id;
    /* Exact menu text, including its Win32 '&' mnemonic. Mnemonics are kept
       byte-for-byte from the original menu, duplicates included (OpenRouter
       and Ollama share 'O'), so this commit changes no visible accelerator.
       Palette, matcher and accessibility consumers must use
       chat_action_plain_label() instead. */
    const wchar_t *menu_label;
    /* Display form of the key binding, or NULL when the action has none. */
    const wchar_t *shortcut;
    unsigned char group;
    unsigned flags;
} ChatActionInfo;

/* Live state the availability predicate reads. The Chat-derived fields are
    filled by chat_action_context_init(); the host then sets the interaction
    flags it owns (generating/editing/selection). */
typedef struct {
    const Chat *chat;
    bool generating, editing;
    bool has_latest_user, has_latest_response, response_replaceable;
    bool has_transcript_selection;
    bool backend_openrouter;
    /* Per-conversation override state of the active conversation, resolved
        for the active backend, plus the profile-library census the
        dynamic submenus and Save availability need. has_global_model is
        the active backend's global slot (nonempty), which "use model for
        this conversation" copies from. */
    bool has_model_override, has_prompt_override, has_global_model;
    int profile_count;
} ChatActionContext;

/* Fills every field derivable from `chat` (the active conversation's turn
   shape and the active backend) and clears the interaction flags. */
void chat_action_context_init(ChatActionContext *context, const Chat *chat);

/* Whether an action may currently be invoked. Unknown ids are unavailable. */
bool chat_action_available(int id, const ChatActionContext *context);

/* Registry lookups. Unknown ids yield NULL (or false for the buffer helper). */
const ChatActionInfo *chat_action_info(int id);
const wchar_t *chat_action_menu_label(int id);
const wchar_t *chat_action_shortcut(int id);
/* Writes the mnemonic-free display label into `out`. Returns false when the id
   is unknown or the buffer cannot hold the label; `out` is always terminated
   when capacity > 0. */
bool chat_action_plain_label(int id, wchar_t *out, size_t capacity);

/* The static registry in presentation order: entries are grouped and ordered
   exactly as the menu lays them out. */
const ChatActionInfo *chat_action_table(size_t *count);

#endif
