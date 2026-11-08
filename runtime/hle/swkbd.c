// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../ui/lumiose_chat_layout.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap, int field);  // runtime/ui/menu.mm

// Lumiose reports when its windows open and close through the guest debug service.
// If a future mod stops reporting these markers, use the ordinary keyboard dialog.
static int lumiose_window;
static int lumiose_header;
static unsigned lumiose_scroll_windows;
static int lumiose_last_tap_y = -1;
static int lumiose_mail_row = -1;
extern const char *g_save_dir;
int lumiose_scroll_menu_open(void) { return __atomic_load_n(&lumiose_scroll_windows, __ATOMIC_ACQUIRE) != 0; }
void lumiose_keyboard_note_buttons(uint64_t pressed) {
    if (!(__atomic_load_n(&lumiose_scroll_windows, __ATOMIC_ACQUIRE) & 2)) return;
    if (pressed & ((1ull << BIND_L) | (1ull << BIND_R))) {
        // Switching tabs resets the write form's selection when it is opened.
        __atomic_store_n(&lumiose_mail_row, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&lumiose_last_tap_y, -1, __ATOMIC_RELEASE);
        return;
    }
    int row = __atomic_load_n(&lumiose_mail_row, __ATOMIC_ACQUIRE);
    if (row < 0) return;
    if (pressed & (1ull << BIND_UP)) row = row > 0 ? row - 1 : 0;
    if (pressed & (1ull << BIND_DOWN)) row = row < 5 ? row + 1 : 5;
    if ((pressed & (1ull << BIND_A)) && row == 0) row = -1;  // A may open the recipient picker.
    __atomic_store_n(&lumiose_mail_row, row, __ATOMIC_RELEASE);
    if (pressed & ((1ull << BIND_UP) | (1ull << BIND_DOWN)))
        __atomic_store_n(&lumiose_last_tap_y, -1, __ATOMIC_RELEASE);
}
// Verified mod builds keep the Market tab and per-tab page/selection in writable module data.
// Other versions keep ordinary up/down wheel navigation until their layout has been verified.
int lumiose_market_page_direction(int wheel_up) {
    const char *id = getenv("LUMIOSE_PREPARE_BUILD_ID");
    if (!id || __atomic_load_n(&lumiose_scroll_windows, __ATOMIC_ACQUIRE) != 1 || g_nmodules <= 2) return 0;
    uint32_t page_off, row_off, tab_off;
    if (!strcmp(id, "0ee0720115fb04dc2eda5000fc8e32d5454436fa")) {
        page_off = 0x1195498; row_off = 0x11954b0; tab_off = 0x1198b9c;
    } else if (!strcmp(id, "ea3373963a6032742895e9d15128388a67df2ac1")) {
        page_off = 0x11f5498; row_off = 0x11f54b0; tab_off = 0x11ffc6c;
    } else return 0;
    if (g_modules[2].size < (uint64_t)tab_off + 4) return 0;
    uint64_t base = g_modules[2].base;
    uint32_t tab = __atomic_load_n((uint32_t *)(uintptr_t)(base + tab_off), __ATOMIC_RELAXED);
    if (tab > 4 || tab == 2) return 0;  // Sell has a differently sized, scrolling list.
    uint32_t page = __atomic_load_n((uint32_t *)(uintptr_t)(base + page_off + tab * 4), __ATOMIC_RELAXED);
    uint32_t row = __atomic_load_n((uint32_t *)(uintptr_t)(base + row_off + tab * 4), __ATOMIC_RELAXED);
    if (row > 11 || page > 100000) return 0;
    if (wheel_up && row == 0 && page > 0) return -1;
    if (!wheel_up && row == 11) return 1;
    return 0;
}
RT_ON_RESET(lumiose_keyboard_reset) {
    __atomic_store_n(&lumiose_window, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lumiose_header, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lumiose_scroll_windows, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lumiose_last_tap_y, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&lumiose_mail_row, -1, __ATOMIC_RELEASE);
}
void lumiose_keyboard_debug(const char *s, size_t n) {
    static const struct { const char *line; int window; unsigned bit; bool open; } markers[] = {
        {"[ui] market opened", 2, 1, true}, {"[ui] market closed", 0, 1, false},
        {"[ui] mailbox opened", 3, 2, true}, {"[ui] mailbox closed", 0, 2, false},
        {"[ui] friends window opened", -1, 4, true}, {"[ui] friends window closed", -1, 4, false},
        {"[ui] player list opened", -1, 8, true}, {"[ui] player list closed", -1, 8, false}
    };
    for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
        size_t len = strlen(markers[i].line);
        if (n >= len && !memcmp(s, markers[i].line, len) && (n == len || s[len] == '\n' || s[len] == '\r')) {
            if (markers[i].window >= 0) __atomic_store_n(&lumiose_window, markers[i].window, __ATOMIC_RELEASE);
            if (markers[i].window == 3 || (markers[i].window == 0 && markers[i].bit == 2))
                __atomic_store_n(&lumiose_last_tap_y, -1, __ATOMIC_RELEASE);
            if (markers[i].window == 3 || (markers[i].window == 0 && markers[i].bit == 2))
                __atomic_store_n(&lumiose_mail_row, -1, __ATOMIC_RELEASE);
            if (markers[i].open) __atomic_fetch_or(&lumiose_scroll_windows, markers[i].bit, __ATOMIC_RELEASE);
            else __atomic_fetch_and(&lumiose_scroll_windows, ~markers[i].bit, __ATOMIC_RELEASE);
            break;
        }
    }
    static const char tap[] = "[ui] tap: down (";
    if (n >= sizeof tap - 1 && !memcmp(s, tap, sizeof tap - 1)) {
        char line[128];
        size_t count = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, s, count); line[count] = 0;
        int x, y;
        if (sscanf(line, "[ui] tap: down (%d, %d)", &x, &y) == 2) {
            __atomic_store_n(&lumiose_last_tap_y, y, __ATOMIC_RELEASE);
            if (__atomic_load_n(&lumiose_scroll_windows, __ATOMIC_ACQUIRE) & 2) {
                float scale = lumiose_chat_scale_read(g_save_dir).ui;
                float message = 540.0f - 210.0f * scale, money = 540.0f - 150.0f * scale;
                int row = -1;
                if (y >= message - 24 * scale && y <= message + 24 * scale) row = 1;
                else if (y >= money - 24 * scale && y <= money + 24 * scale) row = 2;
                else if (y >= 540 - 380 * scale && y <= 540 - 300 * scale &&
                         x >= 960 - 500 * scale && x <= 960 - 200 * scale) row = 0;  // Write tab
                __atomic_store_n(&lumiose_mail_row, row, __ATOMIC_RELEASE);
            }
        }
    }
    static const char chat[] = "[ui] swkbd: arg set (header 'Chat',";
    static const char window[] = "[ui] swkbd: arg set (header 'window',";
    if (n >= sizeof chat - 1 && !memcmp(s, chat, sizeof chat - 1))
        __atomic_store_n(&lumiose_header, 1, __ATOMIC_RELEASE);
    else if (n >= sizeof window - 1 && !memcmp(s, window, sizeof window - 1))
        __atomic_store_n(&lumiose_header, 2, __ATOMIC_RELEASE);
}

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. The answer is UTF-8 only when the game asks
// for it (KeyboardConfig isUseUtf8, +0x3bd: Link's Awakening), else UTF-16: Pokémon Z-A read our UTF-8 bytes two at
// a time, "William" showing as CJK characters ("Wi" = U+6957).
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n + 1 < cap) {
        uint32_t cp = *p++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (more) cp &= 0x3F >> more;
        for (; more && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
        if (more || (cp >= 0x80 && cp < 0xA0)) cp = '?';  // (truncated sequence, or a stray continuation byte)
        if (cp >= 0x10000) {
            if (n + 2 >= cap) break;
            cp -= 0x10000; out[n++] = (uint16_t)(0xD800 | cp >> 10); out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF)); }
        else out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
    return n;
}

HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    // Lumiose's chat calls through a game stub with no name-length setting.
    // Bound its larger entry by the guest's output buffer, while retaining the
    // game's own limits for names and other keyboard requests.
    bool mod_keyboard = g_nmodules > 2 && c->x[30] >= g_modules[2].base &&
                        c->x[30] < g_modules[2].base + g_modules[2].text_size && max_len <= 0;
    bool utf8 = *(const uint8_t *)(uintptr_t)(arg + 0x3bd) != 0;
    int field = 0;
    int header = mod_keyboard ? __atomic_exchange_n(&lumiose_header, 0, __ATOMIC_ACQ_REL) : 0;
    if (header == 1) field = 1;
    else if (header == 2) {
        field = __atomic_load_n(&lumiose_window, __ATOMIC_ACQUIRE);
        if (field == 3) {
            int y = __atomic_exchange_n(&lumiose_last_tap_y, -1, __ATOMIC_ACQ_REL);
            float scale = lumiose_chat_scale_read(g_save_dir).ui;
            float message = 540.0f - 210.0f * scale;
            float money = 540.0f - 150.0f * scale;
            if (y >= message - 24 * scale && y <= message + 24 * scale) field = 3;
            else if (y >= money - 24 * scale && y <= money + 24 * scale) field = 4;
            else {
                int row = __atomic_load_n(&lumiose_mail_row, __ATOMIC_ACQUIRE);
                field = row == 1 ? 3 : row == 2 ? 4 : 0;
            }
        }
    }
    if (mod_keyboard) {
        uint64_t units = utf8 ? size : size / 2;
        max_len = units > 1 ? (int)(units - 1 < 200 ? units - 1 : 200) : 0;
    }
    char initial[1024] = "";
    for (uint32_t i = 0; wb && i < n && i < sizeof initial - 1; i++) {
        uint16_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        initial[i] = ch < 0x80 ? (char)ch : '?';
    }
    if (mod_keyboard && getenv("LARECOMP_TEXT_CONTEXT_LOG")) {
        bool digits = n > 0;
        for (uint32_t i = 0; i < n && i < sizeof initial - 1; i++)
            if (initial[i] < '0' || initial[i] > '9') digits = false;
        LOG("[swkbd-context] field %d initial-units %u numeric %d mode %u input-form %u\n", field, n, digits,
            *(const uint32_t *)(uintptr_t)arg, *(const uint32_t *)(uintptr_t)(arg + 0x3b8));
    }
    char text[1024];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text, field);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    if (mod_keyboard) LOG("[swkbd] Lumiose entry (field %d, max %d, %s): %s (%zu bytes)\n", field, max_len, utf8 ? "UTF-8" : "UTF-16",
                  ok ? "entered" : "cancelled", strlen(text));
    else LOG("[swkbd] name entry (max %d, initial \"%s\", %s): %s \"%s\"\n", max_len, initial, utf8 ? "UTF-8" : "UTF-16",
             ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (utf8) snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        else utf8_to_utf16(text, (uint16_t *)(uintptr_t)ptr, size / 2);
    }
    c->x[0] = 0;
}
