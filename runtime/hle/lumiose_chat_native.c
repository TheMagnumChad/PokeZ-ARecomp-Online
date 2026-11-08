#include "lumiose_chat_native.h"
#include "lumiose_chat_bridge.h"
#include "../input_bind.h"
#include <string.h>

extern void menu_notice(const char *text, int seconds);
extern void input_release_keyboard_for_chat(void);
extern uint64_t input_gamepad_button_bits(int host);
extern bool input_lumiose_queue_direction(int direction);
static char draft[121];
static size_t cursor;
static bool focused, owns_draft, dirty, select_all, send_pending, close_pending, window_edit;
static uint8_t active_field;
static uint8_t click_target;
static uint64_t click_deadline;
static bool key_owned[SDL_SCANCODE_COUNT];
static bool pad_owned[SDL_GAMEPAD_BUTTON_COUNT];

static size_t previous(size_t p) {
    if (p) { --p; while (p && ((unsigned char)draft[p] & 0xc0) == 0x80) --p; }
    return p;
}
static size_t next(size_t p) {
    if (draft[p]) { ++p; while (draft[p] && ((unsigned char)draft[p] & 0xc0) == 0x80) ++p; }
    return p;
}
static void clear_draft(void) { draft[0] = 0; cursor = 0; select_all = false; dirty = true; }

// Reject invalid UTF-8 and control characters rather than silently altering a message.
static bool valid_input(const unsigned char *p) {
    while (*p) {
        unsigned c = *p, n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                        c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!n || c < 0x20 || c == 0x7f) return false;
        for (unsigned i = 1; i < n; ++i) if (!p[i] || (p[i] & 0xc0) != 0x80) return false;
        if ((n == 2 && c == 0xc2 && p[1] < 0xa0) ||
            (n > 2 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f) ||
                       (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)))) return false;
        p += n;
    }
    return true;
}
static void insert(const char *s) {
    if (!s || !valid_input((const unsigned char *)s)) { menu_notice("Chat text contains unsupported characters", 3); return; }
    size_t n = strlen(s), used = select_all ? 0 : strlen(draft);
    size_t limit = active_field == 2 || active_field == 3 || active_field == 7 || active_field == 13 ? 24 :
                   active_field == 4 || active_field == 5 || active_field == 6 || active_field == 9 || active_field == 12 ? 8 :
                   active_field == 10 ? 60 : 120;
    if (n > limit - used) { menu_notice("Text is too long for this field", 3); return; }
    if (select_all) clear_draft();
    memmove(draft + cursor + n, draft + cursor, used - cursor + 1);
    memcpy(draft + cursor, s, n); cursor += n; dirty = true;
}

void lumiose_chat_native_tick(void) {
    LumioseChatSnapshot s;
    bool bridge = lumiose_chat_bridge_snapshot(&s);
    if (click_target) {
        if (!bridge || SDL_GetTicks() >= click_deadline || (s.state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_FIELD_EDIT)))
            click_target = 0;
        else if (s.focus_field == click_target && lumiose_chat_bridge_command(LUMIOSE_CHAT_EDIT, click_target, NULL))
            click_target = 0;
    }
    bool edit = bridge && s.version == 2 && (s.state & LUMIOSE_CHAT_FIELD_EDIT) && s.focus_field >= 2 && s.focus_field <= 13;
    bool chat = bridge && (s.state & LUMIOSE_CHAT_FOCUSED) && !(s.state & LUMIOSE_CHAT_OTHER_WINDOW);
    if ((!chat && !edit) || (s.state & LUMIOSE_CHAT_KEYBOARD)) {
        focused = owns_draft = dirty = select_all = send_pending = close_pending = window_edit = false;
        active_field = 0;
        draft[0] = 0; cursor = 0;
        return;
    }
    uint8_t field = edit ? s.focus_field : 1;
    if (!focused || active_field != field || window_edit != edit) {
        input_release_keyboard_for_chat();
        focused = true; window_edit = edit; active_field = field;
        owns_draft = s.draft_len == 0; draft[0] = 0; cursor = 0;
        dirty = select_all = send_pending = close_pending = false;
    }
    if (close_pending) {
        if (lumiose_chat_bridge_command(LUMIOSE_CHAT_CLOSE, 0, NULL)) close_pending = false;
    } else {
        if (dirty && lumiose_chat_bridge_command(LUMIOSE_CHAT_SET_DRAFT, 0, draft)) dirty = false;
        if (send_pending && !dirty && lumiose_chat_bridge_command(LUMIOSE_CHAT_SEND, 0, NULL)) {
            send_pending = false; owns_draft = false; draft[0] = 0; cursor = 0;
        }
    }
}
bool lumiose_chat_native_window_click(unsigned field) {
    LumioseChatSnapshot s;
    if (!lumiose_chat_bridge_snapshot(&s) || s.version != 2 ||
        !(s.state & LUMIOSE_CHAT_OTHER_WINDOW) || (s.state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_FIELD_EDIT))) return false;
    if (s.focus_field == field) return lumiose_chat_bridge_command(LUMIOSE_CHAT_EDIT, (int16_t)field, NULL);
    if (field < 7 || field > 9 || s.focus_field < 7 || s.focus_field > 9) return false;
    int delta = (int)field - (int)s.focus_field;
    for (int i = 0; i < (delta < 0 ? -delta : delta); i++)
        if (!input_lumiose_queue_direction(delta)) return false;
    click_target = (uint8_t)field;
    click_deadline = SDL_GetTicks() + 2500;
    return true;
}
bool lumiose_chat_native_wants_text(void) {
    LumioseChatSnapshot s;
    return focused && !send_pending && !close_pending && lumiose_chat_bridge_snapshot(&s) &&
           !(s.state & LUMIOSE_CHAT_KEYBOARD) && ((window_edit && (s.state & LUMIOSE_CHAT_FIELD_EDIT)) ||
           (!window_edit && (s.state & LUMIOSE_CHAT_FOCUSED) && !(s.state & LUMIOSE_CHAT_OTHER_WINDOW) && s.selection == 1));
}
bool lumiose_chat_native_click(bool send) {
    LumioseChatSnapshot s;
    if (!lumiose_chat_bridge_snapshot(&s) || !(s.state & LUMIOSE_CHAT_SHOWN) ||
        (s.state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_OTHER_WINDOW))) return false;
    if (!(s.state & LUMIOSE_CHAT_FOCUSED))
        return !send && lumiose_chat_bridge_command(LUMIOSE_CHAT_FOCUS, 0, NULL);
    if (s.selection != 1) return false;
    if (send) {
        if (!focused || !owns_draft || close_pending) return false;
        send_pending = true;
    }
    return true;
}
bool lumiose_chat_native_event(const SDL_Event *e) {
    LumioseChatSnapshot s;
    if (!lumiose_chat_bridge_snapshot(&s)) return false;
    bool can_focus = (s.state & LUMIOSE_CHAT_SHOWN) && !(s.state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_OTHER_WINDOW));
    bool has_edit = s.version == 2 && (s.state & LUMIOSE_CHAT_FIELD_EDIT) && !(s.state & LUMIOSE_CHAT_KEYBOARD);
    bool has_chat = (s.state & LUMIOSE_CHAT_FOCUSED) && can_focus;
    if ((e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
        e->gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
        int button = e->gbutton.button;
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP && pad_owned[button]) { pad_owned[button] = false; return true; }
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            uint64_t bits = input_gamepad_button_bits(button);
            if (has_edit && (bits & ((1ull << BIND_B) | (1ull << BIND_A)))) {
                if (bits & (1ull << BIND_B)) { dirty = send_pending = false; close_pending = true; }
                else send_pending = true;
                pad_owned[button] = true; return true;
            }
            if (s.version == 2 && !has_edit && !(s.state & LUMIOSE_CHAT_KEYBOARD) &&
                s.focus_field >= 2 && s.focus_field <= 13) {
                int slot = s.focus_field == 2 ? BIND_PLUS :
                           s.focus_field == 7 || s.focus_field == 10 || s.focus_field == 11 ? BIND_Y : BIND_A;
                if ((bits & (1ull << slot)) && lumiose_chat_bridge_command(LUMIOSE_CHAT_EDIT, s.focus_field, NULL)) {
                    pad_owned[button] = true; return true;
                }
            }
        }
        return false;
    }
    if (e->type == SDL_EVENT_KEY_UP && e->key.scancode < SDL_SCANCODE_COUNT && key_owned[e->key.scancode]) {
        key_owned[e->key.scancode] = false; return true;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        if ((!has_chat && !has_edit) || !lumiose_chat_native_wants_text()) return false;
        if (!owns_draft) { menu_notice("Press Ctrl+A before replacing existing text", 3); return true; }
        insert(e->text.text); return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return false;
    SDL_Scancode k = e->key.scancode;
    if (!has_chat && !has_edit) {
        if (s.version == 2 && s.focus_field >= 2 && s.focus_field <= 13 &&
            !(s.state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_FIELD_EDIT)) && !e->key.repeat) {
            int slot = s.focus_field == 2 ? BIND_PLUS :
                       s.focus_field == 7 || s.focus_field == 10 || s.focus_field == 11 ? BIND_Y : BIND_A;
            if (k == g_bind_key[slot] && lumiose_chat_bridge_command(LUMIOSE_CHAT_EDIT, s.focus_field, NULL)) {
                if (k < SDL_SCANCODE_COUNT) key_owned[k] = true;
                return true;
            }
        }
        if (can_focus && !e->key.repeat && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) &&
            !(e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_CTRL)) &&
            lumiose_chat_bridge_command(LUMIOSE_CHAT_FOCUS, 0, NULL)) {
            key_owned[k] = true; return true;
        }
        return false;
    }
    // F1 remains the launcher shortcut even when chat is focused.
    if (k == SDL_SCANCODE_F1) return false;
    if (k < SDL_SCANCODE_COUNT) key_owned[k] = true;
    if (e->key.repeat && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_TAB)) return true;
    if (k == SDL_SCANCODE_ESCAPE) { dirty = send_pending = false; close_pending = true; return true; }
    if (has_edit && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER)) { send_pending = true; return true; }
    if (has_edit && (k == SDL_SCANCODE_TAB || k == SDL_SCANCODE_UP || k == SDL_SCANCODE_DOWN ||
                     k == SDL_SCANCODE_PAGEUP || k == SDL_SCANCODE_PAGEDOWN)) return true;
    if (!has_edit && k == SDL_SCANCODE_END) { lumiose_chat_bridge_command(LUMIOSE_CHAT_NEWEST, 0, NULL); return true; }
    if (k == SDL_SCANCODE_TAB) { lumiose_chat_bridge_command(LUMIOSE_CHAT_TAB, (e->key.mod & SDL_KMOD_SHIFT) ? -1 : 1, NULL); return true; }
    if (k == SDL_SCANCODE_PAGEUP || k == SDL_SCANCODE_PAGEDOWN) { lumiose_chat_bridge_command(LUMIOSE_CHAT_PAGE, k == SDL_SCANCODE_PAGEUP ? 1 : -1, NULL); return true; }
    if (k == SDL_SCANCODE_UP || k == SDL_SCANCODE_DOWN) { lumiose_chat_bridge_command(LUMIOSE_CHAT_SCROLL, k == SDL_SCANCODE_UP ? 1 : -1, NULL); return true; }
    if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) {
        if (s.selection == 1 && owns_draft) send_pending = true;
        else if (s.selection == 1) lumiose_chat_bridge_command(LUMIOSE_CHAT_OPEN_KEYBOARD, 0, NULL);
        return true;
    }
    if (!lumiose_chat_native_wants_text()) return true;
    bool ctrl = (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (ctrl && k == SDL_SCANCODE_A) { owns_draft = true; select_all = true; }
    else if (ctrl && k == SDL_SCANCODE_C) { if (select_all) SDL_SetClipboardText(draft); }
    else if (ctrl && k == SDL_SCANCODE_X) { if (select_all) { SDL_SetClipboardText(draft); clear_draft(); } }
    else if (ctrl && k == SDL_SCANCODE_V) {
        if (!owns_draft) menu_notice("Press Ctrl+A before replacing existing text", 3);
        else { char *s = SDL_GetClipboardText(); if (s) { insert(s); SDL_free(s); } }
    }
    else if (k == SDL_SCANCODE_HOME) { cursor = 0; select_all = false; }
    else if (k == SDL_SCANCODE_END) { cursor = strlen(draft); select_all = false; }
    else if (k == SDL_SCANCODE_LEFT) { cursor = previous(cursor); select_all = false; }
    else if (k == SDL_SCANCODE_RIGHT) { cursor = next(cursor); select_all = false; }
    else if (k == SDL_SCANCODE_BACKSPACE || k == SDL_SCANCODE_DELETE) {
        if (select_all) clear_draft();
        else { size_t start = k == SDL_SCANCODE_BACKSPACE ? previous(cursor) : cursor;
               size_t end = k == SDL_SCANCODE_BACKSPACE ? cursor : next(cursor);
               if (end > start) { memmove(draft + start, draft + end, strlen(draft + end) + 1); cursor = start; dirty = true; } }
    }
    return true;
}
