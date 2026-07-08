#ifndef SC_CAPTURE_H
#define SC_CAPTURE_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

// Screenshot / screen-recording helpers, driven by adb. Captures are saved to
// the user's home directory. Toolbar buttons call these; the toolbar renders
// the toast message for feedback.

void
sc_capture_init(const char *serial);

void
sc_capture_destroy(void);

// Save a screenshot of the device to the PC (asynchronous).
void
sc_capture_screenshot(void);

// Start recording, or stop-and-pull if already recording (asynchronous).
void
sc_capture_record_toggle(void);

bool
sc_capture_recording(void);

// Keep the device screen on while plugged in (svc power stayon).
void
sc_capture_stay_awake(bool on);

// Current keep-awake state (read from the device on connect, updated on toggle).
bool
sc_capture_awake_is_on(void);

#endif
