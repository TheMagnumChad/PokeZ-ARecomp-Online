// Pokémon Z-A 2.0.2's follow camera returns a position relative to its target. Scale the distance from that target,
// preserving the camera's orientation and the vertical target offset. Other game versions have no patch at this site.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../cpu.h"
#include "../settings.h"

void za_camera_scale_offset(float v[3], float target_height, int percent) {
    if (percent < 50) percent = 50;
    if (percent > 200) percent = 200;
    float scale = (float)percent / 100.0f;
    v[0] *= scale;
    v[1] = target_height + (v[1] - target_height) * scale;
    v[2] *= scale;
}

void rt_za_camera_distance(uint64_t object, uint64_t caller) {
    // Only the follow-camera consumer: this shared getter also serves the title and other camera properties.
    if (caller != 0x7000371bf4ull ||
        object < 0x7000000000ull || object > 0x7800000000ull - 0x2e8 ||
        LD64(object) != 0x7003c106a0ull) return;
    float offset[3], height;
    memcpy(offset, (const void *)(uintptr_t)(object + 0x2c0), sizeof offset);
    memcpy(&height, (const void *)(uintptr_t)(object + 0x2e4), sizeof height);
    if (!isfinite(height) || !isfinite(offset[0]) || !isfinite(offset[1]) || !isfinite(offset[2]) ||
        fabsf(offset[0]) >= 30 || fabsf(offset[1]) >= 30 || fabsf(offset[2]) >= 30) return;
    // Share the native optical direction, independently of distance. Both
    // camera modes then consume the same mouse-driven pose during aim/return.
    extern void input_native_follow_offset(const float *,float);
    input_native_follow_offset(offset,height);
    if(g_settings.camera_distance==100)return;
    za_camera_scale_offset(offset, height, g_settings.camera_distance);
    memcpy((void *)(uintptr_t)(object + 0x2c0), offset, sizeof offset);
}
