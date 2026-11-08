#include "../../runtime/hle/swkbd.c"
#include <assert.h>

Module g_modules[4];
int g_nmodules;
const char *g_save_dir = "";
static int seen_limit;
static int seen_field;

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap, int field) {
    (void)initial;
    seen_limit = max_len;
    seen_field = field;
    snprintf(out, cap, "A message longer than eight characters");
    return 1;
}

int main(void) {
    uint8_t arg[0x500] = {0};
    uint16_t result[256] = {0};
    uint64_t out[2] = {(uintptr_t)result, sizeof result};
    Cpu c = {0};
    g_nmodules = 3;
    g_modules[2].base = 0x7006200000ull;
    g_modules[2].text_size = 0x1469a0;
    c.x[0] = (uintptr_t)out; c.x[1] = (uintptr_t)arg; c.x[30] = g_modules[2].base + 0x100;
    const char *chat = "[ui] swkbd: arg set (header 'Chat', no sub text";
    lumiose_keyboard_debug(chat, strlen(chat));
    swkbd_show(&c);
    assert(seen_field == 1 && seen_limit == 200 && result[8] != 0 && result[38] == 0);
    const char *market = "[ui] market opened\n";
    const char *window = "[ui] swkbd: arg set (header 'window', no sub text";
    assert(!lumiose_scroll_menu_open());
    lumiose_keyboard_debug(market, strlen(market));
    assert(lumiose_scroll_menu_open());
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 2 && seen_limit == 200);
    const char *market_closed = "[ui] market closed\n";
    lumiose_keyboard_debug(market_closed, strlen(market_closed));
    assert(!lumiose_scroll_menu_open());
    const char *mail = "[ui] mailbox opened\n";
    lumiose_keyboard_debug(mail, strlen(mail));
    assert(lumiose_scroll_menu_open());
    const char *message_tap = "[ui] tap: down (658, 328) up (658, 328), 3 buttons\n";
    lumiose_keyboard_debug(message_tap, strlen(message_tap));
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 3);
    const char *money_tap = "[ui] tap: down (610, 387) up (610, 387), 2 buttons\n";
    lumiose_keyboard_debug(money_tap, strlen(money_tap));
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 4);
    lumiose_keyboard_note_buttons(1ull << BIND_R);
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 0);  // No known selected row: use the standard dialog.
    lumiose_keyboard_note_buttons(1ull << BIND_DOWN);
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 3);  // Controller-selected Message.
    lumiose_keyboard_note_buttons(1ull << BIND_DOWN);
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 4);  // Controller-selected Money.
    const char *close = "[ui] mailbox closed\n";
    lumiose_keyboard_debug(close, strlen(close));
    assert(!lumiose_scroll_menu_open());
    lumiose_keyboard_debug(window, strlen(window));
    c.x[0] = (uintptr_t)out;
    swkbd_show(&c);
    assert(seen_field == 0);  // Unknown mod windows use the ordinary keyboard.
    memset(result, 0, sizeof result);
    *(int32_t *)(arg + 0x3ac) = 12;
    c.x[0] = (uintptr_t)out; c.x[1] = (uintptr_t)arg; c.x[30] = 0x7000000100ull;
    swkbd_show(&c);
    assert(seen_field == 0 && seen_limit == 12);
    // Page changes are enabled only for the verified module build and supported tabs.
    size_t module_size = 0x1200000;
    unsigned char *module = calloc(1, module_size);
    assert(module);
    g_modules[2].base = (uintptr_t)module;
    g_modules[2].size = module_size;
#ifdef _WIN32
    _putenv_s("LUMIOSE_PREPARE_BUILD_ID", "0ee0720115fb04dc2eda5000fc8e32d5454436fa");
#else
    setenv("LUMIOSE_PREPARE_BUILD_ID", "0ee0720115fb04dc2eda5000fc8e32d5454436fa", 1);
#endif
    lumiose_keyboard_debug(market, strlen(market));
    *(uint32_t *)(module + 0x11954b0) = 10;
    assert(lumiose_market_page_direction(0) == 0);
    *(uint32_t *)(module + 0x11954b0) = 11;
    assert(lumiose_market_page_direction(0) == 1);
    *(uint32_t *)(module + 0x11954b0) = 0;
    *(uint32_t *)(module + 0x1195498) = 1;
    assert(lumiose_market_page_direction(1) == -1);
    *(uint32_t *)(module + 0x1198b9c) = 2;  // Sell scrolls a differently sized list.
    assert(lumiose_market_page_direction(1) == 0);
    *(uint32_t *)(module + 0x1198b9c) = 0;
#ifdef _WIN32
    _putenv_s("LUMIOSE_PREPARE_BUILD_ID", "ea3373963a6032742895e9d15128388a67df2ac1");
#else
    setenv("LUMIOSE_PREPARE_BUILD_ID", "ea3373963a6032742895e9d15128388a67df2ac1", 1);
#endif
    *(uint32_t *)(module + 0x11f54b0) = 11;
    assert(lumiose_market_page_direction(0) == 1);
    *(uint32_t *)(module + 0x11f54b0) = 0;
    *(uint32_t *)(module + 0x11f5498) = 1;
    assert(lumiose_market_page_direction(1) == -1);
    *(uint32_t *)(module + 0x11ffc6c) = 2;
    assert(lumiose_market_page_direction(1) == 0);
    lumiose_keyboard_debug(market_closed, strlen(market_closed));
    assert(lumiose_market_page_direction(1) == 0);
    free(module);
    return 0;
}
