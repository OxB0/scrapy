#include "restart.h"

static bool g_pending;

void
sc_restart_request(void) {
    g_pending = true;
}

bool
sc_restart_pending(void) {
    return g_pending;
}
