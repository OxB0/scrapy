#ifndef SC_APPS_H
#define SC_APPS_H

#include "common.h"

#include <stdbool.h>
#include <SDL3/SDL.h>

struct sc_screen;

// Right-side drawer that lists the installed (launchable) apps so they can be
// launched with a click, and exposes display-density controls at the top.
// Shares the right-side region with the terminal drawer (only one open at a
// time).

void
sc_apps_init(const char *serial);

void
sc_apps_destroy(void);

void
sc_apps_toggle(struct sc_screen *screen);

void
sc_apps_close(struct sc_screen *screen);

bool
sc_apps_is_open(void);

// Width (logical px) currently reserved on the right for the drawer.
int
sc_apps_reserved_width(struct sc_screen *screen);

void
sc_apps_render(struct sc_screen *screen);

// Returns true if the event was consumed by the drawer.
bool
sc_apps_handle_event(struct sc_screen *screen, const SDL_Event *event);

#endif
