// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, Space/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../rumble.h"
#include "../led.h"
#include "../ui/menu.h"
#include "lumiose_chat_bridge.h"
#include "za_mouse_drag.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int prompt_input;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;
static bool host_test;  // scripted runs must not vibrate the user's controller

// Guest IPC threads publish values; only the UI thread touches SDL's pad.
// The lock also makes paired value snapshots consistent with permission/reset.
static SDL_SpinLock rumble_lock;
static RumbleValue rumble_values[2];
static bool rumble_allowed = true;
static _Atomic uint64_t rumble_frame_ms;
static uint16_t rumble_last_lo, rumble_last_hi;
static uint64_t rumble_last_ms;
static bool rumble_warned;

static void rumble_clear(void) {
    SDL_LockSpinlock(&rumble_lock);
    memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
void input_rumble_send(uint32_t h, const RumbleValue *v) {
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side < 0) { SDL_UnlockSpinlock(&rumble_lock); return; }
    if (rumble_allowed) rumble_values[side] = *v;
    SDL_UnlockSpinlock(&rumble_lock);
}
RumbleValue input_rumble_get(uint32_t h) {
    RumbleValue v = {0, 160, 0, 320};
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side >= 0 && rumble_allowed) v = rumble_values[side];
    SDL_UnlockSpinlock(&rumble_lock);
    return v;  // requested value, not physical motor feedback
}
void input_rumble_permit(bool allowed) {
    SDL_LockSpinlock(&rumble_lock);
    rumble_allowed = allowed;
    if (!allowed) memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
bool input_rumble_permitted(void) {
    SDL_LockSpinlock(&rumble_lock);
    bool allowed = rumble_allowed;
    SDL_UnlockSpinlock(&rumble_lock);
    return allowed;
}
void input_rumble_poll(void) {
    uint64_t now = SDL_GetTicks();
    uint64_t last_frame = atomic_load(&rumble_frame_ms);
    uint16_t lo = 0, hi = 0;
    // Switch values are latched until changed: they contain no duration.
    // Renew SDL's 250 ms effect every 100 ms while frames are progressing.
    // A stalled render/UI thread therefore cannot leave an infinite effect.
    // This watchdog is a host policy, not a duration supplied by the game.
    bool suppress = (now > last_frame && now - last_frame > 250) || !pad || host_test || !g_settings.rumble || menu_is_open() ||
                    !SDL_GetKeyboardFocus();
    SDL_LockSpinlock(&rumble_lock);
    if (suppress) memset(rumble_values, 0, sizeof rumble_values);
    else if (rumble_allowed) {
        rumble_mix(rumble_values, &lo, &hi);
        int pct = g_settings.rumble_strength;  // (Controls tab: Intensity)
        if (pct > 0 && pct < 100) lo = (uint16_t)(lo * pct / 100), hi = (uint16_t)(hi * pct / 100);
    }
    SDL_UnlockSpinlock(&rumble_lock);
    if (pad && (lo != rumble_last_lo || hi != rumble_last_hi ||
                ((lo || hi) && now - rumble_last_ms >= 100))) {
        if (!SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 250 : 0) && !rumble_warned) {
            LOG("[rumble] unavailable: %s\n", SDL_GetError());
            rumble_warned = true;  // keyboard/non-rumble pads are still usable
        }
        rumble_last_ms = now;
    }
    rumble_last_lo = lo; rumble_last_hi = hi;
}
void input_rumble_stop(void) {  // UI thread: also called before normal app exit
    rumble_clear();
    atomic_store(&rumble_frame_ms, 0);
    if (pad) SDL_RumbleGamepad(pad, 0, 0, 0);
    rumble_last_lo = rumble_last_hi = 0;
}
RT_ON_RESET(input_rumble_reset) {
    input_rumble_stop();
    input_rumble_permit(true);
}

// Controller light (runtime/led.h): the light bar of the game's controller shows Link's health. UI thread only.
// While the option is off (or nothing is in play) SDL_SetGamepadLED is never called: the one call that hands the
// light back is made when we stop owning it.
int game_in_play(void);                // hle/warp.c
int cheats_link_health(int *max);      // ui/cheats.mm
static SDL_JoystickID led_pad_id;      // the pad the state below is about
static bool led_capable, led_lit;      // it has an RGB light / we set its colour
static LedColor led_last;
static uint64_t led_last_ms, led_log_ms, led_hurt_ms;
static int led_prev_hp = -1, led_prev_max;

static void led_forget(void) { led_lit = false, led_prev_hp = -1, led_pad_id = 0; }
static void led_restore(const char *why) {
    if (led_lit && pad) {
        LedColor c = led_default_color(SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS4, SDL_GetGamepadPlayerIndex(pad));
        SDL_SetGamepadLED(pad, c.r, c.g, c.b);
        LOG("[led] %s: controller colour restored\n", why);
    }
    led_lit = false, led_prev_hp = -1;
}
void input_led_stop(void) { led_restore("quit"); }  // before exit / teardown
static void led_atexit(void) { led_restore("exit"); }
RT_ON_RESET(input_led_reset) { led_restore("reset"); }

void input_led_poll(void) {
    if (!pad || host_test) { led_forget(); return; }
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id != led_pad_id) {
        led_forget();
        led_pad_id = id;
        led_capable = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
        if (led_capable) LOG("[led] %s has a light\n", SDL_GetGamepadName(pad));
    }
    if (!led_capable) return;
    if (!g_settings.controller_light) { led_restore("off"); return; }
    int max = 0, hp = game_in_play() ? cheats_link_health(&max) : -1;
    if (hp < 0 || max <= 0) { led_restore("not in play"); return; }
    uint64_t now = SDL_GetTicks();
    if (led_prev_hp >= 0 && hp < led_prev_hp && max == led_prev_max) led_hurt_ms = now;
    if (led_prev_hp < 0) led_hurt_ms = 0;  // (entering play: no flash)
    led_prev_hp = hp, led_prev_max = max;
    LedColor c = led_health_color(hp, max, now, led_hurt_ms ? now - led_hurt_ms : UINT64_MAX);
    if (led_lit && (led_equal(c, led_last) || now - led_last_ms < LED_MIN_INTERVAL_MS)) return;
    if (!SDL_SetGamepadLED(pad, c.r, c.g, c.b)) {
        LOG("[led] unavailable: %s\n", SDL_GetError());
        led_capable = false;
        return;
    }
    static bool registered;
    if (!registered) registered = true, atexit(led_atexit);
    if (!led_lit || now - led_log_ms >= 1000) {  // (the pulse changes it all the time: one line a second at most)
        LOG("[led] %d,%d,%d (health %d/%d quarter-hearts%s)\n", c.r, c.g, c.b, hp, max,
            led_hurt_ms && now - led_hurt_ms < LED_FLASH_MS ? ", hurt" : "");
        led_log_ms = now;
    }
    led_lit = true, led_last = c, led_last_ms = now;
}

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static _Atomic int32_t test_rx, test_ry;  // ("rstick": a stick pushed sideways that also reads a little up)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static _Atomic bool key_cancel_down, key_move_b_down, mouse_aim_active;
static _Atomic uint64_t key_normal_actions, key_normal_unseen, key_normal_taps;
static _Atomic uint64_t cancel_edge_start; // frame + 1; ensures a new cancel edge if sprint is held
extern uint64_t renderer_present_count(void);
static bool key_focus_toggled, key_focus_was_down;
static bool key_focus_acquired;
static uint64_t key_focus_started;
static uint64_t key_focus_started_frame;
static _Atomic uint64_t key_focus_feedback_seen,key_focus_feedback_active;
static _Atomic uint64_t key_focus_object_seen,key_focus_object_active;
static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }
static uint64_t za_menu_state(void);
static bool game_mouse_menu_state(uint64_t state);
#define ZA_BAG_STATE 0xd097e1268fc30e11ull
#define ZA_BIGMAP_STATE 0xd381bdab1369417full
static uint64_t keyboard_menu_buttons(uint64_t b, bool menu, bool bag) {
    if (!menu) return b;
    if (held_key(g_bind_key[BIND_LS_UP])) b |= 1ull << BTN_UP;
    if (held_key(g_bind_key[BIND_LS_DOWN])) b |= 1ull << BTN_DOWN;
    if (bag && held_key(g_bind_key[BIND_LS_LEFT])) b |= 1ull << BTN_L;
    if (bag && held_key(g_bind_key[BIND_LS_RIGHT])) b |= 1ull << BTN_R;
    if (held_key(g_bind_key[BIND_LS_UP]) || held_key(g_bind_key[BIND_LS_DOWN]) ||
        (bag && (held_key(g_bind_key[BIND_LS_LEFT]) || held_key(g_bind_key[BIND_LS_RIGHT])))) b &= ~(1ull << BTN_ZL);
    return b;
}

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    uint64_t scene = za_menu_state();
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    bool focus_down = held_key(g_bind_key[BIND_ZL]);
    if (scene!=ZA_BIGMAP_STATE && focus_down && !key_focus_was_down){
        key_focus_toggled = !key_focus_toggled;key_focus_acquired=false;
        key_focus_started=key_focus_toggled?SDL_GetTicksNS():0;
        key_focus_started_frame=key_focus_toggled?renderer_present_count():0;
    }
    key_focus_was_down = focus_down;
    if (key_focus_toggled) b |= 1ull << BTN_ZL;
    else b &= ~(1ull << BTN_ZL);
    // These face buttons have ordinary actions as well as focused moves.
    // Sprint is handled separately because it must not cancel ball aiming.
    bool cancel = held_key(g_bind_key[BIND_CANCEL]);
    uint64_t normal = b & ((1ull << BTN_A) | (1ull << BTN_X) | (1ull << BTN_Y));
    if (cancel) normal |= 1ull << BTN_B;
    uint64_t previous = atomic_exchange(&key_normal_actions, normal);
    atomic_fetch_or(&key_normal_unseen, normal & ~previous);
    uint64_t released = previous & ~normal;
    uint64_t unseen = atomic_fetch_and(&key_normal_unseen, ~released) & released;
    if (unseen) atomic_fetch_or(&key_normal_taps, unseen);
    bool move_focus=scene!=ZA_BIGMAP_STATE&&(b&(1ull<<BTN_ZL))&&!normal;
    bool move_b = move_focus && held_key(g_bind_key[BIND_MOVE_B]);
    atomic_store(&key_move_b_down, move_b);
    if (move_focus)
        for (int i = 0; i < 4; i++)
            if (held_key(g_bind_key[BIND_MOVE_A + i])) b |= 1ull << (BTN_A + i);
    if (move_focus && held_key(g_bind_key[BIND_MOVE_PLUS])) b |= 1ull << BTN_PLUS;
    // Cancel is independently bindable, even though the game shares a button
    // between sprint and back. Mouse aim suppresses sprint but keeps cancel.
    atomic_store(&key_cancel_down, cancel);
    if (cancel) b |= 1ull << BTN_B;
    if(scene==ZA_BIGMAP_STATE){
        // A field lock latch and the sprint binding have no map action.
        // Escape remains an explicit Back press even when Shift is held.
        b&=~(1ull<<BTN_ZL);
        if(!cancel)b&=~(1ull<<BTN_B);
    }
    // The map consumes a two-axis pan vector. Ordinary menus use W/S as
    // discrete navigation, which must not also suppress the map's Y axis.
    bool bag = scene == ZA_BAG_STATE, menu = game_mouse_menu_state(scene) && scene != ZA_BIGMAP_STATE;
    key_buttons = keyboard_menu_buttons(b, menu, bag);
    key_lx = bag ? 0 : (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = menu ? 0 : (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static bool pad_by_pos;  // (battle_abxy_position: ZL held at the last recompute_pad)
static int pad_host(int i, bool by_pos) { return by_pos && i <= BIND_Y ? bind_pad_default(i) : g_bind_pad[i]; }
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    // battle_abxy_position: Pokémon Z-A's moves are used only with ZL held and sit where the Switch's A/B/X/Y are,
    // so with A/B/X/Y bound by letter (Xbox A = the Switch's A) they read swapped. While ZL is held those four go by
    // position, as in the defaults; the rest of the game keeps the player's bindings (#26).
    int zl = g_bind_pad[BIND_ZL];
    bool by_pos = pad_by_pos = g_settings.battle_abxy_position && zl >= 0 && zl < PADB_HOST_COUNT && (host >> zl & 1);
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++) {
        int h = pad_host(i, by_pos);
        if (h >= 0 && h < PADB_HOST_COUNT && (host >> h & 1)) b |= 1ull << i;
    }
    pad_buttons = b;
    int32_t rx, ry;
    if (!pad || host_test) pad_lx = test_lx, pad_ly = test_ly, rx = test_rx, ry = test_ry;
    else {
        pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    }
    // rstick_snap: a stick pushed sideways also reads a little up or down (an Xbox One pad: 4 degrees on average, up
    // to 12), and the camera tilts with it. That close to an axis the stick is put on the axis; the angles in between
    // are spread over the rest, so nothing jumps at the edge. The length is kept.
    int snap = g_settings.rstick_snap;
    if (snap > 0 && snap < 45 && (rx || ry)) {
        double m = sqrt((double)rx * rx + (double)ry * ry), a = atan2(fabs((double)ry), fabs((double)rx)) * 180 / M_PI;
        a = a <= snap ? 0 : a >= 90 - snap ? 90 : (a - snap) * 90 / (90 - 2 * snap);
        rx = (int32_t)copysign(m * cos(a * M_PI / 180), rx);
        ry = (int32_t)copysign(m * sin(a * M_PI / 180), ry);
    }
    pad_rx = rx, pad_ry = ry;
}

// Every press reaches the game, however short. recompute_pad reads the pad as it is now, so a quick tap whose press
// and release SDL hands over together (a Bluetooth pad's reports often come batched), or that fell between two of the
// game's polls, never showed: the game's menus took a second press now and then. A press the game hasn't polled yet
// that is released is still given to the game's next polls (fill_state). UI thread: note_press; game thread: the rest.
static _Atomic uint64_t press_unseen;  // pressed (events), not yet in a state the game polled
static _Atomic uint64_t tap_buttons;   // released while unseen: the game's next poll gets them
// This private state belongs to the exact verified Lumiose 1.4.3 module. It is
// updated by the mod when the game's scene changes; never guess on other builds.
enum { SHORTCUT_NONE, SHORTCUT_MENU, SHORTCUT_RESEARCH, SHORTCUT_DEX, SHORTCUT_BOXES, SHORTCUT_SATCHEL };
#define ZA_FIELD_STATE 0x6d6b94e5322163c6ull
#define ZA_TITLE_STATE 0xdc11bb8aeed5409eull
static _Atomic uint64_t title_confirm_until;
static bool title_key_confirm_allowed(uint64_t scene,bool repeat,bool captured,int scancode){
    return scene==ZA_TITLE_STATE&&!repeat&&!captured&&scancode>0&&scancode<SDL_SCANCODE_COUNT;
}
#define ZA_MENU_STATE  0x87621892696564daull
#define ZA_BOX_STATE   0xd0abee268fd3cd32ull
#define ZA_DEX_STATE   0xaf1dd685b36e851dull
#define ZA_RESEARCH_STATE 0x6d64288245194473ull
#define ZA_SHOP_STATE 0xe1c4750380e2ef49ull
#define ZA_QUEST_STATE 0xf243285b04081aa4ull
#define ZA_MOVES_STATE 0xb8a5d7646147b713ull
#define ZA_OUTFITS_STATE 0xb74f6a4b4fc79447ull
#include "za_ui_bounds.h"
extern uint64_t renderer_present_count(void);

typedef struct { ZaUiRect rect; uint64_t scene, frame; } ZaMousePane;
static ZaMousePane mouse_live_panes[64];
static pthread_mutex_t mouse_live_pane_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t mouse_outfits_action_until;
static bool mouse_outfits_action_down;
static bool mouse_pane_publish(uint64_t scene, unsigned slot, uint64_t view, const char *name) {
    ZaUiRect rect;
    if (slot >= 64 || !za_ui_rect(za_ui_pane(view, name), &rect)) return false;
    pthread_mutex_lock(&mouse_live_pane_lock);
    mouse_live_panes[slot] = (ZaMousePane){rect, scene, renderer_present_count()};
    pthread_mutex_unlock(&mouse_live_pane_lock);
    return true;
}
static int mouse_pane_hit(uint64_t scene, float x, float y, uint64_t frame) {
    int hit = -1;
    pthread_mutex_lock(&mouse_live_pane_lock);
    for (unsigned i=0; i<64; i++) {
        ZaMousePane p=mouse_live_panes[i];
        if (p.scene==scene && p.frame && frame>=p.frame && frame-p.frame<=4 &&
            x>=p.rect.x && x<p.rect.x+p.rect.width && y>=p.rect.y && y<p.rect.y+p.rect.height) {
            if (hit>=0) { hit=-1; break; }
            hit=(int)i;
        }
    }
    pthread_mutex_unlock(&mouse_live_pane_lock);
    return hit;
}
static _Atomic int shortcut_request;
static _Atomic bool shortcut_running;
static _Atomic uint64_t shortcut_buttons;
// The scene manager belongs to the game, so mod updates do not affect this
// lookup. This offset is verified for the 2.0.2 main program; other game
// builds return unknown until their own scene manager has been verified.
static uint64_t za_game_scene_offset(const uint8_t id[8], uint64_t size) {
    static const uint8_t za_202[8] = {0xb1,0xf1,0x2f,0xd9,0x19,0xea,0xe8,0x6a};
    const uint64_t scene = 0x60fe400ull;
    return !memcmp(id, za_202, sizeof za_202) && size >= scene + 0x104 ? scene : 0;
}
static uint64_t za_menu_state(void) {
    extern uint8_t g_main_build_id[8];
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (!m->name || strcmp(m->name, "main") || m->base != 0x7000000000ull) continue;
        uint64_t offset = za_game_scene_offset(g_main_build_id, m->size);
        if (!offset || offset < m->data_off) return 0;
        uint64_t obj = m->base + offset;
        if (!__atomic_load_n((uint32_t *)(uintptr_t)(obj + 0x100), __ATOMIC_ACQUIRE)) return 0;
        return __atomic_load_n((uint64_t *)(uintptr_t)(obj + 0xc8), __ATOMIC_ACQUIRE);
    }
    return 0;
}
static _Atomic uint64_t mouse_nav_object, mouse_nav_scene, mouse_nav_epoch;
// A recent, validated field-party selector observation: frame in the high
// bits and the selected slot plus one in the low three bits.
static _Atomic uint64_t party_field_observation;
static int party_mouse_button = BTN_UP;
static bool party_deferred;
static uint64_t party_wheel_last = UINT64_MAX, party_pulse_until;
static _Atomic int za_box_active_area = -1; // 0: Box, 1: party, -1: not yet observed
// Other game lists can advertise the same verified selector when navigated.
// Keep only the scene, never a pointer to a possibly destroyed menu object.
static _Atomic uint64_t mouse_scroll_scene;
static _Atomic int mouse_list_wheel;
static _Atomic uint64_t mouse_list_wheel_scene;
static _Atomic uint64_t mouse_list_scene_epoch;
static _Atomic uint64_t mouse_list_wheel_frame;
static _Atomic uint64_t mouse_list_seen_scene, mouse_list_seen_frame;
static _Atomic unsigned mouse_shop_rows;
static _Atomic uint64_t mouse_shop_seen;
static _Atomic uint32_t mouse_tab_info;
static _Atomic uint64_t mouse_tab_seen, mouse_tab_buttons, mouse_tab_scene;
static _Atomic int mouse_tab_request;
static _Atomic int mouse_native_tab_target;
static _Atomic uint64_t mouse_native_tab_scene, mouse_native_tab_frame;
static _Atomic bool mouse_tab_running;
extern int lumiose_scroll_menu_open(void);
static _Atomic bool mouse_bag_capture_top;
static _Atomic uint32_t mouse_bag_start_top;
static _Atomic int mouse_menu_request;
static _Atomic bool mouse_menu_running;
static _Atomic bool mouse_menu_hover;
// Mouse requests for the game's scrolling lists are consumed on the guest
// thread, where the active owner and its data source can be verified.
static _Atomic int direct_list_request;
static _Atomic int direct_grid_selected;
static _Atomic uint64_t direct_grid_selected_at;
static _Atomic uint64_t direct_list_scene, direct_list_queued, direct_list_confirm_start, direct_list_buttons;
static _Atomic bool direct_list_click_pending;
static void direct_list_cancel(void);
static void za_mouse_ui_cancel(void);
static void za_mouse_ui_reset(void);
static bool za_mouse_ui_shop_list(Cpu *,uint64_t,uint64_t,unsigned,unsigned,unsigned,unsigned *);
static _Atomic uint64_t mouse_menu_modal_scene;
// Opaque identity snapshot shared with the host event thread.
typedef struct {uint64_t owner,view,key,epoch,frame;unsigned top,count;} ZaMoveBinding;
static ZaMoveBinding move_binding;
static pthread_mutex_t mouse_moves_lock=PTHREAD_MUTEX_INITIALIZER;
static void mouse_moves_bind_publish(uint64_t,uint64_t,unsigned,unsigned);
static bool mouse_moves_grid_queue(uint64_t,uint64_t,uint64_t,uint64_t,unsigned,bool);
static void mouse_moves_list_queue(unsigned,bool);
static void mouse_moves_cancel(void);
static void mouse_moves_back(void);

static _Atomic uint64_t mouse_dex_grid_observation,mouse_dex_splash_observation;
static bool mouse_dex_open_down;
static bool mouse_ui_observation_shown(uint64_t observation,uint64_t frame){
    uint64_t seen=observation>>1;
    return (observation&1)&&seen&&frame>=seen&&frame-seen<=8;
}
static bool mouse_dex_splash_active(void){
    return za_menu_state()==ZA_DEX_STATE&&mouse_ui_observation_shown(atomic_load(&mouse_dex_splash_observation),renderer_present_count());
}
static bool mouse_dex_grid_active(void){
    return za_menu_state()==ZA_DEX_STATE&&!mouse_dex_splash_active()&&mouse_ui_observation_shown(atomic_load(&mouse_dex_grid_observation),renderer_present_count());
}
static bool mouse_dex_grid_blocked(void){return za_menu_state()==ZA_DEX_STATE&&!mouse_dex_grid_active();}
typedef struct{ZaUiRect rect;uint64_t scene,frame;} ZaMouseAck;
static ZaMouseAck mouse_ack;
static pthread_mutex_t mouse_ack_lock=PTHREAD_MUTEX_INITIALIZER;
static bool mouse_ack_down;
static _Atomic uint64_t mouse_ack_confirm,mouse_ack_confirm_scene;
static bool mouse_ack_active(uint64_t scene,ZaUiRect *rect){
    uint64_t frame=renderer_present_count();pthread_mutex_lock(&mouse_ack_lock);
    bool active=scene&&mouse_ack.scene==scene&&mouse_ack.frame&&frame>=mouse_ack.frame&&frame-mouse_ack.frame<=4;
    if(active&&rect)*rect=mouse_ack.rect;
    pthread_mutex_unlock(&mouse_ack_lock);return active;
}
static bool mouse_ack_hit(float x,float y,int w,int h,ZaUiRect r){
    if(w<=0||h<=0||!isfinite(x)||!isfinite(y))return false;
    float fit=fminf(w/1920.0f,h/1080.0f);x=(x-(w-1920*fit)/2)/fit;y=(y-(h-1080*fit)/2)/fit;
    return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;
}
static _Atomic uint64_t mouse_box_context_seen, mouse_box_context_started;
typedef struct { float x, y, width, height; unsigned count; uint64_t frame; } BoxContextRect;
static BoxContextRect box_context_rect;
static pthread_mutex_t box_context_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int box_context_request;
static _Atomic bool box_context_click_pending;
static _Atomic uint64_t box_context_confirm_start, box_context_buttons;
static int box_context_down = -1, box_context_hover = -1;
static _Atomic uint64_t mouse_menu_buttons;
static _Atomic int box_wheel_steps;
static _Atomic uint64_t box_wheel_buttons;
static _Atomic int reward_wheel_steps;
static _Atomic uint64_t reward_wheel_buttons;
static _Atomic bool menu_back_request;
static _Atomic bool menu_back_active;
static _Atomic uint64_t menu_back_buttons;
static _Atomic bool mouse_drag_visual, mouse_drag_released, mouse_drag_picked;
static _Atomic bool mouse_drag_wheel_allowed;
static _Atomic bool mouse_drag_abort;
static _Atomic int mouse_drag_destination;
static _Atomic int bag_drag_request, bag_give_row;
static _Atomic uint64_t bag_give_seen;
static _Atomic unsigned bag_drag_generation;
static pthread_mutex_t drag_capture_lock = PTHREAD_MUTEX_INITIALIZER;
static ZaDragCapture drag_capture;
static bool drag_capture_pending, drag_capture_bgr;
static unsigned char *drag_capture_pixels;
static int drag_capture_w, drag_capture_h;
static uint64_t drag_capture_ready_serial;
extern bool menu_input_captured(void);
bool input_drag_capture_take(ZaDragCapture *out) {
    if (!out) return false;
    pthread_mutex_lock(&drag_capture_lock);
    bool ok = drag_capture_pending && atomic_load(&mouse_drag_visual);
    if (ok) { *out = drag_capture; drag_capture_pending = false; }
    pthread_mutex_unlock(&drag_capture_lock);
    return ok;
}
static bool drag_capture_ready(void) {
    pthread_mutex_lock(&drag_capture_lock);
    bool ready = drag_capture_ready_serial && drag_capture_ready_serial == drag_capture.serial;
    pthread_mutex_unlock(&drag_capture_lock);
    return ready;
}
void input_drag_capture_submit(uint64_t serial, const void *pixels, int w, int h, bool bgr) {
    if (!pixels || w <= 0 || h <= 0 || w > 256 || h > 256) return;
    pthread_mutex_lock(&drag_capture_lock);
    if (serial == drag_capture.serial && atomic_load(&mouse_drag_visual)) {
        size_t size = (size_t)w * h * 4;
        unsigned char *copy = malloc(size);
        if (copy) {
            memcpy(copy, pixels, size); free(drag_capture_pixels); drag_capture_pixels = copy;
            drag_capture_w = w; drag_capture_h = h; drag_capture_bgr = bgr; drag_capture_ready_serial = serial;
        }
    }
    pthread_mutex_unlock(&drag_capture_lock);
}
static void drag_capture_request(int slot, int w, int h) {
    if (slot < 0 || slot >= 36 || w <= 0 || h <= 0) return;
    float scale = fminf(w / 1920.0f, h / 1080.0f), ox = (w - 1920 * scale) * 0.5f, oy = (h - 1080 * scale) * 0.5f;
    int col = slot >= 30 ? slot - 30 : slot % 6, row = slot / 6;
    pthread_mutex_lock(&drag_capture_lock);
    drag_capture.center_x = ox + (129 + 126 * col) * scale;
    drag_capture.center_y = oy + (slot >= 30 ? 207 : 435 + 126 * row) * scale;
    drag_capture.size = 110 * scale; drag_capture.window_w = w; drag_capture.window_h = h;
    drag_capture.serial++; drag_capture_pending = true; drag_capture_ready_serial = 0;
    free(drag_capture_pixels); drag_capture_pixels = NULL;
    pthread_mutex_unlock(&drag_capture_lock);
}
static void drag_capture_item_request(int row, int w, int h) {
    if (row < 0 || row >= 7 || w <= 0 || h <= 0) return;
    float scale = fminf(w / 1920.0f, h / 1080.0f);
    pthread_mutex_lock(&drag_capture_lock);
    drag_capture.center_x = (w - 1920 * scale) * 0.5f + 150 * scale;
    drag_capture.center_y = (h - 1080 * scale) * 0.5f + (282 + 108 * row) * scale;
    drag_capture.size = 90 * scale; drag_capture.window_w = w; drag_capture.window_h = h;
    drag_capture.serial++; drag_capture_pending = true; drag_capture_ready_serial = 0;
    free(drag_capture_pixels); drag_capture_pixels = NULL;
    pthread_mutex_unlock(&drag_capture_lock);
}
static void drag_cursor_raster(unsigned char *pixels, int pitch, const unsigned char *source, int w, int h, bool bgr) {
    memset(pixels, 0, (size_t)pitch * 80);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        int sx = x * w / 64, sy = y * h / 64;
        const unsigned char *src = source + ((size_t)sy * w + sx) * 4;
        unsigned char *dst = pixels + (size_t)(y + 12) * pitch + (x + 12) * 4;
        int dx = abs(x - 31) - 27, dy = abs(y - 31) - 27;
        if (dx > 0 && dy > 0 && dx * dx + dy * dy > 25) continue;
        dst[0] = src[bgr ? 2 : 0]; dst[1] = src[1]; dst[2] = src[bgr ? 0 : 2]; dst[3] = 240;
    }
    for (int y = 1; y < 19; y++) for (int x = 1; x <= y / 2 + 1; x++) {
        unsigned char *p = pixels + (size_t)y * pitch + x * 4;
        unsigned char v = (x == 1 || x == y / 2 + 1 || y == 18) ? 0 : 255;
        p[0] = p[1] = p[2] = v; p[3] = 255;
    }
}
// SDL cursor APIs run on the UI thread. The transparent rounded crop shows
// the exact icon rendered in the source slot, including forms and shinies.
static void drag_cursor_update(void) {
    static SDL_Cursor *cursor; static uint64_t shown_serial;
    if (!atomic_load(&mouse_drag_visual)) {
        if (cursor) { SDL_SetCursor(SDL_GetDefaultCursor()); SDL_DestroyCursor(cursor); cursor = NULL; }
        shown_serial = 0; return;
    }
    pthread_mutex_lock(&drag_capture_lock);
    if (cursor && shown_serial != drag_capture.serial) {
        SDL_SetCursor(SDL_GetDefaultCursor()); SDL_DestroyCursor(cursor); cursor = NULL; shown_serial = 0;
    }
    if (drag_capture_pixels && drag_capture_ready_serial != shown_serial) {
        SDL_Surface *s = SDL_CreateSurface(80, 80, SDL_PIXELFORMAT_RGBA32);
        if (s) {
            drag_cursor_raster(s->pixels, s->pitch, drag_capture_pixels, drag_capture_w, drag_capture_h, drag_capture_bgr);
            SDL_Cursor *next = SDL_CreateColorCursor(s, 1, 1); SDL_DestroySurface(s);
            if (next && SDL_SetCursor(next)) { if (cursor) SDL_DestroyCursor(cursor); cursor = next; }
            else { if (next) SDL_DestroyCursor(next); LOG("[input] drag cursor unavailable: %s\n", SDL_GetError()); }
            shown_serial = drag_capture_ready_serial;
        }
    }
    pthread_mutex_unlock(&drag_capture_lock);
}
static void box_wheel_tick(uint64_t frame, uint64_t scene, bool busy) {
    static uint64_t until; static bool pressed;
    if (scene != ZA_BOX_STATE || menu_input_captured() || atomic_load(&mouse_menu_modal_scene) == scene) {
        atomic_store(&box_wheel_steps, 0); atomic_store(&box_wheel_buttons, 0); pressed = false; until = 0; return;
    }
    if (busy) { atomic_store(&box_wheel_buttons, 0); pressed = false; until = frame + 4; return; }
    if (frame < until) return;
    if (pressed) { pressed = false; atomic_store(&box_wheel_buttons, 0); until = frame + 4; return; }
    int n = atomic_load(&box_wheel_steps);
    while (n && !atomic_compare_exchange_weak(&box_wheel_steps, &n, n + (n > 0 ? -1 : 1))) {}
    if (n) { pressed = true; atomic_store(&box_wheel_buttons, 1ull << (n > 0 ? BTN_L : BTN_R)); until = frame + 4; }
}
// Mable's two tabs use the game's up/down navigation for wheel scrolling.
static void reward_wheel_tick(uint64_t frame, uint64_t scene) {
    static uint64_t until; static bool pressed;
    if (scene != ZA_RESEARCH_STATE || menu_input_captured() || lumiose_scroll_menu_open() ||
        atomic_load(&mouse_menu_modal_scene) == scene) {
        atomic_store(&reward_wheel_steps, 0); atomic_store(&reward_wheel_buttons, 0); pressed = false; until = 0; return;
    }
    if (atomic_load(&mouse_tab_running) || atomic_load(&mouse_tab_request)) {
        atomic_store(&reward_wheel_steps, 0); atomic_store(&reward_wheel_buttons, 0); pressed = false; until = frame + 4; return;
    }
    if (frame < until) return;
    if (pressed) { pressed = false; atomic_store(&reward_wheel_buttons, 0); until = frame + 4; return; }
    int n = atomic_load(&reward_wheel_steps);
    while (n && !atomic_compare_exchange_weak(&reward_wheel_steps, &n, n + (n > 0 ? -1 : 1))) {}
    if (n) { pressed = true; atomic_store(&reward_wheel_buttons, 1ull << (n > 0 ? BTN_UP : BTN_DOWN)); until = frame + 4; }
}
static void menu_back_tick(uint64_t frame, uint64_t scene) {
    static uint64_t start; static bool active;
    if (atomic_exchange(&menu_back_request, false)) { active = true; start = frame; }
    if (!scene || menu_input_captured() || frame < start) active = false;
    if (active && frame - start >= 6) active = false;
    atomic_store(&menu_back_active, active);
    atomic_store(&menu_back_buttons, active && frame - start >= 2 ? (1ull << BTN_B) : 0);
}

static int za_box_area_after_nav(unsigned rows, unsigned index, int32_t dy) {
    if (rows == 5 && index < 6 && dy < 0) return 1;
    if (rows == 1 && dy > 0) return 0;
    return rows == 1 ? 1 : 0;
}
// The game's own directional-navigation routine receives the selected-index
// object. Observe it only on the two verified screens and expected grids.
void rt_za_menu_nav(Cpu *c) {
    uint64_t scene = za_menu_state(), obj = c->x[0];
    if ((scene != ZA_MENU_STATE && scene != ZA_BOX_STATE) || obj < 0x7400000000ull || obj >= 0x7800000000ull) return;
    if (LD64(obj) != 0x7003bd3b30ull) return;
    unsigned cols = LD32(obj + 12), rows = LD32(obj + 16), index = LD32(obj + 8);
    if (scene == ZA_MENU_STATE ? (!((cols == 1 && rows == 6) || (cols == 6 && rows == 1)) || index >= 6) :
                                 (cols != 6 || (rows != 5 && rows != 1) || index >= cols * rows)) return;
    if (scene == ZA_BOX_STATE) {
        uint64_t direction = c->x[1];
        int32_t dy = 0;
        if (direction >= 0x7400000000ull && direction <= 0x77fffffff8ull) {
            dy = (int32_t)LD32(direction + 4);
        }
        atomic_store(&za_box_active_area, za_box_area_after_nav(rows, index, dy));
    }
    atomic_store(&mouse_nav_object, obj);
    atomic_store(&mouse_nav_scene, scene);
    atomic_fetch_add(&mouse_nav_epoch, 1);
}
void rt_za_bag_nav(Cpu *c) {
    uint64_t obj = c->x[0], scene = za_menu_state();
    if (!scene || scene == ZA_FIELD_STATE || c->x[30] != 0x7000fc40f4ull ||
        obj < 0x7400000000ull || obj >= 0x7800000000ull || LD64(obj) != 0x7003bd3ce0ull) return;
    unsigned count = LD32(obj + 8), top = LD32(obj + 16), row = LD32(obj + 20);
    unsigned cols = LD32(obj + 32), visible_rows = LD32(obj + 36);
    if (count && count <= 10000 && cols && cols <= 20 && visible_rows > 1 && visible_rows <= 100 &&
        top < count && row < count && top * cols + row < count) atomic_store(&mouse_scroll_scene, scene);
    if (scene != ZA_BAG_STATE && scene != ZA_DEX_STATE && scene != ZA_RESEARCH_STATE) return;
    if (!count || count > 10000 || top >= count ||
        (scene == ZA_BAG_STATE ? (cols != 1 || !visible_rows || visible_rows > 12 || row >= visible_rows) :
         scene == ZA_RESEARCH_STATE ? (cols != 1 || visible_rows != 8 || row >= 8) :
                                 (cols != 7 || visible_rows != 5 || row >= 35)) || top * cols + row >= count) return;
    if (atomic_exchange(&mouse_bag_capture_top, false)) atomic_store(&mouse_bag_start_top, top);
    atomic_store(&mouse_nav_object, obj); atomic_store(&mouse_nav_scene, scene);
    atomic_fetch_add(&mouse_nav_epoch, 1);
}

// The list's scrollbar and item selector normally move together. Keep the
// absolute selection while the game's own scroll animator moves the viewport.
// These objects and function addresses are specific to the checked 2.0.2 build.
typedef struct {
    uint64_t owner, selector, scene, epoch, last_tick, scan_frame, candidate, ready;
    int selected, target;
    bool pinned, animating, recover, ambiguous;
} ZaListScroll;
static ZaListScroll za_list;
static pthread_mutex_t za_list_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool za_list_busy;
static _Atomic bool za_list_guard;
static _Atomic uint64_t za_list_guard_owner, za_list_guard_scene, za_list_guard_epoch;
static _Atomic int za_list_guard_selected, za_list_guard_cols;
extern uint64_t input_buttons_now(void);
extern uint64_t renderer_present_count(void);

void rt_za_list_select(Cpu *c) {
    if (!atomic_load(&za_list_guard) || c->x[0] != atomic_load(&za_list_guard_owner) ||
        atomic_load(&za_list_guard_epoch) != atomic_load(&mouse_list_scene_epoch) ||
        atomic_load(&za_list_guard_scene) != za_menu_state()) return;
    int cols = atomic_load(&za_list_guard_cols), top = (int)(uint32_t)c->x[2];
    if (cols > 0 && cols <= 20 && top >= 0 && top <= 10000)
        c->x[1] = (uint32_t)(atomic_load(&za_list_guard_selected) - top * cols);
}

static void za_list_tick_locked(Cpu *c) {
    ZaListScroll *s = &za_list;
    uint64_t scene = za_menu_state(), o = c->x[0], frame = renderer_present_count();
    uint64_t epoch = atomic_load(&mouse_list_scene_epoch);
    if (s->epoch != epoch) { memset(s, 0, sizeof *s); s->epoch = epoch; }
    // A verified active context-menu presenter can appear over the field or
    // another screen without changing the game's scene ID.
    if (scene && o >= 0x7400000000ull && o < 0x7800000000ull && LD64(o) == 0x7003bd1e00ull && LD8(o + 0xa2)) {
        uint64_t sel = LD64(o + 0x630);
        if (sel >= 0x7400000000ull && sel < 0x7800000000ull && LD64(sel) == 0x7003bd3ce0ull &&
            LD32(sel + 8) > 0 && LD32(sel + 8) <= 8 && LD32(sel + 32) == 1 && LD32(sel + 36) == 8) {
            // Query the live scissor pane: context menus are repositioned for
            // each source slot and can be clamped at the edge of the screen.
            uint64_t view = LD64(o + 0x88), source = LD64(o + 0x148);
            if (view >= 0x7400000000ull && view < 0x7800000000ull &&
                source >= 0x7400000000ull && source < 0x7800000000ull &&
                LD64(view) == 0x7003bcf788ull && LD64(source) == 0x7003bd2280ull) {
                atomic_store(&mouse_menu_modal_scene, scene);
                atomic_store(&mouse_box_context_seen, frame);
                unsigned count = LD32(source + 0x68);
                if (scene == ZA_BAG_STATE && count && count <= 8) {
                    uint64_t begin = LD64(view + 0x2d0), end = LD64(view + 0x2d8);
                    int give = -1;
                    if (begin >= 0x7400000000ull && end >= begin && end < 0x7800000000ull &&
                        end - begin >= count * 80 && end - begin <= 8 * 80) {
                        static const char give_id[] = "msg_ui_bag_03_02";
                        bool names_ready = true;
                        for (unsigned row = 0; row < count; row++) {
                            uint64_t entry = begin + row * 80, name = LD64(entry + 8), len = LD64(entry + 16);
                            if (!len || len > 256 || name < 0x7400000000ull || name + len >= 0x7800000000ull) {
                                names_ready = false; break;
                            }
                            if (len == sizeof give_id - 1 && name >= 0x7400000000ull &&
                                name + len < 0x7800000000ull && !memcmp((const void *)(uintptr_t)name, give_id, len)) {
                                give = (int)row;
                            }
                        }
                        if (names_ready) { atomic_store(&bag_give_row, give); atomic_store(&bag_give_seen, frame); }
                    }
                }
                uint64_t wrap = LD64(view + 0xe8);
                if (count && count <= 8 && wrap >= 0x7400000000ull && wrap < 0x7800000000ull &&
                    LD64(wrap) == 0x7003bd4038ull && LD64(wrap + 0x30) >= 0x7400000000ull &&
                    LD64(wrap + 0x30) < 0x7800000000ull) {
                    Cpu saved = *c; c->x[0] = LD64(wrap + 0x30); c->x[1] = view + 0x150;
                    guest_call(c, 0x70000195bcull); uint64_t pane = c->x[0]; *c = saved;
                    if (pane >= 0x7400000000ull && pane < 0x7800000000ull) {
                        uint32_t rw = LD32(pane + 0x50), rh = LD32(pane + 0x54);
                        float width, height, x = 0, y = 0; memcpy(&width, &rw, 4); memcpy(&height, &rh, 4);
                        for (int depth = 0; pane && depth < 16; depth++) {
                            if (pane < 0x7400000000ull || pane >= 0x7800000000ull) { x = NAN; break; }
                            uint32_t rx = LD32(pane + 0x30), ry = LD32(pane + 0x34); float px, py;
                            memcpy(&px, &rx, 4); memcpy(&py, &ry, 4); x += px; y += py; pane = LD64(pane + 0x18);
                        }
                        if (isfinite(x) && isfinite(y) && isfinite(width) && isfinite(height) && width > 0 && width < 1500 && height > 0 && height < 1000) {
                            pthread_mutex_lock(&box_context_lock);
                            box_context_rect = (BoxContextRect){960 + x - width / 2, 540 - y - height / 2, width, height, count, frame};
                            pthread_mutex_unlock(&box_context_lock);
                        }
                    }
                }
                int request = atomic_exchange(&box_context_request, 0), row = (request & 15) - 1;
                if (count && count <= 8 && row >= 0 && row < (int)count && !menu_input_captured()) {
                    Cpu saved = *c; c->x[0] = o; c->x[1] = row; c->x[2] = 0;
                    guest_call(c, 0x7000fc2630ull); *c = saved;
                    if (request & 16) atomic_store(&box_context_confirm_start, frame + 1);
                }
            }
        }
    }
    if (scene != ZA_BAG_STATE && scene != ZA_DEX_STATE && scene != ZA_RESEARCH_STATE && scene != ZA_SHOP_STATE && scene != ZA_QUEST_STATE && scene != ZA_MOVES_STATE && scene != ZA_OUTFITS_STATE) {
        s->pinned = s->animating = s->recover = false;
        atomic_store(&za_list_guard, false);
        atomic_store(&mouse_list_wheel, 0); return;
    }
    if (o < 0x7400000000ull || o >= 0x7800000000ull || LD64(o) != 0x7003bd1e00ull) return;
    uint64_t sel = LD64(o + 0x630), sc = LD64(o + 0x580);
    if (sel < 0x7400000000ull || sel >= 0x7800000000ull || sc < 0x7400000000ull || sc >= 0x7800000000ull ||
        LD64(sel) != 0x7003bd3ce0ull || LD64(sc) != 0x7003bd3d28ull) return;
    unsigned count = LD32(sel + 8), max_top = LD32(sel + 12), top = LD32(sel + 16);
    unsigned cols = LD32(sel + 32), rows = LD32(sel + 36);
    // Empty TM lists and native current-move/reorder focus still belong to
    // this page. Keep its Back route alive independently of selectable rows.
    if(scene==ZA_MOVES_STATE&&cols==1&&rows==6&&count<1024&&
       za_ui_pane(LD64(o+0x88),"L_skill_list_00"))
        mouse_moves_bind_publish(o,LD64(o+0x88),top,count);
    if (scene == ZA_RESEARCH_STATE && count && count < 1024 && cols == 1 && rows == 8) {
        atomic_store(&mouse_tab_info, (2u << 8) | (LD8(o + 0xa2) ? 1u : 0u));
        atomic_store(&mouse_tab_scene, scene);
        atomic_store(&mouse_tab_seen, frame);
    }
    if (!LD8(o + 0xa2)) return;
    // The Pokédex starts with an unpopulated 1024-slot selector behind its
    // splash; its visible grid uses the populated count. Other screens keep
    // hidden placeholder lists with that same capacity.
    if (!count || count > 1024 || (count == 1024 && scene != ZA_DEX_STATE && scene != ZA_SHOP_STATE) || max_top > count || top > max_top ||
        (scene == ZA_BAG_STATE ? (cols != 1 || rows != 7) :
         scene == ZA_RESEARCH_STATE ? (cols != 1 || rows != 8) :
         scene == ZA_SHOP_STATE ? (!cols || cols > 20 || !rows || rows > 20 || cols * rows > 400) :
         scene == ZA_QUEST_STATE ? (cols != 1 || rows != 6 || !za_ui_pane(LD64(o + 0x88), "L_title_list_00/S_scissor_00")) :
         scene == ZA_MOVES_STATE ? (cols != 1 || rows != 6 || !za_ui_pane(LD64(o + 0x88), "L_skill_list_00")) :
         scene == ZA_OUTFITS_STATE ? (cols != 1 || !rows || rows > 20 ||
             (!za_ui_pane(LD64(o + 0x88), "L_tab_00/L_icon_09") && !za_ui_pane(LD64(o + 0x88), "T_choice_00"))) :
         (cols != 7 || rows != 5))) return;
    if(scene==ZA_DEX_STATE){
        bool shown=count<1024&&!mouse_dex_splash_active()&&za_ui_visible(za_ui_pane(LD64(o+0x88),"L_scroll_00"));
        atomic_store(&mouse_dex_grid_observation,(frame<<1)|(shown?1u:0u));
        if(!shown){atomic_store(&mouse_list_wheel,0);direct_list_cancel();return;}
    }
    atomic_store(&mouse_list_seen_scene, scene); atomic_store(&mouse_list_seen_frame, frame);
    // Shops may bind buy/sell lists and a quantity picker at once. Route the
    // visible list under the pointer instead of rejecting every list as
    // ambiguous or clicking an unrelated list's first item.
    if(scene==ZA_OUTFITS_STATE){
        uint64_t view=LD64(o+0x88);
        if(za_ui_pane(view,"L_tab_00/L_icon_09"))mouse_pane_publish(scene,58,view,"T_change_00");
        for(unsigned i=0;i<rows;i++){
            char name[80];
            if(!i)snprintf(name,sizeof name,"L_list_00/L_button_00");
            else snprintf(name,sizeof name,"L_list_00/L_button_00_Copy%u",i-1);
            mouse_pane_publish(scene,i,view,name);
        }
    }
    bool pointer_list=scene==ZA_SHOP_STATE||scene==ZA_OUTFITS_STATE;
    bool shop_pointer = pointer_list && za_mouse_ui_shop_list(c,scene,o,cols,rows,top,&count);
    // Do not guess if multiple matching lists exist on the same screen.
    if(pointer_list){
        if(!shop_pointer)return;
        unsigned data_max=count>rows?count-rows:0;
        if(max_top>data_max)max_top=data_max;
        if(top>max_top)return;
        s->ready=o;
    } else if (s->scan_frame != frame) {
        s->ready = s->ambiguous ? 0 : s->candidate;
        s->candidate = 0; s->ambiguous = false; s->scan_frame = frame;
    }
    if(!pointer_list){
        if (s->candidate && s->candidate != o) s->ambiguous = true;
        else s->candidate = o;
    }
    if (s->ready != o || s->last_tick == frame) return;
    s->last_tick = frame;
    if (scene == ZA_SHOP_STATE && cols == 1 && (rows == 6 || rows == 7)) {
        atomic_store(&mouse_shop_rows, rows); atomic_store(&mouse_shop_seen, frame);
    }
    if (scene == ZA_QUEST_STATE) {
        uint64_t view=LD64(o+0x88);
        for(unsigned i=0;i<rows;i++) {
            char name[80];
            if(!i) snprintf(name,sizeof name,"L_title_list_00/L_list_cell_00");
            else snprintf(name,sizeof name,"L_title_list_00/L_list_cell_00_Copy%u",i-1);
            mouse_pane_publish(scene,i,view,name);
        }
    }
    if(scene==ZA_MOVES_STATE){
        uint64_t view=LD64(o+0x88);
        mouse_moves_bind_publish(o,view,top,count);
        for(unsigned i=0;i<rows;i++){
            char name[80];
            if(!i)snprintf(name,sizeof name,"L_skill_list_00/L_waza_list_00");
            else snprintf(name,sizeof name,"L_skill_list_00/L_waza_list_00_Copy%u",i-1);
            mouse_pane_publish(scene,i,view,name);
        }
    }
    if(scene==ZA_OUTFITS_STATE){
        uint64_t view=LD64(o+0x88);
        mouse_pane_publish(scene,58,view,"T_change_00");
        for(unsigned i=0;i<rows;i++){
            char name[80];
            if(!i)snprintf(name,sizeof name,"L_list_00/L_button_00");
            else snprintf(name,sizeof name,"L_list_00/L_button_00_Copy%u",i-1);
            mouse_pane_publish(scene,i,view,name);
        }
    }
    if (s->scene != scene || s->owner != o || s->selector != sel) {
        s->pinned = s->animating = s->recover = false;
        atomic_store(&za_list_guard, false);
        s->scene = scene; s->owner = o; s->selector = sel;
    }
    int direct = 0;
    uint64_t direct_at = atomic_load(&direct_list_queued);
    int pending_direct = atomic_load(&direct_list_request);
    bool bag_party_request = (scene == ZA_BAG_STATE || scene == ZA_QUEST_STATE) && (pending_direct & 63) >= 49;
    if (!bag_party_request && !menu_input_captured() && !lumiose_scroll_menu_open() &&
        atomic_load(&mouse_menu_modal_scene) != scene && atomic_load(&direct_list_scene) == scene &&
        frame >= direct_at && frame - direct_at <= 15)
        direct = atomic_exchange(&direct_list_request, 0);
    if (direct) {
        bool applied = false;
        unsigned row = (unsigned)((direct & 63) - 1);
        uint64_t source = LD64(o + 0x148);
        if (row < cols * rows && source >= 0x7400000000ull && source < 0x7800000000ull) {
            uint64_t vt = LD64(source);
            if (vt >= 0x7003000000ull && vt < 0x7004000000ull) {
                uint64_t fn = LD64(vt + 0x80);
                if (fn >= 0x7000000000ull && fn < 0x7006200000ull) {
                    Cpu saved = *c; c->x[0] = source; guest_call(c, fn); uint32_t actual = (uint32_t)c->x[0]; *c = saved;
                    if ((uint64_t)top * cols + row < actual && (uint64_t)top * cols + row < count) {
                        applied = true;
                        if (s->pinned) {
                            s->selected = (int)(top * cols + row); s->recover = false;
                            atomic_store(&za_list_guard_selected, s->selected);
                        }
                        saved = *c; c->x[0] = o; c->x[1] = row; c->x[2] = top;
                        guest_call(c, 0x7000fc2630ull); *c = saved;
                        if (direct & 64) {
                            if (scene == ZA_BAG_STATE) {
                                atomic_store(&mouse_box_context_seen, 0); atomic_store(&mouse_box_context_started, frame + 1);
                                atomic_store(&mouse_menu_modal_scene, scene);
                            }
                            atomic_store(&direct_list_confirm_start, frame + 1);
                        }
                    }
                }
            }
        }
        if ((direct & 64) && !applied) atomic_store(&direct_list_click_pending, false);
    }
    if (menu_input_captured() || lumiose_scroll_menu_open() || atomic_load(&mouse_menu_modal_scene) == scene)
        atomic_store(&mouse_list_wheel, 0);
    if (atomic_load(&mouse_list_wheel_scene) != scene) atomic_store(&mouse_list_wheel, 0);
    uint64_t queued_at = atomic_load(&mouse_list_wheel_frame);
    if (frame < queued_at || frame - queued_at > 15) atomic_store(&mouse_list_wheel, 0);
    int wheel = atomic_exchange(&mouse_list_wheel, 0);
    if (wheel) {
        int64_t absolute = (int64_t)top * cols + (int32_t)LD32(sel + 20);
        if (!s->pinned && (absolute < 0 || absolute >= count)) return;
        if (!s->pinned) { s->selected = (int)absolute; s->target = (int)top; s->pinned = true; }
        s->target -= wheel * 3;
        if (s->target < 0) s->target = 0;
        if (s->target > (int)max_top) s->target = (int)max_top;
        Cpu saved = *c; float target = (float)s->target; uint32_t raw;
        memcpy(&raw, &target, sizeof raw); s->animating = true;
        atomic_store(&za_list_guard, false);
        atomic_store(&za_list_guard_owner, o); atomic_store(&za_list_guard_scene, scene);
        atomic_store(&za_list_guard_epoch, epoch); atomic_store(&za_list_guard_cols, (int)cols);
        atomic_store(&za_list_guard_selected, s->selected); atomic_store(&za_list_guard, true);
        c->x[0] = sc; c->x[1] = 3; c->v[0] = raw; guest_call(c, 0x7000fe8b00ull); *c = saved;
    }
    if (!s->pinned) {
        int row = (int32_t)LD32(sel + 20);
        int64_t absolute = (int64_t)top * cols + row;
        // The game can retain its list object when a screen is reopened.
        // Recognize an off-screen selection left by a previous scroll.
        if ((row < 0 || row >= (int)(cols * rows)) && absolute >= 0 && absolute < count) {
            s->selected = (int)absolute; s->target = (int)top; s->pinned = true;
        } else return;
    }
    if ((input_buttons_now() & ((1ull << BTN_UP) | (1ull << BTN_DOWN) | (1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) ||
        abs(atomic_load(&key_lx)) > 16000 || abs(atomic_load(&pad_lx)) > 16000 || abs(atomic_load(&pad_ly)) > 16000 ||
        atomic_load(&mouse_menu_running)) s->recover = true;
    if (LD32(sc + 0x178)) {
        Cpu saved = *c; c->x[0] = o; guest_call(c, 0x7000fc7e00ull); *c = saved;
    }
    if (!LD32(sc + 0x178)) { s->animating = false; atomic_store(&za_list_guard, false); }
    if (s->recover && !s->animating) {
        top = LD32(sel + 16); int row = (int32_t)LD32(sel + 20);
        if (row < 0 || row >= (int)(cols * rows)) {
            unsigned visible = row < 0 ? 0 : cols * rows - 1;
            if (top * cols + visible >= count) visible = count - 1 - top * cols;
            Cpu saved = *c; c->x[0] = o; c->x[1] = visible; c->x[2] = top;
            guest_call(c, 0x7000fc2630ull); *c = saved;
        }
        s->pinned = s->recover = false;
    }
}
void rt_za_list_tick(Cpu *c) {
    if (za_list_busy) return;
    if (pthread_mutex_trylock(&za_list_lock) != 0) return;
    za_list_busy = true;
    za_list_tick_locked(c);
    za_list_busy = false;
    pthread_mutex_unlock(&za_list_lock);
}
// Main-menu and Box selectors share a game-owned grid presenter. This
// hook runs on its guest thread, so no menu pointer escapes its lifetime.
#include "za_mouse_ui.h"
#include "za_map_mouse.h"
#include "za_shop_quantity.h"
#include "za_moves_mouse.h"

#include "za_ball_ui.h"
static bool mouse_ball_ui_closing(void);
#include "za_battle_hud.h"
#include "za_battle_keys.h"
#include "za_key_glyphs.h"
static pthread_mutex_t za_battle_keys_lock=PTHREAD_MUTEX_INITIALIZER;
static ZaBattleKey za_battle_keys[4];
static uint64_t za_battle_keys_frame,za_battle_keys_epoch,za_battle_keys_time;
static void za_battle_keys_publish(uint64_t layout,bool keyboard){
    if(!keyboard||za_menu_state()!=ZA_FIELD_STATE||!(atomic_load(&key_buttons)&(1ull<<BTN_ZL)))return;
    bool horizontal=false;pthread_mutex_lock(&za_battle_hud_lock);
    for(unsigned i=0;i<4;i++)if(za_battle_huds[i].occupied&&za_battle_huds[i].layout==layout&&za_battle_huds[i].horizontal)horizontal=true;
    pthread_mutex_unlock(&za_battle_hud_lock);if(!horizontal)return;
    ZaBattleKey keys[4];const unsigned order[4]={1,3,0,2};
    for(unsigned i=0;i<4;i++){
        char name[48];snprintf(name,sizeof name,"L_waza_%02u/T_00",order[i]);ZaUiRect rect;
        if(!za_ui_rect(za_ui_layout_pane(layout,name),&rect))return;
        keys[i]=(ZaBattleKey){.x=rect.x+rect.width/2,.y=rect.y+rect.height+24,.key=bind_prompt_keyboard_token(BIND_MOVE_A+i,2)};
    }
    pthread_mutex_lock(&za_battle_keys_lock);memcpy(za_battle_keys,keys,sizeof keys);
    za_battle_keys_frame=renderer_present_count();za_battle_keys_epoch=atomic_load(&mouse_list_scene_epoch);za_battle_keys_time=SDL_GetTicksNS();
    pthread_mutex_unlock(&za_battle_keys_lock);
}
bool input_keyboard_attack_keys(ZaBattleKey keys[4]){
    uint64_t now=SDL_GetTicksNS(),frame=renderer_present_count();bool ready;
    pthread_mutex_lock(&za_battle_keys_lock);
    ready=za_battle_keys_time&&now>=za_battle_keys_time&&now-za_battle_keys_time<=250000000ull&&
        frame>=za_battle_keys_frame&&frame-za_battle_keys_frame<=4&&
        za_battle_keys_epoch==atomic_load(&mouse_list_scene_epoch)&&atomic_load(&prompt_input)==PROMPTS_KEYBOARD&&
        (atomic_load(&key_buttons)&(1ull<<BTN_ZL))&&!atomic_load(&mouse_aim_active);
    if(ready){memcpy(keys,za_battle_keys,sizeof za_battle_keys);for(unsigned i=0;i<4;i++)keys[i].pressed=(atomic_load(&key_buttons)&(1ull<<i))!=0;}
    pthread_mutex_unlock(&za_battle_keys_lock);return ready;
}

// Called from the verified live UI-layout update delegate, on its game thread.
// The layout and panes are inspected only during this call.
void input_native_layout_prepare(uint64_t layout){za_ball_ui_prepare(layout);}
void input_native_layout_tick(uint64_t layout) {
    if(!za_menu_state()||!za_ui_span(layout,0x660)||LD64(layout)!=0x7003bd5f20ull)return;
    za_shop_quantity_layout(layout);
    // The closed focus reticle covers environmental targets such as Mega
    // crystals. Pokemon nameplates alone cannot establish an empty target.
    if(za_menu_state()==ZA_FIELD_STATE&&za_ui_layout_pane(layout,"N_focus_mark_00")&&za_ui_layout_pane(layout,"N_cap_rate_00")){
        uint64_t frame=renderer_present_count();
        if(za_ui_logical_visible(za_ui_layout_pane(layout,"N_focus_00"))&&
           za_ui_logical_visible(za_ui_layout_pane(layout,"N_focus_01")))atomic_store(&key_focus_object_active,frame);
        atomic_store(&key_focus_object_seen,frame);
    }
    if(za_menu_state()==ZA_FIELD_STATE&&za_ui_layout_pane(layout,"L_pokeplate_00/N_lockon_00")){
        bool active=false;
        for(unsigned i=0;i<20&&!active;i++)for(unsigned j=0;j<2;j++){
            char name[80];snprintf(name,sizeof name,"L_pokeplate_%02u/N_lockon_%02u",i,j);
            if(za_ui_visible(za_ui_layout_pane(layout,name)))active=true;
        }
        uint64_t frame=renderer_present_count();
        if(active)atomic_store(&key_focus_feedback_active,frame);
        atomic_store(&key_focus_feedback_seen,frame);
    }
    if(za_menu_state()==ZA_DEX_STATE&&za_ui_layout_pane(layout,"N_inout_info_00")&&
        za_ui_layout_pane(layout,"P_i_find_00")&&za_ui_layout_pane(layout,"T_hget_01")){
        bool shown=za_ui_visible(za_ui_layout_pane(layout,"N_inout_info_00"));
        atomic_store(&mouse_dex_splash_observation,(renderer_present_count()<<1)|(shown?1u:0u));
    }
    ZaUiRect ack;
    if(za_ui_visible(za_ui_layout_pane(layout,"L_msg_sys_00/L_cursor_00/P_cursor_00"))&&
        za_ui_rect(za_ui_layout_pane(layout,"L_msg_sys_00/W_base_00"),&ack)){
        pthread_mutex_lock(&mouse_ack_lock);mouse_ack=(ZaMouseAck){ack,za_menu_state(),renderer_present_count()};pthread_mutex_unlock(&mouse_ack_lock);
    }
    bool keyboard=input_prompt_auto()==PROMPTS_KEYBOARD;
    za_battle_hud_tick(layout,keyboard);
    za_battle_keys_publish(layout,keyboard);
    if(keyboard&&za_menu_state()==ZA_MOVES_STATE&&za_ui_layout_pane(layout,"L_skill_list_00")&&za_ui_layout_pane(layout,"L_switchitem_01"))
        za_ball_ui_hide(layout,"L_switch_00");
    za_ball_ui_tick(layout,mouse_ball_ui_closing());
}
void input_native_layout_destroy(uint64_t layout){za_ball_ui_forget(layout);za_battle_hud_forget(layout);}
typedef struct { uint64_t scene, epoch, frame, candidate, ready, last; bool ambiguous; } ZaGridMouse;
static ZaGridMouse za_grid_mouse;
static pthread_mutex_t za_grid_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool za_grid_busy;
static _Atomic uint64_t za_box_party, za_box_party_seen;
static void mouse_quest_grid_tick(Cpu *c, uint64_t o) {
    if (!za_ui_span(o,0x370) || LD64(o)!=0x7003bd0e60ull || !LD8(o+0xa2)) return;
    uint64_t sel=LD64(o+0x368),view=LD64(o+0x88);
    if(!za_ui_span(sel,0x20)||LD64(sel)!=0x7003bd3b30ull||LD32(sel+12)!=1||LD32(sel+16)!=3||
        !za_ui_pane(view,"L_title_list_00/S_scissor_00")) return;
    for(unsigned i=0;i<3;i++){char name[40];snprintf(name,sizeof name,"L_title_cell_%02u",i);mouse_pane_publish(ZA_QUEST_STATE,48+i,view,name);}
    uint64_t frame=renderer_present_count(),queued=atomic_load(&direct_list_queued);
    int request=atomic_load(&direct_list_request),slot=(request&63)-1;
    if(slot<48||slot>50||atomic_load(&direct_list_scene)!=ZA_QUEST_STATE||frame<queued||frame-queued>15||
        menu_input_captured()||lumiose_scroll_menu_open()||atomic_load(&mouse_menu_modal_scene)==ZA_QUEST_STATE) return;
    if(!atomic_compare_exchange_strong(&direct_list_request,&request,0)) return;
    unsigned y=(unsigned)(slot-48),old=LD32(sel+8);
    uint32_t oldx=LD32(o+0x304),oldy=LD32(o+0x308);
    ST32(o+0x304,0);ST32(o+0x308,y);
    Cpu saved=*c;c->sp-=32;ST32(c->sp,0);ST32(c->sp+4,y);c->x[0]=o;c->x[1]=c->sp;c->x[2]=1;c->x[3]=0;
    guest_call(c,0x700098d4e0ull);*c=saved;
    if(LD32(sel+8)==y&&old!=y){c->x[0]=o;c->x[1]=y;c->x[2]=old;c->x[3]=0;c->x[4]=1;guest_call(c,0x7000fad610ull);*c=saved;}
    if(LD32(sel+8)==y){atomic_store(&direct_grid_selected,slot+1);if(request&64)atomic_store(&direct_list_confirm_start,frame+1);}
    else{ST32(o+0x304,oldx);ST32(o+0x308,oldy);if(request&64)atomic_store(&direct_list_click_pending,false);}
}
void rt_za_grid_tick(Cpu *c) {
    uint64_t scene = za_menu_state(), o = c->x[0];
    za_mouse_ui_grid_tick(c,scene,o);
    if(scene==ZA_QUEST_STATE){mouse_quest_grid_tick(c,o);return;}
    if (scene == ZA_FIELD_STATE && o >= 0x7400000000ull && o < 0x7800000000ull && LD64(o) == 0x7003bd0e60ull) {
        uint64_t sel = LD64(o + 0x368), view = LD64(o + 0x88), peer = LD64(o + 0x90);
        if (sel >= 0x7400000000ull && sel < 0x7800000000ull && view >= 0x7400000000ull && view < 0x7800000000ull &&
            peer >= 0x7400000000ull && peer < 0x7800000000ull && LD64(sel) == 0x7003bd3b30ull &&
            LD32(sel + 12) == 6 && LD32(sel + 16) == 1 && LD32(sel + 8) < 6 && LD64(view) == 0x7003bcf5a0ull &&
            LD64(view + 0x120) == 0x7003bcf5a0ull && LD64(peer) == 0x7003bd25d0ull && LD8(o + 0xa2)) {
            static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
            static uint64_t tick = UINT64_MAX, owner;
            static bool ambiguous;
            if (pthread_mutex_trylock(&lock) == 0) {
                uint64_t frame = renderer_present_count();
                if (tick != frame) { tick = frame; owner = o; ambiguous = false; }
                else if (owner != o) ambiguous = true;
                atomic_store(&party_field_observation, ambiguous ? 0 : (frame << 3) | (LD32(sel + 8) + 1));
                pthread_mutex_unlock(&lock);
            }
        }
    }
    if ((scene != ZA_MENU_STATE && scene != ZA_BOX_STATE && scene != ZA_BAG_STATE) || o < 0x7400000000ull || o >= 0x7800000000ull ||
        LD64(o) != 0x7003bd0e60ull) return;
    uint64_t sel = LD64(o + 0x368);
    if (sel < 0x7400000000ull || sel >= 0x7800000000ull || LD64(sel) != 0x7003bd3b30ull) return;
    unsigned cols = LD32(sel + 12), rows = LD32(sel + 16);
    if (scene == ZA_BOX_STATE && cols == 6 && rows == 1 && LD64(o + 0x260) == 0x7000c6a384ull) {
        uint64_t next = LD64(o + 0x90);
        if (next >= 0x7400000000ull && next < 0x7800000000ull && LD64(next) == 0x7003bd0e60ull) {
            uint64_t next_sel = LD64(next + 0x368);
            if (next_sel >= 0x7400000000ull && next_sel < 0x7800000000ull &&
                LD64(next_sel) == 0x7003bd3b30ull && LD32(next_sel + 12) == 6 && LD32(next_sel + 16) == 5) {
                atomic_store(&za_box_party, o);
                atomic_store(&za_box_party_seen, renderer_present_count());
            }
        }
        return;
    }
    if (scene == ZA_MENU_STATE ? (cols != 1 || rows != 6) :
        scene == ZA_BAG_STATE ? (cols != 6 || rows != 1) : (cols != 6 || rows != 5)) return;
    uint64_t peer = LD64(o + 0x90);
    if (scene == ZA_BOX_STATE) {
        uint64_t now = renderer_present_count(), seen = atomic_load(&za_box_party_seen);
        peer = atomic_load(&za_box_party);
        if (!seen || now < seen || now - seen > 2 || peer < 0x7400000000ull || peer >= 0x7800000000ull ||
            LD64(peer + 0x90) != o || LD64(peer + 0x260) != 0x7000c6a384ull) return;
    }
    uint64_t peer_sel = 0;
    if (peer < 0x7400000000ull || peer >= 0x7800000000ull) return;
    if (scene == ZA_BAG_STATE) {
        uint64_t view = LD64(o + 0x88);
        if (LD64(peer) != 0x7003bd0c80ull ||
            view < 0x7400000000ull || view >= 0x7800000000ull || LD64(view) != 0x7003bcf5a0ull ||
            LD64(o + 0x160) != 0x7002b202bcull) return;
    } else {
        if (LD64(peer) != 0x7003bd0e60ull) return;
        peer_sel = LD64(peer + 0x368);
        if (peer_sel < 0x7400000000ull || peer_sel >= 0x7800000000ull || LD64(peer_sel) != 0x7003bd3b30ull ||
            LD32(peer_sel + 12) != 6 || LD32(peer_sel + 16) != 1) return;
    }
    if (za_grid_busy || pthread_mutex_trylock(&za_grid_lock) != 0) return;
    za_grid_busy = true;
    uint64_t frame = renderer_present_count(), epoch = atomic_load(&mouse_list_scene_epoch);
    ZaGridMouse *g = &za_grid_mouse;
    if (g->scene != scene || g->epoch != epoch) { memset(g, 0, sizeof *g); g->scene = scene; g->epoch = epoch; }
    if (g->frame != frame) { g->ready = g->ambiguous ? 0 : g->candidate; g->candidate = 0; g->ambiguous = false; g->frame = frame; }
    if (g->candidate && g->candidate != o) g->ambiguous = true; else g->candidate = o;
    if (g->ready == o && g->last != frame && !menu_input_captured() && !lumiose_scroll_menu_open() &&
        atomic_load(&mouse_menu_modal_scene) != scene && atomic_load(&direct_list_scene) == scene &&
        (scene != ZA_BAG_STATE || (atomic_load(&direct_list_request) & 63) >= 49)) {
        g->last = frame;
        uint64_t queued = atomic_load(&direct_list_queued);
        int request = frame >= queued && frame - queued <= 15 ? atomic_exchange(&direct_list_request, 0) : 0;
        if (request) {
            int slot = (request & 63) - 1;
            bool party = scene == ZA_BAG_STATE || (scene == ZA_MENU_STATE ? slot >= 36 : slot >= 30);
            int index = party ? slot - (scene == ZA_BAG_STATE ? 48 : scene == ZA_MENU_STATE ? 36 : 30) : slot;
            bool valid = party ? index >= 0 && index < 6 : index >= 0 && index < (int)(cols * rows);
            if (valid) {
                bool active_party = atomic_load(&za_box_active_area) == 1;
                if (scene == ZA_BOX_STATE && party != active_party) {
                    // Ask the game's boundary-navigation callback to activate
                    // the requested area before selecting its slot.
                    uint64_t from = party ? o : peer;
                    unsigned col = party ? (unsigned)index : (unsigned)index % cols;
                    uint32_t old_x = LD32(from + 0x304), old_y = LD32(from + 0x308);
                    ST32(from + 0x304, col); ST32(from + 0x308, party ? UINT32_MAX : 1);
                    Cpu saved = *c; c->x[0] = from; c->x[1] = col;
                    guest_call(c, 0x7000faec24ull);
                    if (!c->x[0]) { ST32(from + 0x304, old_x); ST32(from + 0x308, old_y); }
                    *c = saved;
                }
                uint64_t owner = party && scene != ZA_BAG_STATE ? peer : o;
                uint64_t target_sel = party && scene != ZA_BAG_STATE ? peer_sel : sel;
                if(!za_mouse_ui_focus_main(c,scene,owner,party?6:cols,party?1:rows)){
                    if(request&64)atomic_store(&direct_list_click_pending,false);
                    za_grid_busy=false;pthread_mutex_unlock(&za_grid_lock);return;
                }
                unsigned x = party ? (unsigned)index : (unsigned)index % cols;
                unsigned y = party ? 0 : (unsigned)index / cols;
                unsigned orientation = LD32(target_sel + 0x1c);
                unsigned expected = orientation ? y + LD32(target_sel + 16) * x : x + LD32(target_sel + 12) * y;
                uint32_t old_x = 0, old_y = 0, old_index = LD32(target_sel + 8);
                if (scene == ZA_BOX_STATE || scene == ZA_BAG_STATE || scene == ZA_MENU_STATE) {
                    old_x = LD32(owner + 0x304); old_y = LD32(owner + 0x308);
                    ST32(owner + 0x304, x); ST32(owner + 0x308, y);
                }
                Cpu saved = *c; c->sp -= 32; ST32(c->sp, x); ST32(c->sp + 4, y);
                c->x[0] = owner; c->x[1] = c->sp; c->x[2] = 1; c->x[3] = 0;
                guest_call(c, 0x700098d4e0ull); *c = saved;
                if ((scene == ZA_BOX_STATE || scene == ZA_BAG_STATE || scene == ZA_MENU_STATE) &&
                    old_index != expected && LD32(target_sel + 8) == expected) {
                    // The visual setter does not notify the model callback.
                    // Send the same index-change notification as navigation.
                    c->x[0] = owner; c->x[1] = expected; c->x[2] = old_index; c->x[3] = 0; c->x[4] = 1;
                    guest_call(c, 0x7000fad610ull); *c = saved;
                }
                if ((scene == ZA_BOX_STATE || scene == ZA_BAG_STATE || scene == ZA_MENU_STATE) && LD32(target_sel + 8) != expected) {
                    ST32(owner + 0x304, old_x); ST32(owner + 0x308, old_y);
                }
                if (LD32(target_sel + 8) == expected) {
                    if (atomic_load(&direct_grid_selected) != slot + 1 || !atomic_load(&direct_grid_selected_at))
                        atomic_store(&direct_grid_selected_at, frame);
                    atomic_store(&direct_grid_selected, slot + 1);
                }
                if (scene == ZA_BOX_STATE && LD32(target_sel + 8) == expected) {
                    atomic_store(&mouse_nav_object, target_sel); atomic_store(&mouse_nav_scene, scene);
                    atomic_store(&za_box_active_area, party ? 1 : 0);
                }
                if (LD32(target_sel + 8) == expected && (request & 64)) {
                    if (scene != ZA_BAG_STATE) {
                        atomic_store(&mouse_box_context_seen, 0); atomic_store(&mouse_box_context_started, frame + 1);
                        atomic_store(&mouse_menu_modal_scene, scene);
                    }
                    atomic_store(&direct_list_confirm_start, frame + 1);
                } else if ((request & 64) && LD32(target_sel + 8) != expected)
                    atomic_store(&direct_list_click_pending, false);
            } else if (request & 64) atomic_store(&direct_list_click_pending, false);
        }
    }
    za_grid_busy = false; pthread_mutex_unlock(&za_grid_lock);
}
// Observe the game's Satchel tab presenter. The vector is made of 40-byte
// entries; the eight-slot presenter is distinct from the background widgets.
void rt_za_tab_nav(Cpu *c) {
    uint64_t obj = c->x[0];
    uint64_t scene=za_menu_state();
    if ((scene != ZA_BAG_STATE && scene != ZA_MOVES_STATE && scene != ZA_OUTFITS_STATE) || obj < 0x7400000000ull || obj >= 0x7800000000ull ||
        LD64(obj) != 0x7003bd27c8ull || LD32(obj + 0x2d8) != (scene==ZA_MOVES_STATE?2:scene==ZA_OUTFITS_STATE?10:8) || !LD8(obj + 0x2d4)) return;
    if(scene==ZA_MOVES_STATE){
        uint64_t view=LD64(obj+0x88);
        if(!za_ui_pane(view,"L_skill_list_00"))return;
        mouse_pane_publish(scene,48,view,"L_switchitem_00");
        mouse_pane_publish(scene,49,view,"L_switchitem_01");
    }
    if(scene==ZA_OUTFITS_STATE){
        uint64_t view=LD64(obj+0x88);
        if(!za_ui_pane(view,"L_tab_00/L_icon_09"))return;
        for(unsigned i=0;i<10;i++){
            char name[80];snprintf(name,sizeof name,"L_tab_00/L_icon_%02u/P_tab_icon_00",i);
            if(!mouse_pane_publish(scene,48+i,view,name)){
                snprintf(name,sizeof name,"L_tab_00/L_icon_%02u/P_tab_icon_01",i);
                mouse_pane_publish(scene,48+i,view,name);
            }
        }
    }
    uint64_t begin = LD64(obj + 0x230), end = LD64(obj + 0x238);
    if (begin < 0x7400000000ull || end < begin || end >= 0x7800000000ull || (end - begin) % 40) return;
    unsigned count = (unsigned)((end - begin) / 40), current = LD32(obj + 0x2dc);
    if (!count || count > 16 || current >= count) return;
    extern uint64_t renderer_present_count(void);
    uint64_t frame=renderer_present_count(),requested=atomic_load(&mouse_native_tab_frame);
    if(atomic_load(&mouse_native_tab_scene)==scene&&requested&&frame>=requested&&frame-requested<=15&&
        !menu_input_captured()&&!lumiose_scroll_menu_open()&&atomic_load(&mouse_menu_modal_scene)!=scene){
        int target=atomic_exchange(&mouse_native_tab_target,0)-1;
        if(target>=0&&(unsigned)target<count){
            uint64_t callback=LD64(obj+0x210);uint32_t type=LD32(obj+0x208);
            if((unsigned)target!=current&&callback>=0x7000000000ull&&callback<0x7006200000ull&&
                (type==0||type==1||type==UINT32_MAX)){
                ST32(obj+0x2dc,target);ST32(obj+0x2e0,current);ST8(obj+0x2d1,1);ST8(obj+0x2d3,1);
                Cpu saved=*c;c->x[0]=obj+0x310;guest_call(c,0x7000fef938ull);*c=saved;
                c->x[0]=obj;guest_call(c,0x7000fd2030ull);*c=saved;
                c->x[0]=obj+0x1f0;c->x[1]=target;c->x[2]=current;guest_call(c,0x7000fd23e0ull);*c=saved;
                current=LD32(obj+0x2dc);
                if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-tab-direct] frame=%llu scene=%llx target=%d current=%u\n",(unsigned long long)frame,(unsigned long long)scene,target,current);
            }
            atomic_store(&mouse_tab_running,false);
        }
    }
    if(scene==ZA_OUTFITS_STATE){
        static uint64_t observed_owner;static unsigned observed_tab;
        if(observed_owner&&(observed_owner!=obj||observed_tab!=current)){
            atomic_fetch_add(&mouse_list_scene_epoch,1);
            atomic_store(&mouse_list_wheel,0);za_mouse_ui_reset();
        }
        observed_owner=obj;observed_tab=current;
    }
    atomic_store(&mouse_tab_info, (count << 8) | current);
    atomic_store(&mouse_tab_scene, scene);
    atomic_store(&mouse_tab_seen, renderer_present_count());
}
static void mouse_tab_cancel(void) {
    atomic_store(&mouse_tab_request, 0); atomic_store(&mouse_tab_running, false); atomic_store(&mouse_tab_buttons, 0);
    atomic_store(&mouse_native_tab_target,0);atomic_store(&mouse_native_tab_scene,0);atomic_store(&mouse_native_tab_frame,0);
}
static int mouse_tab_hit(float x, float y, int w, int h, unsigned count) {
    if (w <= 0 || h <= 0 || !isfinite(x) || !isfinite(y) || !count || count > 9) return -1;
    float scale = fminf(w / 1920.0f, h / 1080.0f);
    float px = (x - (w - 1920.0f * scale) * 0.5f) / scale;
    float py = (y - (h - 1080.0f * scale) * 0.5f) / scale;
    if (fabsf(py - 186.0f) > 35) return -1;
    for (unsigned i = 0; i < count; i++)
        if (fabsf(px - (443.0f + ((float)i - ((float)count - 1) * 0.5f) * 65.6f)) <= 30) return (int)i;
    return -1;
}
static int mouse_research_tab_hit(float x, float y, int w, int h) {
    if (w <= 0 || h <= 0 || !isfinite(x) || !isfinite(y)) return -1;
    float scale = fminf(w / 1920.0f, h / 1080.0f);
    float px = (x - (w - 1920.0f * scale) * 0.5f) / scale;
    float py = (y - (h - 1080.0f * scale) * 0.5f) / scale;
    if (fabsf(py - 84.0f) > 38) return -1;
    if (fabsf(px - 691.0f) <= 256) return 0;
    if (fabsf(px - 1229.0f) <= 256) return 1;
    return -1;
}
static bool mouse_research_list_active(void) {
    uint64_t now = renderer_present_count(), seen = atomic_load(&mouse_tab_seen);
    return atomic_load(&mouse_tab_scene) == ZA_RESEARCH_STATE && (atomic_load(&mouse_tab_info) & 255) == 1 &&
           now >= seen && now - seen <= 30;
}
static void mouse_tab_tick(uint64_t frame, uint64_t scene) {
    static uint64_t start, next; static int target, phase, expected;
    int request = atomic_exchange(&mouse_tab_request, 0);
    if(scene==ZA_BAG_STATE||scene==ZA_MOVES_STATE||scene==ZA_OUTFITS_STATE){
        if(request){
            atomic_store(&mouse_native_tab_scene,scene);atomic_store(&mouse_native_tab_frame,frame);
            atomic_store(&mouse_native_tab_target,request);atomic_store(&mouse_tab_running,true);
        }
        atomic_store(&mouse_tab_buttons,0);
        uint64_t requested=atomic_load(&mouse_native_tab_frame);
        if(atomic_load(&mouse_tab_running)&&(!requested||frame<requested||frame-requested>15))mouse_tab_cancel();
        return;
    }
    if (request) { target = request - 1; start = frame; next = frame; phase = 1; atomic_store(&mouse_tab_running, true); }
    if (!atomic_load(&mouse_tab_running)) { phase = 0; atomic_store(&mouse_tab_buttons, 0); return; }
    uint64_t seen = atomic_load(&mouse_tab_seen); unsigned info = atomic_load(&mouse_tab_info);
    unsigned count = info >> 8, current = info & 255;
    if ((scene != ZA_BAG_STATE && scene != ZA_RESEARCH_STATE && scene != ZA_MOVES_STATE && scene != ZA_OUTFITS_STATE) || menu_input_captured() || lumiose_scroll_menu_open() ||
        frame < start || frame - start > 300 || frame < seen || frame - seen > 30 || atomic_load(&mouse_tab_scene) != scene ||
        !count || count > 16 || current >= count || target < 0 || (unsigned)target >= count ||
        atomic_load(&mouse_menu_modal_scene) == scene) { mouse_tab_cancel(); phase = 0; return; }
    uint64_t bits = 0;
    if (phase == 1 && frame >= next) {
        if (current == (unsigned)target) { mouse_tab_cancel(); phase = 0; return; }
        unsigned forward = ((unsigned)target + count - current) % count;
        bool right = scene == ZA_RESEARCH_STATE ? (unsigned)target > current : forward <= count - forward;
        expected = (int)((current + (right ? 1 : count - 1)) % count);
        bits = 1ull << (scene==ZA_MOVES_STATE?(right?BTN_ZR:BTN_ZL):(right?BTN_R:BTN_L)); next = frame + 4; phase = 2;
    } else if (phase == 2) {
        if (frame < next) bits = atomic_load(&mouse_tab_buttons);
        else { next = frame + 30; phase = 3; }
    } else if (phase == 3) {
        if (current == (unsigned)expected) { phase = 1; next = frame; }
        else if (frame >= next) { mouse_tab_cancel(); phase = 0; return; }
    }
    atomic_store(&mouse_tab_buttons, bits);
}
static void mouse_menu_cancel(void) {
    atomic_store(&mouse_menu_request, 0); atomic_store(&mouse_menu_running, false); atomic_store(&mouse_menu_buttons, 0);
    atomic_store(&mouse_menu_hover, false);
    atomic_store(&mouse_drag_visual, false); atomic_store(&mouse_drag_picked, false); atomic_store(&mouse_drag_abort, false);
    atomic_store(&mouse_drag_wheel_allowed, false);
    atomic_store(&direct_grid_selected, 0);
    atomic_store(&mouse_bag_capture_top, false);
}
static unsigned mouse_shop_list_rows(void) {
    uint64_t frame = renderer_present_count(), seen = atomic_load(&mouse_shop_seen);
    unsigned rows = atomic_load(&mouse_shop_rows);
    return seen && frame >= seen && frame - seen <= 8 && (rows == 6 || rows == 7) ? rows : 0;
}
// Coordinates are from the verified 1920x1080 game layouts. Fit that design
// into the window instead of treating letterboxing as part of the interface.
static int mouse_menu_hit(uint64_t scene, float x, float y, int w, int h) {
    if (w <= 0 || h <= 0 || !isfinite(x) || !isfinite(y)) return -1;
    float scale = fminf(w / 1920.0f, h / 1080.0f);
    float px = (x - (w - 1920.0f * scale) * 0.5f) / scale;
    float py = (y - (h - 1080.0f * scale) * 0.5f) / scale;
    if (scene == ZA_QUEST_STATE || scene == ZA_MOVES_STATE || scene == ZA_OUTFITS_STATE) {
        return mouse_pane_hit(scene,px,py,renderer_present_count());
    } else if (scene == ZA_MENU_STATE && px >= 90 && px <= 678) {
        for (int i = 0; i < 6; i++) if (fabsf(py - (291.0f + 126.0f * i)) <= 57) return i;
    } else if (scene == ZA_MENU_STATE && fabsf(py - 313.0f) <= 95) {
        for (int i = 0; i < 6; i++) if (fabsf(px - (939.0f + 162.0f * i)) <= 76) return 36 + i;
    } else if (scene == ZA_BAG_STATE && px >= 84 && px <= 804) {
        for (int i = 0; i < 7; i++) if (fabsf(py - (282.0f + 108.0f * i)) <= 48) return i;
    } else if (scene == ZA_BAG_STATE && py >= 160 && py <= 408) {
        for (int i = 0; i < 6; i++) if (fabsf(px - (939.0f + 162.0f * i)) <= 76) return 48 + i;
    } else if (scene == ZA_RESEARCH_STATE && px >= 91 && px <= 887) {
        for (int i = 0; i < 8; i++) if (fabsf(py - (222.0f + 104.0f * i)) <= 46) return i;
    } else if (scene == ZA_SHOP_STATE) {
        // shop_top_00 embeds one of the validated 6- or 7-row list layouts.
        unsigned rows = mouse_shop_list_rows();
        float half_width = rows == 6 ? 465.0f : 405.0f;
        float first_y = rows == 6 ? 384.0f : 366.0f;
        float spacing = rows == 6 ? 108.0f : 96.0f;
        float half_height = rows == 6 ? 48.0f : 42.0f;
        if (rows && fabsf(px - 501.0f) <= half_width)
            for (unsigned i = 0; i < rows; i++) if (fabsf(py - (first_y + spacing * i)) <= half_height) return (int)i;
    } else if (scene == ZA_DEX_STATE) {
        for (int row = 0; row < 5; row++) for (int col = 0; col < 7; col++)
            if (fabsf(px - (1059.0f + 122.0f * col)) <= 55 && fabsf(py - (270.0f + 160.0f * row)) <= 75)
                return row * 7 + col;
    } else if (scene == ZA_BOX_STATE) {
        for (int col = 0; col < 6; col++)
            if (fabsf(px - (129.0f + 126.0f * col)) <= 57 && fabsf(py - 207.0f) <= 57) return 30 + col;
        for (int row = 0; row < 5; row++) for (int col = 0; col < 6; col++)
            if (fabsf(px - (129.0f + 126.0f * col)) <= 57 && fabsf(py - (435.0f + 126.0f * row)) <= 57)
                return row * 6 + col;
    }
    return -1;
}
static void box_context_hover_request(int row) {
    if (atomic_load(&box_context_click_pending)) return;
    int pending = atomic_load(&box_context_request);
    while (!(pending & 16) && !atomic_compare_exchange_weak(&box_context_request, &pending, row + 1)) {}
}
static int box_context_hit(float x, float y, int w, int h, uint64_t frame) {
    if (w <= 0 || h <= 0 || !isfinite(x) || !isfinite(y)) return -2;
    pthread_mutex_lock(&box_context_lock); BoxContextRect r = box_context_rect; pthread_mutex_unlock(&box_context_lock);
    if (!r.count || frame < r.frame || frame - r.frame > 8) return -2;
    float scale = fminf(w / 1920.0f, h / 1080.0f);
    float px = (x - (w - 1920 * scale) * 0.5f) / scale;
    float py = (y - (h - 1080 * scale) * 0.5f) / scale;
    if (px < r.x || px >= r.x + r.width || py < r.y || py >= r.y + r.height) return -1;
    // The scissor reserves eight rows even when the menu has fewer choices.
    int row = (int)((py - r.y) * 8 / r.height);
    return row >= 0 && row < (int)r.count ? row : -1;
}
static bool direct_list_screen(uint64_t scene) {
    return scene == ZA_MENU_STATE || scene == ZA_BOX_STATE || scene == ZA_BAG_STATE || scene == ZA_DEX_STATE || scene == ZA_QUEST_STATE || scene == ZA_MOVES_STATE || scene == ZA_OUTFITS_STATE ||
           (scene == ZA_SHOP_STATE && mouse_shop_list_rows()) || (scene == ZA_RESEARCH_STATE && mouse_research_list_active());
}
static void direct_list_queue(uint64_t scene, int row, bool click) {
    if (row < 0 || row >= 63) return;
    if(scene==ZA_MOVES_STATE){mouse_moves_list_queue((unsigned)row,click);return;}
    if (!click && atomic_load(&direct_list_click_pending)) return;
    if (click) atomic_store(&direct_list_click_pending, true);
    atomic_store(&direct_list_scene, scene);
    atomic_store(&direct_list_queued, renderer_present_count());
    atomic_store(&direct_list_request, row + 1 + (click ? 64 : 0));
}
static void direct_list_cancel(void) {
    atomic_store(&direct_list_request, 0); atomic_store(&direct_list_click_pending, false);
    atomic_store(&direct_list_confirm_start, 0); atomic_store(&direct_list_buttons, 0);
    atomic_store(&direct_list_scene,0);atomic_store(&direct_list_queued,0);
}
// A Satchel drag invokes the game's own Give option, then confirms the party
// slot. The option is identified by its game message ID, never by a guessed row.
static void bag_item_drag_tick(uint64_t frame, uint64_t scene) {
    static unsigned generation;
    static uint64_t started, submitted;
    static int phase, target;
    int request = atomic_exchange(&bag_drag_request, 0);
    if (request) {
        int item = (request & 255) - 1;
        target = ((request >> 8) & 255) - 1;
        if (scene != ZA_BAG_STATE || item < 0 || item >= 7 || target < 0 || target >= 6) { phase = 0; return; }
        generation = atomic_load(&bag_drag_generation); started = frame; phase = 1;
        atomic_store(&bag_give_seen, 0); atomic_store(&bag_give_row, -1);
        direct_list_queue(scene, item, true);
    }
    if (!phase) return;
    if (scene != ZA_BAG_STATE || menu_input_captured() || frame < started || frame - started > 180 ||
        generation != atomic_load(&bag_drag_generation)) {
        if (phase == 3) direct_list_cancel();
        phase = 0; return;
    }
    if (phase == 1) {
        uint64_t seen = atomic_load(&bag_give_seen);
        if (seen && frame >= seen && frame - seen <= 3 && atomic_load(&mouse_menu_modal_scene) == scene) {
            int row = atomic_load(&bag_give_row);
            if (row < 0) { atomic_store(&menu_back_request, true); phase = 0; return; }
            atomic_store(&box_context_click_pending, true);
            atomic_store(&box_context_request, row + 1 + 16);
            submitted = frame; phase = 2;
        }
    } else if (phase == 2 && frame >= submitted + 8 && !atomic_load(&box_context_click_pending) &&
               atomic_load(&mouse_menu_modal_scene) != scene) {
        atomic_store(&direct_grid_selected, 0);
        direct_list_queue(scene, 48 + target, true);
        phase = 3;
    } else if (phase == 3) {
        if (atomic_load(&direct_grid_selected) == 49 + target) phase = 0;
        else if (!atomic_load(&direct_list_request) && !atomic_load(&direct_list_confirm_start))
            direct_list_queue(scene, 48 + target, true);
    }
}
static void mouse_menu_tick(uint64_t frame, uint64_t scene) {
    static uint64_t start, next, seen_epoch, active_scene;
    static int target, destination, phase, probe_button; static bool drag, picked, live;
    int request = atomic_exchange(&mouse_menu_request, 0);
    if (request) {
        target = (request & 63) - 1; active_scene = (request & 262144) ? ZA_RESEARCH_STATE : (request & 65536) ? ZA_DEX_STATE : (request & 32768) ? ZA_BAG_STATE : (request & 64) ? ZA_BOX_STATE : ZA_MENU_STATE; start = frame; next = frame;
        drag = (request & 8192) != 0; live = (request & 16384) != 0;
        atomic_store(&mouse_menu_hover, (request & 131072) != 0);
        destination = ((request >> 7) & 63) - 1; picked = false; atomic_store(&mouse_drag_picked, false);
        probe_button = BTN_RIGHT;
        if (active_scene == ZA_BAG_STATE || active_scene == ZA_DEX_STATE || active_scene == ZA_RESEARCH_STATE) atomic_store(&mouse_bag_capture_top, true);
        seen_epoch = atomic_load(&mouse_nav_epoch); phase = 1;
        atomic_store(&mouse_menu_running, true);
    }
    if (!atomic_load(&mouse_menu_running)) { phase = 0; atomic_store(&mouse_menu_buttons, 0); return; }
    if ((active_scene != ZA_MENU_STATE && active_scene != ZA_BOX_STATE && active_scene != ZA_BAG_STATE && active_scene != ZA_DEX_STATE && active_scene != ZA_RESEARCH_STATE) || scene != active_scene ||
        (active_scene == ZA_RESEARCH_STATE && !mouse_research_list_active()) ||
        frame < start || (phase != 8 && frame - start > 240) || menu_input_captured()) { mouse_menu_cancel(); phase = 0; return; }
    uint64_t bits = 0;
    if (drag && atomic_load(&mouse_drag_abort) && phase != 9) {
        if (picked || phase == 7) { phase = 9; next = frame + 4; bits = 1ull << BTN_B; }
        else { mouse_menu_cancel(); phase = 0; return; }
    }
    if (phase == 1) {
        if (drag && live && active_scene == ZA_BOX_STATE) {
            atomic_store(&direct_grid_selected, 0);
            direct_list_queue(active_scene, target, false);
            // Capture the source while it is still visible in its slot. The
            // renderer may be a frame behind the input thread; picking up
            // immediately can otherwise capture the empty slot instead.
            phase = 10; next = frame + 12;
        } else {
        // A harmless navigation pulse proves the live selector is accepting
        // input before an eventual confirm, including in the presence of a modal.
        if (frame - start < 4) bits = 1ull << BTN_DOWN;
        else if (frame - start >= 12) {
            if (atomic_load(&mouse_nav_epoch) == seen_epoch || atomic_load(&mouse_nav_scene) != active_scene ||
                ((active_scene == ZA_BAG_STATE || active_scene == ZA_DEX_STATE || active_scene == ZA_RESEARCH_STATE) && atomic_load(&mouse_bag_capture_top))) {
                mouse_menu_cancel(); phase = 0; return;
            }
            if (active_scene == ZA_BAG_STATE || active_scene == ZA_DEX_STATE || active_scene == ZA_RESEARCH_STATE)
                target += (int)atomic_load(&mouse_bag_start_top) * (active_scene == ZA_DEX_STATE ? 7 : 1);
            if (active_scene == ZA_BOX_STATE) { phase = 5; next = frame; }
            else { phase = 2; next = frame; }
        }
        }
    }
    if (phase == 10 && atomic_load(&direct_grid_selected) == target + 1 &&
        (!atomic_load(&mouse_drag_visual) || atomic_load(&mouse_drag_released) ||
         drag_capture_ready() || frame >= next)) {
        phase = 7; next = frame + 20; bits = 1ull << BTN_Y;
    }
    if (phase == 2 && frame >= next) {
        uint64_t obj = atomic_load(&mouse_nav_object);
        bool scroll_list = active_scene == ZA_BAG_STATE || active_scene == ZA_DEX_STATE || active_scene == ZA_RESEARCH_STATE;
        uint64_t vt = scroll_list ? 0x7003bd3ce0ull : 0x7003bd3b30ull;
        if (!obj || atomic_load(&mouse_nav_scene) != active_scene || LD64(obj) != vt) { mouse_menu_cancel(); phase = 0; return; }
        unsigned index = scroll_list ? LD32(obj + 16) * LD32(obj + 32) + LD32(obj + 20) : LD32(obj + 8);
        unsigned cols = scroll_list ? LD32(obj + 32) : LD32(obj + 12);
        unsigned rows = scroll_list ? LD32(obj + 36) : LD32(obj + 16);
        if (active_scene == ZA_MENU_STATE && !((cols == 1 && rows == 6) || (cols == 6 && rows == 1))) { mouse_menu_cancel(); phase = 0; return; }
        unsigned limit = scroll_list ? LD32(obj + 8) : active_scene == ZA_MENU_STATE ? 6 : cols * rows;
        if (active_scene == ZA_BOX_STATE && rows != 1 && rows != 5) { mouse_menu_cancel(); phase = 0; return; }
        if (index >= limit) { mouse_menu_cancel(); phase = 0; return; }
        bool party = (active_scene == ZA_BOX_STATE && rows == 1) || (active_scene == ZA_MENU_STATE && cols == 6);
        bool target_party = (active_scene == ZA_BOX_STATE && target >= 30) || (active_scene == ZA_MENU_STATE && target >= 36);
        unsigned wanted = target_party ? (unsigned)target - (active_scene == ZA_BOX_STATE ? 30 : 36) : (unsigned)target;
        if (scroll_list && wanted >= limit) { mouse_menu_cancel(); phase = 0; return; }
        if (active_scene == ZA_BOX_STATE && party != target_party) {
            bits = 1ull << (party ? BTN_DOWN : BTN_UP);
            next = frame + 8;
            phase = party || index < 6 ? 5 : 4; probe_button = BTN_RIGHT;
        }
        else if (active_scene == ZA_MENU_STATE && party != target_party) {
            bits = 1ull << (party ? BTN_LEFT : BTN_RIGHT); next = frame + 8;
            phase = !party || index == 0 ? 5 : 4; probe_button = party ? BTN_DOWN : BTN_RIGHT;
        }
        else if (index == wanted && atomic_load(&mouse_menu_hover)) { mouse_menu_cancel(); phase = 0; return; }
        else if (index == wanted && drag && !picked) { phase = 7; next = frame + 20; bits = 1ull << BTN_Y; }
        else if (index == wanted) { phase = 3; next = frame + 4; bits = 1ull << BTN_A;
            if (!drag) { if (active_scene == ZA_BOX_STATE || active_scene == ZA_BAG_STATE || active_scene == ZA_MENU_STATE) {
                    atomic_store(&mouse_box_context_seen, 0); atomic_store(&mouse_box_context_started, frame + 1);
                    box_context_hover = -1; box_context_down = -1;
                }
                atomic_store(&mouse_menu_modal_scene, active_scene); } }
        else {
            unsigned row = index / cols, col = index % cols, tr = wanted / cols, tc = wanted % cols;
            bits = 1ull << (row < tr ? BTN_DOWN : row > tr ? BTN_UP : col < tc ? BTN_RIGHT : BTN_LEFT);
            next = frame + 8; phase = 4;
        }
    } else if (phase == 4) {
        if (frame + 4 < next) bits = atomic_load(&mouse_menu_buttons);
        else if (frame >= next) { phase = 2; next = frame; }
    } else if (phase == 5) {
        if (frame >= next) { seen_epoch = atomic_load(&mouse_nav_epoch); bits = 1ull << probe_button; next = frame + 8; phase = 6; }
        else if (frame + 4 < next) bits = atomic_load(&mouse_menu_buttons);
    } else if (phase == 6) {
        if (frame + 4 < next) bits = 1ull << probe_button;
        else if (frame >= next) {
            if (atomic_load(&mouse_nav_epoch) == seen_epoch || atomic_load(&mouse_nav_scene) != active_scene) {
                mouse_menu_cancel(); phase = 0; return;
            }
            phase = 2; next = frame;
        }
    } else if (phase == 7) {
        if (frame + 16 < next) bits = 1ull << BTN_Y;
        else if (frame >= next) {
            picked = true; atomic_store(&mouse_drag_picked, true); start = frame;
            if (live) {
                phase = 8; atomic_store(&mouse_drag_wheel_allowed, true);
                int hover = atomic_load(&mouse_drag_destination);
                if (hover >= 0) direct_list_queue(active_scene, hover, false);
            }
            else { target = destination; phase = 2; next = frame; }
        }
    } else if (phase == 8) {
        if (atomic_load(&mouse_drag_released) && !atomic_load(&box_wheel_steps) && !atomic_load(&box_wheel_buttons)) {
            atomic_store(&mouse_drag_wheel_allowed, false);
            destination = atomic_load(&mouse_drag_destination); start = frame;
            if (destination < 0) { phase = 9; next = frame + 4; bits = 1ull << BTN_B; }
            else { target = destination;
                if(atomic_load(&direct_grid_selected)==target+1){
                    // Measure the native focus transition from selection,
                    // rather than restarting a fixed wait on mouse release.
                    next=atomic_load(&direct_grid_selected_at)+6;
                    if(frame>=next){phase=3;next=frame+4;bits=1ull<<BTN_A;}
                    else phase=12;
                }
                else {direct_list_queue(active_scene,target,false);phase=11;}
            }
        }
    } else if (phase == 11) {
        if (atomic_load(&direct_grid_selected) == target + 1) {
            // Very short drags can release before pickup/focus finishes.
            // Confirm once that same destination has settled; a normal
            // drag has already completed this transition while moving.
            uint64_t ready=atomic_load(&direct_grid_selected_at)+6;
            phase=12;next=ready>frame?ready:frame+1;
        }
    } else if (phase == 12) {
        if (frame >= next) { phase = 3; next = frame + 4; bits = 1ull << BTN_A; }
    } else if (phase == 9) {
        if (frame < next) bits = 1ull << BTN_B;
        else { mouse_menu_cancel(); phase = 0; return; }
    } else if (phase == 3) {
        if (frame < next) bits = 1ull << BTN_A;
        else { mouse_menu_cancel(); phase = 0; return; }
    }
    atomic_store(&mouse_menu_buttons, bits);
}
static void shortcut_cancel(void) {
    if (atomic_load(&shortcut_running) && atomic_load(&direct_list_scene) == ZA_MENU_STATE &&
        !atomic_load(&direct_list_click_pending))
        direct_list_cancel();
    atomic_store(&shortcut_request, 0); atomic_store(&shortcut_running, false); atomic_store(&shortcut_buttons, 0);
}
// The game creates the menu selector only after opening the menu. Select the
// requested entry directly as soon as it is ready, without simulated scrolling.
static void shortcut_tick(uint64_t frame, uint64_t state) {
    static int kind, phase, target; static uint64_t started, menu_at, pulse_at;
    int next = atomic_exchange(&shortcut_request, 0);
    if (next) {
        direct_list_cancel(); atomic_store(&direct_grid_selected, 0);
        kind = next; phase = 1; started = frame; atomic_store(&shortcut_running, true);
    }
    if (!atomic_load(&shortcut_running)) { phase = 0; atomic_store(&shortcut_buttons, 0); return; }
    if (frame < started || frame - started > 200 || !state) { shortcut_cancel(); phase = 0; return; }
    uint64_t bits = 0;
    if (phase == 1) {
        if (frame - started < 4) bits = 1ull << BTN_X;
        else phase = 2;
    }
    if (phase == 2 && state == ZA_MENU_STATE) {
        if (kind == SHORTCUT_MENU) { shortcut_cancel(); phase = 0; return; }
        phase = 3; menu_at = frame;
        target = kind == SHORTCUT_DEX ? 2 : kind == SHORTCUT_SATCHEL ? 1 : 0;
    }
    if (phase >= 3 && state != ZA_MENU_STATE) { shortcut_cancel(); phase = 0; return; }
    if (phase == 3 && frame >= menu_at + 4) {
        if (kind == SHORTCUT_RESEARCH) { phase = 5; pulse_at = frame; }
        else { direct_list_queue(state, target, false); phase = 4; }
    }
    if (phase == 4) {
        if (atomic_load(&direct_grid_selected) == target + 1) { phase = 5; pulse_at = frame; }
        else if (!atomic_load(&direct_list_request)) direct_list_queue(state, target, false);
    }
    if (phase == 5) {
        // Menu opening animation may still reject the first press. Retry until
        // the scene changes, with a release between presses and a bounded timeout.
        if (frame - pulse_at >= 48) { shortcut_cancel(); phase = 0; return; }
        if ((frame - pulse_at) % 8 < 4) bits = 1ull << (kind == SHORTCUT_RESEARCH ? BTN_X : BTN_A);
    }
    atomic_store(&shortcut_buttons, bits);
}
static _Atomic int lumiose_chat_wheel_pending;  // Retained if the bridge ring is full.
extern int lumiose_scroll_menu_open(void);

// The pre-1.4.4 mod has no chat-focus signal. Keyboard chat mode is entered
// explicitly with Enter and exited with Escape or when the keyboard opens.
// Keep generated presses separate from physical controls and hold them long
// enough for the mod to observe them even at a low frame rate.
static _Atomic bool legacy_chat_mode;
static pthread_mutex_t legacy_chat_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t button; unsigned ms; } legacy_chat_queue[32];
static unsigned legacy_chat_read, legacy_chat_write;
static uint64_t legacy_chat_until, legacy_chat_release_until, legacy_chat_button;
static bool legacy_chat_enqueue(uint64_t button, unsigned ms) {
    pthread_mutex_lock(&legacy_chat_lock);
    bool room = legacy_chat_write - legacy_chat_read < 32;
    if (room) {
        unsigned i = legacy_chat_write++ % 32;
        legacy_chat_queue[i].button = button;
        legacy_chat_queue[i].ms = ms;
    }
    pthread_mutex_unlock(&legacy_chat_lock);
    return room;
}
bool input_lumiose_queue_direction(int direction) {
    return legacy_chat_enqueue(1ull << (direction < 0 ? BTN_UP : BTN_DOWN), 90);
}
static uint64_t legacy_chat_pulse(void) {
    uint64_t now = SDL_GetTicks(), result = 0;
    pthread_mutex_lock(&legacy_chat_lock);
    if (legacy_chat_button && now >= legacy_chat_until) {
        legacy_chat_button = 0;
        legacy_chat_release_until = now + 80;
    }
    if (!legacy_chat_button && now >= legacy_chat_release_until && legacy_chat_read != legacy_chat_write) {
        unsigned i = legacy_chat_read++ % 32;
        legacy_chat_button = legacy_chat_queue[i].button;
        legacy_chat_until = now + legacy_chat_queue[i].ms;
    }
    result = legacy_chat_button;
    pthread_mutex_unlock(&legacy_chat_lock);
    return result;
}
bool lumiose_legacy_chat_key(const SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) return false;
    LumioseChatSnapshot snapshot;
    if (lumiose_chat_bridge_snapshot(&snapshot)) return false;
    if (!getenv("LUMIOSE_PREPARE_BUILD_ID") || g_nmodules <= 2 || lumiose_scroll_menu_open()) {
        atomic_store(&legacy_chat_mode, false);
        return false;
    }
    SDL_Scancode key = e->key.scancode;
    bool active = atomic_load(&legacy_chat_mode);
    if (getenv("LARECOMP_TEXT_CONTEXT_LOG") && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_TAB))
        LOG("[chat-input] key %d legacy focus %d\n", (int)key, active);
    if (!active && key != SDL_SCANCODE_RETURN && key != SDL_SCANCODE_KP_ENTER) return false;
    if (!active && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_CTRL))) return false;
    uint64_t button = 0; unsigned duration = 90;
    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) {
        button = 1ull << (active ? BTN_A : BTN_MINUS);
        if (!active) duration = 700;
    } else if (active) {
        switch (key) {
        case SDL_SCANCODE_ESCAPE: button = 1ull << BTN_B; break;
        case SDL_SCANCODE_TAB: button = 1ull << ((e->key.mod & SDL_KMOD_SHIFT) ? BTN_L : BTN_R); break;
        case SDL_SCANCODE_LEFT: button = 1ull << BTN_LEFT; break;
        case SDL_SCANCODE_RIGHT: button = 1ull << BTN_RIGHT; break;
        default: return false;
        }
    }
    if (!button) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && legacy_chat_enqueue(button, duration)) {
        if (!active) atomic_store(&legacy_chat_mode, true);
        else if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER)
            atomic_store(&legacy_chat_mode, false);
    }
    return true;
}
// Cancelling the chat keyboard returns to the still-open chat. Confirming it
// sends and closes chat, so only cancellation restores the legacy focus hint.
void lumiose_legacy_chat_keyboard_result(bool cancelled) {
    LumioseChatSnapshot snapshot;
    if (!lumiose_chat_bridge_snapshot(&snapshot) && getenv("LUMIOSE_PREPARE_BUILD_ID") && g_nmodules > 2)
        { atomic_store(&legacy_chat_mode, cancelled);
          if (getenv("LARECOMP_TEXT_CONTEXT_LOG")) LOG("[chat-input] keyboard cancelled %d\n", cancelled); }
}
// The same mapping as recompute_pad's, ZL by position included: by letter here, a move pressed with ZL held left its
// by-letter button unseen, and the release gave the game a tap of it - the move on the other side, used when the
// pressed one was cooling down (#42)
static uint64_t pad_bits(int host) {   // the game buttons bound to host button `host` (SDL button / PADB_*TRIGGER)
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (pad_host(i, pad_by_pos) == host) b |= 1ull << i;
    return b;
}
uint64_t input_gamepad_button_bits(int host) { return pad_bits(host); }
static void note_press(uint64_t down, uint64_t up) {
    if (down) { extern void lumiose_keyboard_note_buttons(uint64_t); lumiose_keyboard_note_buttons(down); }
    if (down) press_unseen |= down;
    if (up) {
        uint64_t u = atomic_fetch_and(&press_unseen, ~up) & up;
        if (u) tap_buttons |= u;
    }
}

// Native chat consumes key-up events too; clear keys held when it takes focus
// so a movement key cannot remain stuck after the player finishes typing.
void input_release_keyboard_for_chat(void) {
    memset(keys_down, 0, sizeof keys_down);
    key_focus_toggled = key_focus_was_down = false;
    recompute_keyboard();
    atomic_store(&key_normal_unseen, 0); atomic_store(&key_normal_taps, 0);
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    pthread_mutex_lock(&za_battle_keys_lock);za_battle_keys_time=0;pthread_mutex_unlock(&za_battle_keys_lock);
    shortcut_cancel();
    mouse_menu_cancel();
    mouse_tab_cancel();
    atomic_store(&mouse_menu_modal_scene, 0);
    atomic_store(&mouse_box_context_seen, 0);
    atomic_store(&mouse_box_context_started, 0);
    atomic_store(&mouse_scroll_scene, 0);
    atomic_store(&mouse_list_wheel, 0);
    atomic_store(&reward_wheel_steps, 0); atomic_store(&reward_wheel_buttons, 0);
    atomic_store(&bag_drag_request, 0); atomic_store(&bag_give_seen, 0); atomic_store(&bag_give_row, -1);
    atomic_fetch_add(&bag_drag_generation, 1);
    press_unseen = 0, tap_buttons = 0;
    // Releases while a text dialog owns input never reach input_handle_event.
    // Drop the previously held game buttons until the next controller event.
    atomic_store(&pad_buttons, 0);
    atomic_store(&lumiose_chat_wheel_pending, 0);
    atomic_store(&legacy_chat_mode, false);
    pthread_mutex_lock(&legacy_chat_lock);
    legacy_chat_read = legacy_chat_write = 0;
    legacy_chat_until = legacy_chat_release_until = legacy_chat_button = 0;
    pthread_mutex_unlock(&legacy_chat_lock);
    rumble_clear();
    memset(keys_down, 0, sizeof keys_down);
    key_focus_toggled = key_focus_was_down = false;
    recompute_keyboard();
    atomic_store(&key_normal_unseen, 0); atomic_store(&key_normal_taps, 0);
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log
volatile int g_shot_request;         // F10: the game image -> screenshots/shot_<frame>.png

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: / mouse:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_WHEEL: return e->wheel.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
static bool input_user_blocked_any(void) {
    static int on = -1;
    static bool explicit_block;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        explicit_block = v && atoi(v) != 0;
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST") || getenv("LARECOMP_REPLAY"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    extern bool g_replay_handed;
    return on && (explicit_block || !g_replay_handed);
}
bool input_user_blocked(const SDL_Event *e) {
    if (!input_user_blocked_any()) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// The mouse cursor during play: hidden as soon as a controller (a button, a stick or trigger past a small deadzone) or
// a key is used, shown again only when the mouse moves (a few points, so a bumped desk doesn't count), clicks or
// scrolls. Every window mode and platform: SDL hides it over our window only. Main thread (the event loops: the
// game's ui_run and the start screen).
static bool cursor_hidden;  // (the start screen keeps ImGui's backend from showing it again meanwhile)
static _Atomic uint32_t prompt_pad;
static int prompt_controller_type(SDL_GamepadType type, uint16_t vendor) {
    bool xbox = type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE;
    return xbox && (!vendor || vendor == 0x045e) ? PROMPTS_XBOX : PROMPTS_NINTENDO;
}
void input_note_prompt_event(const SDL_Event *e) {
    static float motion;
    if (pad && !game_mouse_menu_state(za_menu_state()) && (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e->type == SDL_EVENT_MOUSE_BUTTON_UP || e->type == SDL_EVENT_MOUSE_WHEEL)) return;
    SDL_JoystickID id = 0;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) return;
        if (e->key.scancode >= SDL_SCANCODE_LCTRL && e->key.scancode <= SDL_SCANCODE_RGUI) {
            bool bound = false;
            for (int i = 0; i < BIND_KEY_COUNT; i++) if (g_bind_key[i] == e->key.scancode) bound = true;
            if (!bound) return; // e.g. Alt-Tab is not gameplay
        }
        atomic_store(&prompt_input, PROMPTS_KEYBOARD); motion = 0; return;
    case SDL_EVENT_MOUSE_MOTION:
        motion += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        if (motion <= 6) return;
        atomic_store(&prompt_input, PROMPTS_KEYBOARD); motion = 0; return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_WHEEL:
        atomic_store(&prompt_input, PROMPTS_KEYBOARD); motion = 0; return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: id = e->gbutton.which; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (abs(e->gaxis.value) <= 16000) return;
        id = e->gaxis.which; break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (atomic_load(&prompt_pad) == e->gdevice.which) {
            atomic_store(&prompt_pad, 0);
            atomic_store(&prompt_input, PROMPTS_KEYBOARD);
        }
        return;
    default: return;
    }
    if (id && SDL_IsGamepad(id)) {
        atomic_store(&prompt_pad, id);
        atomic_store(&prompt_input, prompt_controller_type(SDL_GetGamepadTypeForID(id), SDL_GetGamepadVendorForID(id)));
        motion = 0;
    }
}
int input_prompt_auto(void) {
    int active = atomic_load(&prompt_input);
    if (active) return active;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    int style = ids && n ? prompt_controller_type(SDL_GetGamepadTypeForID(ids[0]), SDL_GetGamepadVendorForID(ids[0])) : PROMPTS_KEYBOARD;
    if (ids && n) atomic_store(&prompt_pad, ids[0]);
    SDL_free(ids);
    int expected = 0;
    atomic_compare_exchange_strong(&prompt_input, &expected, style);
    return atomic_load(&prompt_input);
}
bool input_cursor_hidden(void) { return cursor_hidden; }
void input_cursor_event(const SDL_Event *e) {
    static float moved;  // (points the mouse moved since the cursor was hidden)
    bool hide = false, show = false;
    // A controller disables mouse camera motion, but never owns the cursor.
    // Keep the mouse usable for the mod's overworld icons and every menu.
    if (pad) {
        SDL_ShowCursor();
        cursor_hidden = false, moved = 0;
        return;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:  // (a modifier alone - Cmd / Alt for switching apps - isn't play)
        hide = !e->key.repeat && !(e->key.scancode >= SDL_SCANCODE_LCTRL && e->key.scancode <= SDL_SCANCODE_RGUI);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: hide = true; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: hide = abs(e->gaxis.value) > 12000; break;  // (stick drift stays below)
    case SDL_EVENT_MOUSE_MOTION:
        moved += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        show = moved > 6;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_WHEEL: show = true; break;
    default: break;
    }
    if (hide && !cursor_hidden) {
        SDL_HideCursor();
        cursor_hidden = true, moved = 0;
        LOG("[cursor] hidden (%s): SDL cursor visible=%d\n", e->type == SDL_EVENT_KEY_DOWN ? "key" : "controller",
            SDL_CursorVisible());
    } else if (show && cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = false;
        LOG("[cursor] shown (mouse %s): SDL cursor visible=%d\n",
            e->type == SDL_EVENT_MOUSE_MOTION ? "moved" : e->type == SDL_EVENT_MOUSE_WHEEL ? "wheel" : "button",
            SDL_CursorVisible());
    }
}

// "Allow background inputs" (Controls tab, off by default): controllers keep working while the window isn't in front.
// SDL drops controller events without keyboard focus unless this hint is set; it reads the hint live. (On macOS the
// hint makes the system ask for Input Monitoring - reading HID input for a background app - so it was taken out as a
// default there; the player opts in here.)
SDL_JoystickID input_active_pad(void) { return pad ? SDL_GetGamepadID(pad) : 0; }
void input_apply_background(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_settings.bg_input ? "1" : "0");
}

// A controller SDL has no mapping for (on macOS e.g. a generic Bluetooth HID pad, such as a phone app posing as one)
// is only a joystick to SDL, and the game reads gamepads only. gamecontrollerdb.txt in the data root holds the
// player's own mappings, in SDL's format (lines for other platforms are skipped, so SDL_GameControllerDB's file works
// as is). Called right after SDL_Init: a pad already connected becomes a gamepad then (SDL sends GAMEPAD_ADDED).
void input_load_user_mappings(void) {
    if (!SDL_GetPathInfo("gamecontrollerdb.txt", NULL)) return;
    int n = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    if (n < 0) LOG("[input] gamecontrollerdb.txt: %s\n", SDL_GetError());
    else LOG("[input] gamecontrollerdb.txt: %d controller mapping(s) added\n", n);
}

// Controller family for the button prompts: recognized Xbox controllers use
// Xbox glyphs; other controllers keep the game's original drawings.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
int input_family_of_type(int type) {
    return type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE ? 1 : 0;
}
static _Atomic int pad_family;
static void note_family(void) {
    int f = pad ? input_family_of_type(SDL_GetGamepadType(pad)) : 0;
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// Convert each frame's relative movement to a right-stick speed. Cache the result for repeated HID reads in the
// same frame; do not smooth across frames, so stopping or reversing the mouse takes effect immediately. The game
// still applies its own stick response and maximum turning speed.
static _Atomic int32_t mouse_acc_x, mouse_acc_y;
// Direct camera input has its own queue because HID can be sampled more often than the camera. When no supported
// game camera is updating, the ordinary stick path continues to handle the mouse (including in other games).
static _Atomic int32_t mouse_native_x, mouse_native_y;
static _Atomic uint64_t mouse_aim_return_until;
static _Atomic bool mouse_ball_ui_cancelled;
static _Atomic uint64_t mouse_throw_cancel_after, mouse_throw_cancel_until;
static _Atomic uint64_t mouse_native_last;
static _Atomic uint64_t mouse_ball_native_last;
typedef struct {float yaw,pitch;uint64_t frame,time;unsigned generation;} ZaFollowPose;
static ZaFollowPose mouse_follow_pose;
static pthread_mutex_t mouse_follow_pose_lock=PTHREAD_MUTEX_INITIALIZER;
#define MOUSE_NATIVE_TIMEOUT_NS 1000000000ull
#define MOUSE_AIM_RETURN_NS 1000000000ull
static bool mouse_native_enabled(void) {
    const char *s = getenv("LUMIOSE_NATIVE_MOUSE");
    return !s || *s != '0';
}
static bool mouse_native_active(uint64_t now) {
    uint64_t last = atomic_load(&mouse_native_last);
    return mouse_native_enabled() && last && now >= last && now - last <= MOUSE_NATIVE_TIMEOUT_NS;
}
static void mouse_add_motion(float x, float y) {
    int32_t dx = (int32_t)lroundf(x * 16), dy = (int32_t)lroundf(y * 16);
    atomic_fetch_add(&mouse_acc_x, dx);
    atomic_fetch_add(&mouse_acc_y, dy);
    if (mouse_native_active(SDL_GetTicksNS())) {
        atomic_fetch_add(&mouse_native_x, dx);
        atomic_fetch_add(&mouse_native_y, dy);
    }
}
static _Atomic unsigned mouse_generation;
static void mouse_clear(void) {
    atomic_exchange(&mouse_acc_x, 0);
    atomic_exchange(&mouse_acc_y, 0);
    atomic_exchange(&mouse_native_x, 0);
    atomic_exchange(&mouse_native_y, 0);
    atomic_store(&mouse_native_last, 0);
    atomic_store(&mouse_ball_native_last, 0);
    atomic_store(&mouse_aim_return_until, 0);
    atomic_store(&mouse_ball_ui_cancelled,false);
    atomic_store(&mouse_throw_cancel_after, 0); atomic_store(&mouse_throw_cancel_until, 0);
    atomic_fetch_add(&mouse_generation, 1);
}
static void mouse_stick(int32_t *rx, int32_t *ry) {
    static uint64_t last;
    static uint64_t frame_seen = UINT64_MAX;
    static unsigned generation;
    static int32_t cached_x, cached_y;
    static double full = -1, floor_ = -1;
    if (pad) {
        atomic_exchange(&mouse_acc_x, 0); atomic_exchange(&mouse_acc_y, 0);
        cached_x = cached_y = 0; last = 0; frame_seen = UINT64_MAX;
        *rx = *ry = 0; return;
    }
    if (full < 0) {
        full = getenv("LARECOMP_MOUSE_FULL") ? atof(getenv("LARECOMP_MOUSE_FULL")) : 1200;   // px/s for full tilt at speed 1
        floor_ = getenv("LARECOMP_MOUSE_FLOOR") ? atof(getenv("LARECOMP_MOUSE_FLOOR")) : 0.2;  // (the stick's dead zone)
        if (!isfinite(full) || full <= 0) full = 1200;
        if (!isfinite(floor_) || floor_ < 0 || floor_ > 1) floor_ = 0.2;
    }
    extern uint64_t renderer_present_count(void);
    uint64_t frame = renderer_present_count();
    unsigned current_generation = atomic_load(&mouse_generation);
    if (generation != current_generation) {
        generation = current_generation; cached_x = cached_y = 0; last = 0; frame_seen = UINT64_MAX;
    }
    if (frame == frame_seen) { *rx = cached_x; *ry = cached_y; return; }
    frame_seen = frame;
    uint64_t now = SDL_GetTicksNS();
    double dt = last && now > last ? (double)(now - last) / 1e9 : 1.0 / 60.0;
    last = now;
    if (dt > 0.1) dt = 1.0 / 60.0;
    if (dt < 0.001) dt = 0.001;
    double vx = atomic_exchange(&mouse_acc_x, 0) / 16.0 / dt;
    double vy = atomic_exchange(&mouse_acc_y, 0) / 16.0 / dt;
    double v = sqrt(vx * vx + vy * vy);
    if (v == 0) { *rx = *ry = cached_x = cached_y = 0; return; }
    double m = v * g_settings.mouse_sens / full;
    double d = (floor_ + (1 - floor_) * (m > 1 ? 1 : m)) * 32767 / v;
    *rx = cached_x = (int32_t)(vx * d), *ry = cached_y = (int32_t)(-vy * d);
}
// mouse buttons through g_bind_mouse (Switch button bits), counted only while the cursor is held: a click that brings
// the window to the front, or one meant for the menu, is not a ball thrown
static _Atomic bool mouse_held;
static _Atomic uint64_t mouse_buttons;
static _Atomic bool mouse_cancel_clear;
// Packed Switch touch coordinates. A pending press preserves a quick click until the guest reads it.
static _Atomic uint64_t touch_active, touch_pending, touch_sampling;
static SDL_Window *touch_window;
static int mouse_menu_down = -1;
static int mouse_tab_down = -1;
static uint64_t mouse_tab_down_scene;
static uint64_t mouse_menu_down_scene;
static int mouse_hover_last = -1;
static uint64_t mouse_hover_scene;
static float mouse_menu_down_x, mouse_menu_down_y;
static bool mouse_menu_drag_started;
extern bool menu_input_captured(void);

// Called only at the verified 2.0.2 follow-camera update. Values are radians and a normalized vertical position.
// The camera's own setter wraps yaw; the pitch parameter is clamped before it is sent to the game.
void rt_za_native_mouse(Cpu *c) {
    if (!mouse_native_enabled()) return;
    static float last_yaw, last_pitch;
    static bool have_angles;
    static unsigned seen_generation;
    static uint64_t aim_release_until;
    unsigned generation = atomic_load(&mouse_generation);
    if (seen_generation != generation) { seen_generation = generation; have_angles = false; aim_release_until = 0; }
    uint64_t now = SDL_GetTicksNS(), previous = atomic_exchange(&mouse_native_last, now);
    if (!previous || now < previous || now - previous > MOUSE_NATIVE_TIMEOUT_NS || menu_input_captured() || pad) {
        atomic_exchange(&mouse_native_x, 0);
        atomic_exchange(&mouse_native_y, 0);
        have_angles = false; aim_release_until = 0;
        return;
    }
    double dx = atomic_exchange(&mouse_native_x, 0) / 16.0;
    double dy = atomic_exchange(&mouse_native_y, 0) / 16.0;
    float yaw = VSF32(0), pitch = VSF32(1);
    if (!isfinite(yaw) || !isfinite(pitch) || !isfinite(g_settings.mouse_sens)) return;
    uint64_t held = atomic_load(&mouse_buttons) | atomic_load(&key_buttons);
    // Preserve the view through the game's ball-aim and return transitions.
    // Its normal-camera restore arrives several frames after the button release.
    // Ordinary battle focus keeps the game camera; projectile targeting is separate.
    bool ball_aim = (held & (1ull << BTN_ZR)) != 0 || atomic_load(&mouse_aim_active);
    bool battle_only = (held & (1ull << BTN_ZL)) && !ball_aim;
    bool stable = g_settings.mouse_aim_camera && mouse_held && !battle_only;
    if (stable && (held & (1ull << BTN_ZR))) aim_release_until = now + MOUSE_AIM_RETURN_NS;
    else if (!stable) aim_release_until = 0;
    if (stable && have_angles && ((held & (1ull << BTN_ZR)) || (aim_release_until && now < aim_release_until)))
        yaw = last_yaw, pitch = last_pitch;
    yaw -= (float)(dx * 0.003 * g_settings.mouse_sens);
    pitch += (float)(dy * 0.0015 * g_settings.mouse_sens);
    if (pitch < 0) pitch = 0;
    if (pitch > 1) pitch = 1;
    FP_SETS(0, yaw);
    FP_SETS(1, pitch);
    last_yaw = yaw; last_pitch = pitch; have_angles = true;
}
// The follow-camera getter has already evaluated its native yaw/pitch curve.
// Its target-relative offset gives the optical direction without an extra
// mouse queue or a second sensitivity/turn-rate calculation.
void input_native_follow_offset(const float *offset,float height){
    if(!mouse_native_enabled()||!g_settings.mouse_aim_camera||!mouse_held||pad||!isfinite(height)||
       !isfinite(offset[0])||!isfinite(offset[1])||!isfinite(offset[2]))return;
    float radius=hypotf(offset[0],offset[2]);if(!isfinite(radius)||radius<0.0001f)return;
    ZaFollowPose pose={.yaw=atan2f(offset[0],offset[2]),.pitch=atan2f(height-offset[1],radius),
        .frame=renderer_present_count(),.time=SDL_GetTicksNS(),.generation=atomic_load(&mouse_generation)};
    pthread_mutex_lock(&mouse_follow_pose_lock);mouse_follow_pose=pose;pthread_mutex_unlock(&mouse_follow_pose_lock);
}
static bool mouse_follow_pose_get(uint64_t now,ZaFollowPose *pose){
    pthread_mutex_lock(&mouse_follow_pose_lock);*pose=mouse_follow_pose;pthread_mutex_unlock(&mouse_follow_pose_lock);
    uint64_t frame=renderer_present_count();
    return pose->time&&now>=pose->time&&now-pose->time<=MOUSE_NATIVE_TIMEOUT_NS&&
        frame>=pose->frame&&frame-pose->frame<=2&&pose->generation==atomic_load(&mouse_generation);
}
static bool mouse_ball_context_allowed(bool captured,bool controller,bool overlay,bool enabled,
                                       uint64_t scene,bool aiming,bool battle,uint64_t now,uint64_t returning){
    return captured&&!controller&&!overlay&&enabled&&scene==ZA_FIELD_STATE&&
        (aiming||(!battle&&returning&&now<returning));
}
bool input_native_ball_mouse(Cpu *c){
    uint64_t now=SDL_GetTicksNS(),held=atomic_load(&key_buttons),owner=c->x[0];
    if(!mouse_ball_context_allowed(mouse_held,pad!=NULL,menu_input_captured()||lumiose_scroll_menu_open(),
        mouse_native_enabled()&&g_settings.mouse_aim_camera,za_menu_state(),atomic_load(&mouse_aim_active),
        (held&(1ull<<BTN_ZL))!=0,now,atomic_load(&mouse_aim_return_until)))return false;
    if(!za_ui_span(owner,0x1b4)||LD64(owner)!=0x7003e2f1f0ull||!LD8(owner+0x158))return false;
    ZaFollowPose pose;if(!mouse_follow_pose_get(now,&pose))return false;
    // Only this live 2.0.2 ball-camera presenter. Battle focus and projectile
    // target selection retain their game-owned state and action callbacks.
    uint32_t yaw,pitch;memcpy(&yaw,&pose.yaw,4);memcpy(&pitch,&pose.pitch,4);
    ST32(owner+0x15c,yaw);ST32(owner+0x160,pitch);
    atomic_store(&mouse_ball_native_last,now);
    if(getenv("LARECOMP_NATIVE_BALL_LOG"))fprintf(stderr,"[native-ball-camera] frame=%llu source=%llu yaw=%f pitch=%f\n",
        (unsigned long long)renderer_present_count(),(unsigned long long)pose.frame,pose.yaw,pose.pitch);
    return true;
}
static bool mouse_ball_native_active(uint64_t now){
    if(!mouse_native_enabled()||!g_settings.mouse_aim_camera||!mouse_held||pad)return false;
    bool aiming=atomic_load(&mouse_aim_active);
    if(!aiming&&(atomic_load(&key_buttons)&(1ull<<BTN_ZL)))return false;
    uint64_t last=atomic_load(&mouse_ball_native_last);
    if(last&&now>=last&&now-last<=MOUSE_NATIVE_TIMEOUT_NS)return true;
    ZaFollowPose pose;return aiming&&mouse_follow_pose_get(now,&pose);
}
#define TOUCH_VALID (1ull << 63)
// Scene hashes and UI classes belong to the verified 2.0.2 game program.
static bool game_mouse_menu_state(uint64_t state) {
    if(mouse_ack_active(state,NULL))return true;
    if (state == ZA_MENU_STATE || state == ZA_BOX_STATE || state == ZA_BAG_STATE ||
        state == ZA_DEX_STATE || state == ZA_RESEARCH_STATE || state == ZA_BIGMAP_STATE || state == ZA_SHOP_STATE || state == ZA_QUEST_STATE || state == ZA_MOVES_STATE || state == ZA_OUTFITS_STATE) return true;
    uint64_t seen = atomic_load(&mouse_box_context_seen), frame = renderer_present_count();
    return za_mouse_ui_recent(state) || (state && atomic_load(&mouse_menu_modal_scene) == state && seen && frame >= seen && frame - seen <= 8);
}
static bool game_mouse_menu_active(void) { return game_mouse_menu_state(za_menu_state()); }
static bool mouse_ball_ui_closing(void){
    return !pad&&mouse_held&&za_menu_state()==ZA_FIELD_STATE&&!menu_input_captured()&&
        !atomic_load(&mouse_aim_active)&&!(atomic_load(&key_buttons)&(1ull<<BTN_ZR))&&atomic_load(&mouse_ball_ui_cancelled);
}
// Prompt contexts: field, menu, locked-on battle controls, and ball aiming.
int input_keyboard_prompt_context(void) {
    if(za_menu_state()==ZA_TITLE_STATE)return 5;
    if(za_menu_state()==ZA_BOX_STATE)return 6;
    if(za_menu_state()==ZA_MOVES_STATE)return 7;
    if (game_mouse_menu_active()) return za_menu_state() == ZA_BAG_STATE ? 4 : 1;
    if (atomic_load(&mouse_aim_active)) return 3;
    if (atomic_load(&key_buttons) & (1ull << BTN_ZL)) return 2;
    return 0;
}
static bool mouse_touch_enabled(void) {
    return pad != NULL || game_mouse_menu_active() || g_settings.mouse_camera != MOUSE_CAM_CAPTURED || lumiose_scroll_menu_open() || keys_down[SDL_SCANCODE_LALT] || getenv("LUMIOSE_MOUSE_TOUCH_TEST") != NULL;
}
static uint64_t touch_position(float x, float y) {
    int w = 0, h = 0;
    if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) return 0;
    uint32_t tx = (uint32_t)(x * 1280.0f / w), ty = (uint32_t)(y * 720.0f / h);
    return TOUCH_VALID | ((uint64_t)ty << 32) | tx;
}
static int party_button_for_state(unsigned selected, unsigned out) {
    return selected && selected <= 6 && selected == out ? BTN_DOWN : BTN_UP;
}
// Locate the mod's one-based out-slot by the unique code that checks its
// party index. This is a fallback until the equivalent game-owned state is
// identified; no particular mod build ID or fixed mod offset is required.
static uint64_t party_find_out_offset(const uint8_t *text, uint64_t text_off, uint64_t text_size,
                                      uint64_t data_off, uint64_t module_size) {
    uint64_t found = 0; unsigned matches = 0;
    if (module_size < 4 || text_size < 24 || text_off > module_size || text_size > module_size - text_off) return 0;
    for (uint64_t p = 0; p + 24 <= text_size; p += 4) {
        uint32_t w[6]; memcpy(w, text + p, sizeof w);
        if ((w[0] & 0x9f00001fu) != 0x90000000u || (w[1] & 0x9f00001fu) != 0x90000005u ||
            (w[2] & 0xffc003ffu) != 0xb9400002u || w[3] != 0x51000442u || w[4] != 0x7100145fu ||
            (w[5] & 0xff00001fu) != 0x54000008u) continue;
        uint64_t imm = ((w[0] >> 29) & 3u) | (((uint64_t)(w[0] >> 5) & 0x7ffffu) << 2);
        int64_t pages = (int64_t)(imm ^ (1u << 20)) - (1u << 20);
        int64_t dest = (int64_t)((text_off + p) & ~4095ull) + pages * 4096 + (int64_t)(((w[2] >> 10) & 4095u) * 4u);
        if (dest < 0 || (uint64_t)dest < data_off || (uint64_t)dest > module_size - 4 || (dest & 3)) continue;
        found = (uint64_t)dest; if (++matches > 1) return 0;
    }
    return matches == 1 ? found : 0;
}
static int party_field_command(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t cached_base, cached_size, cached_text_size, cached_offset;
    static char cached_id[65];
    const char *id = getenv("LUMIOSE_PREPARE_BUILD_ID"); if (!id) id = "";
    uint64_t obs = atomic_load(&party_field_observation), frame = renderer_present_count(), seen = obs >> 3;
    unsigned selected = (unsigned)(obs & 7), out = 0;
    if (za_menu_state() == ZA_FIELD_STATE && selected && frame >= seen && frame - seen <= 3) {
        for (int i = 0; i < g_nmodules; i++) {
            Module *m = &g_modules[i];
            if (!m->name || strcmp(m->name, "subsdk9") || m->base != 0x7006200000ull) continue;
            pthread_mutex_lock(&lock);
            if (cached_base != m->base || cached_size != m->size || cached_text_size != m->text_size || strcmp(cached_id, id)) {
                cached_base = m->base; cached_size = m->size; cached_text_size = m->text_size;
                snprintf(cached_id, sizeof cached_id, "%s", id);
                cached_offset = party_find_out_offset((const uint8_t *)(uintptr_t)(m->base + m->text_off),
                                                      m->text_off, m->text_size, m->data_off, m->size);
            }
            uint64_t offset = cached_offset;
            pthread_mutex_unlock(&lock);
            if (offset) out = __atomic_load_n((uint32_t *)(uintptr_t)(m->base + offset), __ATOMIC_ACQUIRE);
            break;
        }
    }
    return party_button_for_state(selected, out);
}
// Called only around the verified game's ball-aim cancel predicate. Consume
// one mouse-requested cancel on release; normal controller and battle
// targeting keep the game's own result outside this ball-only predicate.
static bool mouse_throw_cancel_allowed(uint64_t frame, uint64_t after, uint64_t until,
                                       bool controller, bool captured, bool held, bool focus, uint64_t scene) {
    (void)focus; // Releasing the ball also works while battle focus is held.
    return until && frame >= after && frame <= until && !controller && held && !captured && scene == ZA_FIELD_STATE;
}
bool input_mouse_throw_cancel_pending(void) {
    return mouse_throw_cancel_allowed(renderer_present_count(), atomic_load(&mouse_throw_cancel_after),
        atomic_load(&mouse_throw_cancel_until), pad != NULL, menu_input_captured(), mouse_held,
        (atomic_load(&key_buttons) & (1ull << BTN_ZL)) != 0, za_menu_state());
}
uint64_t input_mouse_throw_cancel_result(uint64_t original) {
    if (!input_mouse_throw_cancel_pending()) return original;
    return atomic_exchange(&mouse_throw_cancel_until, 0) ? 1 : original;
}
static uint8_t mouse_down[MOUSEB_COUNT];
static pthread_mutex_t mouse_buttons_lock = PTHREAD_MUTEX_INITIALIZER;
// The game aims while ZR is held and normally throws on release. Cancel aim
// with B while ZR is still down; a click releases ZR to throw instead. Keep
// each transition visible for two presented frames even for a very fast click.
enum { AIM_IDLE, AIM_HELD, AIM_THROW, AIM_CANCEL_CLEAR, AIM_CANCEL };
static int aim_stage, aim_wheel_steps, wheel_context;
static bool aim_blocked_until_release, aim_suppress_throw_until_release;
static bool aim_wheel_pressed;
static uint64_t aim_since, aim_wheel_frame = UINT64_MAX, aim_wheel_pulse;
static void recompute_mouse_locked(void) {
    extern uint64_t renderer_present_count(void);
    uint64_t frame = renderer_present_count();
    bool aim = false;
    for (int m = 0; m < MOUSEB_COUNT; m++)
        if (g_bind_mouse[m] == BIND_MOUSE_AIM && mouse_down[m]) aim = true;
    if (!aim) aim_blocked_until_release = false;
    if (aim_blocked_until_release) aim = false;
    bool left_throw = mouse_down[MOUSEB_LEFT] && g_bind_mouse[MOUSEB_LEFT] == BIND_ZR;
    if (!left_throw) aim_suppress_throw_until_release = false;
    bool click = left_throw && !aim_suppress_throw_until_release;
    if(aim||(aim_stage==AIM_IDLE&&click))atomic_store(&mouse_ball_ui_cancelled,false);
    if (aim_stage == AIM_IDLE && aim) {
        aim_stage = AIM_HELD;
        atomic_store(&mouse_throw_cancel_until, 0);
    }
    if (aim_stage == AIM_HELD && click) { aim_stage = AIM_THROW; aim_since = frame; }
    else if (aim_stage == AIM_HELD && !aim) {
        aim_stage = AIM_CANCEL_CLEAR; aim_since = frame;
        atomic_store(&mouse_ball_ui_cancelled,true);
        atomic_store(&mouse_aim_return_until, SDL_GetTicksNS() + MOUSE_AIM_RETURN_NS);
        atomic_store(&mouse_throw_cancel_after, frame); atomic_store(&mouse_throw_cancel_until, frame + 120);
    }
    // A blocked game cannot observe the cancel press; wait until it regains input.
    if ((aim_stage == AIM_CANCEL_CLEAR || aim_stage == AIM_CANCEL) && (!mouse_held || menu_input_captured())) aim_since = frame;
    if (aim_stage == AIM_CANCEL_CLEAR && frame >= aim_since + 2)
        aim_stage = AIM_CANCEL, aim_since = frame;
    if (aim_stage == AIM_THROW && frame >= aim_since + 2 && (!click || !aim)) {
        // Releasing aim during the throw still needs a cancel edge after the
        // throw release; dropping straight to idle leaves the game aiming.
        if (!aim) {
            atomic_store(&mouse_ball_ui_cancelled,true);
            atomic_store(&mouse_aim_return_until, SDL_GetTicksNS() + MOUSE_AIM_RETURN_NS);
            atomic_store(&mouse_throw_cancel_after, frame); atomic_store(&mouse_throw_cancel_until, frame + 120);
        }
        if (!aim && left_throw) aim_suppress_throw_until_release = true;
        aim_stage = aim ? AIM_HELD : AIM_CANCEL_CLEAR; aim_since = frame;
    }
    if (aim_stage == AIM_CANCEL && frame >= aim_since + 2)
        aim_stage = aim ? AIM_HELD : AIM_IDLE;
    uint64_t b = 0;
    for (int m = 0; m < MOUSEB_COUNT; m++) {
        if (mouse_down[m] && g_bind_mouse[m] == BIND_MOUSE_PARTY_TOGGLE) {
            b |= 1ull << party_mouse_button;
            continue;
        }
        if (!mouse_down[m] || g_bind_mouse[m] < 0 || g_bind_mouse[m] >= BIND_PAD_COUNT) continue;
        if (m == MOUSEB_LEFT && g_bind_mouse[m] == BIND_ZR && (aim_stage != AIM_IDLE || aim_suppress_throw_until_release)) continue;
        b |= 1ull << g_bind_mouse[m];
    }
    if (aim_stage == AIM_HELD || aim_stage == AIM_CANCEL_CLEAR || aim_stage == AIM_CANCEL) b |= 1ull << BTN_ZR;
    if (aim_stage == AIM_CANCEL) b |= 1ull << BTN_B;
    atomic_store(&mouse_cancel_clear, aim_stage == AIM_CANCEL_CLEAR);
    atomic_store(&mouse_aim_active, aim_stage == AIM_HELD || aim_stage == AIM_THROW);
    int context = !mouse_held ? 0 : aim_stage == AIM_HELD ? 1 : aim_stage == AIM_IDLE ? 2 : 0;
    if (context != wheel_context) {
        wheel_context = context; aim_wheel_steps = 0; aim_wheel_pressed = false; aim_wheel_pulse = 0; aim_wheel_frame = frame;
    } else if (!context) {
        aim_wheel_steps = 0; aim_wheel_pressed = false; aim_wheel_pulse = 0; aim_wheel_frame = frame;
    } else if (frame != aim_wheel_frame) {
        aim_wheel_frame = frame; aim_wheel_pulse = 0;
        if (aim_wheel_pressed) aim_wheel_pressed = false;
        else if (aim_wheel_steps) {
            bool up = aim_wheel_steps > 0;
            aim_wheel_steps += up ? -1 : 1;
            aim_wheel_pulse = 1ull << (up ? BTN_LEFT : BTN_RIGHT);
            if (context == 2) party_wheel_last = frame;
            aim_wheel_pressed = true;
        }
    }
    if (!mouse_held || pad || menu_input_captured() || aim_stage != AIM_IDLE) { party_deferred = false; party_pulse_until = 0; }
    if (party_deferred && !aim_wheel_steps && !aim_wheel_pressed && !aim_wheel_pulse &&
        party_wheel_last != UINT64_MAX && frame >= party_wheel_last + 2) {
        party_mouse_button = party_field_command();
        party_deferred = false; party_pulse_until = frame + 3;
    }
    if (frame < party_pulse_until) b |= 1ull << party_mouse_button;
    mouse_buttons = b | aim_wheel_pulse;
}
static void mouse_cancel_aim(void) {
    pthread_mutex_lock(&mouse_buttons_lock);
    if (aim_stage == AIM_HELD || aim_stage == AIM_THROW) {
        extern uint64_t renderer_present_count(void);
        if (aim_stage == AIM_THROW && mouse_down[MOUSEB_LEFT]) aim_suppress_throw_until_release = true;
        aim_blocked_until_release = true;
        aim_stage = AIM_CANCEL_CLEAR; aim_since = renderer_present_count();
        atomic_store(&mouse_ball_ui_cancelled,true);
        atomic_store(&mouse_throw_cancel_after, aim_since); atomic_store(&mouse_throw_cancel_until, aim_since + 120);
        recompute_mouse_locked();
    }
    pthread_mutex_unlock(&mouse_buttons_lock);
}
static bool mouse_game_wheel_add(int steps) {
    pthread_mutex_lock(&mouse_buttons_lock);
    bool active = mouse_held && (aim_stage == AIM_HELD || aim_stage == AIM_IDLE);
    if (active) {
        if (aim_stage == AIM_IDLE && steps) { extern uint64_t renderer_present_count(void); party_wheel_last = renderer_present_count(); }
        aim_wheel_steps += steps;
        if (aim_wheel_steps > 20) aim_wheel_steps = 20;
        if (aim_wheel_steps < -20) aim_wheel_steps = -20;
    }
    pthread_mutex_unlock(&mouse_buttons_lock);
    return active;
}
static void recompute_mouse(void) {
    pthread_mutex_lock(&mouse_buttons_lock);
    recompute_mouse_locked();
    pthread_mutex_unlock(&mouse_buttons_lock);
}
static void mouse_disable_gameplay_for_pad(void) {
    pthread_mutex_lock(&mouse_buttons_lock);
    memset(mouse_down, 0, sizeof mouse_down);
    aim_stage = AIM_IDLE; aim_since = 0; aim_blocked_until_release = false;
    aim_suppress_throw_until_release = false;
    recompute_mouse_locked();
    pthread_mutex_unlock(&mouse_buttons_lock);
    mouse_clear();
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm
extern bool menu_cursor_needed(void);   // (the cursor wanted on screen: the menu, or a tool beside the game)
// Main thread, after each batch of events (runtime/nvn/vulkan/device.c ui_run): the cursor held while the game has
// the window and the controls - not with the menu or the name entry up (they take the mouse), nor without focus.
void input_mouse_update(SDL_Window *w) {
    touch_window = w;
    drag_cursor_update();
    if ((mouse_menu_down_scene == ZA_BOX_STATE || mouse_menu_down_scene == ZA_BAG_STATE) && (za_menu_state() != mouse_menu_down_scene ||
        (!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && !getenv("LUMIOSE_MOUSE_TOUCH_TEST")))) {
        SDL_CaptureMouse(false); mouse_menu_down = -1; mouse_menu_down_scene = 0; mouse_menu_drag_started = false;
        if (atomic_load(&mouse_drag_visual)) { atomic_store(&mouse_drag_visual, false); atomic_store(&mouse_drag_abort, true); }
    }
    int pending_chat = atomic_exchange(&lumiose_chat_wheel_pending, 0);
    if (pending_chat) {
        int result = lumiose_chat_bridge_scroll((int16_t)pending_chat);
        if (result == 0) atomic_fetch_add(&lumiose_chat_wheel_pending, pending_chat);
    }
    if (menu_input_captured() || (!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && !getenv("LUMIOSE_MOUSE_TOUCH_TEST"))) {
        touch_active = 0; touch_pending = 0;
        mouse_clear();
        if (atomic_load(&mouse_drag_visual)) { atomic_store(&mouse_drag_visual, false); atomic_store(&mouse_drag_abort, true); SDL_CaptureMouse(false); }
        if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) keys_down[SDL_SCANCODE_LALT] = 0;
    }
    bool want = g_settings.mouse_camera == MOUSE_CAM_CAPTURED && !menu_cursor_needed() && !input_user_blocked_any() &&
                !mouse_touch_enabled() && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
    if (!want && (pad || keys_down[SDL_SCANCODE_LALT] || game_mouse_menu_active() || lumiose_scroll_menu_open()) && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) {
        SDL_ShowCursor(); cursor_hidden = false;
    }
    if (want == mouse_held) return;
    SDL_SetWindowRelativeMouseMode(w, want);
    mouse_held = want;
    if (want) touch_active = 0;
    pthread_mutex_lock(&mouse_buttons_lock);
    memset(mouse_down, 0, sizeof mouse_down);  // (a button held across the change: released)
    if (pad) { aim_stage = AIM_IDLE; aim_since = 0; } // a connected controller disables mouse input
    recompute_mouse_locked();
    pthread_mutex_unlock(&mouse_buttons_lock);
    mouse_clear();
    LOG("[input] mouse %s\n", want ? "held by the window: it turns the camera" : "free");
}
// Our menus' face buttons: the family whose button names they show, and (Confirm button = Controller convention, #172)
// whose layout they use - Nintendo (0): the right button confirms, the bottom one goes back; Xbox / PlayStation
// (1 / 2): the bottom one confirms, the right one goes back (runtime/ui/pad_nav.mm). "Button icons"
// forced to a style wins (icons and menus agree); Auto: the controller `which` that pressed (0, or not a connected
// gamepad - a test's pushed press: the game's controller / the first connected one, else Nintendo).
int input_menu_family(SDL_JoystickID which) {
    switch (g_settings.prompts) {
    case PROMPTS_NINTENDO: return 0;
    case PROMPTS_XBOX: return 1;
    case PROMPTS_PLAYSTATION: return 2;
    default: break;
    }
    if (getenv("LARECOMP_PAD_TYPE")) return input_pad_family();
    if (which && SDL_IsGamepad(which)) return input_family_of_type(SDL_GetGamepadTypeForID(which));
    return bind_label_family();
}

// Back on the start screen (runtime/ui/launcher.mm, also after the menu's Quit to Start Screen): the cursor is free
// again - the game held it for the camera, and nothing calls input_mouse_update while the start screen is up
void input_mouse_release(SDL_Window *w) {
    touch_active = 0; touch_pending = 0;
    mouse_menu_cancel(); drag_cursor_update(); SDL_CaptureMouse(false);
    if (!mouse_held) return;
    SDL_SetWindowRelativeMouseMode(w, false);
    mouse_held = false;
    pthread_mutex_lock(&mouse_buttons_lock);
    memset(mouse_down, 0, sizeof mouse_down);
    recompute_mouse_locked();
    pthread_mutex_unlock(&mouse_buttons_lock);
    mouse_clear();
    LOG("[input] mouse free (start screen)\n");
}
bool input_title_key_event(const SDL_Event *e){
    // A confirming key was never put in keys_down. Its later OS repeats must
    // not become a first gameplay press after the title transition.
    if(e->type==SDL_EVENT_KEY_DOWN&&e->key.repeat&&e->key.scancode>0&&
       e->key.scancode<SDL_SCANCODE_COUNT&&!keys_down[e->key.scancode]&&!menu_input_captured())return true;
    if((e->type!=SDL_EVENT_KEY_DOWN&&e->type!=SDL_EVENT_KEY_UP)||
       za_menu_state()!=ZA_TITLE_STATE||menu_input_captured()||lumiose_scroll_menu_open())return false;
    if(e->type==SDL_EVENT_KEY_DOWN&&title_key_confirm_allowed(ZA_TITLE_STATE,e->key.repeat,false,e->key.scancode))
        atomic_store(&title_confirm_until,renderer_present_count()+4);
    return true;
}
void input_handle_event(const SDL_Event *e) {
    // A click can open a different presenter before its release arrives.
    // Release the physical touch before any presenter or controller gate can
    // consume that mouse-up; otherwise the game keeps a held touch forever.
    if(e->type==SDL_EVENT_MOUSE_BUTTON_UP&&e->button.button==SDL_BUTTON_LEFT)touch_active=0;
    if((e->type==SDL_EVENT_KEY_DOWN&&e->key.scancode==g_bind_key[BIND_CANCEL])||
       (e->type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e->button.button==SDL_BUTTON_RIGHT&&game_mouse_menu_state(za_menu_state()))){
        touch_active=touch_pending=0;
        mouse_outfits_action_down=false;atomic_store(&mouse_outfits_action_until,0);
    }
    if ((e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) ||
        e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_WHEEL ||
        (e->type == SDL_EVENT_MOUSE_MOTION && (e->motion.xrel || e->motion.yrel)) ||
        e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 16000)) {
        extern void discord_presence_note_input(void);
        discord_presence_note_input();
    }

    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        SDL_free(ids);
    }
    // Mouse clicks still feed the mod's touchscreen UI with a controller
    // connected. Camera and gameplay-button paths retain their own pad gates.
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        if(input_title_key_event(e))return; // Do not carry the confirming key's gameplay action forward.
        if(e->key.scancode==g_bind_key[BIND_CANCEL]&&(move_back_key_down||za_menu_state()==ZA_MOVES_STATE)&&!menu_input_captured()&&!lumiose_scroll_menu_open()){
            if(e->key.scancode<SDL_SCANCODE_COUNT){keys_down[e->key.scancode]=false;recompute_keyboard();}
            if(e->type==SDL_EVENT_KEY_DOWN&&!e->key.repeat){move_back_key_down=true;mouse_tab_cancel();direct_list_cancel();za_mouse_ui_cancel();mouse_moves_back();}
            if(e->type==SDL_EVENT_KEY_UP)move_back_key_down=false;
            return;
        }
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            if(za_menu_state()==ZA_MOVES_STATE)mouse_moves_cancel();
            atomic_fetch_add(&bag_drag_generation, 1);
            atomic_store(&bag_drag_request, 0);
            direct_list_cancel(); za_mouse_ui_cancel();
            atomic_store(&box_context_request, 0); box_context_hover = -1;
            if (atomic_load(&box_context_click_pending)) {
                atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false);
                atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
            }
            mouse_tab_cancel();
            atomic_store(&reward_wheel_steps, 0); atomic_store(&reward_wheel_buttons, 0);
            if (e->key.scancode == g_bind_key[BIND_CANCEL]) {
                atomic_store(&mouse_menu_modal_scene, 0); atomic_store(&box_context_request, 0);
                atomic_store(&box_context_click_pending, false); atomic_store(&box_context_confirm_start, 0);
                atomic_store(&box_context_buttons, 0); box_context_hover = -1; box_context_down = -1;
            }
            else if (e->key.scancode == g_bind_key[BIND_A] && game_mouse_menu_state(za_menu_state())) {
                uint64_t state = za_menu_state();
                if (state == ZA_BOX_STATE || state == ZA_BAG_STATE || state == ZA_MENU_STATE) {
                    atomic_store(&mouse_box_context_seen, 0);
                    atomic_store(&mouse_box_context_started, renderer_present_count() + 1);
                    box_context_hover = -1; box_context_down = -1;
                    atomic_store(&mouse_menu_modal_scene, state);
                }
            }
            if (atomic_load(&mouse_drag_visual) || atomic_load(&mouse_drag_picked)) {
                atomic_store(&mouse_drag_abort, true); atomic_store(&mouse_drag_visual, false);
                mouse_menu_down = -1; mouse_menu_down_scene = 0; mouse_menu_drag_started = false; SDL_CaptureMouse(false);
                return;
            }
            mouse_menu_cancel();
            if (atomic_load(&shortcut_running) || atomic_load(&shortcut_request)) shortcut_cancel();
            else if (!menu_input_captured() && za_menu_state() == ZA_FIELD_STATE &&
                     !atomic_load(&mouse_aim_active) && !(key_buttons & (1ull << BTN_ZR))) {
                int key = e->key.scancode, kind = SHORTCUT_NONE;
                if (key == g_bind_key[BIND_RESEARCH]) kind = SHORTCUT_RESEARCH;
                else if (key == g_bind_key[BIND_POKEDEX]) kind = SHORTCUT_DEX;
                else if (key == g_bind_key[BIND_BOXES]) kind = SHORTCUT_BOXES;
                else if (key == g_bind_key[BIND_X]) kind = SHORTCUT_SATCHEL;
                else if (key == g_bind_key[BIND_CANCEL] && !key_focus_toggled) kind = SHORTCUT_MENU;
                if (kind) {
                    key_focus_toggled = false; recompute_keyboard();
                    atomic_store(&tap_buttons, 0); atomic_store(&press_unseen, 0);
                    atomic_store(&shortcut_request, kind);
                    return;
                }
            }
        }
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F10 && !e->key.repeat) g_shot_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == g_bind_key[BIND_CANCEL]) {
            if ((key_buttons & (1ull << BTN_B)) && !atomic_load(&mouse_aim_active)) {
                extern uint64_t renderer_present_count(void);
                atomic_store(&cancel_edge_start, renderer_present_count() + 1);
            }
            mouse_cancel_aim();
        }
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        // Apply the cursor transition before processing a click in the same event batch.
        if (e->key.scancode == SDL_SCANCODE_LALT && touch_window) input_mouse_update(touch_window);
        uint64_t before = key_buttons;
        recompute_keyboard();
        uint64_t now = key_buttons;
        note_press(now & ~before, before & ~now);
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) {
            pad = SDL_OpenGamepad(e->gdevice.which);
            if (pad) { mouse_disable_gameplay_for_pad(); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_RumbleGamepad(pad, 0, 0, 0);
            SDL_CloseGamepad(pad); pad = NULL;
            // Other controllers may have been connected while this one was
            // active. Keep mouse input disabled until the last one is gone.
            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            for (int i = 0; ids && i < count && !pad; i++)
                if (ids[i] != e->gdevice.which) pad = SDL_OpenGamepad(ids[i]);
            SDL_free(ids);
            if (pad) LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
            note_family();
            touch_active = 0; touch_pending = 0;
            led_forget();  // (gone: it shows its own colour when it comes back)
            rumble_clear(); rumble_last_lo = rumble_last_hi = 0; rumble_warned = false;
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            atomic_fetch_add(&bag_drag_generation, 1); atomic_store(&bag_drag_request, 0);
        }
        if (!host_test || e->gbutton.which == TEST_DEVICE) {  // (the press as the event has it, before reading the pad)
            uint64_t b = pad_bits(e->gbutton.button);
            note_press(e->gbutton.down ? b : 0, e->gbutton.down ? 0 : b);
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            !host_test) {  // (the triggers as buttons: past half way)
            static bool trig[2];
            int t = e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            bool on = e->gaxis.value > 16000;
            if (on != trig[t]) {
                uint64_t b = pad_bits(PADB_LTRIGGER + t);
                note_press(on ? b : 0, on ? 0 : b);
                trig[t] = on;
            }
        }
        recompute_pad();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint64_t scene = za_menu_state();
        if(scene==ZA_MOVES_STATE&&!menu_input_captured()&&!lumiose_scroll_menu_open()){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            // The native Move Editor footer is fixed to this design-space
            // Back label. Clip to its bounds rather than catching the footer.
            bool footer=e->button.button==SDL_BUTTON_LEFT&&mouse_ack_hit(e->button.x,e->button.y,w,h,(ZaUiRect){1740,1010,180,70});
            if(footer||(e->button.button==SDL_BUTTON_LEFT&&move_back_mouse_down)){
                if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN)move_back_mouse_down=footer;
                else{if(move_back_mouse_down&&footer){mouse_tab_cancel();za_mouse_ui_cancel();direct_list_cancel();mouse_moves_back();}move_back_mouse_down=false;}
                break;
            }
            if(e->button.button==SDL_BUTTON_RIGHT){
                if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){mouse_tab_cancel();za_mouse_ui_cancel();direct_list_cancel();mouse_moves_back();}
                break;
            }
        }
        if(scene==ZA_BOX_STATE&&e->button.button==SDL_BUTTON_LEFT&&atomic_load(&mouse_drag_released)&&
            atomic_load(&mouse_drag_picked)&&atomic_load(&mouse_menu_running)&&!menu_input_captured())break;
        if(e->button.button==SDL_BUTTON_LEFT&&mouse_dex_splash_active()&&!menu_input_captured()&&!lumiose_scroll_menu_open()){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            bool inside=e->button.x>=0&&e->button.y>=0&&e->button.x<w&&e->button.y<h;
            if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){mouse_dex_open_down=inside;direct_list_cancel();}
            else{if(mouse_dex_open_down&&inside){atomic_store(&mouse_ack_confirm_scene,scene);atomic_store(&mouse_ack_confirm,renderer_present_count()+1);}mouse_dex_open_down=false;}
            break;
        }
        ZaUiRect ack;
        if(e->button.button==SDL_BUTTON_LEFT&&za_shop_quantity_active()&&!menu_input_captured()&&!lumiose_scroll_menu_open()){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            int hit=za_shop_quantity_hit(e->button.x,e->button.y,w,h);
            if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){za_shop_quantity_down=hit;direct_list_cancel();za_mouse_ui_cancel();}
            else{if(hit==za_shop_quantity_down&&hit>=-1)za_shop_quantity_queue(hit);za_shop_quantity_down=-2;}
            break;
        }
        if(e->button.button==SDL_BUTTON_LEFT&&mouse_ack_active(scene,&ack)&&!menu_input_captured()&&!lumiose_scroll_menu_open()){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            bool hit=mouse_ack_hit(e->button.x,e->button.y,w,h,ack);
            if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){mouse_ack_down=hit;direct_list_cancel();za_mouse_ui_cancel();mouse_menu_cancel();}
            else{
                if(mouse_ack_down&&hit){atomic_store(&mouse_ack_confirm_scene,scene);atomic_store(&mouse_ack_confirm,renderer_present_count()+1);}
                mouse_ack_down=false;
            }
            break;
        }
        if(e->button.button==SDL_BUTTON_LEFT&&mouse_dex_grid_blocked()&&!menu_input_captured()&&!lumiose_scroll_menu_open())break;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            atomic_fetch_add(&bag_drag_generation, 1); atomic_store(&bag_drag_request, 0);
        }
        if (e->button.button == SDL_BUTTON_LEFT && game_mouse_menu_state(scene) &&
            atomic_load(&mouse_menu_modal_scene) == scene && !menu_input_captured() && !lumiose_scroll_menu_open()) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            int hit = box_context_hit(e->button.x, e->button.y, w, h, renderer_present_count());
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (hit >= -1) mouse_menu_cancel();
                box_context_down = hit;
                if (hit == -1) {
                    atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false);
                    atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
                    atomic_store(&menu_back_request, true);
                }
                else if (hit >= 0) box_context_hover_request(hit);
            } else {
                if (hit >= 0 && hit == box_context_down) { atomic_store(&box_context_click_pending, true); atomic_store(&box_context_request, hit + 1 + 16); }
                box_context_down = -1;
            }
            break;
        }
        if (e->button.button == SDL_BUTTON_RIGHT && game_mouse_menu_state(scene) &&
            !menu_input_captured() && !lumiose_scroll_menu_open()) {
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                atomic_fetch_add(&bag_drag_generation, 1);
                za_mouse_ui_cancel();
                mouse_tab_cancel(); mouse_tab_down = -1; direct_list_cancel();
                atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false);
                atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
                box_context_hover = -1; box_context_down = -1; atomic_store(&mouse_menu_modal_scene, 0);
                if (atomic_load(&mouse_drag_visual) || atomic_load(&mouse_drag_picked)) {
                    atomic_store(&mouse_drag_abort, true); atomic_store(&mouse_drag_visual, false);
                    mouse_menu_down = -1; mouse_menu_down_scene = 0; mouse_menu_drag_started = false; SDL_CaptureMouse(false);
                } else { mouse_menu_cancel(); atomic_store(&menu_back_request, true); }
            }
            break;
        }
        if(e->button.button==SDL_BUTTON_LEFT&&scene==ZA_OUTFITS_STATE&&!mouse_held&&
           !menu_input_captured()&&!lumiose_scroll_menu_open()&&atomic_load(&mouse_menu_modal_scene)!=scene){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            bool hit=mouse_menu_hit(scene,e->button.x,e->button.y,w,h)==58;
            if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN&&hit){
                direct_list_cancel();za_mouse_ui_cancel();mouse_outfits_action_down=true;break;
            }
            if(e->type==SDL_EVENT_MOUSE_BUTTON_UP&&mouse_outfits_action_down){
                if(hit)atomic_store(&mouse_outfits_action_until,renderer_present_count()+4);
                mouse_outfits_action_down=false;break;
            }
        }
        if (e->button.button == SDL_BUTTON_LEFT && (scene == ZA_BAG_STATE || scene == ZA_RESEARCH_STATE || scene == ZA_MOVES_STATE || scene == ZA_OUTFITS_STATE) && !mouse_held &&
            !menu_input_captured() && !lumiose_scroll_menu_open() &&
            atomic_load(&mouse_menu_modal_scene) != scene) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            unsigned info = atomic_load(&mouse_tab_info);
            int hit = scene == ZA_BAG_STATE ? mouse_tab_hit(e->button.x, e->button.y, w, h, info >> 8) :
                      scene == ZA_RESEARCH_STATE ? mouse_research_tab_hit(e->button.x, e->button.y, w, h) :
                      mouse_menu_hit(scene,e->button.x,e->button.y,w,h)-48;
            if((scene==ZA_MOVES_STATE||scene==ZA_OUTFITS_STATE)&&(hit<0||hit>=(int)(info>>8)))hit=-1;
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && hit >= 0) {
                if(scene==ZA_MOVES_STATE)mouse_moves_cancel();
                if(scene==ZA_OUTFITS_STATE)za_mouse_ui_cancel();
                mouse_menu_cancel(); direct_list_cancel(); mouse_tab_cancel(); mouse_tab_down = hit; mouse_tab_down_scene = scene; break;
            }
            if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && mouse_tab_down >= 0) {
                if (hit == mouse_tab_down && scene == mouse_tab_down_scene) atomic_store(&mouse_tab_request, hit + 1);
                mouse_tab_down = -1; mouse_tab_down_scene = 0; break;
            }
        }
        if (e->button.button == SDL_BUTTON_LEFT && !mouse_held && za_mouse_ui_recent(scene) &&
            !menu_input_captured() && !lumiose_scroll_menu_open() && atomic_load(&mouse_menu_modal_scene) != scene) {
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            ZaMouseUiTarget hit=za_mouse_ui_hit(scene,e->button.x,e->button.y,w,h);
            if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                za_mouse_ui_down=hit;
                if(hit.valid){za_mouse_ui_queue(hit,false);break;}
            }else if(za_mouse_ui_down.valid){
                if(za_mouse_ui_same(hit,za_mouse_ui_down))za_mouse_ui_queue(hit,true);
                za_mouse_ui_down.valid=false;break;
            }
        }
        if(e->button.button==SDL_BUTTON_LEFT&&(scene==ZA_SHOP_STATE||scene==ZA_OUTFITS_STATE)&&
           !menu_input_captured()&&!lumiose_scroll_menu_open())break;
        if (e->button.button == SDL_BUTTON_LEFT && (scene == ZA_MENU_STATE || scene == ZA_BOX_STATE || scene == ZA_BAG_STATE || scene == ZA_DEX_STATE || scene == ZA_QUEST_STATE || scene == ZA_MOVES_STATE ||
            (scene == ZA_RESEARCH_STATE && mouse_research_list_active())) &&
            !menu_input_captured() && !lumiose_scroll_menu_open() && atomic_load(&mouse_menu_modal_scene) != scene) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            int hit = mouse_held ? -1 : mouse_menu_hit(scene, e->button.x, e->button.y, w, h);
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                mouse_tab_cancel(); mouse_tab_down = -1;
                mouse_menu_cancel(); direct_list_cancel(); mouse_menu_down = hit; mouse_menu_down_scene = scene;
                if (hit >= 0 && direct_list_screen(scene)) direct_list_queue(scene, hit, false);
                mouse_menu_down_x = e->button.x; mouse_menu_down_y = e->button.y; mouse_menu_drag_started = false;
                if ((scene == ZA_BOX_STATE || (scene == ZA_BAG_STATE && hit < 7)) && hit >= 0) SDL_CaptureMouse(true);
            } else {
                if (mouse_menu_drag_started) {
                    if (scene == ZA_BAG_STATE && mouse_menu_down_scene == scene && mouse_menu_down >= 0 &&
                        mouse_menu_down < 7 && hit >= 48 && hit < 54)
                        atomic_store(&bag_drag_request, mouse_menu_down + 1 + ((hit - 48 + 1) << 8));
                    else if (scene == ZA_BOX_STATE) {
                        atomic_store(&mouse_drag_destination, hit); atomic_store(&mouse_drag_released, true);
                    }
                    atomic_store(&mouse_drag_visual, false);
                } else if (hit >= 0 && mouse_menu_down >= 0 && scene == mouse_menu_down_scene &&
                    (hit == mouse_menu_down || scene == ZA_BOX_STATE)) {
                    shortcut_cancel();
                    int request = mouse_menu_down + 1 + (scene == ZA_BOX_STATE ? 64 : 0) + (scene == ZA_BAG_STATE ? 32768 : 0) + (scene == ZA_DEX_STATE ? 65536 : 0) + (scene == ZA_RESEARCH_STATE ? 262144 : 0);
                    if (hit != mouse_menu_down) request |= 8192 | ((hit + 1) << 7);
                    if (direct_list_screen(scene)) direct_list_queue(scene, hit, true);
                    else atomic_store(&mouse_menu_request, request);
                }
                if (scene == ZA_BOX_STATE || scene == ZA_BAG_STATE) SDL_CaptureMouse(false);
                mouse_menu_down = -1; mouse_menu_down_scene = 0; mouse_menu_drag_started = false;
            }
            break;
        }
        if (e->button.button == SDL_BUTTON_RIGHT && e->type == SDL_EVENT_MOUSE_BUTTON_UP && !mouse_held)
            mouse_clear();
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (e->type == SDL_EVENT_MOUSE_BUTTON_UP) touch_active = 0;
            else if (mouse_touch_enabled() && !mouse_held && !menu_input_captured()) {
                uint64_t p = touch_position(e->button.x, e->button.y);
                touch_active = p; touch_pending = p;
            }
        }
        int m = bind_mouse_from_sdl(e->button.button);
        if (m < 0) break;
        pthread_mutex_lock(&mouse_buttons_lock);
        uint64_t before = mouse_buttons;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_UP) mouse_down[m] = 0;
        else if (mouse_held && !pad) {
            if (g_bind_mouse[m] == BIND_MOUSE_PARTY_TOGGLE) {
                uint64_t f = renderer_present_count();
                bool recent = party_wheel_last != UINT64_MAX && f >= party_wheel_last && f - party_wheel_last <= 2;
                if (aim_stage == AIM_IDLE && (aim_wheel_steps || aim_wheel_pressed || aim_wheel_pulse || recent)) {
                    party_deferred = true; mouse_down[m] = 0;
                } else { party_mouse_button = party_field_command(); mouse_down[m] = 1; }
            } else mouse_down[m] = 1;
        }
        recompute_mouse_locked();
        uint64_t after = mouse_buttons;
        pthread_mutex_unlock(&mouse_buttons_lock);
        note_press(after & ~before, before & ~after);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        // The mod's lists already navigate with the d-pad. Do not send wheel input to the game world.
        static float fraction;
        atomic_fetch_add(&bag_drag_generation, 1);
        atomic_store(&bag_drag_request, 0);
        if (menu_input_captured()) { fraction = 0; break; }
        float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (!isfinite(y)) break;
        if (y > 20) y = 20; if (y < -20) y = -20;
        fraction += y;
        int steps = (int)fraction;
        fraction -= steps;
        uint64_t wheel_scene = za_menu_state();
        if(steps&&(wheel_scene==ZA_SHOP_STATE||wheel_scene==ZA_OUTFITS_STATE))za_mouse_ui_cancel();
        if(steps&&za_shop_quantity_active()){
            if(!menu_input_captured()&&!lumiose_scroll_menu_open())for(int i=0;i<abs(steps);i++)za_shop_quantity_queue(steps>0?1:2);
            break;
        }
        if (steps && (mouse_ack_active(wheel_scene,NULL)||mouse_dex_grid_blocked()||(game_mouse_menu_state(wheel_scene) && atomic_load(&mouse_menu_modal_scene) == wheel_scene))) break;
        if (steps && lumiose_scroll_menu_open()) {
            // Directional presses change the highlighted entry; they are not
            // a substitute for scrolling the contents of this list.
            break;
        } else if (steps && atomic_load(&legacy_chat_mode)) {
            for (int i = 0; i < abs(steps); i++)
                if (!legacy_chat_enqueue(1ull << (steps > 0 ? BTN_UP : BTN_DOWN), 90)) break;
        } else if (steps && wheel_scene == ZA_BIGMAP_STATE) {
            za_map_wheel_queue(steps);
        } else if (steps && wheel_scene == ZA_BOX_STATE) {
            int old = atomic_load(&box_wheel_steps), next;
            do { next = old + steps; if (next > 20) next = 20; if (next < -20) next = -20; }
            while (!atomic_compare_exchange_weak(&box_wheel_steps, &old, next));
        } else if (steps && (wheel_scene == ZA_BAG_STATE || wheel_scene == ZA_DEX_STATE || wheel_scene == ZA_RESEARCH_STATE || wheel_scene == ZA_SHOP_STATE || wheel_scene == ZA_QUEST_STATE || wheel_scene == ZA_MOVES_STATE || wheel_scene == ZA_OUTFITS_STATE)) {
            uint64_t now = renderer_present_count();
            if (wheel_scene == ZA_RESEARCH_STATE && !mouse_research_list_active()) {
                int old = atomic_load(&reward_wheel_steps), next;
                do { next = old + steps; if (next > 20) next = 20; if (next < -20) next = -20; }
                while (!atomic_compare_exchange_weak(&reward_wheel_steps, &old, next));
                break;
            }
            if (atomic_exchange(&mouse_list_wheel_scene, wheel_scene) != wheel_scene)
                atomic_store(&mouse_list_wheel, 0);
            atomic_store(&mouse_list_wheel_frame, now);
            int old = atomic_load(&mouse_list_wheel), next;
            do { next = old + steps; if (next > 20) next = 20; if (next < -20) next = -20; }
            while (!atomic_compare_exchange_weak(&mouse_list_wheel, &old, next));
        } else if (steps && (game_mouse_menu_state(wheel_scene) || wheel_scene == ZA_MENU_STATE || wheel_scene == ZA_BAG_STATE ||
                             wheel_scene == ZA_DEX_STATE || wheel_scene == ZA_RESEARCH_STATE || wheel_scene == ZA_BIGMAP_STATE || wheel_scene == ZA_SHOP_STATE ||
                             (wheel_scene && wheel_scene != ZA_FIELD_STATE && wheel_scene == atomic_load(&mouse_scroll_scene)))) {
            // List scrolling must move the viewport without changing selection.
            // Suppress the old directional-input fallback until that is available.
            break;
        } else if (steps) {
            LumioseChatSnapshot chat;
            if (lumiose_chat_bridge_snapshot(&chat) && (chat.state & LUMIOSE_CHAT_FOCUSED)) {
                int pending = atomic_exchange(&lumiose_chat_wheel_pending, 0) + steps;
                if (pending > 32767) pending = 32767;
                if (pending < -32767) pending = -32767;
                int result = lumiose_chat_bridge_scroll((int16_t)pending);
                if (result == 0) atomic_fetch_add(&lumiose_chat_wheel_pending, pending);
            } else {
                // Left/right selects a ball when aiming, or a party member otherwise.
                mouse_game_wheel_add(steps);
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:  // the mouse turns the camera (right stick): cursor held, or the right button held
        if(za_menu_state()==ZA_BOX_STATE&&atomic_load(&mouse_drag_released)&&atomic_load(&mouse_drag_picked)&&
            atomic_load(&mouse_menu_running)&&!menu_input_captured())break;
        if((za_shop_quantity_active()||mouse_ack_active(za_menu_state(),NULL)||mouse_dex_grid_blocked())&&!menu_input_captured()&&!lumiose_scroll_menu_open())break;
        if (!mouse_menu_drag_started && mouse_menu_down >= 0 && mouse_menu_down < 7 && mouse_menu_down_scene == ZA_BAG_STATE &&
            za_menu_state() == ZA_BAG_STATE && !menu_input_captured() &&
            (fabsf(e->motion.x - mouse_menu_down_x) > 5 || fabsf(e->motion.y - mouse_menu_down_y) > 5)) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            mouse_menu_drag_started = true; atomic_store(&mouse_drag_visual, true);
            drag_capture_item_request(mouse_menu_down, w, h);
        }
        if (!mouse_menu_drag_started && mouse_menu_down >= 0 && mouse_menu_down_scene == ZA_BOX_STATE &&
            za_menu_state() == ZA_BOX_STATE && !menu_input_captured() &&
            (fabsf(e->motion.x - mouse_menu_down_x) > 5 || fabsf(e->motion.y - mouse_menu_down_y) > 5)) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            mouse_menu_drag_started = true; shortcut_cancel();
            atomic_store(&mouse_drag_destination, -1); atomic_store(&mouse_drag_released, false);
            atomic_store(&mouse_drag_visual, true); drag_capture_request(mouse_menu_down, w, h);
            atomic_store(&mouse_menu_request, mouse_menu_down + 1 + 64 + 8192 + 16384);
        }
        if (mouse_menu_drag_started && za_menu_state() == ZA_BOX_STATE &&
            !menu_input_captured() && !lumiose_scroll_menu_open()) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            int hit = mouse_menu_hit(ZA_BOX_STATE, e->motion.x, e->motion.y, w, h);
            atomic_store(&mouse_drag_destination, hit);
            if (hit >= 0 && atomic_load(&mouse_drag_picked)) direct_list_queue(ZA_BOX_STATE, hit, false);
        }
        if (!mouse_held && !e->motion.state && game_mouse_menu_state(za_menu_state()) &&
            atomic_load(&mouse_menu_modal_scene) == za_menu_state() && !menu_input_captured() && !lumiose_scroll_menu_open()) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            int hit = box_context_hit(e->motion.x, e->motion.y, w, h, renderer_present_count());
            if (hit != box_context_hover) { box_context_hover = hit; if (hit >= 0) box_context_hover_request(hit); }
        } else box_context_hover = -1;
        // Do not send hover navigation while a context menu may have focus.
        // Unknown screens require their own verified hit geometry first.
        uint64_t hover_scene = za_menu_state();
        if(!mouse_held&&!e->motion.state&&!menu_input_captured()&&!lumiose_scroll_menu_open()&&
            atomic_load(&mouse_menu_modal_scene)!=hover_scene&&za_mouse_ui_recent(hover_scene)){
            int w=0,h=0;if(touch_window)SDL_GetWindowSize(touch_window,&w,&h);
            ZaMouseUiTarget hit=za_mouse_ui_hit(hover_scene,e->motion.x,e->motion.y,w,h);
            if(hit.valid){if(!za_mouse_ui_same(hit,za_mouse_ui_hover)&&za_mouse_ui_queue(hit,false))za_mouse_ui_hover=hit;break;}
            za_mouse_ui_hover.valid=false;
        }
        if (!mouse_held && mouse_menu_down < 0 && !e->motion.state && !menu_input_captured() &&
            !lumiose_scroll_menu_open() && (hover_scene == ZA_MENU_STATE || hover_scene == ZA_BOX_STATE ||
            hover_scene == ZA_BAG_STATE || hover_scene == ZA_DEX_STATE || hover_scene == ZA_QUEST_STATE || hover_scene == ZA_MOVES_STATE ||
            (hover_scene == ZA_RESEARCH_STATE && mouse_research_list_active())) &&
            atomic_load(&mouse_menu_modal_scene) != hover_scene && !atomic_load(&shortcut_running) &&
            !atomic_load(&shortcut_request) && !atomic_load(&mouse_tab_running) && !atomic_load(&mouse_tab_request)) {
            int w = 0, h = 0; if (touch_window) SDL_GetWindowSize(touch_window, &w, &h);
            int hit = mouse_menu_hit(hover_scene, e->motion.x, e->motion.y, w, h);
            if (hit != mouse_hover_last || mouse_hover_scene != hover_scene) {
                mouse_hover_last = hit; mouse_hover_scene = hover_scene;
                if (hit >= 0 && direct_list_screen(hover_scene)) direct_list_queue(hover_scene, hit, false);
                else if (hit >= 0 && (!atomic_load(&mouse_menu_running) || atomic_load(&mouse_menu_hover)))
                    atomic_store(&mouse_menu_request, hit + 1 + 131072 + (hover_scene == ZA_BOX_STATE ? 64 : 0));
            }
        } else { mouse_hover_last = -1; mouse_hover_scene = 0; }
        if (touch_active) touch_active = touch_position(e->motion.x, e->motion.y);
        // (the cursor free while the game plays - hold, or a tool's panels beside the game: the right button held turns it)
        if (!pad && (mouse_held || (!game_mouse_menu_active() && !keys_down[SDL_SCANCODE_LALT] && g_settings.mouse_camera != MOUSE_CAM_OFF && !menu_input_captured() && (e->motion.state & SDL_BUTTON_RMASK)))) {
            mouse_add_motion(e->motion.xrel, e->motion.yrel);
        }
        break;
    }
}

// Lumiose requests one-finger Switch touchscreen samples through this SDK import.
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", lumiose_touch_states) {
    if (!c->x[0] || (int32_t)c->x[1] <= 0) { c->x[0] = 0; return; }
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    uint64_t p = atomic_exchange(&touch_pending, 0);
    if (!p) p = atomic_load(&touch_active);
    if (menu_input_captured()) p = 0;
    memset(out, 0, 56);
    *(uint64_t *)(out + 0) = ++touch_sampling;
    if (p & TOUCH_VALID) {
        *(uint32_t *)(out + 8) = 1;
        *(uint32_t *)(out + 32) = (uint32_t)p;
        *(uint32_t *)(out + 36) = (uint32_t)((p >> 32) & 0x7fffffff);
        *(uint32_t *)(out + 40) = 8;
        *(uint32_t *)(out + 44) = 8;
        static int logged;
        if (logged++ < 5) LOG("[touch] mouse tap %u,%u\n", *(uint32_t *)(out + 32), *(uint32_t *)(out + 36));
    }
    c->x[0] = 1;
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
// "frame:ls:<x>/<y>:frames" / "frame:rs:<x>/<y>:frames" hold a stick there (-32767..32767, y up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;
static const char *const input_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                          "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// LARECOMP_RECORD_INPUT=<file>: what the player holds, frame by frame, written as LARECOMP_INPUT replays it - a battle
// played once from a save, then played again by a test (tools/za/perf-win.sh IN=@<file>) as often as needed.
// Sticks in steps of 256 with a dead zone (a resting controller's drift would make an entry every frame).
static FILE *rec_f;
static uint64_t rec_start, rec_held;
static int32_t rec_v[4];
static void rec_write(uint64_t frame) {  // what was held from rec_start to this frame
    if (frame <= rec_start) return;
    unsigned long long s = rec_start, n = frame - rec_start;
    for (int i = 0; i < 16; i++) if (rec_held >> i & 1) fprintf(rec_f, "%llu:%s:%llu,", s, input_names[i], n);
    if (rec_v[0] || rec_v[1]) fprintf(rec_f, "%llu:ls:%d/%d:%llu,", s, rec_v[0], rec_v[1], n);
    if (rec_v[2] || rec_v[3]) fprintf(rec_f, "%llu:rs:%d/%d:%llu,", s, rec_v[2], rec_v[3], n);
    fflush(rec_f);
}
static void rec_at_exit(void) {  // (what is still held as the game closes)
    extern uint64_t renderer_present_count(void);
    rec_write(renderer_present_count());
}
static void record_input(uint64_t frame, uint64_t b, const int32_t st_[4]) {
    static int on = -1;
    if (on < 0 && (on = getenv("LARECOMP_RECORD_INPUT") && (rec_f = fopen(getenv("LARECOMP_RECORD_INPUT"), "w"))))
        atexit(rec_at_exit);
    if (!on) return;
    int32_t q[4];
    for (int a = 0; a < 4; a++) q[a] = abs(st_[a]) < 4000 ? 0 : abs(st_[a]) > 32767 - 256 ? st_[a] : st_[a] / 256 * 256;
    b &= 0xFFFF;
    if (b == rec_held && !memcmp(q, rec_v, sizeof q)) return;
    rec_write(frame);
    rec_start = frame, rec_held = b;
    memcpy(rec_v, q, sizeof rec_v);
}

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick),
//   "frame:rstick:<x>/<y>:frames" the right one.
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:mouse:<dx>/<dy>" moves the mouse, "frame:mouse:click" clicks it,
// "frame:mouse:at/<x>/<y>" moves to a window position, and
// "frame:mouse:wheel/<y>" scrolls (real SDL events).
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// "frame:resize:<W>x<H>" resizes the window to W x H points (menu_apply_window_requests: the main thread).
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
uint32_t g_window_resize_request;  // (W << 16 | H: menu_apply_window_requests applies it)
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "quit")) {
        if (frame == start) { e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }
        return;
    }
    if (!strcmp(dev, "resize")) {
        unsigned w = 0, h = 0;
        if (frame == start && sscanf(arg, "%ux%u", &w, &h) == 2 && w && h && w < 65536 && h < 65536) {
            LOG("[input] test: window to %ux%u\n", w, h);
            __atomic_store_n(&g_window_resize_request, w << 16 | h, __ATOMIC_RELAXED);
        }
        return;
    }
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "mouse")) {
        float tx, ty;
        if (sscanf(arg, "wheel/%f", &ty) == 1) {
            if (frame == start && isfinite(ty)) {
                e.type = SDL_EVENT_MOUSE_WHEEL;
                e.wheel.which = TEST_DEVICE; e.wheel.y = ty;
                e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                SDL_PushEvent(&e);
            }
            return;
        }
        if (sscanf(arg, "at/%f/%f", &tx, &ty) == 2) {
            if (frame == start && isfinite(tx) && isfinite(ty)) {
                float x, y; SDL_GetMouseState(&x, &y);
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.which = TEST_DEVICE; e.motion.x = tx; e.motion.y = ty;
                e.motion.xrel = tx - x; e.motion.yrel = ty - y;
                SDL_PushEvent(&e);
            }
            return;
        }
        if (sscanf(arg, "tap/%f/%f", &tx, &ty) == 2) {
            if (frame == start || frame == start + len) {
                e.type = frame == start ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.which = TEST_DEVICE; e.button.button = SDL_BUTTON_LEFT;
                e.button.down = frame == start; e.button.x = tx; e.button.y = ty;
                SDL_PushEvent(&e);
            }
            return;
        }
        if (frame != start) return;
        float mx, my, dx = 0, dy = 0;
        SDL_GetMouseState(&mx, &my);
        if (!strcmp(arg, "click")) {
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.which = TEST_DEVICE, e.button.button = SDL_BUTTON_LEFT, e.button.down = true, e.button.clicks = 1;
            e.button.x = mx, e.button.y = my;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
        } else {
            sscanf(arg, "%f/%f", &dx, &dy);
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.which = TEST_DEVICE, e.motion.x = mx + dx, e.motion.y = my + dy, e.motion.xrel = dx, e.motion.yrel = dy;
        }
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    static char *buf;  // (a copy to cut up; as long as the longest spec: a long scripted run is thousands of steps)
    static size_t cap;
    size_t need = 2048;
    if (hspec && strlen(hspec) + 1 > need) need = strlen(hspec) + 1;
    if (bspec && strlen(bspec) + 1 > need) need = strlen(bspec) + 1;
    if (need > cap) { free(buf); buf = malloc(need); cap = need; }
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0, srx = 0, sry = 0;
        snprintf(buf, cap, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") ||
                !strcmp(dev, "resize") || !strcmp(dev, "mouse") || !strcmp(dev, "quit")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else if (!strcmp(dev, "rstick")) sscanf(name, "%d/%d", &srx, &sry);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0,
             pch = padm != test_pad || sx != test_lx || sy != test_ly || srx != test_rx || sry != test_ry;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy, test_rx = srx, test_ry = sry;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, cap, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) {
    za_mouse_ui_reset();
    za_map_wheel_reset();
    za_shop_quantity_reset();
    za_ball_ui_reset();
    za_battle_hud_reset();
    shortcut_cancel();
    direct_list_cancel(); mouse_menu_cancel(); mouse_tab_cancel(); atomic_store(&mouse_tab_info, 0); atomic_store(&mouse_tab_seen, 0); atomic_store(&mouse_tab_scene, 0);
    atomic_store(&mouse_nav_object, 0); atomic_store(&mouse_nav_scene, 0); atomic_store(&mouse_scroll_scene, 0); atomic_store(&mouse_menu_modal_scene, 0); atomic_store(&mouse_list_wheel, 0);
    atomic_store(&za_box_party, 0); atomic_store(&za_box_party_seen, 0);
    atomic_store(&za_box_active_area, -1);
    atomic_store(&mouse_box_context_seen, 0); atomic_store(&mouse_box_context_started, 0); atomic_fetch_add(&mouse_list_scene_epoch, 1); atomic_store(&za_list_guard, false);
    atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false); atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
    pthread_mutex_lock(&box_context_lock); memset(&box_context_rect, 0, sizeof box_context_rect); pthread_mutex_unlock(&box_context_lock);
    box_context_down = box_context_hover = -1;
    atomic_store(&box_wheel_steps, 0); atomic_store(&box_wheel_buttons, 0);
    atomic_store(&reward_wheel_steps, 0); atomic_store(&reward_wheel_buttons, 0);
    atomic_store(&menu_back_request, false); atomic_store(&menu_back_active, false); atomic_store(&menu_back_buttons, 0);
    pthread_mutex_lock(&drag_capture_lock);
    drag_capture.serial++; drag_capture_pending = false; drag_capture_ready_serial = 0;
    free(drag_capture_pixels); drag_capture_pixels = NULL;
    pthread_mutex_unlock(&drag_capture_lock);
    mouse_menu_down = -1; mouse_tab_down = -1; mouse_tab_down_scene = 0; mouse_menu_down_scene = 0; mouse_menu_drag_started = false;
    mouse_hover_last = -1; mouse_hover_scene = 0;
    g_pad_obj = 0; g_inject_plus = 0;
    key_focus_toggled = key_focus_was_down = false;
    atomic_store(&title_confirm_until,0);
    key_focus_acquired=false;key_focus_started=0;key_focus_started_frame=0;atomic_store(&key_focus_feedback_seen,0);atomic_store(&key_focus_feedback_active,0);atomic_store(&key_focus_object_seen,0);atomic_store(&key_focus_object_active,0);
    atomic_store(&mouse_dex_grid_observation,0);atomic_store(&mouse_dex_splash_observation,0);mouse_dex_open_down=false;
    atomic_store(&mouse_ack_confirm,0);atomic_store(&mouse_ack_confirm_scene,0);mouse_ack_down=false;
    pthread_mutex_lock(&mouse_ack_lock);memset(&mouse_ack,0,sizeof mouse_ack);pthread_mutex_unlock(&mouse_ack_lock);
    atomic_store(&key_normal_actions, 0); atomic_store(&key_normal_unseen, 0); atomic_store(&key_normal_taps, 0);
    atomic_store(&cancel_edge_start, 0);
    pthread_mutex_lock(&mouse_buttons_lock);
    aim_stage = AIM_IDLE; aim_since = 0; aim_blocked_until_release = false; aim_suppress_throw_until_release = false; wheel_context = 0; aim_wheel_steps = 0; aim_wheel_pulse = 0;
    atomic_store(&mouse_throw_cancel_until, 0); atomic_store(&mouse_throw_cancel_after, 0);
    atomic_store(&mouse_ball_ui_cancelled,false);
    aim_wheel_pressed = false; aim_wheel_frame = UINT64_MAX; mouse_buttons = 0; mouse_cancel_clear = false; mouse_aim_active = false;
    party_deferred = false; party_wheel_last = UINT64_MAX; party_pulse_until = 0; party_mouse_button = BTN_UP;
    atomic_store(&party_field_observation, 0);
    memset(mouse_down, 0, sizeof mouse_down);
    pthread_mutex_unlock(&mouse_buttons_lock);
}  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    recompute_mouse(); // advance aim/cancel transitions even without more mouse events
    uint64_t scene = za_menu_state();
    if(scene!=ZA_OUTFITS_STATE||menu_input_captured()||lumiose_scroll_menu_open()){
        mouse_outfits_action_down=false;atomic_store(&mouse_outfits_action_until,0);
    }
    if(scene!=ZA_MOVES_STATE||menu_input_captured()||lumiose_scroll_menu_open())mouse_moves_cancel();
    if(key_focus_toggled&&!pad&&scene==ZA_FIELD_STATE&&!menu_input_captured()&&!atomic_load(&mouse_aim_active)){
        uint64_t seen=atomic_load(&key_focus_object_seen),active=atomic_load(&key_focus_object_active);
        bool fresh=seen&&frame>=seen&&frame-seen<=2&&active>=key_focus_started_frame&&active&&frame>=active&&frame-active<=2;
        if(fresh)key_focus_acquired=true;
        // A short initial pipeline allowance lets the game's target query
        // finish. Thereafter the toggle exists only while a closed reticle
        // confirms a target, including rocks, crystals, and other objects.
        bool failed=!key_focus_acquired&&key_focus_started&&frame>=key_focus_started_frame&&frame-key_focus_started_frame>=6;
        bool lost=key_focus_acquired&&!fresh;
        if(failed||lost){
            key_focus_toggled=false;key_focus_started=0;key_focus_started_frame=0;recompute_keyboard();
            if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-target-empty] frame=%llu lost=%d\n",(unsigned long long)frame,lost);
        }
    }
    if (scene != ZA_BAG_STATE && scene != ZA_DEX_STATE && scene != ZA_RESEARCH_STATE && scene != ZA_SHOP_STATE && scene != ZA_QUEST_STATE && scene != ZA_MOVES_STATE && scene != ZA_OUTFITS_STATE) atomic_store(&mouse_list_wheel, 0);
    uint64_t context_seen = atomic_load(&mouse_box_context_seen);
    uint64_t context_start = atomic_load(&mouse_box_context_started);
    if (game_mouse_menu_state(scene) && atomic_load(&mouse_menu_modal_scene) == scene &&
        ((context_seen && frame >= context_seen && frame - context_seen > 8) ||
         (!context_seen && context_start && frame >= context_start && frame - context_start > 60))) {
        atomic_store(&mouse_menu_modal_scene, 0); atomic_store(&mouse_box_context_seen, 0); atomic_store(&mouse_box_context_started, 0);
        atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false); atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
    }
    static uint64_t keyboard_scene;
    if (scene != keyboard_scene) {
        if (scene == ZA_BOX_STATE || keyboard_scene == ZA_BOX_STATE) atomic_store(&za_box_active_area, -1);
        keyboard_scene = scene; atomic_fetch_add(&mouse_list_scene_epoch, 1);
        atomic_store(&mouse_list_wheel, 0); recompute_keyboard();
    }
    if (scene != atomic_load(&mouse_menu_modal_scene)) atomic_store(&mouse_menu_modal_scene, 0);
    if (!game_mouse_menu_state(scene)) {
        box_context_hover = -1; box_context_down = -1;
        atomic_store(&box_context_request, 0); atomic_store(&box_context_click_pending, false); atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_buttons, 0);
    }
    shortcut_tick(frame, scene);
    if(scene!=ZA_BIGMAP_STATE)za_map_wheel_reset();
    if (atomic_load(&direct_list_scene) && atomic_load(&direct_list_scene) != scene)
        direct_list_cancel();
    uint64_t direct_queued = atomic_load(&direct_list_queued);
    if (atomic_load(&direct_list_request) && frame >= direct_queued && frame - direct_queued > 15)
        direct_list_cancel();
    uint64_t direct_start = atomic_load(&direct_list_confirm_start);
    if (!direct_start || !atomic_load(&direct_list_click_pending) || atomic_load(&direct_list_scene) != scene ||
        menu_input_captured() || frame + 1 < direct_start || frame >= direct_start + 6) {
        atomic_store(&direct_list_buttons, 0);
        if (direct_start && (atomic_load(&direct_list_scene) != scene || frame >= direct_start + 6)) direct_list_cancel();
    } else atomic_store(&direct_list_buttons, frame >= direct_start + 2 ? 1ull << BTN_A : 0);
    uint64_t context_confirm = atomic_load(&box_context_confirm_start);
    if (!context_confirm || !atomic_load(&box_context_click_pending) || !game_mouse_menu_state(scene) || atomic_load(&mouse_menu_modal_scene) != scene ||
        menu_input_captured() || frame + 1 < context_confirm || frame >= context_confirm + 6) {
        atomic_store(&box_context_buttons, 0);
        if (context_confirm && frame >= context_confirm + 6) { atomic_store(&box_context_confirm_start, 0); atomic_store(&box_context_click_pending, false); }
    } else atomic_store(&box_context_buttons, frame >= context_confirm + 2 ? 1ull << BTN_A : 0);
    bag_item_drag_tick(frame, scene);
    za_mouse_ui_frame(frame,scene);
    za_shop_quantity_frame(frame);
    mouse_menu_tick(frame, scene);
    mouse_tab_tick(frame, scene);
    if (scene != ZA_BAG_STATE && scene != ZA_RESEARCH_STATE && scene != ZA_MOVES_STATE && scene != ZA_OUTFITS_STATE) {
        atomic_store(&mouse_tab_info, 0); atomic_store(&mouse_tab_seen, 0); atomic_store(&mouse_tab_scene, 0);
    }
    box_wheel_tick(frame, scene, (atomic_load(&mouse_menu_running) && !atomic_load(&mouse_drag_wheel_allowed)) || atomic_load(&shortcut_running));
    reward_wheel_tick(frame, scene);
    menu_back_tick(frame, game_mouse_menu_state(scene) ? scene : 0);
    if (scene != ZA_MENU_STATE && scene != ZA_BOX_STATE && scene != ZA_BAG_STATE && scene != ZA_DEX_STATE && scene != ZA_RESEARCH_STATE) { atomic_store(&mouse_nav_object, 0); atomic_store(&mouse_nav_scene, 0); }
    atomic_store(&rumble_frame_ms, SDL_GetTicks());
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    // (parsed once: a recording (LARECOMP_RECORD_INPUT) is thousands of entries; a later entry's axis wins)
    typedef struct { uint64_t start, len, b; int32_t v[4]; uint8_t set; } Ev;  // (v: lx ly rx ry, set: which of them)
    static Ev *ev;
    static int nev = -1;
    if (nev < 0) {
        char *buf = NULL;
        if (spec[0] == '@') {  // @<file>: the entries in a file (a recording: past the 32 KB a variable can hold)
            FILE *sf = fopen(spec + 1, "rb");
            long n = 0;
            if (sf && !fseek(sf, 0, SEEK_END) && (n = ftell(sf)) >= 0 && !fseek(sf, 0, SEEK_SET) && (buf = malloc(n + 1)))
                buf[fread(buf, 1, n, sf)] = 0;
            if (sf) fclose(sf);
            LOG("[input] script %s: %s\n", spec + 1, buf ? "read" : "not found");
        }
        if (!buf) buf = strdup(spec[0] == '@' ? "" : spec);
        int cap = 0;
        nev = 0;
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            unsigned long start = 0, len = 10;
            char name[32] = "";
            int x = 0, y = 0;
            Ev e = {0};
            if (sscanf(tok, "%lu:ls:%d/%d:%lu", &start, &x, &y, &len) >= 3) e.set = 3, e.v[0] = x, e.v[1] = y;
            else if (sscanf(tok, "%lu:rs:%d/%d:%lu", &start, &x, &y, &len) >= 3) e.set = 12, e.v[2] = x, e.v[3] = y;
            else if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
            else if (!strcmp(name, "STICK_UP")) e.set = 2, e.v[1] = 32767;
            else if (!strcmp(name, "STICK_DOWN")) e.set = 2, e.v[1] = -32767;
            else if (!strcmp(name, "STICK_LEFT")) e.set = 1, e.v[0] = -32767;
            else if (!strcmp(name, "STICK_RIGHT")) e.set = 1, e.v[0] = 32767;
            else if (!strcmp(name, "RSTICK_UP")) e.set = 8, e.v[3] = 32767;
            else if (!strcmp(name, "RSTICK_DOWN")) e.set = 8, e.v[3] = -32767;
            else if (!strcmp(name, "RSTICK_LEFT")) e.set = 4, e.v[2] = -32767;
            else if (!strcmp(name, "RSTICK_RIGHT")) e.set = 4, e.v[2] = 32767;
            else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
                double r = atoi(name + 6) * 3.14159265358979 / 180;
                e.set = 3, e.v[0] = (int32_t)lround(cos(r) * 32767), e.v[1] = (int32_t)lround(sin(r) * 32767);
            } else
                for (int i = 0; i < 16; i++) if (!strcmp(name, input_names[i])) e.b |= 1ull << i;
            e.start = start, e.len = len;
            if (nev == cap) ev = realloc(ev, (cap = cap ? cap * 2 : 64) * sizeof *ev);
            ev[nev++] = e;
        }
        free(buf);
    }
    uint64_t b = 0;
    int32_t v[4] = {0};
    for (int i = 0; i < nev; i++) {
        const Ev *e = &ev[i];
        if (frame < e->start || frame >= e->start + e->len) continue;
        b |= e->b;
        for (int a = 0; a < 4; a++) if (e->set >> a & 1) v[a] = e->v[a];
    }
    int32_t lx = v[0], ly = v[1], rx = v[2], ry = v[3];
    {  // test hooks steering the stick (runtime/hle/doorjump.c LARECOMP_DOORJUMP_TEST)
        extern int32_t g_test_lx, g_test_ly;
        extern uint64_t g_test_stick_until;
        if (frame < g_test_stick_until) { lx = g_test_lx; ly = g_test_ly; }
        extern uint64_t g_test_buttons, g_test_buttons_until;  // (LARECOMP_PLACE action script)
        if (frame < g_test_buttons_until) b |= g_test_buttons;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

// Recorded play (tools/za/replay-record.sh / replay-play.sh, docs/dev-tools.md): LARECOMP_RECORD=<file> writes player 1's state as
// the game reads it - "<read> <buttons hex> <lx> <ly> <rx> <ry>" each time it changes, <read> counting the game's
// reads - and LARECOMP_REPLAY=<file> gives the game those states at the same reads instead of the player's (the
// keyboard / controllers are ignored). By read, not by time: the same press lands on the same game frame on a faster
// or slower PC. Not exact over long runs (the game's own timing and threads differ a little run to run).
// The settings go along ("S <read> <hex of g_settings>": at the start and each change made in the F1 menu - 60 fps,
// resolution...: the game runs differently with them), applied at the same read.
// LARECOMP_REPLAY_UNTIL=<read>: the replay stops there and the controls are the player's again (ZA DevTools'
// "replay to the bug": its mark is a read number, g_input_reads at that moment).
typedef struct { uint64_t read, b; int32_t lx, ly, rx, ry; } RecState;
uint64_t g_input_reads;    // (the game's reads of player 1 so far)
// Keyed by the game's own time when it has one (Pokemon Z-A: g_game_vsyncs, runtime/hle/framepace.c - its frames step
// by the vsyncs that really passed, so a read count drifts on a faster / slower PC or with LARECOMP_SYNC_SUBMIT):
// a recording that starts with "K time" has times where the read numbers were. g_replay_key: the clock now.
uint64_t g_replay_key;
bool g_replay_handed;      // (the replay reached LARECOMP_REPLAY_UNTIL: live input again)
typedef struct { uint64_t read; Settings s; } RecSettings;
static int hexval(int ch) { return ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10; }
static void record_state(uint64_t read, uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    static int mode = -1;  // 0 off, 1 record, 2 replay
    static FILE *rec;
    static RecState *tab, last = {.read = ~0ull};
    static size_t n, at;
    static RecSettings *stab, slast;
    static size_t sn, sat;
    static int key_time = -1;  // (1: the clock is the game's time, g_game_vsyncs; 0: the read count)
    extern uint64_t g_game_vsyncs;
    if (mode < 0) {
        mode = 0;
        const char *r = getenv("LARECOMP_RECORD"), *p = getenv("LARECOMP_REPLAY");
        if (p && *p) {
            FILE *f = fopen(p, "rb");
            size_t cap = 0, scap = 0;
            RecState e;
            unsigned long long rd, bb;
            static char line[2 * sizeof(Settings) + 64];
            while (f && fgets(line, sizeof line, f)) {
                int off = 0;
                if (!strncmp(line, "K time", 6)) { key_time = 1; continue; }
                if (line[0] == 'S' && sscanf(line, "S %llu %n", &rd, &off) == 1 && off > 0) {
                    const char *h = line + off;
                    if (strlen(h) < 2 * sizeof(Settings)) continue;
                    if (sn == scap) stab = realloc(stab, (scap = scap ? scap * 2 : 16) * sizeof *stab);
                    stab[sn].read = rd;
                    for (size_t i = 0; i < sizeof(Settings); i++)
                        ((uint8_t *)&stab[sn].s)[i] = (uint8_t)(hexval(h[2 * i]) << 4 | hexval(h[2 * i + 1]));
                    sn++;
                } else if (sscanf(line, "%llu %llx %d %d %d %d", &rd, &bb, &e.lx, &e.ly, &e.rx, &e.ry) == 6) {
                    if (n == cap) tab = realloc(tab, (cap = cap ? cap * 2 : 4096) * sizeof *tab);
                    e.read = rd, e.b = bb;
                    tab[n++] = e;
                }
            }
            if (f) fclose(f);
            LOG("[replay] %s: %zu changes, %zu settings, up to %s %llu\n", p, n, sn, key_time == 1 ? "game time" : "read",
                n ? (unsigned long long)tab[n - 1].read : 0ull);
            if (key_time < 0) key_time = 0;
            mode = 2;
        } else if (r && *r) {
            rec = fopen(r, "wb");
            extern bool g_za_frame_hook;  // (runtime/hle/framepace.c: the game's time is known)
            key_time = g_za_frame_hook;
            if (rec && key_time) fputs("K time\n", rec);
            if (rec) mode = 1, LOG("[replay] recording to %s (by %s)\n", r, key_time ? "game time" : "read");
        }
    }
    g_input_reads = read;
    uint64_t key = key_time == 1 ? g_game_vsyncs : read;
    g_replay_key = key;
    if (mode == 2) {
        static long long until = -2;
        if (until == -2) until = getenv("LARECOMP_REPLAY_UNTIL") ? atoll(getenv("LARECOMP_REPLAY_UNTIL")) : -1;
        if (until >= 0 && (long long)key >= until) {
            mode = 0, g_replay_handed = true;
            extern uint64_t renderer_present_count(void);
            LOG("[replay] reached the mark (%s %llu, read %llu, frame %llu): the controls are yours\n",
                key_time == 1 ? "game time" : "read", (unsigned long long)key, (unsigned long long)read,
                (unsigned long long)renderer_present_count());
            return;
        }
    }
    if (mode == 1) {
        static bool first_s = true;
        if (first_s || memcmp(&g_settings, &slast.s, sizeof g_settings)) {
            first_s = false;
            slast.s = g_settings;
            fprintf(rec, "S %llu ", (unsigned long long)key);
            for (size_t i = 0; i < sizeof g_settings; i++) fprintf(rec, "%02x", ((const uint8_t *)&g_settings)[i]);
            fputc('\n', rec);
            if (read) LOG("[replay] settings changed at read %llu\n", (unsigned long long)read);
        }
        if (*b != last.b || *lx != last.lx || *ly != last.ly || *rx != last.rx || *ry != last.ry || last.read == ~0ull) {
            last = (RecState){key, *b, *lx, *ly, *rx, *ry};
            fprintf(rec, "%llu %llx %d %d %d %d\n", (unsigned long long)key, (unsigned long long)*b, *lx, *ly, *rx, *ry);
            fflush(rec);  // (the game may be closed any way: every change is on disk)
        }
    } else if (mode == 2) {
        for (; sat < sn && stab[sat].read <= key; sat++) {
            if (getenv("LARECOMP_REPLAY_ONLY_FPS")) g_settings.fps60 = stab[sat].s.fps60;  // (the rest: this run's own)
            else g_settings = stab[sat].s;
            LOG("[replay] settings of read %llu applied (fps60 %d, resolution %d)\n", (unsigned long long)stab[sat].read,
                g_settings.fps60, g_settings.res_mode);
        }
        while (at < n && tab[at].read <= key) last = tab[at++];
        if (last.read == ~0ull) last = (RecState){0};
        *b = last.b, *lx = last.lx, *ly = last.ly, *rx = last.rx, *ry = last.ry;
        static bool said;
        if (at == n && !said) said = true, LOG("[replay] end of the recording (read %llu)\n", (unsigned long long)read);
    }
}

static uint64_t keyboard_without_aim_sprint(uint64_t keyboard) {
    if ((atomic_load(&mouse_aim_active) || (keyboard & (1ull << BTN_ZL))) &&
        !atomic_load(&key_cancel_down) && !atomic_load(&key_move_b_down))
        keyboard &= ~(1ull << BTN_B);
    return keyboard;
}
static uint64_t keyboard_cancel_edges(uint64_t buttons, uint64_t frame) {
    uint64_t packed = atomic_load(&cancel_edge_start);
    bool cancel = atomic_load(&key_cancel_down);
    if (packed && frame >= packed - 1) {
        uint64_t elapsed = frame - (packed - 1);
        if (elapsed < 2) buttons &= ~(1ull << BTN_B);
        else if (elapsed < 4) buttons |= 1ull << BTN_B;
        else atomic_compare_exchange_strong(&cancel_edge_start, &packed, 0);
        if (elapsed < 4) cancel = true;
    }
    // Escape is cancel/back, not the B move while focus is held.
    if (cancel) buttons &= ~(1ull << BTN_ZL);
    return buttons;
}
static uint64_t keyboard_normal_action_focus(uint64_t buttons, uint64_t actions) {
    // A normal action must reach the game without ZL; otherwise the same
    // face-button bit means a move. The keyboard focus toggle stays latched.
    return actions ? buttons & ~(1ull << BTN_ZL) : buttons;
}
static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    if (g_game_is_la) {
    extern void warp_game_tick(Cpu *c);
    warp_game_tick(c);  // game main thread, once per frame: apply a queued warp
    { extern void give_item_tick(Cpu *c, uint64_t frame); extern uint64_t renderer_present_count(void);
      give_item_tick(c, renderer_present_count());
      extern void chamber_game_tick(Cpu *c, uint64_t frame);
      chamber_game_tick(c, renderer_present_count());
      extern void digs_game_tick(Cpu *c, uint64_t frame);
      digs_game_tick(c, renderer_present_count());
      extern void pot_bush_game_tick(Cpu *c, uint64_t frame);
      pot_bush_game_tick(c, renderer_present_count());
      extern void shell_spots_tick(Cpu *c, uint64_t frame);
      shell_spots_tick(c, renderer_present_count());
      extern void qol_tick(void);
      qol_tick();
      extern void run_timer_tick(void);
      run_timer_tick();  // (run timer / LiveSplit: runtime/hle/run_timer.c)
      extern void pets_game_tick(Cpu *c);
      pets_game_tick(c); }
    }
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t keyboard = keyboard_without_aim_sprint(key_buttons);
    bool suppress_sprint = (atomic_load(&mouse_aim_active) || (key_buttons & (1ull << BTN_ZL))) &&
                           !atomic_load(&key_cancel_down) && !atomic_load(&key_move_b_down);
    uint64_t held = keyboard | pad_buttons;
    // Release sprint/back before the synthetic cancel press, so holding Shift
    // cannot hide the B press edge required to put the ball away.
    if (atomic_load(&mouse_cancel_clear)) held &= ~(1ull << BTN_B);
    held |= mouse_buttons;
    atomic_fetch_and(&press_unseen, ~held);  // (the game sees these now)
    static uint64_t tap, tap_frame;  // (pressed and released since the last poll: held for the rest of this frame)
    static uint64_t action_tap, action_tap_frame;
    {
        extern uint64_t renderer_present_count(void);
        uint64_t t = atomic_exchange(&tap_buttons, 0), f = renderer_present_count();
        uint64_t actions = atomic_load(&key_normal_actions);
        atomic_fetch_and(&key_normal_unseen, ~actions);
        uint64_t at = atomic_exchange(&key_normal_taps, 0);
        if (at) action_tap = at, action_tap_frame = f;
        else if (f > action_tap_frame) action_tap = 0;
        if (t) {
            tap |= t, tap_frame = f;
            if (getenv("LARECOMP_INPUT")) LOG("[input] short press %#llx\n", (unsigned long long)t);
        } else if (f > tap_frame) tap = 0;
    }
    uint64_t b = menu ? script_buttons : held | tap | script_buttons;
    uint64_t title_until=atomic_load(&title_confirm_until),title_frame=renderer_present_count();
    if(!menu&&title_until&&za_menu_state()==ZA_TITLE_STATE&&title_frame<title_until)b|=1ull<<BTN_A;
    else if(title_until)atomic_store(&title_confirm_until,0);
    if (!menu && (atomic_load(&mouse_cancel_clear) || (suppress_sprint && !(mouse_buttons & (1ull << BTN_B)))))
        b &= ~(1ull << BTN_B);
    if (!menu) {
        extern uint64_t renderer_present_count(void);
        b = keyboard_cancel_edges(b, renderer_present_count());
        b = keyboard_normal_action_focus(b, atomic_load(&key_normal_actions) | action_tap);
        b |= legacy_chat_pulse();
    }
    if (!menu && atomic_load(&shortcut_running)) {
        const uint64_t mask = (1ull << BTN_A) | (1ull << BTN_B) | (1ull << BTN_X) | (1ull << BTN_Y) |
                              (1ull << BTN_ZL) | (1ull << BTN_UP) | (1ull << BTN_DOWN);
        b = (b & ~mask) | atomic_load(&shortcut_buttons);
    }
    if (!menu && atomic_load(&mouse_menu_running)) {
        const uint64_t mask = (1ull << BTN_A) | (1ull << BTN_B) | (1ull << BTN_X) | (1ull << BTN_Y) |
                              (1ull << BTN_ZL) | (1ull << BTN_LEFT) | (1ull << BTN_RIGHT) |
                              (1ull << BTN_UP) | (1ull << BTN_DOWN);
        b = (b & ~mask) | atomic_load(&mouse_menu_buttons);
    }
    if (!menu && atomic_load(&mouse_tab_running)) {
        uint64_t mask = (1ull << BTN_L) | (1ull << BTN_R);
        if(za_menu_state()==ZA_MOVES_STATE)mask|=(1ull<<BTN_ZL)|(1ull<<BTN_ZR);
        b = (b & ~mask) | atomic_load(&mouse_tab_buttons);
    }
    uint64_t outfits_until=atomic_load(&mouse_outfits_action_until);
    if(!menu&&za_menu_state()==ZA_OUTFITS_STATE&&renderer_present_count()<outfits_until)b|=1ull<<BTN_MINUS;
    else if(outfits_until){atomic_store(&mouse_outfits_action_until,0);mouse_outfits_action_down=false;}
    if (!menu) b |= atomic_load(&direct_list_buttons);
    if (!menu) b |= atomic_load(&za_mouse_ui_buttons);
    if (!menu) b |= atomic_load(&za_shop_quantity_buttons);
    if (!menu) b |= atomic_load(&box_context_buttons);
    if (!menu) b |= atomic_load(&box_wheel_buttons);
    if (!menu) b |= atomic_load(&reward_wheel_buttons);
    uint64_t ack_frame=renderer_present_count(),ack_confirm=atomic_load(&mouse_ack_confirm);
    if(!menu&&ack_confirm&&atomic_load(&mouse_ack_confirm_scene)==za_menu_state()&&ack_frame>=ack_confirm&&ack_frame-ack_confirm<4)
        b=(b&~(1ull<<BTN_ZL))|(1ull<<BTN_A);
    else if(ack_confirm&&ack_frame>=ack_confirm+4)atomic_store(&mouse_ack_confirm,0);
    if (!menu && atomic_load(&menu_back_active)) { b &= ~((1ull << BTN_ZL) | (1ull << BTN_B)); b |= atomic_load(&menu_back_buttons); }
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t mrx, mry;
    mouse_stick(&mrx, &mry);
    // The ball camera consumes the evaluated native follow pose. Suppress its
    // mouse-stick copy, including entry and return, so each delta turns once.
    uint64_t return_until = atomic_load(&mouse_aim_return_until);
    uint64_t mouse_now=SDL_GetTicksNS();
    if (mouse_native_active(mouse_now) && (mouse_ball_native_active(mouse_now)||
        (!atomic_load(&mouse_aim_active)&&(!return_until||mouse_now>=return_until))))mrx=mry=0;
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mrx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + mry) + script_ry);
    if (b || abs(lx) > 16000 || abs(ly) > 16000 || abs(rx) > 16000 || abs(ry) > 16000) {
        extern void discord_presence_note_input(void);
        discord_presence_note_input();
    }
    {
        extern uint64_t renderer_present_count(void);
        record_input(renderer_present_count(), b, (int32_t[4]){lx, ly, rx, ry});  // (before mirror mode swaps them)
    }
    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    {   // door jumps stop Link for a moment before moving him (runtime/hle/doorjump.c): stick + d-pad released
        extern int32_t g_stick_lx, g_stick_ly;  // (what the player holds, before that: doorjump.c reads it)
        g_stick_lx = lx, g_stick_ly = ly;
        extern uint64_t g_stick_freeze_until;
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < g_stick_freeze_until) {
            lx = ly = 0;
            b &= ~(0xfull << 12);
        }
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    {   // the pause menu's companion slot (runtime/hle/pets.c) takes the d-pad / sticks / A while the cursor is on it
        extern uint64_t pets_menu_input(uint64_t b, int32_t st[4]);
        int32_t st[4] = {lx, ly, rx, ry};
        b = pets_menu_input(b, st);
        // the check tracker drives the Map page in randomizer seeds (runtime/ui/tracker_overlay.mm): its cursor takes
        // the d-pad / left stick / A / Y (and B while one of its panels is open)
        extern uint64_t tracker_map_input(uint64_t b, int32_t st[4]);
        b = tracker_map_input(b, st);
        lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    }
    {
        static uint64_t reads;
        record_state(reads++, &b, &lx, &ly, &rx, &ry);
    }
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    static int32_t last_rx, last_ry;
    if ((rx != last_rx || ry != last_ry) && getenv("LARECOMP_INPUT")) LOG("[input] id %#x right stick %d %d\n", id, rx, ry);
    last_rx = rx, last_ry = ry;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    { extern void input_note_rstick(int32_t ry); input_note_rstick(ry); }
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// The SDK vibration node graph stays recompiled; HID IPC 201/206 sends its
// output to rumble.h / SDL using the native-layout handles below.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    // Native wire layout is style, player, device index, padding (one byte each).
    // The old synthetic handles placed the style byte in padding.
    for (int i = 0; i < n; i++) out[i] = 3u | ((id & 0xFF) << 8) | ((uint32_t)i << 16);
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    int side = rumble_side(h);
    info[1] = side < 0 ? 0 : (uint32_t)side + 1;  // None=0, Left=1, Right=2
}
