#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "apps.h"
#include "userconf.h"
#include "events.h"
#include "font8x8_basic.h"
#include "logview.h"
#include "restart.h"
#include "screen.h"
#include "shell.h"
#include "toolbar.h"

#define SC_SET_MIN   320  // minimum reserved width for the video fit
#define SC_SET_W     640  // width the window grows by when the drawer opens
#define SC_SET_TABY  12
#define SC_SET_TABH  38
#define SC_SET_TOP   64   // first content row y (below the tab bar)
#define SC_SET_ROWH  42   // button-list row height (checkbox + label inline)
#define SC_SET_RINLI 48   // inline field row (label left, small input right)
#define SC_SET_FROWH 82   // wide field row (label above, full-width input below)
#define SC_SET_SAVEH 52   // save-bar height at the bottom
#define SC_SET_SBARW 8    // scrollbar width

// Built-in defaults (mirror the values used elsewhere), shown pre-filled in the
// fields so the user sees the effective value instead of a blank box.
#define DEF_SHELL_W 600
#define DEF_APPS_W  600
#define DEF_LOG_W   760
#define DEF_TERM_SZ 1.8f
#define DEF_NTIME   7.0f
#define DEF_NSIZE   2.0f
#define DEF_SCROLLBACK 5000

enum { TAB_BUTTONS = 0, TAB_SHELL = 1, TAB_MISC = 2, TAB_COUNT = 3 };
enum { FK_BOOL, FK_TEXT };

static struct sc_settings {
    bool ready;
    bool open;
    int tab;
    int focus;   // index into FIELDS of the focused text box, or -1
    int caret;   // caret position within the focused text field
    int sel_a;   // selection anchor (-1 = none); selection is [sel_a, caret]
    int fld_off; // horizontal scroll (first visible char) of the focused field
    bool field_drag; // mouse is dragging out a selection in the focused field
    int scroll;  // row offset for the current tab

    bool btn_vis[64];
    int nbtn;

    char t_shell_w[16];
    char t_term_sz[16];
    char t_scrollback[16];
    char t_apps_w[16];
    char t_log_w[16];
    char t_ntime[16];
    char t_nsize[16];
    char t_density[16];
    char t_capdir[512];
    char t_logpath[512];
    bool b_notif;
    bool b_pin;
    bool b_logfile;
} s;

// Editable field descriptors for the Shell (tab 1) and Misc (tab 2) tabs.
struct sfield {
    int tab;
    const char *label;
    int kind;
    void *ptr;   // &bool (FK_BOOL) or char[] (FK_TEXT)
    int cap;
    bool numeric;
    bool wide;   // true: label above a full-width input (paths); else inline
};

static const struct sfield FIELDS[] = {
    //  tab         label                          kind      ptr           cap                 numeric wide
    {TAB_SHELL, "Terminal drawer width (px)",  FK_TEXT, s.t_shell_w, sizeof s.t_shell_w, true,  false},
    {TAB_SHELL, "Terminal text size (x)",      FK_TEXT, s.t_term_sz, sizeof s.t_term_sz, true,  false},
    {TAB_SHELL, "Scrollback (lines)",          FK_TEXT, s.t_scrollback, sizeof s.t_scrollback, true, false},
    {TAB_SHELL, "Save shell output to file",   FK_BOOL, &s.b_logfile, 0,                 false, false},
    {TAB_SHELL, "Shell log file",              FK_TEXT, s.t_logpath, sizeof s.t_logpath, false, true},

    {TAB_MISC,  "Show notifications",          FK_BOOL, &s.b_notif,  0,                  false, false},
    {TAB_MISC,  "Notification time (s)",       FK_TEXT, s.t_ntime,   sizeof s.t_ntime,   true,  false},
    {TAB_MISC,  "Notification text size (x)",  FK_TEXT, s.t_nsize,   sizeof s.t_nsize,   true,  false},
    {TAB_MISC,  "Pin on top at start",         FK_BOOL, &s.b_pin,    0,                  false, false},
    {TAB_MISC,  "Apps drawer width (px)",      FK_TEXT, s.t_apps_w,  sizeof s.t_apps_w,  true,  false},
    {TAB_MISC,  "Log drawer width (px)",       FK_TEXT, s.t_log_w,   sizeof s.t_log_w,   true,  false},
    {TAB_MISC,  "Default density (dpi, 0=off)",FK_TEXT, s.t_density, sizeof s.t_density, true,  false},
    {TAB_MISC,  "Capture folder",              FK_TEXT, s.t_capdir,  sizeof s.t_capdir,  false, true},
};
#define NFIELDS ((int) (sizeof(FIELDS) / sizeof(FIELDS[0])))

static const char *TAB_NAMES[TAB_COUNT] = {"Buttons", "Shell", "Misc"};

void
sc_settings_init(void) {
    s.ready = true;
    s.focus = -1;
    s.sel_a = -1;
}

// Selection bounds of the focused field (lo <= hi). Returns false if empty.
static bool
fld_sel_range(int *lo, int *hi) {
    if (s.sel_a < 0 || s.sel_a == s.caret) {
        return false;
    }
    *lo = s.sel_a < s.caret ? s.sel_a : s.caret;
    *hi = s.sel_a < s.caret ? s.caret : s.sel_a;
    return true;
}

// Delete the current selection from `t` (updating *len and the caret). Returns
// true if something was deleted.
static bool
fld_delete_sel(char *t, int *len) {
    int lo, hi;
    if (!fld_sel_range(&lo, &hi)) {
        return false;
    }
    memmove(t + lo, t + hi, (size_t) (*len - hi + 1)); // include the '\0'
    *len -= hi - lo;
    s.caret = lo;
    s.sel_a = -1;
    return true;
}

// --- config <-> working state ---

static void
num_to_buf(char *buf, int n, int v) {
    if (v > 0) {
        snprintf(buf, n, "%d", v);
    } else {
        buf[0] = '\0';
    }
}

static void
flt_to_buf(char *buf, int n, float v) {
    if (v > 0) {
        snprintf(buf, n, "%g", v);
    } else {
        buf[0] = '\0';
    }
}

// Is `name` currently in the visible-button set?
static bool
button_visible(const char *name) {
    if (!sc_conf.has_buttons) {
        return true; // absent config = all buttons
    }
    char list[512];
    snprintf(list, sizeof(list), "%s", sc_conf.buttons);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') {
            ++tok;
        }
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (!strcmp(tok, name)) {
            return true;
        }
    }
    return false;
}

static void
load_from_conf(void) {
    // Show the effective value: the configured one, or the built-in default.
    num_to_buf(s.t_shell_w, sizeof s.t_shell_w,
               sc_conf.shell_width > 0 ? sc_conf.shell_width : DEF_SHELL_W);
    flt_to_buf(s.t_term_sz, sizeof s.t_term_sz,
               sc_conf.terminal_text_size > 0 ? sc_conf.terminal_text_size
                                              : DEF_TERM_SZ);
    num_to_buf(s.t_scrollback, sizeof s.t_scrollback,
               sc_conf.shell_scrollback > 0 ? sc_conf.shell_scrollback
                                            : DEF_SCROLLBACK);
    num_to_buf(s.t_apps_w, sizeof s.t_apps_w,
               sc_conf.apps_width > 0 ? sc_conf.apps_width : DEF_APPS_W);
    num_to_buf(s.t_log_w, sizeof s.t_log_w,
               sc_conf.log_width > 0 ? sc_conf.log_width : DEF_LOG_W);
    flt_to_buf(s.t_ntime, sizeof s.t_ntime,
               sc_conf.notification_time > 0 ? sc_conf.notification_time
                                             : DEF_NTIME);
    flt_to_buf(s.t_nsize, sizeof s.t_nsize,
               sc_conf.notification_text_size > 0 ? sc_conf.notification_text_size
                                                  : DEF_NSIZE);
    // Density: 0 is meaningful ("off"), so always show the number.
    snprintf(s.t_density, sizeof s.t_density, "%d", sc_conf.default_density);
    snprintf(s.t_capdir, sizeof s.t_capdir, "%s", sc_conf.capture_dir);
    s.b_notif = sc_conf.notifications;
    s.b_pin = sc_conf.pin_on_top;
    s.b_logfile = sc_conf.shell_log_to_file;
    // Pre-fill the log path with the configured value, or the default location.
    if (sc_conf.shell_log_path[0]) {
        snprintf(s.t_logpath, sizeof s.t_logpath, "%s", sc_conf.shell_log_path);
    } else {
        sc_shell_default_log_path(s.t_logpath, sizeof s.t_logpath);
    }

    s.nbtn = sc_toolbar_all_count();
    if (s.nbtn > (int) (sizeof(s.btn_vis) / sizeof(s.btn_vis[0]))) {
        s.nbtn = (int) (sizeof(s.btn_vis) / sizeof(s.btn_vis[0]));
    }
    for (int i = 0; i < s.nbtn; ++i) {
        s.btn_vis[i] = button_visible(sc_toolbar_all_name(i));
    }
}

static void
apply_to_conf(void) {
    sc_conf.shell_width = atoi(s.t_shell_w);
    sc_conf.terminal_text_size = (float) atof(s.t_term_sz);
    sc_conf.shell_scrollback = atoi(s.t_scrollback);
    sc_conf.apps_width = atoi(s.t_apps_w);
    sc_conf.log_width = atoi(s.t_log_w);
    sc_conf.notification_time = (float) atof(s.t_ntime);
    sc_conf.notification_text_size = (float) atof(s.t_nsize);
    sc_conf.default_density = atoi(s.t_density);
    snprintf(sc_conf.capture_dir, sizeof sc_conf.capture_dir, "%s", s.t_capdir);
    sc_conf.notifications = s.b_notif;
    sc_conf.pin_on_top = s.b_pin;
    sc_conf.shell_log_to_file = s.b_logfile;
    // Store the path only if it differs from the default, so "default" keeps
    // following the app location instead of freezing an absolute path.
    char defpath[512];
    sc_shell_default_log_path(defpath, sizeof defpath);
    if (strcmp(s.t_logpath, defpath) == 0) {
        sc_conf.shell_log_path[0] = '\0';
    } else {
        snprintf(sc_conf.shell_log_path, sizeof sc_conf.shell_log_path, "%s",
                 s.t_logpath);
    }

    char buf[512];
    int pos = 0;
    for (int i = 0; i < s.nbtn; ++i) {
        if (!s.btn_vis[i]) {
            continue;
        }
        const char *nm = sc_toolbar_all_name(i);
        int need = (int) strlen(nm) + (pos ? 1 : 0);
        if (pos + need >= (int) sizeof(buf)) {
            break;
        }
        if (pos) {
            buf[pos++] = ',';
        }
        memcpy(buf + pos, nm, strlen(nm));
        pos += (int) strlen(nm);
    }
    buf[pos] = '\0';
    sc_conf.has_buttons = true;
    snprintf(sc_conf.buttons, sizeof sc_conf.buttons, "%s",
             pos ? buf : "none");
}

static void
do_save(void) {
    apply_to_conf();
    sc_config_save();
    sc_restart_request();
    SDL_Event q;
    memset(&q, 0, sizeof(q));
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q); // unwind the session; the main loop then relaunches
}

// --- open / close ---

void
sc_settings_toggle(struct sc_screen *screen) {
    if (!s.ready) {
        return;
    }
    if (!s.open) {
        if (sc_shell_is_open()) {
            sc_shell_close(screen);
        }
        if (sc_apps_is_open()) {
            sc_apps_close(screen);
        }
        if (sc_logview_is_open()) {
            sc_logview_close(screen);
        }
    }
    s.open = !s.open;
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    if (s.open) {
        SDL_SetWindowAspectRatio(screen->window, 0.f, 0.f);
        s.tab = TAB_BUTTONS;
        s.focus = -1;
        s.scroll = 0;
        load_from_conf();
        SDL_SetWindowSize(screen->window, w + SC_SET_W, h);
        SDL_StartTextInput(screen->window);
    } else {
        int nw = w - SC_SET_W;
        SDL_SetWindowSize(screen->window, nw < 200 ? 200 : nw, h);
        SDL_StopTextInput(screen->window);
    }
}

void
sc_settings_close(struct sc_screen *screen) {
    if (s.open) {
        sc_settings_toggle(screen);
    }
}

bool
sc_settings_is_open(void) {
    return s.open;
}

bool
sc_settings_step_anim(void) {
    return false;
}

int
sc_settings_reserved_width(struct sc_screen *screen) {
    (void) screen;
    return s.open ? SC_SET_MIN : 0;
}

// --- layout (logical coordinates) ---

static float
set_x0(struct sc_screen *screen) {
    return screen->rect.x + screen->rect.w;
}

static void
tab_rect(struct sc_screen *screen, int i, SDL_FRect *r) {
    float x0 = set_x0(screen);
    *r = (SDL_FRect){x0 + 14 + i * 138, SC_SET_TABY, 130, SC_SET_TABH};
}

// Row count on the current tab.
static int
tab_rows(void) {
    if (s.tab == TAB_BUTTONS) {
        return s.nbtn;
    }
    int n = 0;
    for (int i = 0; i < NFIELDS; ++i) {
        if (FIELDS[i].tab == s.tab) {
            ++n;
        }
    }
    return n;
}

// FIELDS index of the n-th row on the current (Shell/Misc) tab.
static int
field_of_row(int visidx) {
    int n = 0;
    for (int i = 0; i < NFIELDS; ++i) {
        if (FIELDS[i].tab != s.tab) {
            continue;
        }
        if (n == visidx) {
            return i;
        }
        ++n;
    }
    return -1;
}

// Height of tab row `ridx` (logical). Buttons and inline fields are compact; a
// "wide" field (a path) is taller because its input sits below the label.
// Sets *out_field to the FIELDS index (or -1 for a button row).
static int
row_height(int ridx, int *out_field) {
    if (s.tab == TAB_BUTTONS) {
        if (out_field) {
            *out_field = -1;
        }
        return SC_SET_ROWH;
    }
    int fi = field_of_row(ridx);
    if (out_field) {
        *out_field = fi;
    }
    return (fi >= 0 && FIELDS[fi].wide) ? SC_SET_FROWH : SC_SET_RINLI;
}

static float
content_top(void) {
    return SC_SET_TOP;
}

static float
content_bottom(struct sc_screen *screen) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
    return (float) h - SC_SET_SAVEH - 8;
}

static float
total_content_h(void) {
    int rows = tab_rows();
    float t = 0;
    for (int i = 0; i < rows; ++i) {
        t += row_height(i, NULL);
    }
    return t;
}

static void
clamp_scroll(struct sc_screen *screen) {
    float maxs = total_content_h() - (content_bottom(screen) - content_top());
    if (maxs < 0) {
        maxs = 0;
    }
    if (s.scroll > (int) maxs) {
        s.scroll = (int) maxs;
    }
    if (s.scroll < 0) {
        s.scroll = 0;
    }
}

// Control rect for a field row: full-width below the label for wide fields,
// otherwise a compact control on the right, vertically centered.
static void
ctrl_rect(const SDL_FRect *row, const struct sfield *f, SDL_FRect *r) {
    if (f->wide) {
        *r = (SDL_FRect){row->x, row->y + 30, row->w, 34}; // full-width input
    } else if (f->kind == FK_BOOL) {
        *r = (SDL_FRect){row->x + row->w - 30, row->y + (row->h - 26) / 2,
                         26, 26};
    } else {
        float cw = 150;
        *r = (SDL_FRect){row->x + row->w - cw, row->y + (row->h - 34) / 2,
                         cw, 34};
    }
}

static void
save_rect(struct sc_screen *screen, SDL_FRect *r) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    float x0 = set_x0(screen);
    *r = (SDL_FRect){x0 + 14, h - SC_SET_SAVEH - 2, 160, SC_SET_SAVEH - 8};
    (void) w;
}

// --- rendering ---

static void
draw_text(SDL_Renderer *r, float x, float y, float px, const char *sn,
          Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (int i = 0; sn[i]; ++i) {
        unsigned char ch = (unsigned char) sn[i];
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

void
sc_settings_render(struct sc_screen *screen) {
    if (!s.open) {
        return;
    }
    SDL_Renderer *rr = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    float x0 = set_x0(screen);
    float pw = (float) w - x0;
    if (pw < 1) {
        return;
    }

    SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_BLEND);
    SDL_FRect panel = {x0 * scale, 0, pw * scale, (float) h * scale};
    SDL_SetRenderDrawColor(rr, 18, 19, 23, 255);
    SDL_RenderFillRect(rr, &panel);
    SDL_SetRenderDrawColor(rr, 70, 72, 80, 255);
    SDL_RenderLine(rr, x0 * scale, 0, x0 * scale, (float) h * scale);

    SDL_Rect clip = {(int) (x0 * scale), 0, (int) (pw * scale), (int) (h * scale)};
    SDL_SetRenderClipRect(rr, &clip);

    // Fractional (no rounding) so the larger size takes effect even at 1x scale.
    float px = SDL_max(2.2f, 2.2f * scale);
    float mx = -1, my = -1;
    SDL_GetMouseState(&mx, &my);

    // Tab bar.
    for (int i = 0; i < TAB_COUNT; ++i) {
        SDL_FRect t;
        tab_rect(screen, i, &t);
        bool hover = mx >= t.x && mx < t.x + t.w && my >= t.y && my < t.y + t.h;
        SDL_FRect box = {t.x * scale, t.y * scale, t.w * scale, t.h * scale};
        bool sel = s.tab == i;
        SDL_SetRenderDrawColor(rr, sel ? 40 : (hover ? 52 : 34),
                               sel ? 100 : (hover ? 54 : 36),
                               sel ? 176 : (hover ? 64 : 42), 255);
        SDL_RenderFillRect(rr, &box);
        SDL_SetRenderDrawColor(rr, 80, 82, 92, 255);
        SDL_RenderRect(rr, &box);
        float tw = strlen(TAB_NAMES[i]) * 8 * px;
        draw_text(rr, box.x + (box.w - tw) / 2, box.y + (box.h - 8 * px) / 2,
                  px, TAB_NAMES[i], 232, 233, 238);
    }

    clamp_scroll(screen);
    float ctop = content_top();
    float cbot = content_bottom(screen);
    int nrows = tab_rows();
    float clx = x0 + 14;
    float cwidth = ((float) w - 14 - SC_SET_SBARW) - clx;

    float y = ctop - s.scroll;
    for (int ridx = 0; ridx < nrows; ++ridx) {
        int fi;
        int rh = row_height(ridx, &fi);
        if (y + rh > ctop && y < cbot) {
            SDL_FRect row = {clx, y, cwidth, rh - 6};
            float lyc = row.y * scale + (row.h * scale - 8 * px) / 2;

            if (s.tab == TAB_BUTTONS) {
                SDL_FRect cbb = {row.x * scale, (row.y + 4) * scale, 26 * scale,
                                 26 * scale};
                SDL_SetRenderDrawColor(rr, 40, 42, 50, 255);
                SDL_RenderFillRect(rr, &cbb);
                SDL_SetRenderDrawColor(rr, 110, 112, 120, 255);
                SDL_RenderRect(rr, &cbb);
                if (s.btn_vis[ridx]) {
                    SDL_FRect tick = {cbb.x + 5 * scale, cbb.y + 5 * scale,
                                      cbb.w - 10 * scale, cbb.h - 10 * scale};
                    SDL_SetRenderDrawColor(rr, 90, 200, 130, 255);
                    SDL_RenderFillRect(rr, &tick);
                }
                draw_text(rr, (row.x + 36) * scale, lyc, px,
                          sc_toolbar_all_label(ridx), 220, 222, 228);
            } else if (fi >= 0) {
                const struct sfield *f = &FIELDS[fi];
                SDL_FRect cr;
                ctrl_rect(&row, f, &cr);
                // Label: on its own line (wide) or vertically centered (inline).
                float lyl = f->wide ? (row.y + 3) * scale : lyc;
                draw_text(rr, row.x * scale, lyl, px, f->label, 215, 217, 223);
                if (f->kind == FK_BOOL) {
                    SDL_FRect cbb = {cr.x * scale, cr.y * scale, cr.w * scale,
                                     cr.h * scale};
                    SDL_SetRenderDrawColor(rr, 40, 42, 50, 255);
                    SDL_RenderFillRect(rr, &cbb);
                    SDL_SetRenderDrawColor(rr, 110, 112, 120, 255);
                    SDL_RenderRect(rr, &cbb);
                    if (*(bool *) f->ptr) {
                        SDL_FRect tk = {cbb.x + 5 * scale, cbb.y + 5 * scale,
                                        cbb.w - 10 * scale, cbb.h - 10 * scale};
                        SDL_SetRenderDrawColor(rr, 90, 200, 130, 255);
                        SDL_RenderFillRect(rr, &tk);
                    }
                } else {
                    bool focused = s.focus == fi;
                    SDL_FRect bb = {cr.x * scale, cr.y * scale, cr.w * scale,
                                    cr.h * scale};
                    SDL_SetRenderDrawColor(rr, 30, 32, 38, 255);
                    SDL_RenderFillRect(rr, &bb);
                    SDL_SetRenderDrawColor(rr, focused ? 90 : 70,
                                           focused ? 140 : 72,
                                           focused ? 200 : 80, 255);
                    SDL_RenderRect(rr, &bb);
                    const char *txt = (const char *) f->ptr;
                    float tpad = 6 * scale;
                    float chw = 8 * px;
                    int maxch = (int) ((bb.w - 2 * tpad) / chw);
                    if (maxch < 1) {
                        maxch = 1;
                    }
                    int off = 0;
                    if (focused) {
                        // Window the horizontal scroll so the caret stays in view.
                        if (s.caret < s.fld_off) {
                            s.fld_off = s.caret;
                        }
                        if (s.caret > s.fld_off + maxch) {
                            s.fld_off = s.caret - maxch;
                        }
                        if (s.fld_off < 0) {
                            s.fld_off = 0;
                        }
                        off = s.fld_off;
                    }
                    // Clip the text to the box so it doesn't spill into the panel.
                    SDL_Rect bclip = {(int) bb.x, (int) bb.y, (int) bb.w,
                                      (int) bb.h};
                    SDL_SetRenderClipRect(rr, &bclip);
                    int slo, shi;
                    if (focused && fld_sel_range(&slo, &shi)) {
                        float hx = bb.x + tpad + (slo - off) * chw;
                        SDL_FRect hl = {hx, bb.y + 4 * scale,
                                        (shi - slo) * chw, bb.h - 8 * scale};
                        SDL_SetRenderDrawColor(rr, 60, 100, 180, 160);
                        SDL_RenderFillRect(rr, &hl);
                    }
                    draw_text(rr, bb.x + tpad, bb.y + (bb.h - 8 * px) / 2, px,
                              txt + off, 225, 227, 232);
                    if (focused) {
                        float cx = bb.x + tpad + (s.caret - off) * chw;
                        SDL_FRect caret = {cx, bb.y + 4 * scale, 2 * scale,
                                           bb.h - 8 * scale};
                        SDL_SetRenderDrawColor(rr, 210, 212, 220, 255);
                        SDL_RenderFillRect(rr, &caret);
                    }
                    SDL_SetRenderClipRect(rr, &clip); // restore the panel clip
                }
            }
        }
        y += rh;
    }

    // Scrollbar, shown only when the content overflows the viewport.
    float total = total_content_h();
    float vh = cbot - ctop;
    if (total > vh + 1) {
        float bx = (float) w - SC_SET_SBARW - 3;
        SDL_FRect track = {bx * scale, ctop * scale, SC_SET_SBARW * scale,
                           vh * scale};
        SDL_SetRenderDrawColor(rr, 34, 36, 42, 255);
        SDL_RenderFillRect(rr, &track);
        float th = vh * (vh / total);
        if (th < 26) {
            th = 26;
        }
        float ty = ctop + (vh - th) * ((float) s.scroll / (total - vh));
        SDL_FRect thumb = {bx * scale, ty * scale, SC_SET_SBARW * scale,
                           th * scale};
        SDL_SetRenderDrawColor(rr, 96, 99, 110, 255);
        SDL_RenderFillRect(rr, &thumb);
    }

    // Save bar.
    SDL_FRect sv;
    save_rect(screen, &sv);
    bool svhover = mx >= sv.x && mx < sv.x + sv.w && my >= sv.y
                && my < sv.y + sv.h;
    SDL_FRect svb = {sv.x * scale, sv.y * scale, sv.w * scale, sv.h * scale};
    SDL_SetRenderDrawColor(rr, svhover ? 52 : 40, svhover ? 130 : 100,
                           svhover ? 210 : 176, 255);
    SDL_RenderFillRect(rr, &svb);
    SDL_SetRenderDrawColor(rr, 90, 140, 200, 255);
    SDL_RenderRect(rr, &svb);
    const char *lbl = "Save";
    float lw = strlen(lbl) * 8 * px;
    draw_text(rr, svb.x + (svb.w - lw) / 2, svb.y + (svb.h - 8 * px) / 2, px,
              lbl, 240, 242, 248);
    draw_text(rr, (sv.x + sv.w + 14) * scale,
              sv.y * scale + (svb.h - 8 * px) / 2, px,
              "restarts on save", 170, 172, 182);

    SDL_SetRenderClipRect(rr, NULL);
}

// --- input ---

// Map a mouse x-position to a caret index within field `fi`, given its current
// horizontal scroll `off`.
static int
field_caret_at(struct sc_screen *screen, int fi, float mx, int off) {
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    float px = SDL_max(2.2f, 2.2f * scale);
    float chw = 8 * px;
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) h;
    float clx = set_x0(screen) + 14;
    float cwidth = ((float) w - 14 - SC_SET_SBARW) - clx;
    SDL_FRect frow = {clx, 0, cwidth, 0};
    SDL_FRect fcr;
    ctrl_rect(&frow, &FIELDS[fi], &fcr);
    float textx = (fcr.x + 6) * scale;
    int rel = (int) ((mx * scale - textx) / chw + 0.5f);
    int len = (int) strlen((char *) FIELDS[fi].ptr);
    int caret = off + rel;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;
    return caret;
}

bool
sc_settings_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    if (!s.open) {
        return false;
    }
    float x0 = set_x0(screen);

    switch (event->type) {
        case SDL_EVENT_TEXT_INPUT:
            if (s.focus >= 0 && FIELDS[s.focus].kind == FK_TEXT) {
                const struct sfield *f = &FIELDS[s.focus];
                char *t = (char *) f->ptr;
                int len = (int) strlen(t);
                if (s.caret > len) {
                    s.caret = len;
                }
                fld_delete_sel(t, &len); // typing replaces the selection
                for (const char *p = event->text.text; *p; ++p) {
                    if (f->numeric && !(isdigit((unsigned char) *p)
                                        || *p == '.')) {
                        continue;
                    }
                    if (len < f->cap - 1) {
                        // Insert at the caret (shift the tail, including '\0').
                        memmove(t + s.caret + 1, t + s.caret,
                                (size_t) (len - s.caret + 1));
                        t[s.caret] = *p;
                        ++s.caret;
                        ++len;
                    }
                }
                sc_push_event(SC_EVENT_SHELL_UPDATE);
            }
            return true;
        case SDL_EVENT_KEY_DOWN:
            if (event->key.key == SDLK_ESCAPE) {
                sc_settings_close(screen);
                return true;
            }
            if (event->key.key == SDLK_RETURN
                    || event->key.key == SDLK_KP_ENTER) {
                do_save();
                return true;
            }
            // Caret editing + selection within the focused text field.
            if (s.focus >= 0 && FIELDS[s.focus].kind == FK_TEXT) {
                const struct sfield *f = &FIELDS[s.focus];
                char *t = (char *) f->ptr;
                int len = (int) strlen(t);
                if (s.caret > len) {
                    s.caret = len;
                }
                bool ctrl = event->key.mod & SDL_KMOD_CTRL;
                bool shift = event->key.mod & SDL_KMOD_SHIFT;
                SDL_Keycode k = event->key.key;
                int lo, hi;

                if (ctrl && k == SDLK_A) {
                    s.sel_a = 0; // select all
                    s.caret = len;
                } else if (ctrl && k == SDLK_C) {
                    if (fld_sel_range(&lo, &hi)) {
                        char tmp[512];
                        int n = hi - lo;
                        if (n > (int) sizeof(tmp) - 1) n = sizeof(tmp) - 1;
                        memcpy(tmp, t + lo, n);
                        tmp[n] = '\0';
                        SDL_SetClipboardText(tmp);
                    }
                } else if (ctrl && k == SDLK_X) {
                    if (fld_sel_range(&lo, &hi)) {
                        char tmp[512];
                        int n = hi - lo;
                        if (n > (int) sizeof(tmp) - 1) n = sizeof(tmp) - 1;
                        memcpy(tmp, t + lo, n);
                        tmp[n] = '\0';
                        SDL_SetClipboardText(tmp);
                        fld_delete_sel(t, &len);
                    }
                } else if (ctrl && k == SDLK_V) {
                    fld_delete_sel(t, &len);
                    char *clip = SDL_GetClipboardText();
                    if (clip) {
                        for (char *p = clip; *p; ++p) {
                            if (*p == '\n' || *p == '\r') continue;
                            if (f->numeric && !(isdigit((unsigned char) *p)
                                                || *p == '.')) continue;
                            if (len < f->cap - 1) {
                                memmove(t + s.caret + 1, t + s.caret,
                                        (size_t) (len - s.caret + 1));
                                t[s.caret] = *p;
                                ++s.caret;
                                ++len;
                            }
                        }
                        SDL_free(clip);
                    }
                } else if (k == SDLK_LEFT) {
                    if (ctrl) { // select from the caret to the start
                        if (s.sel_a < 0) s.sel_a = s.caret;
                        s.caret = 0;
                    } else if (shift) {
                        if (s.sel_a < 0) s.sel_a = s.caret;
                        if (s.caret > 0) --s.caret;
                    } else if (fld_sel_range(&lo, &hi)) {
                        s.caret = lo;
                        s.sel_a = -1;
                    } else {
                        if (s.caret > 0) --s.caret;
                    }
                } else if (k == SDLK_RIGHT) {
                    if (ctrl) { // select from the caret to the end
                        if (s.sel_a < 0) s.sel_a = s.caret;
                        s.caret = len;
                    } else if (shift) {
                        if (s.sel_a < 0) s.sel_a = s.caret;
                        if (s.caret < len) ++s.caret;
                    } else if (fld_sel_range(&lo, &hi)) {
                        s.caret = hi;
                        s.sel_a = -1;
                    } else {
                        if (s.caret < len) ++s.caret;
                    }
                } else if (k == SDLK_HOME) {
                    if (shift) {
                        if (s.sel_a < 0) s.sel_a = s.caret;
                    } else {
                        s.sel_a = -1;
                    }
                    s.caret = 0;
                } else if (k == SDLK_END) {
                    if (shift) {
                        if (s.sel_a < 0) s.sel_a = s.caret;
                    } else {
                        s.sel_a = -1;
                    }
                    s.caret = len;
                } else if (k == SDLK_BACKSPACE) {
                    if (!fld_delete_sel(t, &len) && s.caret > 0) {
                        memmove(t + s.caret - 1, t + s.caret,
                                (size_t) (len - s.caret + 1));
                        --s.caret;
                    }
                } else if (k == SDLK_DELETE) {
                    if (!fld_delete_sel(t, &len) && s.caret < len) {
                        memmove(t + s.caret, t + s.caret + 1,
                                (size_t) (len - s.caret));
                    }
                }
                sc_push_event(SC_EVENT_SHELL_UPDATE);
                return true;
            }
            return s.focus >= 0; // swallow typing keys while a field is focused
        case SDL_EVENT_MOUSE_MOTION:
            if (s.field_drag && s.focus >= 0) {
                // Extend the selection to the dragged position (anchor stays).
                s.caret = field_caret_at(screen, s.focus, event->motion.x,
                                         s.fld_off);
                sc_push_event(SC_EVENT_SHELL_UPDATE);
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (s.field_drag) {
                s.field_drag = false;
                if (s.sel_a == s.caret) {
                    s.sel_a = -1; // no drag: it was just a click (caret moved)
                }
                sc_push_event(SC_EVENT_SHELL_UPDATE);
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_WHEEL: {
            float mx, my;
            SDL_GetMouseState(&mx, &my);
            if (mx >= x0) {
                s.scroll -= (int) (event->wheel.y * 40); // pixels per notch
                clamp_scroll(screen);
                sc_push_event(SC_EVENT_SHELL_UPDATE);
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (event->button.button != SDL_BUTTON_LEFT) {
                return event->button.x >= x0;
            }
            float bx = event->button.x, by = event->button.y;
            if (bx < x0) {
                return false;
            }
            // Tabs.
            for (int i = 0; i < TAB_COUNT; ++i) {
                SDL_FRect t;
                tab_rect(screen, i, &t);
                if (bx >= t.x && bx < t.x + t.w && by >= t.y && by < t.y + t.h) {
                    s.tab = i;
                    s.focus = -1;
                    s.scroll = 0;
                    sc_push_event(SC_EVENT_SHELL_UPDATE);
                    return true;
                }
            }
            // Save.
            SDL_FRect sv;
            save_rect(screen, &sv);
            if (bx >= sv.x && bx < sv.x + sv.w && by >= sv.y
                    && by < sv.y + sv.h) {
                do_save();
                return true;
            }
            // Rows (matched by the same running-Y layout as the renderer).
            s.focus = -1;
            float ctop = content_top();
            float cbot = content_bottom(screen);
            if (by >= ctop && by < cbot) {
                int nrows = tab_rows();
                float y = ctop - s.scroll;
                for (int ridx = 0; ridx < nrows; ++ridx) {
                    int fi;
                    int rh = row_height(ridx, &fi);
                    if (by >= y && by < y + rh - 6) {
                        if (s.tab == TAB_BUTTONS) {
                            s.btn_vis[ridx] = !s.btn_vis[ridx];
                        } else if (fi >= 0) {
                            if (FIELDS[fi].kind == FK_BOOL) {
                                bool *b = (bool *) FIELDS[fi].ptr;
                                *b = !*b;
                            } else {
                                // Focus the field, put the caret where the click
                                // landed, and arm a drag-select from there.
                                int off = (s.focus == fi) ? s.fld_off : 0;
                                if (s.focus != fi) {
                                    s.fld_off = 0;
                                }
                                int caret = field_caret_at(screen, fi, bx, off);
                                s.focus = fi;
                                s.caret = caret;
                                s.sel_a = caret; // anchor; a drag extends it
                                s.field_drag = true;
                            }
                        }
                        break;
                    }
                    y += rh;
                }
            }
            sc_push_event(SC_EVENT_SHELL_UPDATE);
            return true;
        }
        default:
            return false;
    }
}
