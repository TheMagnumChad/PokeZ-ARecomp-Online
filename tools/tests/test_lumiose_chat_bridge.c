#include "../../runtime/hle/lumiose_chat_bridge.c"
#include <assert.h>
#include <stdio.h>

Module g_modules[4];
int g_nmodules;

int main(void) {
    _Alignas(16) unsigned char data[1024] = {0};
    LumioseChatBridge *b = (LumioseChatBridge *)(data + 32);
    memcpy(b->magic, "VMPCHAT\0", 8);
    b->version = 1; b->size = sizeof *b;
    assert(lumiose_chat_bridge_find(data, sizeof data) == b);
    b->version = 2;
    assert(!lumiose_chat_bridge_find(data, sizeof data));
    b->version = 1;
    LumioseChatBridge *duplicate = (LumioseChatBridge *)(data + 512);
    memcpy(duplicate->magic, "VMPCHAT\0", 8);
    duplicate->version = 1; duplicate->size = sizeof *duplicate;
    assert(!lumiose_chat_bridge_find(data, sizeof data));
    memset(duplicate, 0, sizeof *duplicate);
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SCROLL, 1, NULL));  // not focused
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_FOCUS, 0, NULL));
    b->done = b->head;
    b->state = LUMIOSE_CHAT_FOCUSED;
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SET_DRAFT, 0, "Ol\xc3\xa1"));
    assert(!strcmp(b->text, "Ol\xc3\xa1"));
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SET_DRAFT, 0, "overwrite"));
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SEND, 0, NULL));
    assert(b->commands[1].cmd == LUMIOSE_CHAT_SET_DRAFT && b->commands[2].cmd == LUMIOSE_CHAT_SEND);
    b->done = b->head;
    char oversized[122]; memset(oversized, 'x', 121); oversized[121] = 0;
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SEND_TEXT, 0, oversized));
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SEND_TEXT, 0, "bad\xc0\x80"));
    for (int i = 0; i < 16; i++) assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SCROLL, 1, NULL));
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SCROLL, 1, NULL));
    b->done = b->head;
    b->state |= LUMIOSE_CHAT_OTHER_WINDOW;
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_FOCUS, 0, NULL));
    b->version = 2; b->size = 0xf0; b->state = LUMIOSE_CHAT_OTHER_WINDOW;
    ((unsigned char *)b)[0xe4] = 8;
    assert(lumiose_chat_bridge_find(data, sizeof data) == b);
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_EDIT, 9, NULL));
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_EDIT, 8, NULL));
    b->done = b->head; b->state |= LUMIOSE_CHAT_FIELD_EDIT;
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SET_DRAFT, 0, "message"));
    assert(lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_SEND, 0, NULL));
    assert(!lumiose_chat_bridge_enqueue(b, LUMIOSE_CHAT_TAB, 1, NULL));
    puts("Lumiose chat bridge protocol checks passed");
    return 0;
}
