#ifndef SC_TOAST_H
#define SC_TOAST_H

#include "common.h"

#include <stdbool.h>

struct sc_screen;

// Show a transient message centered at the bottom of the window (a few seconds,
// then it fades out). Safe to call from any thread: it just posts an event.
void
sc_toast_show(const char *msg, bool error);

// Adopt an SC_EVENT_TOAST payload on the main thread (data may be a heap message
// to take ownership of, or NULL for an expiry/repaint tick).
void
sc_toast_accept(void *data);

// Draw the active toast, if any (no-op when none is showing or it has expired).
void
sc_toast_render(struct sc_screen *screen);

#endif
