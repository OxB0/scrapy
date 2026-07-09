#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

// Configurable keyboard shortcuts, one per toolbar action.
enum sc_shortcut {
    SC_SHORTCUT_SHELL,
    SC_SHORTCUT_APPS,
    SC_SHORTCUT_LOG,
    SC_SHORTCUT_SETTINGS,
    SC_SHORTCUT_SCREENSHOT,
    SC_SHORTCUT_RECORD,
    SC_SHORTCUT_AWAKE,
    SC_SHORTCUT_PIN,
    SC_SHORTCUT_BACK,
    SC_SHORTCUT_HOME,
    SC_SHORTCUT_RECENTS,
    SC_SHORTCUT_MENU,
    SC_SHORTCUT_NOTIF,
    SC_SHORTCUT_VOLUP,
    SC_SHORTCUT_VOLDOWN,
    SC_SHORTCUT_ROTATE,
    SC_SHORTCUT_POWER,
    SC_SHORTCUT_COUNT,
};

// User configuration, loaded once at startup from "scrapy.conf" next to the
// executable (a commented template is written there on first run).
struct sc_config {
    bool has_buttons;       // was a "buttons" line present?
    char buttons[512];      // comma-separated button names, or "none"
    int shell_width;        // terminal drawer width (0 = default)
    int apps_width;         // apps/density drawer width (0 = default)
    int log_width;          // log drawer width (0 = default)
    char capture_dir[512];  // where screenshots/recordings go (empty = home)
    bool pin_on_top;        // start pinned always-on-top
    int default_density;    // set this dpi on connect (0 = leave as-is)
    float terminal_text_size; // terminal font scale (0 = default 1.8)
    bool notifications;       // show bottom-screen notifications (default true)
    float notification_time;  // seconds a notification stays (0 = default)
    float notification_text_size; // notification font scale (0 = default 2.0)
    struct {
        unsigned mod;       // SDL_Keymod mask
        int key;            // SDL_Keycode (0 = unbound)
    } shortcuts[SC_SHORTCUT_COUNT];
};

extern struct sc_config sc_conf;

// Load the config (idempotent). Writes a default template if none exists.
void
sc_config_load(void);

// Write the current sc_conf values back to the config file (used by the
// Settings drawer's Save button). Returns true on success.
bool
sc_config_save(void);

// Format shortcut `idx` (an sc_shortcut) as e.g. "Alt+Shift+T" into `buf`
// (empty string if unbound).
void
sc_shortcut_format(int idx, char *buf, size_t n);

#endif
