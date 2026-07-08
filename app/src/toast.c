#include "toast.h"

#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "events.h"
#include "font8x8_basic.h"
#include "logview.h"
#include "screen.h"
#include "userconf.h"

#define SC_TOAST_MAX      512  // stored message length (bytes)
#define SC_TOAST_MS       7000 // time on screen before it disappears
#define SC_TOAST_FADE     500  // fade-out duration at the tail
#define SC_TOAST_ALPHA    235
#define SC_TOAST_MAXLINES 8    // wrap long messages across up to this many rows
#define SC_TOAST_BATCH    1024 // glyph pixels drawn per SDL_RenderFillRects call

struct sc_toast_msg {
    char text[SC_TOAST_MAX];
    bool error;
};

// Main-thread-only state (mutated when SC_EVENT_TOAST is handled, read while
// rendering — both on the main thread, so no lock is needed).
static struct {
    char text[SC_TOAST_MAX];
    bool error;
    bool active;
    Uint64 expire; // SDL_GetTicks() value at which it vanishes
} g_toast;

// Configured lifetime in ms (config is in seconds; 0 = built-in default).
static Uint64
toast_duration_ms(void) {
    if (sc_conf.notification_time > 0.f) {
        return (Uint64) (sc_conf.notification_time * 1000.f);
    }
    return SC_TOAST_MS;
}

void
sc_toast_show(const char *msg, bool error) {
    if (!msg || !sc_conf.notifications || sc_logview_is_open()) {
        // Notifications disabled, or the log drawer is open (it already shows
        // the same result as a log line): don't even post the event.
        return;
    }
    struct sc_toast_msg *m = malloc(sizeof(*m));
    if (!m) {
        return;
    }
    // Copy onto a single line: collapse runs of whitespace (adb output is often
    // multi-line) so the toast stays a tidy one-liner.
    size_t j = 0;
    bool prev_space = false;
    for (size_t i = 0; msg[i] && j < SC_TOAST_MAX - 1; ++i) {
        char c = msg[i];
        bool space = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        if (space) {
            if (j == 0 || prev_space) {
                continue; // trim leading + collapse interior whitespace
            }
            c = ' ';
        }
        m->text[j++] = c;
        prev_space = space;
    }
    while (j > 0 && m->text[j - 1] == ' ') {
        --j; // trim trailing space
    }
    m->text[j] = '\0';
    m->error = error;

    if (!sc_push_event_with_data(SC_EVENT_TOAST, m)) {
        free(m);
    }
}

// One-shot timer: repaint once the toast has expired so it clears even when the
// mirror is static (no video frames arriving to trigger a redraw).
static Uint32 SDLCALL
toast_expire_cb(void *userdata, SDL_TimerID id, Uint32 interval) {
    (void) userdata;
    (void) id;
    (void) interval;
    sc_push_event(SC_EVENT_TOAST); // NULL data: just a repaint tick
    return 0; // do not repeat
}

void
sc_toast_accept(void *data) {
    if (!data) {
        return; // expiry/repaint tick: nothing to adopt
    }
    struct sc_toast_msg *m = data;
    Uint64 dur = toast_duration_ms();
    memcpy(g_toast.text, m->text, sizeof(g_toast.text));
    g_toast.error = m->error;
    g_toast.active = true;
    g_toast.expire = SDL_GetTicks() + dur;
    free(m);

    SDL_AddTimer((Uint32) dur + 30, toast_expire_cb, NULL);
}

// Draw a string from the 8x8 font. Every lit pixel is a small rect, but they
// are submitted in bulk via SDL_RenderFillRects (one call per SC_TOAST_BATCH
// pixels) instead of one draw call each — otherwise a multi-line toast would
// issue thousands of draw calls per frame and visibly stutter the mirror.
static void
toast_text(SDL_Renderer *r, float x, float y, float px, const char *s, int len,
           Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_FRect batch[SC_TOAST_BATCH];
    int n = 0;
    for (int i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char) s[i];
        if (ch >= 128) {
            ch = '?';
        }
        const char *gl = font8x8_basic[ch];
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                if (gl[row] & (1 << col)) {
                    if (n == SC_TOAST_BATCH) {
                        SDL_RenderFillRects(r, batch, n);
                        n = 0;
                    }
                    batch[n].x = x + (i * 8 + col) * px;
                    batch[n].y = y + row * px;
                    batch[n].w = px;
                    batch[n].h = px;
                    ++n;
                }
            }
        }
    }
    if (n > 0) {
        SDL_RenderFillRects(r, batch, n);
    }
}

void
sc_toast_render(struct sc_screen *screen) {
    if (!g_toast.active || sc_logview_is_open()) {
        return; // hide any active toast while the log drawer is open
    }
    Uint64 now = SDL_GetTicks();
    if (now >= g_toast.expire) {
        g_toast.active = false;
        return;
    }

    SDL_Renderer *r = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);

    float mul = sc_conf.notification_text_size > 0.f
                    ? sc_conf.notification_text_size : 2.0f;
    float px = SDL_max(1.f, mul * scale); // font pixel size
    float ch_w = 8 * px;
    float glyph_h = 8 * px;
    float linegap = 4 * scale;
    float padx = 12 * scale;
    float pady = 9 * scale;
    float margin = 20 * scale;

    // Characters that fit on one line across the window.
    int maxc = (int) ((w * scale - 2 * margin - 2 * padx) / ch_w);
    if (maxc < 8) {
        maxc = 8;
    }
    if (maxc > SC_TOAST_MAX - 4) {
        maxc = SC_TOAST_MAX - 4; // keep line buffers safe
    }

    // Greedy word-wrap the message into up to SC_TOAST_MAXLINES rows, breaking
    // on spaces (hard-splitting any word longer than a line).
    char lines[SC_TOAST_MAXLINES][SC_TOAST_MAX];
    int linelen[SC_TOAST_MAXLINES];
    int nlines = 0;
    const char *p = g_toast.text;
    while (*p && nlines < SC_TOAST_MAXLINES) {
        while (*p == ' ') {
            ++p; // trim leading spaces
        }
        if (!*p) {
            break;
        }
        int avail = (int) strlen(p);
        int take = avail < maxc ? avail : maxc;
        if (take < avail) {
            // Prefer to break at the last space within the line.
            int brk = -1;
            for (int i = 0; i < take; ++i) {
                if (p[i] == ' ') {
                    brk = i;
                }
            }
            if (brk > 0) {
                take = brk;
            }
        }
        while (take > 0 && p[take - 1] == ' ') {
            --take; // trim trailing spaces
        }
        if (take <= 0) {
            take = 1; // never stall
        }
        memcpy(lines[nlines], p, take);
        lines[nlines][take] = '\0';
        linelen[nlines] = take;
        ++nlines;
        p += take;
    }
    if (nlines == 0) {
        return;
    }
    // If the message overflowed the line budget, mark the last line truncated.
    while (*p == ' ') {
        ++p;
    }
    if (*p) {
        int li = nlines - 1;
        int keep = linelen[li];
        if (keep > maxc - 3) {
            keep = maxc - 3;
        }
        if (keep < 0) {
            keep = 0;
        }
        lines[li][keep] = '.';
        lines[li][keep + 1] = '.';
        lines[li][keep + 2] = '.';
        lines[li][keep + 3] = '\0';
        linelen[li] = keep + 3;
    }

    int widest = 0;
    for (int i = 0; i < nlines; ++i) {
        if (linelen[i] > widest) {
            widest = linelen[i];
        }
    }

    float boxw = widest * ch_w + 2 * padx;
    float boxh = nlines * glyph_h + (nlines - 1) * linegap + 2 * pady;

    // Centered over the mirror area, clamped inside the window.
    float cx = (screen->rect.x + screen->rect.w / 2.f) * scale;
    float bx = cx - boxw / 2.f;
    float right = w * scale - margin;
    if (bx + boxw > right) {
        bx = right - boxw;
    }
    if (bx < margin) {
        bx = margin;
    }
    if (bx < 0) {
        bx = 0;
    }
    float by = h * scale - margin - boxh;

    Uint8 alpha = SC_TOAST_ALPHA;
    Uint64 remain = g_toast.expire - now;
    if (remain < SC_TOAST_FADE) {
        alpha = (Uint8) ((Uint64) SC_TOAST_ALPHA * remain / SC_TOAST_FADE);
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_FRect box = {bx, by, boxw, boxh};
    if (g_toast.error) {
        SDL_SetRenderDrawColor(r, 48, 22, 24, alpha);
    } else {
        SDL_SetRenderDrawColor(r, 22, 36, 27, alpha);
    }
    SDL_RenderFillRect(r, &box);
    if (g_toast.error) {
        SDL_SetRenderDrawColor(r, 214, 92, 92, alpha);
    } else {
        SDL_SetRenderDrawColor(r, 92, 202, 132, alpha);
    }
    SDL_RenderRect(r, &box);

    Uint8 tr = g_toast.error ? 245 : 226;
    Uint8 tg = g_toast.error ? 208 : 246;
    Uint8 tb = g_toast.error ? 208 : 226;
    float ty = by + pady;
    for (int i = 0; i < nlines; ++i) {
        toast_text(r, bx + padx, ty, px, lines[i], linelen[i], tr, tg, tb,
                   alpha);
        ty += glyph_h + linegap;
    }
}
