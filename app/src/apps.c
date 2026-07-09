#include "apps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include "adb/adb.h"
#include "userconf.h"
#include "events.h"
#include "font8x8_basic.h"
#include "logview.h"
#include "screen.h"
#include "settings.h"
#include "shell.h"
#include "util/log.h"
#include "util/process.h"

#define SC_APPS_MIN  320   // minimum drawer width (video-fit reservation)
#define SC_APPS_MAX  512
#define SC_APPS_ROWH 30    // list row height (logical px)
#define SC_APPS_TABY 12    // tab bar y (logical px)
#define SC_APPS_TABH 34    // tab bar height
#define SC_APPS_SEARCHY 56 // search box y (apps page)
#define SC_APPS_TOGGLEY 94 // system toggle y (apps page)
#define SC_APPS_TOP  128   // first list row y, below search + toggle
#define SC_APPS_STEP 40    // density adjustment step (dpi)

enum { SC_PAGE_APPS = 0, SC_PAGE_DENSITY = 1 };

// Width the window grows by when the drawer opens (config override, else 600).
static int
apps_target(void) {
    return sc_conf.apps_width > 0 ? sc_conf.apps_width : 600;
}

struct sc_apps {
    bool ready;
    char serial[128];

    bool open;
    float anim; // reserved width (0 or SC_APPS_W)
    int page;   // SC_PAGE_APPS or SC_PAGE_DENSITY

    sc_mutex mutex;
    char names[SC_APPS_MAX][128];
    int count;
    int filtered[SC_APPS_MAX]; // indices into names[] matching the search
    int fcount;
    char filter[64];
    bool search_focus; // the search box is selected for typing
    SDL_TimerID blink_timer; // repaints so the caret can blink
    bool show_system;
    bool loading;
    int scroll;      // first visible row
    int hover;       // hovered row index, or -1

    int dens_cur;    // effective density (0 = unknown)
    int dens_def;    // physical/default density
};

static struct sc_apps g;

// --- adb helpers ---

static const char *
apps_adb(void) {
    const char *adb = sc_adb_get_executable();
    if (!adb) {
        sc_adb_init();
        adb = sc_adb_get_executable();
    }
    return adb;
}

static void
wake(void) {
    sc_push_event(SC_EVENT_SHELL_UPDATE); // repaint
}

// Build argv: adb [-s serial] <tail...>. Caller passes a NULL-terminated tail.
static int
build_argv(const char **argv, int cap, const char *const *tail) {
    const char *adb = apps_adb();
    if (!adb) {
        return 0;
    }
    int n = 0;
    argv[n++] = adb;
    if (g.serial[0]) {
        argv[n++] = "-s";
        argv[n++] = g.serial;
    }
    for (int i = 0; tail[i] && n < cap - 1; ++i) {
        argv[n++] = tail[i];
    }
    argv[n] = NULL;
    return n;
}

static bool
run_adb(const char *const *tail) {
    const char *argv[24];
    if (!build_argv(argv, 24, tail)) {
        return false;
    }
    sc_pid pid;
    if (sc_process_execute_p(argv, &pid, 0, NULL, NULL, NULL)
            != SC_PROCESS_SUCCESS) {
        return false;
    }
    return sc_process_wait(pid, true) == 0;
}

static int
run_adb_capture(const char *const *tail, char *buf, int bufsize) {
    const char *argv[24];
    if (!build_argv(argv, 24, tail)) {
        return 0;
    }
    sc_pid pid;
    sc_pipe pout;
    if (sc_process_execute_p(argv, &pid, 0, NULL, &pout, NULL)
            != SC_PROCESS_SUCCESS) {
        return 0;
    }
    ssize_t r = sc_pipe_read_all(pout, buf, bufsize - 1);
    sc_pipe_close(pout);
    sc_process_wait(pid, true);
    if (r < 0) {
        r = 0;
    }
    buf[r] = '\0';
    return (int) r;
}

// --- data loading ---

static int
cmp_name(const void *a, const void *b) {
    return strcmp((const char *) a, (const char *) b);
}

static bool
ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) {
        return true;
    }
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i]
               && tolower((unsigned char) p[i]) == tolower((unsigned char)
                                                           needle[i])) {
            ++i;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

// Rebuild the filtered index list from the search string. Caller holds mutex.
static void
rebuild_filter(void) {
    g.fcount = 0;
    for (int i = 0; i < g.count; ++i) {
        if (ci_contains(g.names[i], g.filter)) {
            g.filtered[g.fcount++] = i;
        }
    }
}

static int
load_apps_thread(void *userdata) {
    (void) userdata;
    static char buf[1 << 17];
    const char *tail3[] = {"shell", "pm", "list", "packages", "-3", NULL};
    const char *tailall[] = {"shell", "pm", "list", "packages", NULL};
    run_adb_capture(g.show_system ? tailall : tail3, buf, sizeof(buf));

    sc_mutex_lock(&g.mutex);
    g.count = 0;
    char *line = buf;
    while (line && *line && g.count < SC_APPS_MAX) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *p = strstr(line, "package:");
        if (p) {
            p += 8;
            char *e = p;
            while (*e && *e != '\r' && *e != '\n' && *e != ' ') {
                ++e;
            }
            *e = '\0';
            if (*p) {
                snprintf(g.names[g.count], sizeof(g.names[0]), "%s", p);
                g.count++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    qsort(g.names, g.count, sizeof(g.names[0]), cmp_name);
    rebuild_filter();
    g.loading = false;
    sc_mutex_unlock(&g.mutex);
    wake();
    return 0;
}

static void
load_density(void) {
    char buf[1024];
    const char *tail[] = {"shell", "wm", "density", NULL};
    run_adb_capture(tail, buf, sizeof(buf));
    int phys = 0, over = 0;
    char *p = strstr(buf, "Physical density:");
    if (p) {
        phys = atoi(p + 17);
    }
    char *o = strstr(buf, "Override density:");
    if (o) {
        over = atoi(o + 17);
    }
    sc_mutex_lock(&g.mutex);
    g.dens_def = phys;
    g.dens_cur = over ? over : phys;
    sc_mutex_unlock(&g.mutex);
    wake();
}

static int
load_density_thread(void *userdata) {
    (void) userdata;
    load_density();
    return 0;
}

static int
set_density_thread(void *userdata) {
    int val = (int) (intptr_t) userdata; // 0 => reset
    if (val <= 0) {
        const char *t[] = {"shell", "wm", "density", "reset", NULL};
        run_adb(t);
    } else {
        char sval[16];
        snprintf(sval, sizeof(sval), "%d", val);
        const char *t[] = {"shell", "wm", "density", sval, NULL};
        run_adb(t);
    }
    load_density(); // refresh the shown value
    return 0;
}

static int
launch_thread(void *userdata) {
    char *pkg = userdata;
    const char *t[] = {"shell", "monkey", "-p", pkg, "-c",
                       "android.intent.category.LAUNCHER", "1", NULL};
    run_adb(t);
    free(pkg);
    return 0;
}

// Open the app in a fresh scrcpy window on a new virtual display.
static int
newdisplay_thread(void *userdata) {
    char *pkg = userdata;
    char self[1024];
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, self, sizeof(self))) {
        snprintf(self, sizeof(self), "scrcpy");
    }
#else
    snprintf(self, sizeof(self), "scrcpy");
#endif
    char startapp[192];
    snprintf(startapp, sizeof(startapp), "--start-app=+%s", pkg);
    const char *argv[10];
    int n = 0;
    argv[n++] = self;
    if (g.serial[0]) {
        argv[n++] = "-s";
        argv[n++] = g.serial;
    }
    argv[n++] = "--new-display";
    argv[n++] = startapp;
    argv[n] = NULL;
    sc_pid pid;
    if (sc_process_execute_p(argv, &pid, 0, NULL, NULL, NULL)
            == SC_PROCESS_SUCCESS) {
        sc_process_wait(pid, true); // reap when that window closes
    }
    free(pkg);
    return 0;
}

static void
detach(int (*fn)(void *), const char *name, void *data) {
    SDL_Thread *t = SDL_CreateThread(fn, name, data);
    if (t) {
        SDL_DetachThread(t);
    }
}

// Fires ~twice a second while the drawer is open so the render loop repaints
// and the search caret blinks even when the mirror is static.
static Uint32 SDLCALL
blink_cb(void *userdata, SDL_TimerID id, Uint32 interval) {
    (void) userdata;
    (void) id;
    (void) interval;
    if (!g.open) {
        return 0; // stop the timer
    }
    sc_push_event(SC_EVENT_SHELL_UPDATE);
    return 500;
}

// --- open / close ---

void
sc_apps_toggle(struct sc_screen *screen) {
    if (!g.ready) {
        return;
    }
    if (g.open) {
        sc_apps_close(screen);
        return;
    }
    if (sc_shell_is_open()) {
        sc_shell_close(screen); // share the right-side region
    }
    if (sc_logview_is_open()) {
        sc_logview_close(screen);
    }
    if (sc_settings_is_open()) {
        sc_settings_close(screen);
    }
    g.open = true;
    g.scroll = 0;
    g.hover = -1;
    g.page = SC_PAGE_APPS;
    g.search_focus = true; // ready to type immediately
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    SDL_SetWindowAspectRatio(screen->window, 0.f, 0.f);
    g.anim = SC_APPS_MIN;
    SDL_SetWindowSize(screen->window, w + apps_target(), h);
    SDL_StartTextInput(screen->window); // for the app search box
    g.blink_timer = SDL_AddTimer(500, blink_cb, NULL);

    sc_mutex_lock(&g.mutex);
    g.loading = true;
    sc_mutex_unlock(&g.mutex);
    detach(load_apps_thread, "sc-apps", NULL);
    detach(load_density_thread, "sc-dens", NULL);
}

void
sc_apps_close(struct sc_screen *screen) {
    if (!g.open) {
        return;
    }
    g.open = false;
    g.anim = 0;
    if (g.blink_timer) {
        SDL_RemoveTimer(g.blink_timer);
        g.blink_timer = 0;
    }
    SDL_StopTextInput(screen->window);
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    int nw = w - apps_target();
    SDL_SetWindowSize(screen->window, nw < 200 ? 200 : nw, h);
}

bool
sc_apps_is_open(void) {
    return g.open;
}

int
sc_apps_reserved_width(struct sc_screen *screen) {
    (void) screen;
    // Only a minimum is reserved for the video fit; the drawer renders from the
    // video's right edge to the window edge, growing with the window.
    return g.open ? SC_APPS_MIN : 0;
}

// Left edge of the drawer, in logical px: the right edge of the video.
static float
apps_x0(struct sc_screen *screen) {
    return screen->rect.x + screen->rect.w;
}

// --- layout (logical coordinates) ---

// Page tabs: id 0 = Apps, 1 = Density.
static void
tab_rect(struct sc_screen *screen, int id, SDL_FRect *r) {
    float x0 = apps_x0(screen);
    *r = (SDL_FRect){x0 + 14 + id * 136, SC_APPS_TABY, 128, SC_APPS_TABH};
}

static void
search_rect(struct sc_screen *screen, SDL_FRect *r) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) h;
    float x0 = apps_x0(screen);
    *r = (SDL_FRect){x0 + 14, SC_APPS_SEARCHY, (w - 14) - (x0 + 14), 32};
}

static void
toggle_rect(struct sc_screen *screen, SDL_FRect *r) {
    float x0 = apps_x0(screen);
    *r = (SDL_FRect){x0 + 14, SC_APPS_TOGGLEY, 26, 26};
}

// Density buttons (on the Density page): id 0 = minus, 1 = plus, 2 = reset.
static void
dens_btn_rect(struct sc_screen *screen, int id, SDL_FRect *r) {
    float x0 = apps_x0(screen);
    float y = 124;
    if (id == 0) {
        *r = (SDL_FRect){x0 + 14, y, 48, 38};
    } else if (id == 1) {
        *r = (SDL_FRect){x0 + 70, y, 48, 38};
    } else {
        *r = (SDL_FRect){x0 + 130, y, 96, 38};
    }
}

static int
visible_rows(struct sc_screen *screen) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
    int n = (int) ((h - SC_APPS_TOP - 10) / SC_APPS_ROWH);
    return n < 1 ? 1 : n;
}

// --- rendering ---

static void
draw_text(SDL_Renderer *r, float x, float y, float px, const char *s,
          Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (int i = 0; s[i]; ++i) {
        unsigned char ch = (unsigned char) s[i];
        if (ch >= 128) {
            ch = '?';
        }
        const char *gl = font8x8_basic[ch];
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                if (gl[row] & (1 << col)) {
                    SDL_FRect p = {x + (i * 8 + col) * px, y + row * px, px, px};
                    SDL_RenderFillRect(r, &p);
                }
            }
        }
    }
}

static void
draw_button(SDL_Renderer *r, const SDL_FRect *rc, float scale, float px,
            const char *label, bool hover) {
    SDL_FRect box = {rc->x * scale, rc->y * scale, rc->w * scale,
                     rc->h * scale};
    SDL_SetRenderDrawColor(r, hover ? 60 : 46, hover ? 62 : 48, hover ? 72 : 56,
                           255);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 90, 92, 100, 255);
    SDL_RenderRect(r, &box);
    float tw = strlen(label) * 8 * px;
    draw_text(r, box.x + (box.w - tw) / 2, box.y + (box.h - 8 * px) / 2, px,
              label, 232, 233, 238);
}

void
sc_apps_render(struct sc_screen *screen) {
    if (!g.open) {
        return;
    }
    SDL_Renderer *renderer = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    float x0 = apps_x0(screen);
    float pw = (float) w - x0;
    if (pw < 1) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect panel = {x0 * scale, 0, pw * scale, (float) h * scale};
    SDL_SetRenderDrawColor(renderer, 18, 19, 23, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 70, 72, 80, 255);
    SDL_RenderLine(renderer, x0 * scale, 0, x0 * scale, (float) h * scale);

    SDL_Rect clip = {(int) (x0 * scale), 0, (int) (pw * scale),
                     (int) (h * scale)};
    SDL_SetRenderClipRect(renderer, &clip);

    float px = SDL_max(2.f, roundf(1.6f * scale));
    float sx = (x0 + 14) * scale;

    float mx = -1, my = -1;
    SDL_GetMouseState(&mx, &my);

    char line[96];
    sc_mutex_lock(&g.mutex);
    int cur = g.dens_cur, def = g.dens_def;
    bool loading = g.loading;
    int count = g.count;
    sc_mutex_unlock(&g.mutex);

    // --- page tabs ---
    const char *tabs[] = {"APPS", "DENSITY"};
    for (int i = 0; i < 2; ++i) {
        SDL_FRect rc;
        tab_rect(screen, i, &rc);
        bool active = g.page == i;
        bool hover = mx >= rc.x && mx < rc.x + rc.w && my >= rc.y
                  && my < rc.y + rc.h;
        SDL_FRect box = {rc.x * scale, rc.y * scale, rc.w * scale,
                         rc.h * scale};
        SDL_SetRenderDrawColor(renderer, active ? 40 : 26, active ? 90 : 27,
                               active ? 150 : 32, 255);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, hover ? 120 : 70, 92, 100, 255);
        SDL_RenderRect(renderer, &box);
        float tw = strlen(tabs[i]) * 8 * px;
        draw_text(renderer, box.x + (box.w - tw) / 2,
                  box.y + (box.h - 8 * px) / 2, px, tabs[i],
                  active ? 245 : 190, active ? 246 : 192, active ? 250 : 200);
    }

    if (g.page == SC_PAGE_DENSITY) {
        // --- density page ---
        draw_text(renderer, sx, 66 * scale, px, "DISPLAY DENSITY", 150, 190,
                  235);
        if (cur > 0) {
            if (def > 0 && cur != def) {
                snprintf(line, sizeof(line), "%d dpi   (default %d)", cur, def);
            } else {
                snprintf(line, sizeof(line), "%d dpi", cur);
            }
        } else {
            snprintf(line, sizeof(line), "reading...");
        }
        draw_text(renderer, sx, 94 * scale, px, line, 220, 222, 228);

        const char *labels[] = {"-", "+", "Reset"};
        for (int i = 0; i < 3; ++i) {
            SDL_FRect rc;
            dens_btn_rect(screen, i, &rc);
            bool hover = mx >= rc.x && mx < rc.x + rc.w && my >= rc.y
                      && my < rc.y + rc.h;
            draw_button(renderer, &rc, scale, px, labels[i], hover);
        }
        draw_text(renderer, sx, 178 * scale, roundf(px * 0.85f),
                  "Lower dpi = more fits on screen.", 140, 142, 150);
    } else {
        // --- apps page: search box ---
        SDL_FRect sr;
        search_rect(screen, &sr);
        SDL_FRect sbox = {sr.x * scale, sr.y * scale, sr.w * scale,
                          sr.h * scale};
        SDL_SetRenderDrawColor(renderer, g.search_focus ? 34 : 30,
                               g.search_focus ? 38 : 32,
                               g.search_focus ? 48 : 38, 255);
        SDL_RenderFillRect(renderer, &sbox);
        // Focused: a bright blue border makes it obvious you're typing here.
        if (g.search_focus) {
            SDL_SetRenderDrawColor(renderer, 60, 130, 210, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 90, 92, 100, 255);
        }
        SDL_RenderRect(renderer, &sbox);
        float tx = sbox.x + 8 * scale;
        float ty = sbox.y + 9 * scale;
        if (g.filter[0]) {
            draw_text(renderer, tx, ty, px, g.filter, 235, 236, 240);
        } else {
            draw_text(renderer, tx, ty, px,
                      g.search_focus ? "Type to search apps..."
                                     : "Click to search apps...",
                      120, 122, 130);
        }
        if (g.search_focus && (SDL_GetTicks() / 500) % 2 == 0) {
            // Blinking caret after the typed text (or at the start when empty).
            float cx = tx + strlen(g.filter) * 8 * px;
            SDL_FRect caret = {cx, sbox.y + 7 * scale, 2 * scale, 8 * px};
            SDL_SetRenderDrawColor(renderer, 150, 220, 150, 220);
            SDL_RenderFillRect(renderer, &caret);
        }

        // system-apps toggle
        SDL_FRect tr;
        toggle_rect(screen, &tr);
        SDL_FRect tbox = {tr.x * scale, tr.y * scale, tr.w * scale,
                          tr.h * scale};
        SDL_SetRenderDrawColor(renderer, g.show_system ? 40 : 30,
                               g.show_system ? 90 : 32, g.show_system ? 150 : 38,
                               255);
        SDL_RenderFillRect(renderer, &tbox);
        SDL_SetRenderDrawColor(renderer, 90, 92, 100, 255);
        SDL_RenderRect(renderer, &tbox);
        if (g.show_system) {
            draw_text(renderer, tbox.x + 6 * scale, tbox.y + 6 * scale, px, "X",
                      240, 240, 245);
        }
        draw_text(renderer, (tr.x + 34) * scale, (tr.y + 6) * scale, px,
                  loading ? "loading..." : "Show system apps", 200, 202, 210);
        draw_text(renderer, sx, 120 * scale, roundf(px * 0.72f),
                  "click = launch    right-click = new display", 130, 132, 140);

        // --- app list (filtered) ---
        int vis = visible_rows(screen);
        sc_mutex_lock(&g.mutex);
        int fcount = g.fcount;
        int maxscroll = fcount - vis;
        if (maxscroll < 0) {
            maxscroll = 0;
        }
        if (g.scroll > maxscroll) {
            g.scroll = maxscroll;
        }
        if (g.scroll < 0) {
            g.scroll = 0;
        }
        for (int i = 0; i < vis; ++i) {
            int fi = g.scroll + i;
            if (fi >= g.fcount) {
                break;
            }
            const char *name = g.names[g.filtered[fi]];
            float ry = SC_APPS_TOP + i * SC_APPS_ROWH;
            bool hover = mx >= x0 && my >= ry && my < ry + SC_APPS_ROWH;
            if (hover) {
                SDL_FRect hr = {x0 * scale, ry * scale, pw * scale,
                                SC_APPS_ROWH * scale};
                SDL_SetRenderDrawColor(renderer, 40, 44, 54, 255);
                SDL_RenderFillRect(renderer, &hr);
            }
            draw_text(renderer, sx, (ry + 6) * scale, px, name,
                      hover ? 235 : 205, hover ? 236 : 207, hover ? 240 : 212);
        }
        sc_mutex_unlock(&g.mutex);
        (void) count;
    }

    SDL_SetRenderClipRect(renderer, NULL);
}

// --- input ---

bool
sc_apps_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    if (!g.open) {
        return false;
    }
    float x0 = apps_x0(screen);

    switch (event->type) {
        case SDL_EVENT_TEXT_INPUT:
            // Type into the app search box (when it is focused).
            if (g.page == SC_PAGE_APPS && g.search_focus) {
                size_t len = strlen(g.filter);
                const char *t = event->text.text;
                while (*t && len < sizeof(g.filter) - 1) {
                    g.filter[len++] = *t++;
                }
                g.filter[len] = '\0';
                sc_mutex_lock(&g.mutex);
                rebuild_filter();
                sc_mutex_unlock(&g.mutex);
                g.scroll = 0;
                wake();
            }
            return true;
        case SDL_EVENT_KEY_DOWN:
            if (event->key.key == SDLK_ESCAPE) {
                sc_apps_close(screen);
                return true;
            }
            if (event->key.key == SDLK_BACKSPACE && g.page == SC_PAGE_APPS
                    && g.search_focus) {
                size_t len = strlen(g.filter);
                if (len > 0) {
                    g.filter[len - 1] = '\0';
                    sc_mutex_lock(&g.mutex);
                    rebuild_filter();
                    sc_mutex_unlock(&g.mutex);
                    g.scroll = 0;
                    wake();
                }
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_WHEEL: {
            float mx, my;
            SDL_GetMouseState(&mx, &my);
            if (mx >= x0 && g.page == SC_PAGE_APPS) {
                g.scroll -= (int) (event->wheel.y * 3);
                if (g.scroll < 0) {
                    g.scroll = 0;
                }
                wake();
                return true;
            }
            return mx >= x0;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            bool left = event->button.button == SDL_BUTTON_LEFT;
            bool right = event->button.button == SDL_BUTTON_RIGHT;
            if (!left && !right) {
                return false;
            }
            float bx = event->button.x, by = event->button.y;
            if (bx < x0) {
                return false; // click landed on the video, not the drawer
            }
            if (left) {
                // Page tabs.
                for (int i = 0; i < 2; ++i) {
                    SDL_FRect rc;
                    tab_rect(screen, i, &rc);
                    if (bx >= rc.x && bx < rc.x + rc.w && by >= rc.y
                            && by < rc.y + rc.h) {
                        g.page = i;
                        g.search_focus = (i == SC_PAGE_APPS);
                        wake();
                        return true;
                    }
                }
            }
            if (g.page == SC_PAGE_DENSITY) {
                if (left) {
                    for (int i = 0; i < 3; ++i) {
                        SDL_FRect rc;
                        dens_btn_rect(screen, i, &rc);
                        if (bx >= rc.x && bx < rc.x + rc.w && by >= rc.y
                                && by < rc.y + rc.h) {
                            int target = 0; // reset
                            if (i == 0 && g.dens_cur > 120) {
                                target = g.dens_cur - SC_APPS_STEP;
                            } else if (i == 1) {
                                target = (g.dens_cur > 0 ? g.dens_cur
                                                         : g.dens_def)
                                       + SC_APPS_STEP;
                            }
                            detach(set_density_thread, "sc-setdens",
                                   (void *) (intptr_t) target);
                            return true;
                        }
                    }
                }
                return true; // consume clicks on the density page
            }
            // Apps page.
            if (left) {
                // Search box: clicking it focuses for typing; clicking anywhere
                // else on the page unfocuses it.
                SDL_FRect sr;
                search_rect(screen, &sr);
                bool inbox = bx >= sr.x && bx < sr.x + sr.w && by >= sr.y
                          && by < sr.y + sr.h;
                g.search_focus = inbox;
                if (inbox) {
                    wake();
                    return true;
                }
                // System-apps toggle.
                SDL_FRect tr;
                toggle_rect(screen, &tr);
                if (bx >= tr.x && bx < tr.x + 220 && by >= tr.y
                        && by < tr.y + tr.h) {
                    g.show_system = !g.show_system;
                    sc_mutex_lock(&g.mutex);
                    g.loading = true;
                    sc_mutex_unlock(&g.mutex);
                    detach(load_apps_thread, "sc-apps", NULL);
                    wake();
                    return true;
                }
            }
            // App rows: left = launch, right = open on a new display.
            int vis = visible_rows(screen);
            for (int i = 0; i < vis; ++i) {
                float ry = SC_APPS_TOP + i * SC_APPS_ROWH;
                if (by >= ry && by < ry + SC_APPS_ROWH) {
                    int fi = g.scroll + i;
                    sc_mutex_lock(&g.mutex);
                    char *pkg = (fi < g.fcount)
                              ? strdup(g.names[g.filtered[fi]]) : NULL;
                    sc_mutex_unlock(&g.mutex);
                    if (pkg) {
                        detach(right ? newdisplay_thread : launch_thread,
                               right ? "sc-newdisp" : "sc-launch", pkg);
                    }
                    return true;
                }
            }
            return true; // consume clicks anywhere in the drawer
        }
        default:
            return false;
    }
}

// --- lifecycle ---

void
sc_apps_init(const char *serial) {
    memset(&g, 0, sizeof(g));
    if (serial) {
        snprintf(g.serial, sizeof(g.serial), "%s", serial);
    }
    g.hover = -1;
    sc_mutex_init(&g.mutex);
    g.ready = true;

    // Apply a configured default display density on connect.
    if (sc_conf.default_density > 0) {
        detach(set_density_thread, "sc-defdens",
               (void *) (intptr_t) sc_conf.default_density);
    }
}

void
sc_apps_destroy(void) {
    if (!g.ready) {
        return;
    }
    sc_mutex_destroy(&g.mutex);
    g.ready = false;
}
