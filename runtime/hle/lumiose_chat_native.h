#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

// SDL event-thread only. The mod draws the draft; the launcher owns no visible widget.
void lumiose_chat_native_tick(void);
bool lumiose_chat_native_event(const SDL_Event *event);
bool lumiose_chat_native_wants_text(void);
bool lumiose_chat_native_click(bool send);
bool lumiose_chat_native_window_click(unsigned field);
