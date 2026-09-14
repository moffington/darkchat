#include "../chat/chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    Chat *chat=calloc(1,sizeof *chat); CHECK(chat);
    chat_init(chat); chat_clear(chat);
    uint64_t id=chat_active(chat)->id;
    CHECK(chat_begin_response(chat,CHAT_RETRY,NULL)==-1);
    int n=chat_begin_response(chat,CHAT_SEND,L"Question"); CHECK(n==1);
    ChatConversation *c=&chat->conversations[0];
    CHECK(c->message_count==2 && c->messages[1].generation.state==CHAT_GENERATION_RUNNING);
    /* Stable identity contract: the user turn keeps its id across every
       replacement mode; each replacement assistant turn gets a fresh id. */
    uint64_t user_id=c->messages[0].id, assistant_id=c->messages[1].id;
    CHECK(user_id && assistant_id && user_id!=assistant_id);
    CHECK(chat_begin_response(chat,CHAT_SEND,L"duplicate")==-1);
    CHECK(!chat_history_message(&c->messages[1]));
    for (int state=CHAT_GENERATION_CANCELLED;state<=CHAT_GENERATION_FAILED;state++) {
        c->messages[1].generation.state=state;
        wcscpy(c->messages[1].text,L"Partial response");
        CHECK(chat_begin_response(chat,CHAT_RETRY,NULL)==1);
        CHECK(c->message_count==2 && !wcscmp(c->messages[0].text,L"Question"));
        CHECK(!c->messages[1].text[0] && c->messages[1].generation.cost==-1);
        CHECK(c->messages[0].id==user_id && c->messages[1].id!=assistant_id);
        assistant_id=c->messages[1].id;
    }
    c->messages[1].generation.state=CHAT_GENERATION_COMPLETE;
    CHECK(chat_history_message(&c->messages[1]));
    CHECK(chat_begin_response(chat,CHAT_RETRY,NULL)==-1);
    CHECK(chat_begin_response(chat,CHAT_REGENERATE,NULL)==1);
    CHECK(c->messages[0].id==user_id && c->messages[1].id!=assistant_id);
    assistant_id=c->messages[1].id;
    c->messages[1].generation.state=CHAT_GENERATION_COMPLETE;
    CHECK(chat_begin_response(chat,CHAT_EDIT_RESEND,L"")==-1);
    CHECK(!wcscmp(c->messages[0].text,L"Question"));
    CHECK(chat_begin_response(chat,CHAT_EDIT_RESEND,L"Edited question")==1);
    CHECK(c->message_count==2 && !wcscmp(c->messages[0].text,L"Edited question"));
    /* Edit-and-resend keeps the edited user id and creates a new assistant id. */
    CHECK(c->messages[0].id==user_id && c->messages[1].id!=assistant_id);
    assistant_id=c->messages[1].id;
    c->messages[1].generation.state=CHAT_GENERATION_COMPLETE;
    while (chat_remaining(chat)>=2) {
        n=chat_begin_response(chat,CHAT_SEND,L"Another turn"); CHECK(n>=0);
        c->messages[n].generation.state=CHAT_GENERATION_COMPLETE;
    }
    CHECK(chat_begin_response(chat,CHAT_SEND,L"Full")==-1);
    CHECK(chat_begin_response(chat,CHAT_REGENERATE,NULL)==CHAT_MAX_MESSAGES-1);
    CHECK(c->message_count==CHAT_MAX_MESSAGES);
    CHECK(chat_rename(chat,L"Manual title"));
    chat_clear(chat);
    CHECK(chat_active(chat)->id==id);
    CHECK(chat_begin_response(chat,CHAT_SEND,L"Don't rename")==1);
    CHECK(!wcscmp(c->title,L"Manual title"));
    CHECK(chat_delete(chat));
    CHECK(chat_active(chat)->id!=id && chat->conversation_count==1);
    CHECK(chat_new_conversation(chat)==1);
    CHECK(chat_append(chat,CHAT_ROLE_USER,L"Remove every conversation")>=0);
    CHECK(chat_delete_all(chat));
    CHECK(chat->conversation_count==1 && chat->active==0);
    CHECK(chat_active(chat)->message_count==0);
    for (int i=0;i<CHAT_MODEL_HISTORY+5;i++) { swprintf(chat->model,CHAT_MODEL_TEXT,L"model/%d",i); chat_remember_model(chat); }
    CHECK(chat->model_history_count==CHAT_MODEL_HISTORY);
    chat_remember_model(chat); CHECK(chat->model_history_count==CHAT_MODEL_HISTORY);
    free(chat); puts("Generation lifecycle, replacement, capacity and stable identity tests passed"); return 0;
}
