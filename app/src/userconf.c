#include "userconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#endif

struct sc_config sc_conf;

static const char *DEFAULT_TEMPLATE =
    "# scrapy configuration. Remove the leading '#' to enable a setting.\n"
    "\n"
    "# Toolbar buttons to show, comma-separated, in the order given.\n"
    "# Use \"none\" for no buttons at all.\n"
    "# Available: pin, awake, shell, apps, screenshot, record, back, home,\n"
    "#            recents, menu, notifications, volup, voldown, rotate, power\n"
    "# buttons = pin,awake,shell,apps,screenshot,record,back,home,recents,menu,notifications,volup,voldown,rotate,power\n"
    "\n"
    "# Width (px) the window grows by when a drawer opens.\n"
    "# shell_width = 600\n"
    "# apps_width = 600\n"
    "\n"
    "# Folder for screenshots and recordings (default: your home folder).\n"
    "# capture_dir = C:\\Users\\You\\Pictures\n"
    "\n"
    "# Start with the window pinned always-on-top (true/false).\n"
    "# pin_on_top = false\n"
    "\n"
    "# Force a display density (dpi) on connect (0 = leave the device as-is).\n"
    "# default_density = 0\n";

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
        } else if (!strcmp(key, "capture_dir")) {
            snprintf(sc_conf.capture_dir, sizeof(sc_conf.capture_dir), "%s",
                     val);
        } else if (!strcmp(key, "pin_on_top")) {
            sc_conf.pin_on_top = is_true(val);
        } else if (!strcmp(key, "default_density")) {
            sc_conf.default_density = atoi(val);
        }
    }
    fclose(f);
}
