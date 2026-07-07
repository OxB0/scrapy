#include "capture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include "adb/adb.h"
#include "events.h"
#include "util/log.h"
#include "util/process.h"
#include "util/thread.h"

static char g_serial[128];
static bool g_ready;

static SDL_AtomicInt g_recording;
static sc_pid g_rec_pid;
static char g_rec_devpath[256];
static char g_rec_pcpath[600];

static sc_mutex g_toast_mutex;
static char g_toast[128];
static Uint64 g_toast_until;

// --- helpers ---

static const char *
capture_adb(void) {
    const char *adb = sc_adb_get_executable();
    if (!adb) {
        sc_adb_init();
        adb = sc_adb_get_executable();
    }
    return adb;
}

static void
home_path(char *buf, size_t n, const char *name) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
    char sep = '\\';
#else
    const char *h = getenv("HOME");
    char sep = '/';
#endif
    if (!h || !*h) {
        h = ".";
    }
    snprintf(buf, n, "%s%c%s", h, sep, name);
}

static void
timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, n, "%Y%m%d-%H%M%S", &tmv);
}

static void
set_toast(const char *msg) {
    sc_mutex_lock(&g_toast_mutex);
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until = SDL_GetTicks() + 3000;
    sc_mutex_unlock(&g_toast_mutex);
    sc_push_event(SC_EVENT_SHELL_UPDATE); // wake the render loop to show it
}

bool
sc_capture_toast(char *out, size_t out_size) {
    bool show = false;
    sc_mutex_lock(&g_toast_mutex);
    if (SDL_GetTicks() < g_toast_until) {
        snprintf(out, out_size, "%s", g_toast);
        show = true;
    }
    sc_mutex_unlock(&g_toast_mutex);
    return show;
}

// Run `adb [-s serial] <tail...>` and block until it finishes. Returns true on
// exit code 0.
static bool
run_adb(const char *const *tail) {
    const char *adb = capture_adb();
    if (!adb) {
        return false;
    }
    const char *argv[24];
    int n = 0;
    argv[n++] = adb;
    if (g_serial[0]) {
        argv[n++] = "-s";
        argv[n++] = g_serial;
    }
    for (int i = 0; tail[i] && n < 22; ++i) {
        argv[n++] = tail[i];
    }
    argv[n] = NULL;

    sc_pid pid;
    if (sc_process_execute_p(argv, &pid, 0, NULL, NULL, NULL)
            != SC_PROCESS_SUCCESS) {
        return false;
    }
    return sc_process_wait(pid, true) == 0;
}

// --- screenshot ---

#ifdef _WIN32
static bool
do_screencap(const char *path) {
    const char *adb = capture_adb();
    if (!adb) {
        return false;
    }
    char cmd[1024];
    if (g_serial[0]) {
        snprintf(cmd, sizeof(cmd), "\"%s\" -s %s exec-out screencap -p", adb,
                 g_serial);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\" exec-out screencap -p", adb);
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE fh = CreateFileA(path, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = fh;   // the PNG goes straight to the file
    si.hStdError = nul;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    CloseHandle(fh);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!ok) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}
#else
static bool
do_screencap(const char *path) {
    const char *adb = capture_adb();
    if (!adb) {
        return false;
    }
    char cmd[1024];
    if (g_serial[0]) {
        snprintf(cmd, sizeof(cmd),
                 "'%s' -s %s exec-out screencap -p > '%s'", adb, g_serial, path);
    } else {
        snprintf(cmd, sizeof(cmd), "'%s' exec-out screencap -p > '%s'", adb,
                 path);
    }
    const char *argv[] = {"sh", "-c", cmd, NULL};
    sc_pid pid;
    if (sc_process_execute_p(argv, &pid, 0, NULL, NULL, NULL)
            != SC_PROCESS_SUCCESS) {
        return false;
    }
    return sc_process_wait(pid, true) == 0;
}
#endif

static int
shot_thread(void *userdata) {
    (void) userdata;
    char ts[32];
    timestamp(ts, sizeof(ts));
    char name[64];
    snprintf(name, sizeof(name), "scrcpy-%s.png", ts);
    char path[600];
    home_path(path, sizeof(path), name);
    bool ok = do_screencap(path);
    set_toast(ok ? "Screenshot saved to home folder" : "Screenshot failed");
    return 0;
}

void
sc_capture_screenshot(void) {
    if (!g_ready) {
        return;
    }
    SDL_Thread *t = SDL_CreateThread(shot_thread, "sc-shot", NULL);
    if (t) {
        SDL_DetachThread(t);
    }
}

// --- recording ---

static int
rec_stop_thread(void *userdata) {
    (void) userdata;
    // SIGINT lets screenrecord finalize a valid mp4 before exiting.
    const char *pk[] = {"shell", "pkill", "-INT", "screenrecord", NULL};
    run_adb(pk);
    sc_process_wait(g_rec_pid, true); // wait for it to finish writing
    const char *pull[] = {"pull", g_rec_devpath, g_rec_pcpath, NULL};
    bool ok = run_adb(pull);
    const char *rm[] = {"shell", "rm", g_rec_devpath, NULL};
    run_adb(rm);
    SDL_SetAtomicInt(&g_recording, 0);
    set_toast(ok ? "Recording saved to home folder" : "Recording failed");
    return 0;
}

void
sc_capture_record_toggle(void) {
    if (!g_ready) {
        return;
    }
    if (SDL_GetAtomicInt(&g_recording)) {
        SDL_Thread *t = SDL_CreateThread(rec_stop_thread, "sc-rec-stop", NULL);
        if (t) {
            SDL_DetachThread(t);
        }
        return;
    }

    char ts[32];
    timestamp(ts, sizeof(ts));
    snprintf(g_rec_devpath, sizeof(g_rec_devpath), "/sdcard/scrcpy-%s.mp4", ts);
    char name[64];
    snprintf(name, sizeof(name), "scrcpy-%s.mp4", ts);
    home_path(g_rec_pcpath, sizeof(g_rec_pcpath), name);

    const char *adb = capture_adb();
    if (!adb) {
        set_toast("Could not start recording");
        return;
    }
    const char *argv[16];
    int n = 0;
    argv[n++] = adb;
    if (g_serial[0]) {
        argv[n++] = "-s";
        argv[n++] = g_serial;
    }
    argv[n++] = "shell";
    argv[n++] = "screenrecord";
    argv[n++] = "--bit-rate";
    argv[n++] = "8000000";
    argv[n++] = g_rec_devpath;
    argv[n] = NULL;
    if (sc_process_execute_p(argv, &g_rec_pid, 0, NULL, NULL, NULL)
            != SC_PROCESS_SUCCESS) {
        set_toast("Could not start recording");
        return;
    }
    SDL_SetAtomicInt(&g_recording, 1);
    set_toast("Recording... (click Record again to stop)");
}

bool
sc_capture_recording(void) {
    return SDL_GetAtomicInt(&g_recording) != 0;
}

// --- lifecycle ---

void
sc_capture_init(const char *serial) {
    if (serial) {
        snprintf(g_serial, sizeof(g_serial), "%s", serial);
    } else {
        g_serial[0] = '\0';
    }
    SDL_SetAtomicInt(&g_recording, 0);
    g_toast_until = 0;
    sc_mutex_init(&g_toast_mutex);
    g_ready = true;
}

void
sc_capture_destroy(void) {
    if (!g_ready) {
        return;
    }
    if (SDL_GetAtomicInt(&g_recording)) {
        // Best effort: stop the device recording so it isn't left running.
        const char *pk[] = {"shell", "pkill", "-INT", "screenrecord", NULL};
        run_adb(pk);
        sc_process_wait(g_rec_pid, true);
        SDL_SetAtomicInt(&g_recording, 0);
    }
    sc_mutex_destroy(&g_toast_mutex);
    g_ready = false;
}
