#ifndef SC_TOOLBAR_H
#define SC_TOOLBAR_H

#include "common.h"

#include <stdbool.h>

struct sc_screen;

// Build the visible button set from config and apply pin-on-top default.
void
sc_toolbar_init(struct sc_screen *screen);

// Toggle the always-on-top ("pin") state.
void
sc_toolbar_toggle_pin(struct sc_screen *screen);

// Perform the toolbar action bound to the given sc_shortcut value.
void
sc_toolbar_trigger(struct sc_screen *screen, int shortcut);

// Width (logical px) of the toolbar gutter reserved on the right of the window.
int
sc_toolbar_width(void);

// Enumerate every known toolbar button (the full set, not the visible subset),
// so the Settings drawer can present visibility checkboxes.
int
sc_toolbar_all_count(void);

const char *
sc_toolbar_all_name(int i);

const char *
sc_toolbar_all_label(int i);

// Render the always-visible on-screen button toolbar over the mirror.
// Called at the end of the screen render, before presenting.
void
sc_toolbar_render(struct sc_screen *screen);

// Handle a left mouse-button-down at logical window coordinates (x, y).
// Returns true if a toolbar button was hit (and its action performed), so the
// click must not be forwarded to the device.
bool
sc_toolbar_click(struct sc_screen *screen, float x, float y);

#endif
