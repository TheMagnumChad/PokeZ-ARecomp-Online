#include "../../runtime/hle/lumiose_chat_native.h"
#include "../../runtime/hle/lumiose_chat_bridge.h"
#include "../../runtime/input_bind.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static LumioseChatSnapshot state;
static bool available = true, busy;
static int calls[32], count, notices;
static char last_text[128];
int16_t g_bind_key[BIND_KEY_COUNT];
bool lumiose_chat_bridge_snapshot(LumioseChatSnapshot *out) { if (!available) return false; *out = state; return true; }
bool lumiose_chat_bridge_command(uint16_t command, int16_t arg, const char *text) {
    (void)arg; if (busy) return false;
    calls[count++] = command;
    if (text) snprintf(last_text, sizeof last_text, "%s", text);
    return true;
}
void menu_notice(const char *text, int seconds) { (void)text; (void)seconds; ++notices; }
void input_release_keyboard_for_chat(void) {}
uint64_t input_gamepad_button_bits(int host) { return host == SDL_GAMEPAD_BUTTON_EAST ? 1ull << BIND_A : 0; }
bool input_lumiose_queue_direction(int direction) { (void)direction; return true; }
static void key(SDL_Scancode sc) {
    SDL_Event e = {0}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = sc;
    assert(lumiose_chat_native_event(&e));
    e.type = SDL_EVENT_KEY_UP; assert(lumiose_chat_native_event(&e));
}
static void type(const char *s) {
    SDL_Event e = {0}; e.type = SDL_EVENT_TEXT_INPUT; e.text.text = s;
    assert(lumiose_chat_native_event(&e));
}
int main(void) {
    state.state = LUMIOSE_CHAT_SHOWN | LUMIOSE_CHAT_FOCUSED; state.selection = 1;
    lumiose_chat_native_tick(); assert(lumiose_chat_native_wants_text());
    assert(lumiose_chat_native_click(false));
    type("Ol\xc3\xa1"); busy = true; lumiose_chat_native_tick(); assert(count == 0);
    busy = false; lumiose_chat_native_tick(); assert(count == 1 && calls[0] == LUMIOSE_CHAT_SET_DRAFT);
    assert(!strcmp(last_text, "Ol\xc3\xa1"));
    key(SDL_SCANCODE_BACKSPACE); lumiose_chat_native_tick(); assert(!strcmp(last_text, "Ol"));
    type("a"); key(SDL_SCANCODE_RETURN); lumiose_chat_native_tick();
    assert(!strcmp(last_text, "Ola") && calls[count-2] == LUMIOSE_CHAT_SET_DRAFT && calls[count-1] == LUMIOSE_CHAT_SEND);
    state.state = 0; lumiose_chat_native_tick();
    state.state = LUMIOSE_CHAT_SHOWN | LUMIOSE_CHAT_FOCUSED; state.draft_len = 3;
    lumiose_chat_native_tick(); assert(lumiose_chat_native_wants_text());
    key(SDL_SCANCODE_RETURN); assert(calls[count-1] == LUMIOSE_CHAT_OPEN_KEYBOARD);
    state.state = 0; state.draft_len = 0; lumiose_chat_native_tick();
    state.state = LUMIOSE_CHAT_SHOWN | LUMIOSE_CHAT_FOCUSED; lumiose_chat_native_tick();
    char long_text[122]; memset(long_text, 'x', 121); long_text[121] = 0; type(long_text); assert(notices == 1);
    key(SDL_SCANCODE_ESCAPE); lumiose_chat_native_tick(); assert(calls[count-1] == LUMIOSE_CHAT_CLOSE);
    state.state = LUMIOSE_CHAT_OTHER_WINDOW; state.version = 2; state.focus_field = 8;
    lumiose_chat_native_tick(); g_bind_key[BIND_A] = SDL_SCANCODE_SPACE;
    key(SDL_SCANCODE_SPACE); assert(calls[count-1] == LUMIOSE_CHAT_EDIT);
    SDL_Event pad = {0}; pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; pad.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
    assert(lumiose_chat_native_event(&pad)); assert(calls[count-1] == LUMIOSE_CHAT_EDIT);
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_UP; assert(lumiose_chat_native_event(&pad));
    assert(lumiose_chat_native_window_click(8)); assert(calls[count-1] == LUMIOSE_CHAT_EDIT);
    state.state |= LUMIOSE_CHAT_FIELD_EDIT; lumiose_chat_native_tick();
    assert(lumiose_chat_native_wants_text()); type("hello");
    key(SDL_SCANCODE_RETURN); lumiose_chat_native_tick();
    assert(!strcmp(last_text, "hello") && calls[count-2] == LUMIOSE_CHAT_SET_DRAFT && calls[count-1] == LUMIOSE_CHAT_SEND);
    puts("Lumiose native chat input checks passed");
}
