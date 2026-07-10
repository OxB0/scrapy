#include "logview.h"

#include <string.h>
#include <SDL3/SDL.h>

#include "apps.h"
#include "userconf.h"
#include "events.h"
#include "font8x16_basic.h"
#include "screen.h"
#include "settings.h"
#include "shell.h"

#define SC_LV_LINES 4000 // scrollback ring size
#define SC_LV_COLS  512  // max stored chars per line
#define SC_LV_MIN   320  // minimum drawer width (video-fit reservation)
#define SC_LV_FONT_MUL 1.4f // font size: pixels per source pixel (at scale 1)
#define SC_LV_MAXDR 500  // max wrapped display rows built per frame

static struct sc_logview {
    bool ready;
    bool open;

    sc_mutex mutex; // protects the ring buffer
    char lines[SC_LV_LINES][SC_LV_COLS];
    int lens[SC_LV_LINES];
    unsigned char sev[SC_LV_LINES]; // 0 info, 1 warn, 2 error
    int head;  // index of oldest line
    int count; // number of stored lines

    int scroll; // scrollback offset in rows (0 = bottom / newest)
} lv;

// Width the window grows by when the drawer opens (config override, else 760).
static int
lv_target(void) {
    return sc_conf.log_width > 0 ? sc_conf.log_width : 760;
}

void
sc_logview_init(void) {
    if (lv.ready) {
        return;
    }
    sc_mutex_init(&lv.mutex);
    lv.ready = true;
}

static unsigned char
lv_severity(const char *msg) {
    if (!strncmp(msg, "ERROR", 5) || !strncmp(msg, "CRITICAL", 8)) {
        return 2;
    }
    if (!strncmp(msg, "WARN", 4)) {
        return 1;
    }
    return 0;
}

// Wake the render loop so newly-logged lines appear promptly even when the
// mirror is static. Coalesced with a pending flag (same idea as the shell).
static SDL_AtomicInt lv_wake_pending;

static void
lv_wake(void) {
    if (SDL_CompareAndSwapAtomicInt(&lv_wake_pending, 0, 1)) {
        sc_push_event(SC_EVENT_SHELL_UPDATE);
    }
}

void
sc_logview_push(const char *msg) {
    if (!lv.ready || !msg) {
        return;
    }
    unsigned char sev = lv_severity(msg);

    sc_mutex_lock(&lv.mutex);
    const char *p = msg;
    // A message may span several physical lines; store each separately.
    for (;;) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int) (nl - p) : (int) strlen(p);
        if (len > SC_LV_COLS - 1) {
            len = SC_LV_COLS - 1;
        }
        int slot = (lv.head + lv.count) % SC_LV_LINES;
        memcpy(lv.lines[slot], p, len);
        lv.lines[slot][len] = '\0';
        lv.lens[slot] = len;
        lv.sev[slot] = sev;
        if (lv.count < SC_LV_LINES) {
            lv.count++;
        } else {
            lv.head = (lv.head + 1) % SC_LV_LINES;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
        if (!*p) {
            break; // trailing newline: no empty final line
        }
    }
    sc_mutex_unlock(&lv.mutex);

    if (lv.open) {
        lv_wake();
    }
}

void
sc_logview_toggle(struct sc_screen *screen) {
    if (!lv.ready) {
        return;
    }
    if (!lv.open) {
        // Share the right-side region with the other drawers.
        if (sc_shell_is_open()) {
            sc_shell_close(screen);
        }
        if (sc_apps_is_open()) {
            sc_apps_close(screen);
        }
        if (sc_settings_is_open()) {
            sc_settings_close(screen);
        }
    }
    lv.open = !lv.open;
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    if (lv.open) {
        SDL_SetWindowAspectRatio(screen->window, 0.f, 0.f);
        lv.scroll = 0; // jump to newest
        SDL_SetWindowSize(screen->window, w + lv_target(), h);
    } else {
        int nw = w - lv_target();
        SDL_SetWindowSize(screen->window, nw < 200 ? 200 : nw, h);
    }
}

void
sc_logview_close(struct sc_screen *screen) {
    if (lv.open) {
        sc_logview_toggle(screen);
    }
}

bool
sc_logview_is_open(void) {
    return lv.open;
}

bool
sc_logview_step_anim(void) {
    return false; // opens instantly (window grows outward)
}

int
sc_logview_reserved_width(struct sc_screen *screen) {
    (void) screen;
    return lv.open ? lv_target() : 0; // fixed-width panel
}

// --- rendering (own 8x16 font atlas, mirroring the shell drawer) ---

static SDL_Texture *lv_font_tex = NULL;

static void
lv_ensure_font(SDL_Renderer *r) {
    if (lv_font_tex) {
        return;
    }
    static Uint32 px[128 * 128];
    memset(px, 0, sizeof(px));
    for (int c = 0; c < 128; ++c) {
        int cx = (c % 16) * 8;
        int cy = (c / 16) * 16;
        const unsigned char *gl = font8x16_basic[c];
        for (int row = 0; row < 16; ++row) {
            for (int col = 0; col < 8; ++col) {
                if (gl[row] & (1 << col)) {
                    px[(cy + row) * 128 + (cx + col)] = 0xFFFFFFFFu;
                }
            }
        }
    }
    lv_font_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_STATIC, 128, 128);
    if (lv_font_tex) {
        SDL_UpdateTexture(lv_font_tex, NULL, px, 128 * 4);
        SDL_SetTextureBlendMode(lv_font_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(lv_font_tex, SDL_SCALEMODE_NEAREST);
    }
}

static void
lv_draw_text(SDL_Renderer *r, float x, float y, float px, const char *s,
             int len, Uint8 cr, Uint8 cg, Uint8 cb) {
    lv_ensure_font(r);
    if (!lv_font_tex) {
        return;
    }
    SDL_SetTextureColorMod(lv_font_tex, cr, cg, cb);
    for (int ci = 0; ci < len; ++ci) {
        unsigned char ch = (unsigned char) s[ci];
        if (ch >= 128) {
            ch = '?';
        }
        SDL_FRect src = {(ch % 16) * 8.f, (ch / 16) * 16.f, 8.f, 16.f};
        SDL_FRect dst = {x + ci * 8 * px, y, 8 * px, 16 * px};
        SDL_RenderTexture(r, lv_font_tex, &src, &dst);
    }
}

void
sc_logview_render(struct sc_screen *screen) {
    if (!lv.open) {
        return;
    }
    SDL_SetAtomicInt(&lv_wake_pending, 0);
    SDL_Renderer *renderer = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);

    float x0 = sc_screen_drawer_left(screen);
    float pw = (float) w - x0;
    if (pw < 1) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Panel background + left divider + title strip.
    SDL_FRect panel = {x0 * scale, 0, pw * scale, (float) h * scale};
    SDL_SetRenderDrawColor(renderer, 14, 15, 18, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 70, 72, 80, 255);
    SDL_RenderLine(renderer, x0 * scale, 0, x0 * scale, (float) h * scale);

    SDL_Rect clip = {(int) (x0 * scale), 0, (int) (pw * scale), (int) (h * scale)};
    SDL_SetRenderClipRect(renderer, &clip);

    float mul = sc_conf.terminal_text_size > 0 ? sc_conf.terminal_text_size
                                               : SC_LV_FONT_MUL;
    float px = SDL_max(1.f, scale * mul);
    float line_h = 16 * px;
    float pad = 8 * scale;
    float left = x0 * scale + pad;
    float area_top = pad;
    float area_bot = (float) h * scale - pad;

    int out_rows = (int) ((area_bot - area_top) / line_h);
    if (out_rows < 1) {
        out_rows = 1;
    }
    float char_w = 8 * px;
    int cols = (int) ((pw * scale - 2 * pad) / char_w);
    if (cols < 1) {
        cols = 1;
    }
    if (cols > SC_LV_COLS) {
        cols = SC_LV_COLS;
    }

    // Build visible wrapped rows bottom-up under the lock, then draw them.
    static char dr[SC_LV_MAXDR][SC_LV_COLS];
    static int dr_len[SC_LV_MAXDR];
    static unsigned char dr_sev[SC_LV_MAXDR];
    int ndr = 0;
    int scroll;

    sc_mutex_lock(&lv.mutex);
    int total = lv.count;

    // Total wrapped rows, for scroll clamping.
    int totald = 0;
    for (int idx = 0; idx < total; ++idx) {
        int len = lv.lens[(lv.head + idx) % SC_LV_LINES];
        if (len > SC_LV_COLS) {
            len = SC_LV_COLS;
        }
        totald += len > 0 ? (len + cols - 1) / cols : 1;
    }
    int max_scroll = totald - out_rows;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (lv.scroll > max_scroll) {
        lv.scroll = max_scroll;
    }
    if (lv.scroll < 0) {
        lv.scroll = 0;
    }
    scroll = lv.scroll;

    int need = out_rows + scroll;
    for (int idx = total - 1; idx >= 0 && ndr < need && ndr < SC_LV_MAXDR;
         --idx) {
        int li = (lv.head + idx) % SC_LV_LINES;
        const char *p = lv.lines[li];
        int len = lv.lens[li];
        if (len > SC_LV_COLS) {
            len = SC_LV_COLS;
        }
        int rows = len > 0 ? (len + cols - 1) / cols : 1;
        for (int k = rows - 1; k >= 0 && ndr < need && ndr < SC_LV_MAXDR; --k) {
            int off = k * cols;
            int seglen = len - off;
            if (seglen < 0) {
                seglen = 0;
            }
            if (seglen > cols) {
                seglen = cols;
            }
            memcpy(dr[ndr], p + off, seglen);
            dr_len[ndr] = seglen;
            dr_sev[ndr] = lv.sev[li];
            ndr++;
        }
    }
    sc_mutex_unlock(&lv.mutex);

    // dr[0] is the bottom-most row; the viewport bottom is dr[scroll].
    float y = area_bot - line_h;
    for (int i = 0; i < out_rows; ++i) {
        int di = scroll + i;
        if (di >= ndr) {
            break;
        }
        Uint8 cr = 205, cg = 207, cb = 212;
        if (dr_sev[di] == 2) {
            cr = 235; cg = 120; cb = 120; // error: red
        } else if (dr_sev[di] == 1) {
            cr = 230; cg = 200; cb = 120; // warn: amber
        }
        lv_draw_text(renderer, left, y, px, dr[di], dr_len[di], cr, cg, cb);
        y -= line_h;
    }

    SDL_SetRenderClipRect(renderer, NULL);
}

bool
sc_logview_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    if (!lv.open) {
        return false;
    }
    float x0 = sc_screen_drawer_left(screen); // drawer left edge
    switch (event->type) {
        case SDL_EVENT_MOUSE_WHEEL: {
            float mx, my;
            SDL_GetMouseState(&mx, &my);
            if (mx >= x0) {
                lv.scroll += (int) (event->wheel.y * 3);
                if (lv.scroll < 0) {
                    lv.scroll = 0;
                }
                lv_wake();
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            // Swallow clicks inside the panel so they don't reach the device.
            return event->button.x >= x0;
        case SDL_EVENT_KEY_DOWN:
            if (event->key.key == SDLK_PAGEUP) {
                lv.scroll += 8;
                lv_wake();
                return true;
            }
            if (event->key.key == SDLK_PAGEDOWN) {
                lv.scroll -= 8;
                if (lv.scroll < 0) {
                    lv.scroll = 0;
                }
                lv_wake();
                return true;
            }
            if (event->key.key == SDLK_HOME) {
                lv.scroll = 1 << 30; // clamped to top on next render
                lv_wake();
                return true;
            }
            if (event->key.key == SDLK_END) {
                lv.scroll = 0; // newest
                lv_wake();
                return true;
            }
            if (event->key.key == SDLK_ESCAPE) {
                sc_logview_toggle(screen);
                return true;
            }
            return false;
        default:
            return false;
    }
}
