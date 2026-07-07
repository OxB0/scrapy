#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
#endif

#include "adb/adb.h"
#include "apps.h"
#include "userconf.h"
#include "events.h"
#include "font8x16_basic.h"
#include "screen.h"
#include "toolbar.h"
#include "util/log.h"
#include "util/process.h"
#include "util/thread.h"

#define SC_SH_LINES 600     // scrollback ring size
#define SC_SH_COLS  512     // max stored chars per line
#define SC_SH_INPUT 1024
#define SC_SH_MIN    300    // minimum drawer width (video-fit reservation)
#define SC_SH_MAXDR  400    // max wrapped display rows built per frame
#define SC_SH_FONT_MUL 1.8f // font size: pixels per source pixel (at scale 1)

// Width the window grows by when the drawer opens (config override, else 600).
static int
sh_target(void) {
    return sc_conf.shell_width > 0 ? sc_conf.shell_width : 600;
}

// Character columns that fit across the panel. The device is told this via
// `stty cols` so it wraps output itself; the renderer wraps to the same width.
static int
sh_wrap_cols(void) {
    int cols = (int) ((sh_target() - 16) / (8.f * SC_SH_FONT_MUL));
    return cols < 1 ? 1 : cols;
}

struct sc_shell {
    bool ready;
    const char *serial;

    bool open;
    float anim; // current drawer height (logical px)

    sc_pid pid;
    sc_pipe pin;
    sc_pipe pout;
    sc_pipe perr;
    sc_thread thread;
    bool thread_started;

    sc_mutex mutex; // protects the ring buffer + cur line
    char lines[SC_SH_LINES][SC_SH_COLS];
    int lens[SC_SH_LINES];
    int head;  // index of oldest line
    int count; // number of stored lines
    char cur[SC_SH_COLS]; // the current (live) line from the PTY
    int curlen;           // its length
    int curcol;           // the cursor column within it

    int scroll; // scrollback offset in lines (0 = bottom)

    int esc; // ANSI escape state: 0 none, 1 got ESC, 2 in CSI
};

static struct sc_shell g;

static void
shdbg(const char *fmt, ...) {
    const char *tmp = getenv("TEMP");
    if (!tmp) {
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s\\scrapy-shell.log", tmp);
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void
sc_shell_init(const char *serial) {
    memset(&g, 0, sizeof(g));
    g.serial = serial;
    g.pid = SC_PROCESS_NONE;
    sc_mutex_init(&g.mutex);
    g.ready = true;
}

// --- ring buffer (call under mutex) ---

static void
push_cur(void) {
    int slot = (g.head + g.count) % SC_SH_LINES;
    memcpy(g.lines[slot], g.cur, g.curlen);
    g.lens[slot] = g.curlen;
    if (g.count < SC_SH_LINES) {
        g.count++;
    } else {
        g.head = (g.head + 1) % SC_SH_LINES;
    }
    g.curlen = 0;
    g.curcol = 0;
}

// Feed one output byte into the single-line terminal model: printable chars
// overwrite at the cursor column (so the shell's own line editing renders
// correctly); \r moves to column 0; \b/DEL move left; ESC[K erases to end of
// line; other ANSI escapes (colors, cursor moves) are stripped.
static void
append_byte(char b) {
    if (g.esc == 1) {
        g.esc = (b == '[') ? 2 : 0;
        return;
    }
    if (g.esc == 2) {
        if ((unsigned char) b >= 0x40 && (unsigned char) b <= 0x7e) {
            if (b == 'K') {
                g.curlen = g.curcol; // erase to end of line
            }
            g.esc = 0;
        }
        return;
    }
    if (b == 0x1b) {
        g.esc = 1;
    } else if (b == '\n') {
        push_cur();
    } else if (b == '\r') {
        g.curcol = 0;
    } else if (b == 0x08 || b == 0x7f) {
        if (g.curcol > 0) {
            g.curcol--;
        }
    } else if (b == 0x07) {
        // bell: ignore
    } else if (b == '\t') {
        do {
            if (g.curcol < SC_SH_COLS - 1) {
                g.cur[g.curcol++] = ' ';
                if (g.curcol > g.curlen) {
                    g.curlen = g.curcol;
                }
            }
        } while (g.curcol % 4 != 0 && g.curcol < SC_SH_COLS - 1);
    } else if ((unsigned char) b >= 0x20) {
        if (g.curcol < SC_SH_COLS - 1) {
            g.cur[g.curcol++] = b;
            if (g.curcol > g.curlen) {
                g.curlen = g.curcol;
            }
        }
    }
}

// --- subprocess ---

// Wake the main render loop so the terminal repaints promptly. The loop uses
// SDL_WaitEvent and otherwise only repaints on video frames, so without this
// both shell output (from the reader thread) and typed echo would only appear
// on the next frame.
//
// Coalesce with a pending flag rather than a time throttle: push one repaint
// event and don't push another until it has been consumed (the flag is cleared
// at the start of sc_shell_render). A time throttle would swallow the device's
// echo that lands a few ms after a keystroke, leaving the display one character
// behind — exactly the bug this replaces. Coalescing bounds the render rate
// under a flood (e.g. logcat) without ever dropping the final state.
static SDL_AtomicInt g_wake_pending;

static void
shell_wake(void) {
    if (SDL_CompareAndSwapAtomicInt(&g_wake_pending, 0, 1)) {
        sc_push_event(SC_EVENT_SHELL_UPDATE);
    }
}

static int
shell_reader(void *userdata) {
    (void) userdata;
    char buf[4096];
    for (;;) {
        ssize_t r = sc_pipe_read(g.pout, buf, sizeof(buf));
        if (r <= 0) {
            break;
        }
        sc_mutex_lock(&g.mutex);
        g.scroll = 0; // new output: jump to the bottom
        for (ssize_t i = 0; i < r; ++i) {
            append_byte(buf[i]);
        }
        sc_mutex_unlock(&g.mutex);
        shell_wake();
    }
    return 0;
}

static void
shell_fail(const char *msg) {
    sc_mutex_lock(&g.mutex);
    for (const char *p = msg; *p; ++p) {
        append_byte(*p);
    }
    append_byte('\n');
    sc_mutex_unlock(&g.mutex);
}

static void
shell_start(void) {
    const char *adb = sc_adb_get_executable();
    bool spawned = false;

#ifdef _WIN32
    // Spawn adb ourselves with CREATE_NO_WINDOW: adb's interactive shell needs
    // a console, but scrcpy's shared spawner forces DETACHED_PROCESS (no console
    // at all), which makes `adb shell` exit immediately. CREATE_NO_WINDOW gives
    // a hidden console. stderr is merged into stdout.
    // "-t -t" forces a PTY (now that adb has a hidden console via
    // CREATE_NO_WINDOW): gives a prompt, line-buffered output, and — crucially —
    // Ctrl+C is delivered as SIGINT to the foreground command.
    //
    // The device command sets the terminal width with `stty cols` so the shell
    // and programs (ls, ps, ...) wrap their output to our panel, then `exec sh`
    // replaces it with a clean interactive shell — so no stty line is echoed.
    int cols = sh_wrap_cols();
    char cmdline[1024];
    if (g.serial) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" -s %s shell -t -t \"stty cols %d rows 40; exec sh\"",
                 adb, g.serial, cols);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" shell -t -t \"stty cols %d rows 40; exec sh\"", adb,
                 cols);
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
        shdbg("start: CreatePipe failed");
        shell_fail("(could not create pipes)");
        return;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    shdbg("start(win): cmdline=%s ok=%d err=%lu", cmdline, ok,
          ok ? 0UL : GetLastError());
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        shell_fail("(could not start adb shell)");
        return;
    }
    CloseHandle(pi.hThread);
    g.pid = pi.hProcess;
    g.pin = in_w;
    g.pout = out_r;
    spawned = true;
#else
    char initcmd[128];
    snprintf(initcmd, sizeof(initcmd), "stty cols %d rows 40; exec sh",
             sh_wrap_cols());
    const char *argv_serial[] = {adb, "-s", g.serial, "shell", "-t", "-t",
                                 initcmd, NULL};
    const char *argv_plain[] = {adb, "shell", "-t", "-t", initcmd, NULL};
    const char *const *argv = g.serial ? argv_serial : argv_plain;
    enum sc_process_result r =
        sc_process_execute_p((const char *const *) argv, &g.pid, 0,
                             &g.pin, &g.pout, NULL);
    shdbg("start: result=%d", (int) r);
    if (r != SC_PROCESS_SUCCESS) {
        g.pid = SC_PROCESS_NONE;
        shell_fail("(could not start adb shell)");
        return;
    }
    spawned = true;
#endif

    if (spawned && !sc_thread_create(&g.thread, shell_reader, "scrcpy-shell",
                                     NULL)) {
        LOGE("Could not start shell reader thread");
        return;
    }
    g.thread_started = true;
}

static void
shell_write(const char *data, int len) {
    if (g.pid == SC_PROCESS_NONE) {
        return;
    }
#ifdef _WIN32
    DWORD w;
    WriteFile(g.pin, data, (DWORD) len, &w, NULL);
#else
    ssize_t rc = write(g.pin, data, len);
    (void) rc;
#endif
}

void
sc_shell_toggle(struct sc_screen *screen) {
    if (!g.ready) {
        return;
    }
    if (!g.open && sc_apps_is_open()) {
        sc_apps_close(screen); // share the right-side region
    }
    g.open = !g.open;
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);
    if (g.open) {
        // Grow the window outward by the panel width and reserve that same
        // width, so the video area stays identical (no reflow, no lag). Clear
        // any aspect-ratio lock first, or SDL fights the width change.
        SDL_SetWindowAspectRatio(screen->window, 0.f, 0.f);
        g.anim = SC_SH_MIN;
        SDL_SetWindowSize(screen->window, w + sh_target(), h);
        if (g.pid == SC_PROCESS_NONE) {
            shell_start();
        }
        SDL_StartTextInput(screen->window);
    } else {
        g.anim = 0;
        int nw = w - sh_target();
        SDL_SetWindowSize(screen->window, nw < 200 ? 200 : nw, h);
        SDL_StopTextInput(screen->window);
    }
}

void
sc_shell_close(struct sc_screen *screen) {
    if (g.open) {
        sc_shell_toggle(screen);
    }
}

bool
sc_shell_is_open(void) {
    return g.open;
}

// The panel opens instantly (the window grows outward), so there is no
// per-frame animation to advance.
bool
sc_shell_step_anim(void) {
    return false;
}

int
sc_shell_reserved_width(struct sc_screen *screen) {
    (void) screen;
    // Reserve only a minimum for the video fit; the drawer actually renders from
    // the video's right edge to the window edge, so it grows with the window.
    return g.open ? SC_SH_MIN : 0;
}

// --- rendering ---

// Font atlas: all 128 glyphs of the Terminus 8x16 font baked once into a
// 128x128 texture (16 cols x 8 rows of 8x16 cells), so drawing a character is a
// single textured quad instead of dozens of filled rects.
static SDL_Texture *g_font_tex = NULL;

static void
ensure_font_tex(SDL_Renderer *r) {
    if (g_font_tex) {
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
    g_font_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STATIC, 128, 128);
    if (g_font_tex) {
        SDL_UpdateTexture(g_font_tex, NULL, px, 128 * 4);
        SDL_SetTextureBlendMode(g_font_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_font_tex, SDL_SCALEMODE_NEAREST);
    }
}

// px = pixels per source pixel. Each glyph is 8 wide x 16 tall in the atlas;
// characters advance by 8*px and are drawn 8*px x 16*px.
static void
draw_text(SDL_Renderer *r, float x, float y, float px, const char *s, int len,
          Uint8 cr, Uint8 cg, Uint8 cb) {
    ensure_font_tex(r);
    if (!g_font_tex) {
        return;
    }
    SDL_SetTextureColorMod(g_font_tex, cr, cg, cb);
    for (int ci = 0; ci < len; ++ci) {
        unsigned char ch = (unsigned char) s[ci];
        if (ch >= 128) {
            ch = '?';
        }
        SDL_FRect src = {(ch % 16) * 8.f, (ch / 16) * 16.f, 8.f, 16.f};
        SDL_FRect dst = {x + ci * 8 * px, y, 8 * px, 16 * px};
        SDL_RenderTexture(r, g_font_tex, &src, &dst);
    }
}

void
sc_shell_render(struct sc_screen *screen) {
    if (!g.open) {
        return;
    }
    // Clear the pending flag before snapshotting: any output/echo that lands
    // after this point re-arms shell_wake() and gets its own repaint, so the
    // final state is never lost to coalescing.
    SDL_SetAtomicInt(&g_wake_pending, 0);
    SDL_Renderer *renderer = screen->renderer;
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale <= 0) {
        scale = 1;
    }
    int w, h;
    SDL_GetWindowSize(screen->window, &w, &h);

    // The drawer occupies everything to the right of the video, so it fills any
    // extra window width.
    float x0 = screen->rect.x + screen->rect.w;
    float pw = (float) w - x0;
    if (pw < 1) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Right-side panel background + left divider.
    SDL_FRect panel = {x0 * scale, 0, pw * scale, (float) h * scale};
    SDL_SetRenderDrawColor(renderer, 16, 17, 20, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 70, 72, 80, 255);
    SDL_RenderLine(renderer, x0 * scale, 0, x0 * scale, (float) h * scale);

    // Clip text to the panel so long lines don't spill into the toolbar/mirror.
    SDL_Rect clip = {(int) (x0 * scale), 0, (int) (pw * scale), (int) (h * scale)};
    SDL_SetRenderClipRect(renderer, &clip);

    // Font size: pixels per source pixel.
    float px = SDL_max(1.f, scale * SC_SH_FONT_MUL);
    float line_h = 16 * px;
    float pad = 8 * scale;
    float left = x0 * scale + pad;

    float area_top = pad;
    float area_bot = (float) h * scale - pad;

    int out_rows = (int) ((area_bot - area_top) / line_h);
    if (out_rows < 1) {
        out_rows = 1;
    }

    // Word wrap: number of character columns that fit across the panel. Long
    // logical lines are split into segments of this width so nothing is clipped.
    float char_w = 8 * px;
    int cols = (int) ((pw * scale - 2 * pad) / char_w);
    if (cols < 1) {
        cols = 1;
    }
    if (cols > SC_SH_COLS) {
        cols = SC_SH_COLS;
    }

    // Build the visible display rows bottom-up under the lock (so a slow draw
    // never blocks the reader thread), then draw them outside it. Each logical
    // line expands into ceil(len/cols) wrapped rows; the live line (cur) also
    // carries the cursor.
    static char dr[SC_SH_MAXDR][SC_SH_COLS];
    static int dr_len[SC_SH_MAXDR];
    static int dr_cursor[SC_SH_MAXDR]; // cursor column in this row, or -1
    int ndr = 0;
    int scroll;

    sc_mutex_lock(&g.mutex);
    int total = g.count + 1;
    int curcol = g.curcol;

    // Total wrapped rows across all logical lines, for scroll clamping.
    int totald = 0;
    for (int idx = 0; idx < total; ++idx) {
        bool live = idx >= g.count;
        int len = live ? g.curlen : g.lens[(g.head + idx) % SC_SH_LINES];
        if (len > SC_SH_COLS) {
            len = SC_SH_COLS;
        }
        int rows = len > 0 ? (len + cols - 1) / cols : 1;
        if (live && curcol / cols + 1 > rows) {
            rows = curcol / cols + 1;
        }
        totald += rows;
    }
    int max_scroll = totald - out_rows;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (g.scroll > max_scroll) {
        g.scroll = max_scroll;
    }
    scroll = g.scroll;

    int need = out_rows + scroll;
    for (int idx = total - 1; idx >= 0 && ndr < need && ndr < SC_SH_MAXDR; --idx) {
        bool live = idx >= g.count;
        const char *p = live ? g.cur : g.lines[(g.head + idx) % SC_SH_LINES];
        int len = live ? g.curlen : g.lens[(g.head + idx) % SC_SH_LINES];
        if (len > SC_SH_COLS) {
            len = SC_SH_COLS;
        }
        int rows = len > 0 ? (len + cols - 1) / cols : 1;
        int curseg = -1;
        if (live) {
            curseg = curcol / cols;
            if (curseg + 1 > rows) {
                rows = curseg + 1;
            }
        }
        // Emit this line's segments bottom (last) to top (first).
        for (int k = rows - 1; k >= 0 && ndr < need && ndr < SC_SH_MAXDR; --k) {
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
            dr_cursor[ndr] = (live && k == curseg) ? (curcol - off) : -1;
            ndr++;
        }
    }
    sc_mutex_unlock(&g.mutex);

    // dr[0] is the bottom-most row; the viewport bottom is dr[scroll].
    float y = area_bot - line_h;
    for (int i = 0; i < out_rows; ++i) {
        int di = scroll + i;
        if (di >= ndr) {
            break;
        }
        draw_text(renderer, left, y, px, dr[di], dr_len[di], 205, 207, 212);
        if (dr_cursor[di] >= 0) {
            SDL_FRect cb = {left + dr_cursor[di] * 8 * px, y, 8 * px, 16 * px};
            SDL_SetRenderDrawColor(renderer, 150, 220, 150, 130);
            SDL_RenderFillRect(renderer, &cb);
        }
        y -= line_h;
    }

    SDL_SetRenderClipRect(renderer, NULL);
}

// --- input ---

// Send bytes / an escape sequence straight to the shell PTY.
static void
send_seq(const char *s) {
    shell_write(s, (int) strlen(s));
}

bool
sc_shell_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    if (!g.open) {
        return false;
    }
    switch (event->type) {
        case SDL_EVENT_TEXT_INPUT:
            // Character mode: forward typed text to the PTY, which echoes it
            // inline after its own prompt.
            shell_write(event->text.text, (int) strlen(event->text.text));
            g.scroll = 0;
            shell_wake();
            return true;
        case SDL_EVENT_MOUSE_WHEEL: {
            float mx, my;
            SDL_GetMouseState(&mx, &my);
            float x0 = screen->rect.x + screen->rect.w; // drawer left edge
            if (mx >= x0) {
                g.scroll += (int) (event->wheel.y * 3);
                if (g.scroll < 0) {
                    g.scroll = 0;
                }
                shell_wake();
                return true;
            }
            return false;
        }
        case SDL_EVENT_KEY_DOWN:
            // Scrollback and drawer control stay local.
            if (event->key.key == SDLK_PAGEUP) {
                g.scroll += 8;
                shell_wake();
                return true;
            }
            if (event->key.key == SDLK_PAGEDOWN) {
                g.scroll -= 8;
                if (g.scroll < 0) {
                    g.scroll = 0;
                }
                shell_wake();
                return true;
            }
            if (event->key.key == SDLK_ESCAPE) {
                sc_shell_toggle(screen);
                return true;
            }
            g.scroll = 0; // any key that goes to the shell jumps to the bottom
            shell_wake(); // repaint now, don't wait for the PTY echo round-trip
            switch (event->key.key) {
                case SDLK_RETURN:
                case SDLK_KP_ENTER: send_seq("\n"); return true;
                case SDLK_BACKSPACE: send_seq("\x7f"); return true;
                case SDLK_TAB: send_seq("\t"); return true;
                case SDLK_UP: send_seq("\x1b[A"); return true;
                case SDLK_DOWN: send_seq("\x1b[B"); return true;
                case SDLK_RIGHT: send_seq("\x1b[C"); return true;
                case SDLK_LEFT: send_seq("\x1b[D"); return true;
                case SDLK_HOME: send_seq("\x1b[H"); return true;
                case SDLK_END: send_seq("\x1b[F"); return true;
                case SDLK_DELETE: send_seq("\x1b[3~"); return true;
                case SDLK_C:
                    if (event->key.mod & SDL_KMOD_CTRL) {
                        send_seq("\x03");
                    }
                    return true;
                case SDLK_D:
                    if (event->key.mod & SDL_KMOD_CTRL) {
                        send_seq("\x04");
                    }
                    return true;
                case SDLK_V:
                    if (event->key.mod & SDL_KMOD_CTRL) {
                        char *clip = SDL_GetClipboardText();
                        if (clip) {
                            shell_write(clip, (int) strlen(clip));
                            SDL_free(clip);
                        }
                    }
                    return true;
                default:
                    return true; // consume all keys while the shell is open
            }
        default:
            return false;
    }
}

void
sc_shell_destroy(void) {
    if (!g.ready) {
        return;
    }
    if (g.pid != SC_PROCESS_NONE) {
        sc_process_terminate(g.pid);
        if (g.thread_started) {
            sc_thread_join(&g.thread, NULL);
        }
        sc_process_wait(g.pid, true);
        sc_pipe_close(g.pin);
        sc_pipe_close(g.pout);
    }
    if (g_font_tex) {
        SDL_DestroyTexture(g_font_tex);
        g_font_tex = NULL;
    }
    sc_mutex_destroy(&g.mutex);
    g.ready = false;
}
