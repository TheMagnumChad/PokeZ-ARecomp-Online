// Optional launcher interface supplied by Lumiose Online 1.4.4 and later.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LumioseChatBridge {
    char magic[8];
    uint32_t version, size, state;
    uint8_t tab, tabs, selection, newer;
    uint32_t done, draft_len, head;
    struct { uint16_t cmd; int16_t arg; } commands[16];
    char text[128];
} LumioseChatBridge;

enum { LUMIOSE_CHAT_SHOWN = 1, LUMIOSE_CHAT_FOCUSED = 2, LUMIOSE_CHAT_KEYBOARD = 4,
       LUMIOSE_CHAT_OTHER_WINDOW = 8, LUMIOSE_CHAT_CONNECTED = 16, LUMIOSE_CHAT_FIELD_EDIT = 32 };
enum { LUMIOSE_CHAT_FOCUS = 1, LUMIOSE_CHAT_CLOSE = 2, LUMIOSE_CHAT_SCROLL = 3,
       LUMIOSE_CHAT_PAGE = 4, LUMIOSE_CHAT_NEWEST = 5, LUMIOSE_CHAT_TAB = 6,
       LUMIOSE_CHAT_TAB_SET = 7, LUMIOSE_CHAT_SET_DRAFT = 8, LUMIOSE_CHAT_SEND = 9,
       LUMIOSE_CHAT_SEND_TEXT = 10, LUMIOSE_CHAT_OPEN_KEYBOARD = 11, LUMIOSE_CHAT_EDIT = 12 };
typedef struct { uint32_t state, draft_len, version; uint8_t selection, focus_field; } LumioseChatSnapshot;

// Caller serializes producers. Never writes outside the launcher-owned fields.
LumioseChatBridge *lumiose_chat_bridge_find(void *data, size_t size);
bool lumiose_chat_bridge_enqueue(LumioseChatBridge *bridge, uint16_t cmd, int16_t arg, const char *text);
// Returns -1 when no focused bridge is available, 0 if busy, or 1 if queued.
int lumiose_chat_bridge_scroll(int16_t lines);
void lumiose_chat_bridge_init(void);
bool lumiose_chat_bridge_snapshot(LumioseChatSnapshot *out);
bool lumiose_chat_bridge_command(uint16_t cmd, int16_t arg, const char *text);

#ifdef __cplusplus
}
#endif
