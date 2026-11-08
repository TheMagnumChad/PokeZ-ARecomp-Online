#include "lumiose_chat_bridge.h"
#include "../hle.h"
#include <pthread.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(LumioseChatBridge, head) == 0x20, "chat bridge head offset");
_Static_assert(offsetof(LumioseChatBridge, commands) == 0x24, "chat bridge ring offset");
_Static_assert(offsetof(LumioseChatBridge, text) == 0x64, "chat bridge text offset");
_Static_assert(sizeof(LumioseChatBridge) == 0xe4, "chat bridge size");

LumioseChatBridge *lumiose_chat_bridge_find(void *data, size_t size) {
    if (!data || size < sizeof(LumioseChatBridge)) return NULL;
    unsigned char *p = data;
    size_t start = (16 - ((uintptr_t)p & 15)) & 15;
    LumioseChatBridge *found = NULL;
    for (size_t i = start; i <= size - sizeof(LumioseChatBridge); i += 16) {
        if (memcmp(p + i, "VMPCHAT\0", 8)) continue;
        LumioseChatBridge *b = (LumioseChatBridge *)(p + i);
        uint32_t version = __atomic_load_n(&b->version, __ATOMIC_ACQUIRE);
        uint32_t length = __atomic_load_n(&b->size, __ATOMIC_ACQUIRE);
        if ((version != 1 && version != 2) || length < (version == 2 ? 0xe8 : sizeof *b) || length > size - i) continue;
        if (found) return NULL;  // An ambiguous scan must never write to either candidate.
        found = b;
    }
    return found;
}

// The bridge text is UTF-8, at most 120 bytes. Reject invalid input and oversize
// strings so callers can inform the player instead of silently cutting a message.
static bool valid_text(const char *s, size_t *bytes) {
    if (!s) return false;
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        unsigned char c = *p;
        unsigned count = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                         c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!count || c < 0x20 || c == 0x7f || n + count > 120) return false;
        for (unsigned i = 1; i < count; i++) if (!p[i] || (p[i] & 0xc0) != 0x80) return false;
        if (count > 2 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f) ||
                          (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f))) return false;
        if (count == 2 && c == 0xc2 && p[1] < 0xa0) return false;  // C1 controls
        p += count; n += count;
    }
    *bytes = n;
    return true;
}

bool lumiose_chat_bridge_enqueue(LumioseChatBridge *b, uint16_t cmd, int16_t arg, const char *text) {
    if (!b || cmd < LUMIOSE_CHAT_FOCUS || cmd > LUMIOSE_CHAT_EDIT) return false;
    uint32_t state = __atomic_load_n(&b->state, __ATOMIC_ACQUIRE);
    uint32_t version = __atomic_load_n(&b->version, __ATOMIC_ACQUIRE);
    bool edit = version == 2 && (state & LUMIOSE_CHAT_FIELD_EDIT);
    if (state & LUMIOSE_CHAT_KEYBOARD) return false;
    if (cmd == LUMIOSE_CHAT_EDIT) {
        if (version != 2 || edit || arg < 2 || arg > 13 ||
            __atomic_load_n((uint8_t *)b + 0xe4, __ATOMIC_ACQUIRE) != arg) return false;
    } else if (edit) {
        if (cmd != LUMIOSE_CHAT_CLOSE && cmd != LUMIOSE_CHAT_SET_DRAFT &&
            cmd != LUMIOSE_CHAT_SEND && cmd != LUMIOSE_CHAT_SEND_TEXT) return false;
    } else {
        if (state & LUMIOSE_CHAT_OTHER_WINDOW) return false;
        if (cmd != LUMIOSE_CHAT_FOCUS && !(state & LUMIOSE_CHAT_FOCUSED)) return false;
    }
    uint32_t head = __atomic_load_n(&b->head, __ATOMIC_RELAXED);
    uint32_t done = __atomic_load_n(&b->done, __ATOMIC_ACQUIRE);
    if ((uint32_t)(head - done) >= 16) return false;
    if (cmd == LUMIOSE_CHAT_SET_DRAFT || cmd == LUMIOSE_CHAT_SEND_TEXT) {
        size_t n;
        if (head != done || !valid_text(text, &n)) return false;
        memset(b->text, 0, sizeof b->text);
        memcpy(b->text, text, n);
    }
    b->commands[head % 16].cmd = cmd;
    b->commands[head % 16].arg = arg;
    __atomic_store_n(&b->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static LumioseChatBridge *active;

RT_ON_RESET(lumiose_chat_bridge_reset) {
    pthread_mutex_lock(&bridge_lock);
    active = NULL;
    pthread_mutex_unlock(&bridge_lock);
}

void lumiose_chat_bridge_init(void) {
    pthread_mutex_lock(&bridge_lock);
    active = NULL;
    if (g_nmodules > 2) {
        Module *m = &g_modules[2];
        if (m->base && m->data_off <= m->size && m->data_size <= m->size - m->data_off)
            active = lumiose_chat_bridge_find((void *)(uintptr_t)(m->base + m->data_off), (size_t)m->data_size);
    }
    if (active) LOG("[lumiose] chat bridge version %u available\n", active->version);
    else LOG("[lumiose] no supported chat bridge; using existing chat input\n");
    pthread_mutex_unlock(&bridge_lock);
}

bool lumiose_chat_bridge_snapshot(LumioseChatSnapshot *out) {
    if (!out) return false;
    pthread_mutex_lock(&bridge_lock);
    bool ok = active != NULL;
    if (ok) {
        out->state = __atomic_load_n(&active->state, __ATOMIC_ACQUIRE);
        out->selection = __atomic_load_n(&active->selection, __ATOMIC_RELAXED);
        out->draft_len = __atomic_load_n(&active->draft_len, __ATOMIC_RELAXED);
        out->version = active->version;
        out->focus_field = active->version == 2 ? __atomic_load_n((uint8_t *)active + 0xe4, __ATOMIC_ACQUIRE) : 0;
    }
    pthread_mutex_unlock(&bridge_lock);
    return ok;
}

bool lumiose_chat_bridge_command(uint16_t cmd, int16_t arg, const char *text) {
    pthread_mutex_lock(&bridge_lock);
    bool ok = lumiose_chat_bridge_enqueue(active, cmd, arg, text);
    pthread_mutex_unlock(&bridge_lock);
    return ok;
}

int lumiose_chat_bridge_scroll(int16_t lines) {
    pthread_mutex_lock(&bridge_lock);
    int result = -1;
    if (active) {
        uint32_t state = __atomic_load_n(&active->state, __ATOMIC_ACQUIRE);
        if ((state & LUMIOSE_CHAT_FOCUSED) && !(state & (LUMIOSE_CHAT_KEYBOARD | LUMIOSE_CHAT_OTHER_WINDOW)))
            result = lumiose_chat_bridge_enqueue(active, LUMIOSE_CHAT_SCROLL, lines, NULL) ? 1 : 0;
    }
    pthread_mutex_unlock(&bridge_lock);
    return result;
}
