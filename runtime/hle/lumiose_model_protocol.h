#pragma once
#include <stdint.h>
#include <stdbool.h>
#define LUMIOSE_MODEL_V2_TYPE UINT64_C(0x4d000000)
#define LUMIOSE_MODEL_V2_CAPABILITY UINT64_C(0x4d4f4432)
typedef struct { unsigned key, species, form, gender, shiny; } LumioseModelIdentity;
static inline bool lumiose_model_decode(uint64_t type, LumioseModelIdentity *out) {
    if (!out || (type & ~UINT64_C(0xfffff)) != LUMIOSE_MODEL_V2_TYPE) return false;
    const unsigned key = (unsigned)(type & 0xfffffu);
    const unsigned species = key & 0x7ffu, gender = (key >> 17) & 3u;
    if (!species || gender > 2) return false;
    *out = (LumioseModelIdentity){key, species, (key >> 11) & 63u, gender, (key >> 19) & 1u};
    return true;
}
