#include "userconf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <windows.h>
#endif

struct sc_config sc_conf;

// Parse a shortcut like "alt+shift+t" into an SDL modifier mask + keycode.
static bool
parse_combo(const char *s, unsigned *mod, int *key) {
    unsigned m = 0;
    int k = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        while (*tok == ' ') {
            ++tok;
        }
        char low[32];
        int i = 0;
        for (; tok[i] && i < 31; ++i) {
            low[i] = (char) tolower((unsigned char) tok[i]);
        }
        low[i] = '\0';
        while (i > 0 && low[i - 1] == ' ') {
            low[--i] = '\0';
        }
        if (!strcmp(low, "alt")) {
            m |= SDL_KMOD_ALT;
        } else if (!strcmp(low, "ctrl") || !strcmp(low, "control")) {
            m |= SDL_KMOD_CTRL;
        } else if (!strcmp(low, "shift")) {
            m |= SDL_KMOD_SHIFT;
        } else if (!strcmp(low, "super") || !strcmp(low, "gui")
                || !strcmp(low, "win") || !strcmp(low, "cmd")) {
            m |= SDL_KMOD_GUI;
        } else if (!strcmp(low, "space")) {
            k = SDLK_SPACE;
        } else if (!strcmp(low, "up")) {
            k = SDLK_UP;
        } else if (!strcmp(low, "down")) {
            k = SDLK_DOWN;
        } else if (!strcmp(low, "left")) {
            k = SDLK_LEFT;
        } else if (!strcmp(low, "right")) {
            k = SDLK_RIGHT;
        } else if (low[0] && !low[1]) {
            k = (int) low[0]; // single char: lowercase ASCII == SDL_Keycode
        }
    }
    if (!k) {
        return false;
    }
    *mod = m;
    *key = k;
    return true;
}

static void
set_default_shortcuts(void) {
    unsigned as = SDL_KMOD_ALT | SDL_KMOD_SHIFT;
    sc_conf.shortcuts[SC_SHORTCUT_APPS].mod = SDL_KMOD_ALT;
    sc_conf.shortcuts[SC_SHORTCUT_APPS].key = SDLK_A;
    sc_conf.shortcuts[SC_SHORTCUT_SHELL].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_SHELL].key = SDLK_T;
    sc_conf.shortcuts[SC_SHORTCUT_LOG].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_LOG].key = SDLK_L;
    sc_conf.shortcuts[SC_SHORTCUT_SCREENSHOT].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_SCREENSHOT].key = SDLK_S;
    sc_conf.shortcuts[SC_SHORTCUT_RECORD].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_RECORD].key = SDLK_R;
    sc_conf.shortcuts[SC_SHORTCUT_AWAKE].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_AWAKE].key = SDLK_K;
    sc_conf.shortcuts[SC_SHORTCUT_PIN].mod = as;
    sc_conf.shortcuts[SC_SHORTCUT_PIN].key = SDLK_P;

    // Device actions default to scrcpy's built-in bindings (Alt = MOD).
    unsigned a = SDL_KMOD_ALT;
    sc_conf.shortcuts[SC_SHORTCUT_BACK].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_BACK].key = SDLK_B;
    sc_conf.shortcuts[SC_SHORTCUT_HOME].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_HOME].key = SDLK_H;
    sc_conf.shortcuts[SC_SHORTCUT_RECENTS].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_RECENTS].key = SDLK_S;
    sc_conf.shortcuts[SC_SHORTCUT_MENU].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_MENU].key = SDLK_M;
    sc_conf.shortcuts[SC_SHORTCUT_NOTIF].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_NOTIF].key = SDLK_N;
    sc_conf.shortcuts[SC_SHORTCUT_VOLUP].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_VOLUP].key = SDLK_UP;
    sc_conf.shortcuts[SC_SHORTCUT_VOLDOWN].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_VOLDOWN].key = SDLK_DOWN;
    sc_conf.shortcuts[SC_SHORTCUT_ROTATE].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_ROTATE].key = SDLK_R;
    sc_conf.shortcuts[SC_SHORTCUT_POWER].mod = a;
    sc_conf.shortcuts[SC_SHORTCUT_POWER].key = SDLK_P;
}

void
sc_shortcut_format(int idx, char *buf, size_t n) {
    if (idx < 0 || idx >= SC_SHORTCUT_COUNT || sc_conf.shortcuts[idx].key == 0) {
        if (n) {
            buf[0] = '\0';
        }
        return;
    }
    unsigned m = sc_conf.shortcuts[idx].mod;
    int k = sc_conf.shortcuts[idx].key;
    char keyname[12];
    if (k == SDLK_UP) {
        snprintf(keyname, sizeof(keyname), "Up");
    } else if (k == SDLK_DOWN) {
        snprintf(keyname, sizeof(keyname), "Down");
    } else if (k == SDLK_LEFT) {
        snprintf(keyname, sizeof(keyname), "Left");
    } else if (k == SDLK_RIGHT) {
        snprintf(keyname, sizeof(keyname), "Right");
    } else if (k == SDLK_SPACE) {
        snprintf(keyname, sizeof(keyname), "Space");
    } else if (k >= 32 && k < 127) {
        keyname[0] = (char) toupper(k);
        keyname[1] = '\0';
    } else {
        snprintf(keyname, sizeof(keyname), "?");
    }
    snprintf(buf, n, "%s%s%s%s%s",
             (m & SDL_KMOD_CTRL) ? "Ctrl+" : "",
             (m & SDL_KMOD_ALT) ? "Alt+" : "",
             (m & SDL_KMOD_SHIFT) ? "Shift+" : "",
             (m & SDL_KMOD_GUI) ? "Super+" : "", keyname);
}

static int
shortcut_index(const char *name) {
    if (!strcmp(name, "shell")) return SC_SHORTCUT_SHELL;
    if (!strcmp(name, "apps")) return SC_SHORTCUT_APPS;
    if (!strcmp(name, "log")) return SC_SHORTCUT_LOG;
    if (!strcmp(name, "screenshot")) return SC_SHORTCUT_SCREENSHOT;
    if (!strcmp(name, "record")) return SC_SHORTCUT_RECORD;
    if (!strcmp(name, "awake")) return SC_SHORTCUT_AWAKE;
    if (!strcmp(name, "pin")) return SC_SHORTCUT_PIN;
    if (!strcmp(name, "back")) return SC_SHORTCUT_BACK;
    if (!strcmp(name, "home")) return SC_SHORTCUT_HOME;
    if (!strcmp(name, "recents")) return SC_SHORTCUT_RECENTS;
    if (!strcmp(name, "menu")) return SC_SHORTCUT_MENU;
    if (!strcmp(name, "notifications")) return SC_SHORTCUT_NOTIF;
    if (!strcmp(name, "volup")) return SC_SHORTCUT_VOLUP;
    if (!strcmp(name, "voldown")) return SC_SHORTCUT_VOLDOWN;
    if (!strcmp(name, "rotate")) return SC_SHORTCUT_ROTATE;
    if (!strcmp(name, "power")) return SC_SHORTCUT_POWER;
    return -1;
}

static const char *DEFAULT_TEMPLATE =
    "# ==========================================================================\n"
    "#  scrapy configuration\n"
    "# --------------------------------------------------------------------------\n"
    "#  Lines starting with '#' are disabled; the values shown are the DEFAULTS.\n"
    "#  Remove the leading '#' from a line to change that setting, then relaunch.\n"
    "# ==========================================================================\n"
    "\n"
    "# ----- Toolbar -----------------------------------------------------------\n"
    "\n"
    "# Which buttons to show, comma-separated, in the order given.\n"
    "# Use \"none\" for no toolbar at all.\n"
    "# Names: pin, awake, shell, apps, log, screenshot, record, back, home,\n"
    "#        recents, menu, notifications, volup, voldown, rotate, power\n"
    "# buttons = pin,awake,shell,apps,log,screenshot,record,back,home,recents,menu,notifications,volup,voldown,rotate,power\n"
    "\n"
    "# Start with the window pinned always-on-top.\n"
    "# pin_on_top = false\n"
    "\n"
    "# ----- Drawers (terminal + apps) -----------------------------------------\n"
    "\n"
    "# Width (px) the window grows by when a drawer opens.\n"
    "# shell_width = 600\n"
    "# apps_width = 600\n"
    "# log_width = 760\n"
    "\n"
    "# Terminal font size multiplier (larger = bigger text).\n"
    "# terminal_text_size = 1.8\n"
    "\n"
    "# ----- Captures ----------------------------------------------------------\n"
    "\n"
    "# Folder for screenshots and recordings (default: your home folder).\n"
    "# capture_dir = C:\\Users\\You\\Pictures\n"
    "\n"
    "# ----- Notifications -----------------------------------------------------\n"
    "\n"
    "# Show a message at the bottom of the window after actions (install a file,\n"
    "# copy a file, screenshot, recording). Set to false to hide them all.\n"
    "# notifications = true\n"
    "\n"
    "# How long (seconds) a notification stays on screen.\n"
    "# notification_time = 7\n"
    "\n"
    "# Notification text size multiplier (larger = bigger text).\n"
    "# notification_text_size = 2.0\n"
    "\n"
    "# ----- Device ------------------------------------------------------------\n"
    "\n"
    "# Force a display density (dpi) on connect (0 = leave the device as-is).\n"
    "# default_density = 0\n"
    "\n"
    "# ----- Shortcuts ---------------------------------------------------------\n"
    "#\n"
    "# Combine alt / ctrl / shift / super with a key (a letter, or up/down/\n"
    "# left/right). Use \"none\" to disable a shortcut.\n"
    "\n"
    "# Custom buttons:\n"
    "# key_shell = alt+shift+t\n"
    "# key_apps = alt+a\n"
    "# key_log = alt+shift+l\n"
    "# key_screenshot = alt+shift+s\n"
    "# key_record = alt+shift+r\n"
    "# key_awake = alt+shift+k\n"
    "# key_pin = alt+shift+p\n"
    "\n"
    "# Device buttons (defaults match scrcpy):\n"
    "# key_back = alt+b\n"
    "# key_home = alt+h\n"
    "# key_recents = alt+s\n"
    "# key_menu = alt+m\n"
    "# key_notifications = alt+n\n"
    "# key_volup = alt+up\n"
    "# key_voldown = alt+down\n"
    "# key_rotate = alt+r\n"
    "# key_power = alt+p\n";

static void
config_path(char *buf, size_t n) {
#ifdef _WIN32
    char exe[1024];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len > 0 && len < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *(slash + 1) = '\0';
            snprintf(buf, n, "%sscrapy.conf", exe);
            return;
        }
    }
    snprintf(buf, n, "scrapy.conf");
#else
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.scrapy.conf", home && *home ? home : ".");
#endif
}

static void
write_default(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs(DEFAULT_TEMPLATE, f);
    fclose(f);
}

static void
trim(char *s) {
    // strip leading whitespace
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    // strip trailing whitespace / newline
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'
                       || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static bool
is_true(const char *v) {
    return !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes")
        || !strcmp(v, "on");
}

void
sc_config_load(void) {
    // defaults
    memset(&sc_conf, 0, sizeof(sc_conf));
    set_default_shortcuts();
    sc_conf.notifications = true; // on unless explicitly disabled

    char path[1024];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        write_default(path); // first run: leave a template to edit
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (!strcmp(key, "buttons")) {
            snprintf(sc_conf.buttons, sizeof(sc_conf.buttons), "%s", val);
            sc_conf.has_buttons = true;
        } else if (!strcmp(key, "shell_width")) {
            sc_conf.shell_width = atoi(val);
        } else if (!strcmp(key, "apps_width")) {
            sc_conf.apps_width = atoi(val);
        } else if (!strcmp(key, "log_width")) {
            sc_conf.log_width = atoi(val);
        } else if (!strcmp(key, "capture_dir")) {
            snprintf(sc_conf.capture_dir, sizeof(sc_conf.capture_dir), "%s",
                     val);
        } else if (!strcmp(key, "pin_on_top")) {
            sc_conf.pin_on_top = is_true(val);
        } else if (!strcmp(key, "default_density")) {
            sc_conf.default_density = atoi(val);
        } else if (!strcmp(key, "terminal_text_size")) {
            sc_conf.terminal_text_size = (float) atof(val);
        } else if (!strcmp(key, "notifications")) {
            sc_conf.notifications = is_true(val);
        } else if (!strcmp(key, "notification_time")) {
            sc_conf.notification_time = (float) atof(val);
        } else if (!strcmp(key, "notification_text_size")) {
            sc_conf.notification_text_size = (float) atof(val);
        } else if (!strncmp(key, "key_", 4)) {
            int idx = shortcut_index(key + 4);
            if (idx >= 0) {
                if (!strcmp(val, "none") || !strcmp(val, "off")) {
                    sc_conf.shortcuts[idx].mod = 0;
                    sc_conf.shortcuts[idx].key = 0;
                } else {
                    unsigned m;
                    int k;
                    if (parse_combo(val, &m, &k)) {
                        sc_conf.shortcuts[idx].mod = m;
                        sc_conf.shortcuts[idx].key = k;
                    }
                }
            }
        }
    }
    fclose(f);
}
