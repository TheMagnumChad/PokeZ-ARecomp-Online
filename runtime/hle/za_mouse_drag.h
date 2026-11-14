#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float center_x, center_y, size;
    int window_w, window_h;
    uint64_t serial;
} ZaDragCapture;

// The render thread consumes a single pending request and returns its pixels;
// the UI thread creates and owns the SDL cursor.
bool input_drag_capture_take(ZaDragCapture *out);
void input_drag_capture_submit(uint64_t serial, const void *pixels, int w, int h, bool bgr);
