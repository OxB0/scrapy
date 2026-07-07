#include "picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "adb/adb.h"
#include "font8x8_basic.h"
#include "util/log.h"
#include "util/process.h"

#define SC_PK_MAX 16

struct pk_dev {
    char serial[128];
    char model[128];
};

// Run `adb devices -l` and parse the connected ("device" state) devices.
static int
pk_list(struct pk_dev *out, int max) {
    const char *adb = sc_adb_get_executable();
    if (!adb) {
        // adb is normally initialized later in the mirror flow; the picker runs
        // first, so make sure the executable path is resolved.
        sc_adb_init();
        adb = sc_adb_get_executable();
        if (!adb) {
            return 0;
        }
    }
    const char *argv[] = {adb, "devices", "-l", NULL};
    sc_pid pid;
    sc_pipe pout;
    enum sc_process_result r =
        sc_process_execute_p(argv, &pid, 0, NULL, &pout, NULL);
    if (r != SC_PROCESS_SUCCESS) {
        return 0;
    }
    char buf[8192];
    ssize_t n = sc_pipe_read_all(pout, buf, sizeof(buf) - 1);
    sc_pipe_close(pout);
    sc_process_wait(pid, true);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    int count = 0;
    char *line = buf;
    while (line && *line && count < max) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        // Lines look like: "SERIAL   device product:... model:... ..."
        if (!strstr(line, "List of devices")) {
            char serial[128] = {0};
            char state[64] = {0};
            if (sscanf(line, "%127s %63s", serial, state) == 2
                    && !strcmp(state, "device")) {
                snprintf(out[count].serial, sizeof(out[count].serial), "%s",
                         serial);
                out[count].model[0] = '\0';
                char *m = strstr(line, "model:");
                if (m) {
                    sscanf(m + 6, "%127[^ \t\r]", out[count].model);
                    for (char *p = out[count].model; *p; ++p) {
                        if (*p == '_') {
                            *p = ' ';
                        }
                    }
                }
                count++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return count;
}

static void
pk_text(SDL_Renderer *r, float x, float y, float px, const char *s,
        Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (int i = 0; s[i]; ++i) {
        unsigned char ch = (unsigned char) s[i];
        if (ch >= 128) {
            ch = '?';
        }
        const char *g = font8x8_basic[ch];
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                if (g[row] & (1 << col)) {
                    SDL_FRect p = {x + (i * 8 + col) * px, y + row * px,
                                   px, px};
                    SDL_RenderFillRect(r, &p);
                }
            }
        }
    }
}

#define PK_W 460
#define PK_ROW_Y 70
#define PK_ROW_H 46
#define PK_ROW_GAP 8

static SDL_FRect
pk_row_rect(int i) {
    SDL_FRect rc = {14, (float) (PK_ROW_Y + i * (PK_ROW_H + PK_ROW_GAP)),
                    PK_W - 28, PK_ROW_H};
    return rc;
}

bool
sc_picker_run(char **out_serial) {
    struct pk_dev devs[SC_PK_MAX];
    int ndev = pk_list(devs, SC_PK_MAX);

    // Exactly one device: connect immediately, no window.
    if (ndev == 1) {
        *out_serial = strdup(devs[0].serial);
        return true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGW("Picker: SDL init failed, falling back to default selection");
        *out_serial = NULL;
        return true;
    }

    int h = PK_ROW_Y + SC_PK_MAX * (PK_ROW_H + PK_ROW_GAP) + 20;
    if (h > 640) {
        h = 640;
    }
    SDL_Window *win = SDL_CreateWindow("scrapy \xe2\x80\x94 select a device",
                                       PK_W, h, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!win || !ren) {
        if (win) {
            SDL_DestroyWindow(win);
        }
        *out_serial = NULL;
        return true;
    }

    bool selected = false;
    char *result = NULL;
    bool running = true;
    Uint64 last_poll = SDL_GetTicks();

    while (running) {
        // Re-scan for devices about once a second.
        Uint64 now = SDL_GetTicks();
        if (now - last_poll > 900) {
            ndev = pk_list(devs, SC_PK_MAX);
            last_poll = now;
            if (ndev == 1) {
                result = strdup(devs[0].serial);
                selected = true;
                break;
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                       && e.button.button == SDL_BUTTON_LEFT) {
                for (int i = 0; i < ndev; ++i) {
                    SDL_FRect rc = pk_row_rect(i);
                    if (e.button.x >= rc.x && e.button.x < rc.x + rc.w
                            && e.button.y >= rc.y && e.button.y < rc.y + rc.h) {
                        result = strdup(devs[i].serial);
                        selected = true;
                        running = false;
                        break;
                    }
                }
            }
        }
        if (!running) {
            break;
        }

        float mx = -1, my = -1;
        SDL_GetMouseState(&mx, &my);

        SDL_SetRenderDrawColor(ren, 24, 25, 29, 255);
        SDL_RenderClear(ren);

        if (ndev == 0) {
            pk_text(ren, 20, 24, 2.f, "Waiting for a device...", 220, 222, 228);
            pk_text(ren, 20, 60, 1.5f,
                    "Connect a device over USB (with USB debugging on).",
                    150, 152, 160);
        } else {
            pk_text(ren, 20, 22, 2.f, "Select a device", 235, 236, 240);
            for (int i = 0; i < ndev; ++i) {
                SDL_FRect rc = pk_row_rect(i);
                bool hover = mx >= rc.x && mx < rc.x + rc.w && my >= rc.y
                          && my < rc.y + rc.h;
                SDL_SetRenderDrawColor(ren, hover ? 52 : 38, hover ? 54 : 40,
                                       hover ? 64 : 47, 255);
                SDL_RenderFillRect(ren, &rc);
                SDL_SetRenderDrawColor(ren, 80, 82, 92, 255);
                SDL_RenderRect(ren, &rc);
                const char *name = devs[i].model[0] ? devs[i].model
                                                     : devs[i].serial;
                pk_text(ren, rc.x + 12, rc.y + 8, 2.f, name, 240, 240, 245);
                pk_text(ren, rc.x + 12, rc.y + 26, 1.5f, devs[i].serial,
                        150, 190, 150);
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);

    if (selected) {
        *out_serial = result;
        return true;
    }
    return false; // window closed without a selection
}
