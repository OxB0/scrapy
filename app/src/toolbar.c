#include "toolbar.h"

#include <math.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "android/input.h"
#include "android/keycodes.h"
#include "capture.h"
#include "control_msg.h"
#include "controller.h"
#include "font8x8_basic.h"
#include "screen.h"
#include "shell.h"
#include "util/log.h"

// Toolbar geometry, in logical (window) coordinates.
#define SC_TB_BTN 54
#define SC_TB_PAD 9
#define SC_TB_GAP 7

enum sc_tb_icon {
    IC_BACK, IC_HOME, IC_RECENTS, IC_MENU, IC_NOTIF,
    IC_VOLUP, IC_VOLDN, IC_ROTATE, IC_POWER, IC_PIN, IC_SHELL,
    IC_SHOT, IC_REC,
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
};

struct sc_tb_button {
    enum sc_tb_icon icon;
    enum sc_tb_action action;
    enum android_keycode keycode; // for SC_TB_KEY
    const char *label;
};

// Toggle state for the pin (always-on-top) button.
static bool sc_tb_pinned = false;

static const struct sc_tb_button sc_toolbar[] = {
    {IC_PIN,     SC_TB_PIN,         0,                    "Pin on top"},
    {IC_SHELL,   SC_TB_SHELL,       0,                    "Shell"},
    {IC_SHOT,    SC_TB_SHOT,        0,                    "Screenshot"},
    {IC_REC,     SC_TB_REC,         0,                    "Record"},
    {IC_BACK,    SC_TB_BACK_SCREEN, 0,                    "Back"},
    {IC_HOME,    SC_TB_KEY,         AKEYCODE_HOME,        "Home"},
    {IC_RECENTS, SC_TB_KEY,         AKEYCODE_APP_SWITCH,  "Recents"},
    {IC_MENU,    SC_TB_KEY,         AKEYCODE_MENU,        "Menu"},
    {IC_NOTIF,   SC_TB_NOTIF,       0,                    "Notifications"},
    {IC_VOLUP,   SC_TB_KEY,         AKEYCODE_VOLUME_UP,   "Volume up"},
    {IC_VOLDN,   SC_TB_KEY,         AKEYCODE_VOLUME_DOWN, "Volume down"},
    {IC_ROTATE,  SC_TB_ROTATE,      0,                    "Rotate"},
    {IC_POWER,   SC_TB_KEY,         AKEYCODE_POWER,       "Power"},
};

#define SC_TB_COUNT (sizeof(sc_toolbar) / sizeof(sc_toolbar[0]))

// Notification button cycle state: 0 collapsed, 1 notifications, 2 settings.
static int sc_tb_notif_state = 0;

int
sc_toolbar_width(void) {
    return SC_TB_BTN + 2 * SC_TB_PAD;
}

// Button size shrinks from SC_TB_BTN so all buttons fit the window height (the
// gutter width stays fixed; buttons are centered in it horizontally).
static float
sc_tb_btn_size(struct sc_screen *screen) {
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    (void) w;
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

// --- drawing helpers (physical pixel coordinates) ---

static SDL_FColor
fcol(Uint8 r, Uint8 g, Uint8 b) {
    return (SDL_FColor){r / 255.f, g / 255.f, b / 255.f, 1.f};
}

static void
fill_tri(SDL_Renderer *r, SDL_FColor c,
         float ax, float ay, float bx, float by, float cx, float cy) {
    SDL_Vertex v[3] = {
        {{ax, ay}, c, {0, 0}},
        {{bx, by}, c, {0, 0}},
        {{cx, cy}, c, {0, 0}},
    };
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

static void
fill_fan(SDL_Renderer *r, SDL_FColor c, const float *pts, int n) {
    // Triangle fan around pts[0].
    for (int i = 1; i + 1 < n; ++i) {
        fill_tri(r, c, pts[0], pts[1], pts[i * 2], pts[i * 2 + 1],
                 pts[(i + 1) * 2], pts[(i + 1) * 2 + 1]);
    }
}

static void
thick_line(SDL_Renderer *r, float x1, float y1, float x2, float y2, float t) {
    for (float dx = -t; dx <= t; dx += 1.f) {
        for (float dy = -t; dy <= t; dy += 1.f) {
            SDL_RenderLine(r, x1 + dx, y1 + dy, x2 + dx, y2 + dy);
        }
    }
}

static void
arc(SDL_Renderer *r, float cx, float cy, float rad, float a0, float a1,
    float t) {
    const int steps = 24;
    float px = 0, py = 0;
    for (int i = 0; i <= steps; ++i) {
        float a = a0 + (a1 - a0) * i / steps;
        float x = cx + rad * cosf(a);
        float y = cy + rad * sinf(a);
        if (i > 0) {
            thick_line(r, px, py, x, y, t);
        }
        px = x;
        py = y;
    }
}

// Draw the icon for a button, filling its box at (bx,by) size `sz` (logical),
// scaled by `scale`, in color (r,g,b).
static void
draw_icon(SDL_Renderer *rr, enum sc_tb_icon ic, float bx, float by, float sz,
          float scale, Uint8 r, Uint8 g, Uint8 b) {
#define X(n) ((bx + (n) * sz) * scale)
#define Y(n) ((by + (n) * sz) * scale)
#define RECT(x0, y0, x1, y1) do { \
        SDL_FRect _q = {X(x0), Y(y0), ((x1) - (x0)) * sz * scale, \
                        ((y1) - (y0)) * sz * scale}; \
        SDL_RenderFillRect(rr, &_q); \
    } while (0)
    SDL_FColor c = fcol(r, g, b);
    SDL_SetRenderDrawColor(rr, r, g, b, 255);
    float t = 0.5f * scale;

    switch (ic) {
        case IC_BACK:
            fill_tri(rr, c, X(0.62f), Y(0.27f), X(0.34f), Y(0.5f),
                     X(0.62f), Y(0.73f));
            break;
        case IC_HOME:
            fill_tri(rr, c, X(0.5f), Y(0.23f), X(0.19f), Y(0.54f),
                     X(0.81f), Y(0.54f));
            RECT(0.32f, 0.5f, 0.68f, 0.78f);
            SDL_SetRenderDrawColor(rr, 44, 45, 51, 255); // door cutout
            RECT(0.45f, 0.6f, 0.55f, 0.78f);
            break;
        case IC_RECENTS:
            RECT(0.30f, 0.30f, 0.70f, 0.37f); // top
            RECT(0.30f, 0.63f, 0.70f, 0.70f); // bottom
            RECT(0.30f, 0.30f, 0.37f, 0.70f); // left
            RECT(0.63f, 0.30f, 0.70f, 0.70f); // right
            break;
        case IC_MENU:
            RECT(0.28f, 0.34f, 0.72f, 0.42f);
            RECT(0.28f, 0.46f, 0.72f, 0.54f);
            RECT(0.28f, 0.58f, 0.72f, 0.66f);
            break;
        case IC_NOTIF: {
            float bell[] = {
                X(0.5f), Y(0.5f),
                X(0.38f), Y(0.6f), X(0.4f), Y(0.4f), X(0.45f), Y(0.3f),
                X(0.55f), Y(0.3f), X(0.6f), Y(0.4f), X(0.62f), Y(0.6f),
            };
            fill_fan(rr, c, bell, 7);
            RECT(0.35f, 0.6f, 0.65f, 0.65f); // rim
            fill_tri(rr, c, X(0.44f), Y(0.66f), X(0.56f), Y(0.66f),
                     X(0.5f), Y(0.73f)); // clapper
            break;
        }
        case IC_VOLUP:
            RECT(0.47f, 0.3f, 0.53f, 0.7f);
            RECT(0.3f, 0.47f, 0.7f, 0.53f);
            break;
        case IC_VOLDN:
            RECT(0.3f, 0.47f, 0.7f, 0.53f);
            break;
        case IC_ROTATE:
            arc(rr, X(0.5f), Y(0.5f), 0.22f * sz * scale, -2.2f, 1.9f, t);
            fill_tri(rr, c, X(0.5f), Y(0.24f), X(0.4f), Y(0.34f),
                     X(0.56f), Y(0.36f)); // arrowhead
            break;
        case IC_POWER:
            arc(rr, X(0.5f), Y(0.54f), 0.2f * sz * scale, -1.0f, 4.14f, t);
            RECT(0.47f, 0.24f, 0.53f, 0.52f);
            break;
        case IC_PIN: {
            // Thumbtack: round head + needle.
            float head[] = {
                X(0.5f), Y(0.36f),
                X(0.38f), Y(0.36f), X(0.4f), Y(0.28f), X(0.5f), Y(0.24f),
                X(0.6f), Y(0.28f), X(0.62f), Y(0.36f),
            };
            fill_fan(rr, c, head, 6);
            RECT(0.44f, 0.36f, 0.56f, 0.44f); // collar
            RECT(0.485f, 0.44f, 0.515f, 0.74f); // needle
            break;
        }
        case IC_SHELL:
            // ">_" prompt
            thick_line(rr, X(0.3f), Y(0.36f), X(0.44f), Y(0.5f), t);
            thick_line(rr, X(0.44f), Y(0.5f), X(0.3f), Y(0.64f), t);
            RECT(0.5f, 0.6f, 0.72f, 0.66f);
            break;
        case IC_SHOT:
            // Camera: body + lens + shutter button.
            RECT(0.24f, 0.36f, 0.76f, 0.72f);
            RECT(0.4f, 0.30f, 0.6f, 0.38f); // viewfinder hump
            SDL_SetRenderDrawColor(rr, 44, 45, 51, 255); // lens cutout
            RECT(0.42f, 0.46f, 0.58f, 0.64f);
            SDL_SetRenderDrawColor(rr, r, g, b, 255);
            RECT(0.64f, 0.40f, 0.71f, 0.45f); // flash
            break;
        case IC_REC:
            // Filled record dot (turns red while recording via the color arg).
            fill_fan(rr, c, (float[]){
                X(0.5f), Y(0.5f),
                X(0.32f), Y(0.5f), X(0.37f), Y(0.35f), X(0.5f), Y(0.3f),
                X(0.63f), Y(0.35f), X(0.68f), Y(0.5f), X(0.63f), Y(0.65f),
                X(0.5f), Y(0.7f), X(0.37f), Y(0.65f), X(0.32f), Y(0.5f),
            }, 10);
            break;
    }
#undef X
#undef Y
#undef RECT
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
    if (hovered >= 0) {
        float bx, by, bs;
        sc_toolbar_button_rect(screen, (unsigned) hovered, &bx, &by, &bs);
        const char *label = sc_toolbar[hovered].label;
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

    // Transient status toast (screenshot/record feedback), centered near the
    // bottom of the video area.
    char toast[128];
    if (sc_capture_toast(toast, sizeof(toast))) {
        float px = SDL_max(2.f, roundf(1.5f * scale));
        float tw = strlen(toast) * 8 * px;
        float padx = 12 * scale, pady = 7 * scale;
        float boxw = tw + 2 * padx;
        float boxh = 8 * px + 2 * pady;
        float boxx = (gutter * scale) + ((float) w * scale - gutter * scale
                                         - boxw) / 2.f;
        float boxy = (float) h * scale - boxh - 24 * scale;
        SDL_FRect box = {boxx, boxy, boxw, boxh};
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 235);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 90, 92, 100, 255);
        SDL_RenderRect(renderer, &box);
        draw_text(renderer, boxx + padx, boxy + pady, px, toast, 235, 236, 240);
    }
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
