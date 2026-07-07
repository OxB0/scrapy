#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include "common.h"

#include <stdbool.h>

// User configuration, loaded once at startup from "scrapy.conf" next to the
// executable (a commented template is written there on first run).
struct sc_config {
    bool has_buttons;       // was a "buttons" line present?
    char buttons[512];      // comma-separated button names, or "none"
    int shell_width;        // terminal drawer width (0 = default)
    int apps_width;         // apps/density drawer width (0 = default)
    char capture_dir[512];  // where screenshots/recordings go (empty = home)
    bool pin_on_top;        // start pinned always-on-top
    int default_density;    // set this dpi on connect (0 = leave as-is)
};

extern struct sc_config sc_conf;

// Load the config (idempotent). Writes a default template if none exists.
void
sc_config_load(void);

#endif
