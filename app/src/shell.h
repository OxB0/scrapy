#ifndef SC_SHELL_H
#define SC_SHELL_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>

struct sc_screen;

// Store the device serial to target with `adb -s <serial> shell`.
void
sc_shell_init(const char *serial);

// Write the default shell-log path into `buf` (used to pre-fill the setting).
void
sc_shell_default_log_path(char *buf, size_t n);

// Open/close the sliding terminal drawer (spawns adb shell on first open).
void
sc_shell_toggle(struct sc_screen *screen);

// Close the drawer if it is open (no-op otherwise).
void
sc_shell_close(struct sc_screen *screen);

bool
sc_shell_is_open(void);

// Advance the open/close slide animation one frame. Returns true if the drawer
// height changed (the caller should then recompute the video content rect).
bool
sc_shell_step_anim(void);

// Current animated drawer width, in logical (window) px (0 when fully closed).
// The terminal opens as a panel on the right, left of the toolbar gutter.
int
sc_shell_reserved_width(struct sc_screen *screen);

// Draw the drawer + terminal contents. Call at the end of the screen render.
void
sc_shell_render(struct sc_screen *screen);

// Feed a keyboard/text event to the terminal when it is open. Returns true if
// the event was consumed (must not be forwarded to the device).
bool
sc_shell_handle_event(struct sc_screen *screen, const SDL_Event *event);

void
sc_shell_destroy(void);

#endif
