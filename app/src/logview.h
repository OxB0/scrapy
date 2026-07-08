#ifndef SC_LOGVIEW_H
#define SC_LOGVIEW_H

#include "common.h"

#include <stdbool.h>
#include <SDL3/SDL.h>

struct sc_screen;

// Right-side drawer that shows the scrcpy log (everything printed via LOGI/
// LOGW/LOGE and FFmpeg), so it is visible even when running without a console.
// Shares the right-side region with the terminal and apps drawers (only one
// open at a time).

// Initialize the capture buffer. Called once from sc_log_configure(), before
// any window exists, so early startup logs are captured too.
void
sc_logview_init(void);

// Append a formatted log line (may contain newlines). Thread-safe.
void
sc_logview_push(const char *msg);

void
sc_logview_toggle(struct sc_screen *screen);

void
sc_logview_close(struct sc_screen *screen);

bool
sc_logview_is_open(void);

bool
sc_logview_step_anim(void);

int
sc_logview_reserved_width(struct sc_screen *screen);

void
sc_logview_render(struct sc_screen *screen);

// Returns true if the event was consumed by the drawer.
bool
sc_logview_handle_event(struct sc_screen *screen, const SDL_Event *event);

#endif
