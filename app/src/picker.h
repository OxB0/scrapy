#ifndef SC_PICKER_H
#define SC_PICKER_H

#include "common.h"

#include <stdbool.h>

// Startup device picker: if no device was specified, open a small window that
// lists connected devices. Auto-connects a single device (no window shown),
// waits when none are connected, and lets the user click when several are.
//
// On return:
//  - returns true and sets *out_serial (malloc'd, caller owns) to the chosen
//    device, or leaves it NULL to fall back to scrcpy's default selection;
//  - returns false if the user closed the window (the app should exit).
bool
sc_picker_run(char **out_serial);

#endif
