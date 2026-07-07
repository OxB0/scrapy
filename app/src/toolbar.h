#ifndef SC_TOOLBAR_H
#define SC_TOOLBAR_H

#include "common.h"

#include <stdbool.h>

struct sc_screen;

// Width (logical px) of the toolbar gutter reserved on the right of the window.
int
sc_toolbar_width(void);

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
