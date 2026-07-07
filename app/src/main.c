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
#else
#include <unistd.h>
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
    // are present). On disconnect, re-exec the process so the picker starts
    // fresh with clean adb state.
    bool picker_mode = !args.opts.serial;
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

#ifdef HAVE_USB
        ret = args.opts.otg ? scrcpy_otg(&args.opts) : scrcpy(&args.opts);
#else
        ret = scrcpy(&args.opts);
#endif

        // On disconnect in picker mode, re-exec the whole process so adb
        // state (transport ids, server handle, etc.) starts completely clean.
        if (picker_mode && ret != SCRCPY_EXIT_SUCCESS) {
            sc_main_thread_destroy();
            net_cleanup();

#ifdef _WIN32
            char self[1024];
            if (GetModuleFileNameA(NULL, self, sizeof(self))) {
                STARTUPINFOA si;
                memset(&si, 0, sizeof(si));
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi;
                char *cmdline = strdup(GetCommandLineA());
                if (cmdline && CreateProcessA(self, cmdline, NULL, NULL,
                        FALSE, 0, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    free(cmdline);
                    return SCRCPY_EXIT_SUCCESS;
                }
                free(cmdline);
            }
#else
            execv(argv[0], argv);
#endif
            // re-exec failed, fall through to normal exit
            break;
        }
        break;
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
