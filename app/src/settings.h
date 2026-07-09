#ifndef SC_SETTINGS_H
#define SC_SETTINGS_H

#include "common.h"

#include <stdbool.h>
#include <SDL3/SDL.h>

struct sc_screen;

// Right-side drawer with tabs (Buttons / Shell / Misc) that edits scrapy.conf.
// Pressing Save writes the config and relaunches the app so the new settings
// take effect. Shares the right-side region with the other drawers.

void
sc_settings_init(void);

void
sc_settings_toggle(struct sc_screen *screen);

void
sc_settings_close(struct sc_screen *screen);

bool
sc_settings_is_open(void);

bool
sc_settings_step_anim(void);

int
sc_settings_reserved_width(struct sc_screen *screen);

void
sc_settings_render(struct sc_screen *screen);

bool
sc_settings_handle_event(struct sc_screen *screen, const SDL_Event *event);

#endif
