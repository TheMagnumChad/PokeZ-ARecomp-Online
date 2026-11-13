// Exact verified private module profile. Unknown identities fail closed.
#pragma once
#include "rt.h"
#define LUMIOSE_NATIVE_PROFILE_ENABLED 1
static const uint8_t lumiose_native_build_id[32] = {0xfe,0xd1,0xe6,0x4f,0xbb,0x84,0x79,0xba,0xa7,0x8e,0x4f,0x57,0xaf,0xda,0x7f,0x46,0x8c,0x7d,0x4a,0xd2,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static inline bool lumiose_native_module(void) {
    return LUMIOSE_NATIVE_PROFILE_ENABLED && g_nmodules > 2 &&
        !memcmp(g_modules[2].build_id, lumiose_native_build_id, 32);
}
static inline bool lumiose_native_caller(uint64_t lr) {
    return lumiose_native_module() && lr >= g_modules[2].base + g_modules[2].text_off &&
        lr < g_modules[2].base + g_modules[2].text_off + g_modules[2].text_size;
}
static inline bool lumiose_native_camera_query(uint64_t lr) {
    if (!lumiose_native_module()) return false;
    uint64_t offset = lr - g_modules[2].base;
    return offset == 0x152b0 || offset == 0x15320 || offset == 0x15400;
}
