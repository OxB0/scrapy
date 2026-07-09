#ifndef SC_RESTART_H
#define SC_RESTART_H

#include "common.h"

#include <stdbool.h>

// Request that the app relaunch itself once the current session exits (used
// after saving settings that only take effect at startup). The main loop
// performs the actual re-exec.
void
sc_restart_request(void);

bool
sc_restart_pending(void);

#endif
