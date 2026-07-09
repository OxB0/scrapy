#include "toolbar.h"

#include <math.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "android/input.h"
#include "android/keycodes.h"
#include "apps.h"
#include "capture.h"
#include "userconf.h"
#include "control_msg.h"
#include "controller.h"
#include "font8x8_basic.h"
#include "icons_svg.h"
#include "logview.h"
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include "screen.h"
#include "settings.h"
#include "shell.h"
#include "util/log.h"

// Toolbar geometry, in logical (window) coordinates.
#define SC_TB_BTN 54
#define SC_TB_PAD 9
#define SC_TB_GAP 7

enum sc_tb_icon {
    IC_BACK, IC_HOME, IC_RECENTS, IC_MENU, IC_NOTIF,
    IC_VOLUP, IC_VOLDN, IC_ROTATE, IC_POWER, IC_PIN, IC_SHELL,
    IC_SHOT, IC_REC, IC_APPS, IC_AWAKE, IC_LOG, IC_SETTINGS,
    IC_COUNT,
};

enum sc_tb_action {
    SC_TB_KEY,          // inject a keycode (down+up)
    SC_TB_BACK_SCREEN,  // BACK, or turn screen on if off
    SC_TB_NOTIF,        // cycle notifications / quick settings / collapse
    SC_TB_ROTATE,       // rotate device
    SC_TB_PIN,          // toggle always-on-top (client-side)
    SC_TB_SHELL,        // toggle the terminal drawer
    SC_TB_SHOT,         // save a screenshot to the PC
    SC_TB_REC,          // toggle screen recording
    SC_TB_APPS,         // toggle the apps/density drawer
    SC_TB_AWAKE,        // toggle keep-screen-awake
    SC_TB_LOG,          // toggle the log drawer
    SC_TB_SETTINGS,     // toggle the settings drawer
};

struct sc_tb_button {
    enum sc_tb_icon icon;
    enum sc_tb_action action;
    enum android_keycode keycode; // for SC_TB_KEY
    const char *label;
    const char *name;   // config token
    int shortcut;       // sc_shortcut enum
};

// Toggle state for the pin (always-on-top) button.
static bool sc_tb_pinned = false;

static const struct sc_tb_button sc_toolbar_all[] = {
    {IC_PIN,     SC_TB_PIN,         0,          "Pin on top",  "pin",   SC_SHORTCUT_PIN},
    {IC_AWAKE,   SC_TB_AWAKE,       0,          "Keep awake",  "awake", SC_SHORTCUT_AWAKE},
    {IC_SHELL,   SC_TB_SHELL,       0,          "Shell",       "shell", SC_SHORTCUT_SHELL},
    {IC_APPS,    SC_TB_APPS,        0,      "Apps & density",  "apps",  SC_SHORTCUT_APPS},
    {IC_LOG,     SC_TB_LOG,         0,          "Log",         "log",   SC_SHORTCUT_LOG},
    {IC_SETTINGS,SC_TB_SETTINGS,    0,          "Settings",    "settings", SC_SHORTCUT_SETTINGS},
    {IC_SHOT,    SC_TB_SHOT,        0,          "Screenshot",  "screenshot", SC_SHORTCUT_SCREENSHOT},
    {IC_REC,     SC_TB_REC,         0,          "Record",      "record", SC_SHORTCUT_RECORD},
    {IC_BACK,    SC_TB_BACK_SCREEN, 0,          "Back",        "back",  SC_SHORTCUT_BACK},
    {IC_HOME,    SC_TB_KEY,   AKEYCODE_HOME,    "Home",        "home",  SC_SHORTCUT_HOME},
    {IC_RECENTS, SC_TB_KEY, AKEYCODE_APP_SWITCH, "Recents",    "recents", SC_SHORTCUT_RECENTS},
    {IC_MENU,    SC_TB_KEY,   AKEYCODE_MENU,    "Menu",        "menu",  SC_SHORTCUT_MENU},
    {IC_NOTIF,   SC_TB_NOTIF,       0,        "Notifications", "notifications", SC_SHORTCUT_NOTIF},
    {IC_VOLUP,   SC_TB_KEY, AKEYCODE_VOLUME_UP, "Volume up",   "volup", SC_SHORTCUT_VOLUP},
    {IC_VOLDN,   SC_TB_KEY, AKEYCODE_VOLUME_DOWN, "Volume down", "voldown", SC_SHORTCUT_VOLDOWN},
    {IC_ROTATE,  SC_TB_ROTATE,      0,          "Rotate",      "rotate", SC_SHORTCUT_ROTATE},
    {IC_POWER,   SC_TB_KEY,   AKEYCODE_POWER,   "Power",       "power", SC_SHORTCUT_POWER},
};

#define SC_TB_ALL (sizeof(sc_toolbar_all) / sizeof(sc_toolbar_all[0]))

// The visible subset/order, built from config (all by default).
static struct sc_tb_button sc_toolbar[SC_TB_ALL];
static unsigned sc_tb_count;
#define SC_TB_COUNT sc_tb_count

// Build the visible button list from sc_conf.buttons ("none" = empty, absent =
// all in default order, otherwise the named buttons in the given order).
static void
sc_toolbar_build(void) {
    if (!sc_conf.has_buttons) {
        for (unsigned i = 0; i < SC_TB_ALL; ++i) {
            sc_toolbar[i] = sc_toolbar_all[i];
        }
        sc_tb_count = SC_TB_ALL;
        return;
    }
    sc_tb_count = 0;
    char list[512];
    snprintf(list, sizeof(list), "%s", sc_conf.buttons);
    if (!strcmp(list, "none")) {
        return;
    }
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') {
            ++tok;
        }
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        for (unsigned i = 0; i < SC_TB_ALL; ++i) {
            if (!strcmp(tok, sc_toolbar_all[i].name)) {
                sc_toolbar[sc_tb_count++] = sc_toolbar_all[i];
                break;
            }
        }
    }
}

// Notification button cycle state: 0 collapsed, 1 notifications, 2 settings.
static int sc_tb_notif_state = 0;

static bool sc_tb_built = false;

static void
ensure_built(void) {
    if (!sc_tb_built) {
        sc_toolbar_build();
        sc_tb_built = true;
    }
}

void
sc_toolbar_init(struct sc_screen *screen) {
    ensure_built();
    if (sc_conf.pin_on_top) {
        sc_tb_pinned = true;
        SDL_SetWindowAlwaysOnTop(screen->window, true);
    }
}

void
sc_toolbar_toggle_pin(struct sc_screen *screen) {
    sc_tb_pinned = !sc_tb_pinned;
    SDL_SetWindowAlwaysOnTop(screen->window, sc_tb_pinned);
}

int
sc_toolbar_width(void) {
    ensure_built();
    if (sc_tb_count == 0) {
        return 0; // no buttons configured: no gutter
    }
    return SC_TB_BTN + 2 * SC_TB_PAD;
}

int
sc_toolbar_all_count(void) {
    return (int) SC_TB_ALL;
}

const char *
sc_toolbar_all_name(int i) {
    if (i < 0 || i >= (int) SC_TB_ALL) {
        return "";
    }
    return sc_toolbar_all[i].name;
}

const char *
sc_toolbar_all_label(int i) {
    if (i < 0 || i >= (int) SC_TB_ALL) {
        return "";
    }
    return sc_toolbar_all[i].label;
}

// Button size shrinks from SC_TB_BTN so all buttons fit the window height (the
// gutter width stays fixed; buttons are centered in it horizontally).
static float
sc_tb_btn_size(struct sc_screen *screen) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
    if (SC_TB_COUNT == 0) {
        return SC_TB_BTN;
    }
    float avail = (float) h - 2 * SC_TB_PAD - (SC_TB_COUNT - 1) * SC_TB_GAP;
    float s = avail / SC_TB_COUNT;
    if (s > SC_TB_BTN) {
        s = SC_TB_BTN;
    }
    if (s < 20) {
        s = 20;
    }
    return s;
}

static void
sc_toolbar_button_rect(struct sc_screen *screen, unsigned i,
                       float *x, float *y, float *size) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
    float s = sc_tb_btn_size(screen);
    float total = SC_TB_COUNT * s + (SC_TB_COUNT - 1) * SC_TB_GAP;
    *x = SC_TB_PAD + (SC_TB_BTN - s) / 2.0f; // centered in the fixed gutter
    float top = (h - total) / 2.0f;
    if (top < SC_TB_PAD) {
        top = SC_TB_PAD;
    }
    *y = top + i * (s + SC_TB_GAP);
    *size = s;
}

// --- icon rendering ---

// Each icon is a Lucide SVG (white strokes) rasterized once by nanosvg into a
// high-resolution mask texture, then drawn at button size as a linear-filtered
// downscale (anti-aliased) and tinted per state via color-mod.
#define ICON_PX 256
static SDL_Texture *g_icon_tex[IC_COUNT];

static SDL_Texture *
icon_texture(SDL_Renderer *rr, enum sc_tb_icon ic) {
    if ((unsigned) ic >= IC_COUNT || !g_icon_svg[ic]) {
        return NULL;
    }
    if (g_icon_tex[ic]) {
        return g_icon_tex[ic];
    }

    // nanosvg parses in place, so hand it a mutable copy.
    char *copy = SDL_strdup(g_icon_svg[ic]);
    if (!copy) {
        return NULL;
    }
    NSVGimage *img = nsvgParse(copy, "px", 96.f);
    SDL_free(copy);
    if (!img) {
        return NULL;
    }

    static NSVGrasterizer *rast; // reused across icons; freed at process exit
    if (!rast) {
        rast = nsvgCreateRasterizer();
    }
    unsigned char *pixels = SDL_calloc(1, ICON_PX * ICON_PX * 4);
    SDL_Texture *tex = NULL;
    if (rast && pixels) {
        float vb = img->width > 0 ? img->width : 24.f; // Lucide viewBox is 24
        float scale = ICON_PX / vb;
        nsvgRasterize(rast, img, 0, 0, scale, pixels, ICON_PX, ICON_PX,
                      ICON_PX * 4);
        tex = SDL_CreateTexture(rr, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, ICON_PX, ICON_PX);
        if (tex) {
            SDL_UpdateTexture(tex, NULL, pixels, ICON_PX * 4);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }
    SDL_free(pixels);
    nsvgDelete(img);
    g_icon_tex[ic] = tex;
    return tex;
}

// Draw an icon into its button box at (bx,by) size `sz` (logical), scaled by
// `scale`, tinted (r,g,b).
static void
draw_icon(SDL_Renderer *rr, enum sc_tb_icon ic, float bx, float by, float sz,
          float scale, Uint8 r, Uint8 g, Uint8 b) {
    SDL_Texture *tex = icon_texture(rr, ic);
    if (!tex) {
        return;
    }
    SDL_SetTextureColorMod(tex, r, g, b);
    SDL_FRect dst = {bx * scale, by * scale, sz * scale, sz * scale};
    SDL_RenderTexture(rr, tex, NULL, &dst);
}

static void
draw_text(SDL_Renderer *r, float x, float y, float px, const char *s,
          Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (unsigned ci = 0; s[ci]; ++ci) {
        unsigned char ch = (unsigned char) s[ci];
        if (ch >= 128) {
            ch = '?';
        }
        const char *glyph = font8x8_basic[ch];
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                if (glyph[row] & (1 << col)) {
                    SDL_FRect p = {x + (ci * 8 + col) * px, y + row * px,
                                   px, px};
                    SDL_RenderFillRect(r, &p);
                }
            }
        }
    }
}

void
sc_toolbar_render(struct sc_screen *screen) {
    SDL_Renderer *renderer = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);

    float mx = -1, my = -1;
    SDL_GetMouseState(&mx, &my);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Gutter panel background (opaque strip on the LEFT, matching the buttons;
    // the video is shifted right by this width). Drawing it on the right used to
    // cover the video's right edge — the "cut off on the right" bug.
    (void) w;
    float gutter = sc_toolbar_width();
    SDL_FRect panel = {0, 0, gutter * scale, h * scale};
    SDL_SetRenderDrawColor(renderer, 28, 28, 32, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 60, 60, 66, 255);
    SDL_RenderLine(renderer, gutter * scale, 0, gutter * scale, h * scale);

    int hovered = -1;
    for (unsigned i = 0; i < SC_TB_COUNT; ++i) {
        float bx, by, bs;
        sc_toolbar_button_rect(screen, i, &bx, &by, &bs);
        bool hover = mx >= bx && mx < bx + bs && my >= by && my < by + bs;
        if (hover) {
            hovered = i;
        }

        bool recording = sc_toolbar[i].action == SC_TB_REC
                      && sc_capture_recording();
        SDL_FRect bg = {bx * scale, by * scale, bs * scale, bs * scale};
        bool active = (sc_toolbar[i].action == SC_TB_PIN && sc_tb_pinned)
                   || (sc_toolbar[i].action == SC_TB_SHELL && sc_shell_is_open())
                   || (sc_toolbar[i].action == SC_TB_APPS && sc_apps_is_open())
                   || (sc_toolbar[i].action == SC_TB_LOG && sc_logview_is_open())
                   || (sc_toolbar[i].action == SC_TB_SETTINGS
                       && sc_settings_is_open())
                   || (sc_toolbar[i].action == SC_TB_AWAKE
                       && sc_capture_awake_is_on())
                   || recording;
        if (active) {
            SDL_SetRenderDrawColor(renderer, 40, 100, 176, 255);
        } else if (hover) {
            SDL_SetRenderDrawColor(renderer, 64, 66, 74, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 44, 45, 51, 255);
        }
        SDL_RenderFillRect(renderer, &bg);
        SDL_SetRenderDrawColor(renderer, 78, 80, 88, 255);
        SDL_RenderRect(renderer, &bg);

        Uint8 lum = hover ? 255 : 220;
        // The record dot glows red while recording.
        if (recording) {
            draw_icon(renderer, sc_toolbar[i].icon, bx, by, bs, scale,
                      240, 70, 70);
        } else {
            draw_icon(renderer, sc_toolbar[i].icon, bx, by, bs, scale,
                      lum, lum, hover ? 255 : 228);
        }
    }

    // Tooltip for the hovered button, to the right of the toolbar.
    // SCRCPY_LABELS=1 shows every label at once (for a labelled screenshot).
    bool all_labels = getenv("SCRCPY_LABELS") != NULL;
    for (unsigned ti = 0; ti < SC_TB_COUNT; ++ti) {
        if (!all_labels && (int) ti != hovered) {
            continue;
        }
        float bx, by, bs;
        sc_toolbar_button_rect(screen, ti, &bx, &by, &bs);
        // Label plus its keyboard shortcut, e.g. "Shell  (Alt+Shift+T)".
        char sc[48];
        sc_shortcut_format(sc_toolbar[ti].shortcut, sc, sizeof(sc));
        char label[80];
        if (sc[0]) {
            snprintf(label, sizeof(label), "%s  (%s)", sc_toolbar[ti].label, sc);
        } else {
            snprintf(label, sizeof(label), "%s", sc_toolbar[ti].label);
        }
        float px = SDL_max(2.f, roundf(1.5f * scale));
        float tw = strlen(label) * 8 * px;
        float th = 8 * px;
        float padx = 8 * scale, pady = 5 * scale;
        float boxw = tw + 2 * padx;
        float boxh = th + 2 * pady;
        float boxx = (bx + bs) * scale + 8 * scale;
        float boxy = (by + bs / 2.f) * scale - boxh / 2.f;
        SDL_FRect tip = {boxx, boxy, boxw, boxh};
        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 235);
        SDL_RenderFillRect(renderer, &tip);
        SDL_SetRenderDrawColor(renderer, 90, 92, 100, 255);
        SDL_RenderRect(renderer, &tip);
        draw_text(renderer, boxx + padx, boxy + pady, px, label,
                  235, 236, 240);
    }
    // Status toasts (screenshot/record/install/copy feedback) are drawn by the
    // shared toast module in sc_screen_render.
}

// --- actions ---

static void
sc_toolbar_push(struct sc_screen *screen, struct sc_control_msg *msg) {
    if (!screen->controller) {
        return;
    }
    if (!sc_controller_push_msg(screen->controller, msg)) {
        LOGW("Could not push toolbar control message");
    }
}

static void
sc_toolbar_send_key(struct sc_screen *screen, enum android_keycode keycode) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
    msg.inject_keycode.keycode = keycode;
    msg.inject_keycode.metastate = 0;
    msg.inject_keycode.repeat = 0;
    msg.inject_keycode.action = AKEY_EVENT_ACTION_DOWN;
    sc_toolbar_push(screen, &msg);
    msg.inject_keycode.action = AKEY_EVENT_ACTION_UP;
    sc_toolbar_push(screen, &msg);
}

static void
sc_toolbar_notif_cycle(struct sc_screen *screen) {
    struct sc_control_msg msg;
    switch (sc_tb_notif_state) {
        case 0:
            msg.type = SC_CONTROL_MSG_TYPE_EXPAND_NOTIFICATION_PANEL;
            sc_tb_notif_state = 1;
            break;
        case 1:
            msg.type = SC_CONTROL_MSG_TYPE_EXPAND_SETTINGS_PANEL;
            sc_tb_notif_state = 2;
            break;
        default:
            msg.type = SC_CONTROL_MSG_TYPE_COLLAPSE_PANELS;
            sc_tb_notif_state = 0;
            break;
    }
    sc_toolbar_push(screen, &msg);
}

static void
sc_toolbar_do(struct sc_screen *screen, const struct sc_tb_button *btn) {
    struct sc_control_msg msg;
    switch (btn->action) {
        case SC_TB_KEY:
            sc_toolbar_send_key(screen, btn->keycode);
            break;
        case SC_TB_BACK_SCREEN:
            msg.type = SC_CONTROL_MSG_TYPE_BACK_OR_SCREEN_ON;
            msg.back_or_screen_on.action = AKEY_EVENT_ACTION_DOWN;
            sc_toolbar_push(screen, &msg);
            msg.back_or_screen_on.action = AKEY_EVENT_ACTION_UP;
            sc_toolbar_push(screen, &msg);
            break;
        case SC_TB_NOTIF:
            sc_toolbar_notif_cycle(screen);
            break;
        case SC_TB_ROTATE:
            msg.type = SC_CONTROL_MSG_TYPE_ROTATE_DEVICE;
            sc_toolbar_push(screen, &msg);
            break;
        case SC_TB_PIN:
            sc_tb_pinned = !sc_tb_pinned;
            SDL_SetWindowAlwaysOnTop(screen->window, sc_tb_pinned);
            break;
        case SC_TB_SHELL:
            sc_shell_toggle(screen);
            break;
        case SC_TB_SHOT:
            sc_capture_screenshot();
            break;
        case SC_TB_REC:
            sc_capture_record_toggle();
            break;
        case SC_TB_APPS:
            sc_apps_toggle(screen);
            break;
        case SC_TB_AWAKE:
            sc_capture_stay_awake(!sc_capture_awake_is_on());
            break;
        case SC_TB_LOG:
            sc_logview_toggle(screen);
            break;
        case SC_TB_SETTINGS:
            sc_settings_toggle(screen);
            break;
    }
}

void
sc_toolbar_trigger(struct sc_screen *screen, int shortcut) {
    // Look up the full button table (not just the visible subset) so a shortcut
    // still works even if its button is hidden via config.
    for (unsigned i = 0; i < SC_TB_ALL; ++i) {
        if (sc_toolbar_all[i].shortcut == shortcut) {
            sc_toolbar_do(screen, &sc_toolbar_all[i]);
            return;
        }
    }
}

bool
sc_toolbar_click(struct sc_screen *screen, float x, float y) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
    (void) h;
    if (x >= sc_toolbar_width()) {
        return false; // not in the left gutter
    }
    for (unsigned i = 0; i < SC_TB_COUNT; ++i) {
        float bx, by, bs;
        sc_toolbar_button_rect(screen, i, &bx, &by, &bs);
        if (x >= bx && x < bx + bs && y >= by && y < by + bs) {
            sc_toolbar_do(screen, &sc_toolbar[i]);
            break;
        }
    }
    return true;
}
