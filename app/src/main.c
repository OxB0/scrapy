#include "common.h"

#include <stdbool.h>
#include <stdio.h>
#ifdef HAVE_V4L2
# include <libavdevice/avdevice.h>
#endif
#include <SDL3/SDL.h>

#include "cli.h"
#include "userconf.h"
#include "events.h"
#include "options.h"
#include "picker.h"
#include "scrcpy.h"
#ifdef HAVE_USB
# include "usb/scrcpy_otg.h"
#endif
#include "util/log.h"
#include "util/net.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include "util/str.h"
#endif

static int
main_scrcpy(int argc, char *argv[]) {
#ifdef _WIN32
    // disable buffering, we want logs immediately
    // even line buffering (setvbuf() with mode _IOLBF) is not sufficient
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
#endif

    printf("scrcpy " SCRCPY_VERSION
           " <https://github.com/Genymobile/scrcpy>\n");

    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_UNDEFINED,
    };

#ifndef NDEBUG
    args.opts.log_level = SC_LOG_LEVEL_DEBUG;
#endif

    enum scrcpy_exit_code ret;

    if (!scrcpy_parse_args(&args, argc, argv)) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_set_log_level(args.opts.log_level);

    if (args.help) {
        scrcpy_print_usage(argv[0]);
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

    if (args.version) {
        scrcpy_print_version();
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

#ifdef SCRCPY_LAVF_REQUIRES_REGISTER_ALL
    av_register_all();
#endif

#ifdef HAVE_V4L2
    if (args.opts.v4l2_device) {
        avdevice_register_all();
    }
#endif

    if (!net_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_log_configure();

    if (!sc_main_thread_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto net_cleanup;
    }

    // Load user configuration (toolbar buttons, drawer widths, capture dir, ...)
    sc_config_load();

    // Picker mode: when no device was requested on the command line, show a
    // startup picker (opens a window immediately, waits when nothing is
    // connected, auto-connects a lone device, lets the user choose when several
    // are present). Then loop: if the device disconnects, drop back to the
    // picker so the app stays open and reconnects automatically. Closing either
    // window quits.
    bool picker_mode = !args.opts.serial;
    int quick_fail_streak = 0;
    for (;;) {
        if (picker_mode) {
            char *picked = NULL;
            if (!sc_picker_run(&picked)) {
                // User closed the picker without choosing a device.
                ret = SCRCPY_EXIT_SUCCESS;
                break;
            }
            if (picked) {
                args.opts.serial = picked;
            }
        }

        Uint64 session_start = SDL_GetTicks();
#ifdef HAVE_USB
        ret = args.opts.otg ? scrcpy_otg(&args.opts) : scrcpy(&args.opts);
#else
        ret = scrcpy(&args.opts);
#endif
        bool quick = SDL_GetTicks() - session_start < 3000;

        // Any non-SUCCESS exit in picker mode means the device is gone: a clean
        // unplug returns DISCONNECTED, but an abrupt one races into a demuxer/
        // controller error (FAILURE). Both should return to the picker so the
        // app keeps running across reconnects. Only give up if connections keep
        // failing immediately (a broken setup) — otherwise we'd loop invisibly.
        if (picker_mode && ret != SCRCPY_EXIT_SUCCESS) {
            free((void *) args.opts.serial);
            args.opts.serial = NULL;
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); // drop stale events
            if (ret == SCRCPY_EXIT_FAILURE && quick) {
                if (++quick_fail_streak >= 3) {
                    break; // repeated immediate failures: stop retrying
                }
                SDL_Delay(700); // back off before retrying
            } else {
                quick_fail_streak = 0; // a real session ran; keep reconnecting
            }
            continue;
        }
        break; // user quit, or a fixed -s device: done
    }

    sc_main_thread_destroy();

net_cleanup:
    net_cleanup();

end:
    if (args.pause_on_exit == SC_PAUSE_ON_EXIT_TRUE ||
            (args.pause_on_exit == SC_PAUSE_ON_EXIT_IF_ERROR &&
                ret != SCRCPY_EXIT_SUCCESS)) {
        printf("Press Enter to continue...\n");
        getchar();
    }

    return ret;
}

int
main(int argc, char *argv[]) {
#ifndef _WIN32
    return main_scrcpy(argc, argv);
#else
    (void) argc;
    (void) argv;
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        LOG_OOM();
        return SCRCPY_EXIT_FAILURE;
    }

    char **argv_utf8 = malloc((wargc + 1) * sizeof(*argv_utf8));
    if (!argv_utf8) {
        LOG_OOM();
        LocalFree(wargv);
        return SCRCPY_EXIT_FAILURE;
    }

    argv_utf8[wargc] = NULL;

    for (int i = 0; i < wargc; ++i) {
        argv_utf8[i] = sc_str_from_wchars(wargv[i]);
        if (!argv_utf8[i]) {
            LOG_OOM();
            for (int j = 0; j < i; ++j) {
                free(argv_utf8[j]);
            }
            LocalFree(wargv);
            free(argv_utf8);
            return SCRCPY_EXIT_FAILURE;
        }
    }

    LocalFree(wargv);

    int ret = main_scrcpy(wargc, argv_utf8);

    for (int i = 0; i < wargc; ++i) {
        free(argv_utf8[i]);
    }
    free(argv_utf8);

    return ret;
#endif
}
