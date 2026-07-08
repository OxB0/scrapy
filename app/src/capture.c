#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include "adb/adb.h"
#include "userconf.h"
#include "events.h"
#include "toast.h"
#include "util/log.h"
#include "util/process.h"
#include "util/thread.h"

static char g_serial[128];
static bool g_ready;

static SDL_AtomicInt g_recording;
static SDL_AtomicInt g_awake; // device keep-awake state (mirrors the device)
static sc_pid g_rec_pid;
static char g_rec_devpath[256];
static char g_rec_pcpath[600];

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
    char sep = '\\';
    const char *h = getenv("USERPROFILE");
#else
    char sep = '/';
    const char *h = getenv("HOME");
#endif
    // A configured capture_dir overrides the home folder.
    if (sc_conf.capture_dir[0]) {
        h = sc_conf.capture_dir;
    }
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
set_toast(const char *msg, bool error) {
    sc_toast_show(msg, error);
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

// Like run_adb but captures stdout into `buf`. Safe here because the adb
// server is already running by the time capture is used (mirror connected).
static int
run_adb_out(const char *const *tail, char *buf, int bufsize) {
    const char *adb = capture_adb();
    if (!adb) {
        return 0;
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
    sc_pipe pout;
    if (sc_process_execute_p(argv, &pid, 0, NULL, &pout, NULL)
            != SC_PROCESS_SUCCESS) {
        return 0;
    }
    ssize_t r = sc_pipe_read_all(pout, buf, bufsize - 1);
    sc_pipe_close(pout);
    sc_process_wait(pid, true);
    if (r < 0) {
        r = 0;
    }
    buf[r] = '\0';
    return (int) r;
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
    set_toast(ok ? "Screenshot saved to home folder" : "Screenshot failed",
              !ok);
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
    set_toast(ok ? "Recording saved to home folder" : "Recording failed", !ok);
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
        set_toast("Could not start recording", true);
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
        set_toast("Could not start recording", true);
        return;
    }
    SDL_SetAtomicInt(&g_recording, 1);
    set_toast("Recording... (click Record again to stop)", false);
}

bool
sc_capture_recording(void) {
    return SDL_GetAtomicInt(&g_recording) != 0;
}

// --- keep awake ---

static int
stayawake_thread(void *userdata) {
    int on = (int) (intptr_t) userdata;
    const char *t[] = {"shell", "svc", "power", "stayon",
                       on ? "true" : "false", NULL};
    bool ok = run_adb(t);
    if (ok) {
        SDL_SetAtomicInt(&g_awake, on ? 1 : 0);
        set_toast(on ? "Screen will stay awake" : "Screen can sleep again",
                  false);
    } else {
        set_toast("Could not change keep-awake", true);
    }
    return 0;
}

// Read the device's current stay-awake setting so the button reflects reality.
static int
awake_query_thread(void *userdata) {
    (void) userdata;
    char buf[128];
    const char *t[] = {"shell", "settings", "get", "global",
                       "stay_on_while_plugged_in", NULL};
    int n = run_adb_out(t, buf, sizeof(buf));
    int val = (n > 0) ? atoi(buf) : 0; // 0 = off, nonzero bitmask = on
    SDL_SetAtomicInt(&g_awake, val != 0 ? 1 : 0);
    sc_push_event(SC_EVENT_SHELL_UPDATE); // repaint so the button reflects it
    return 0;
}

void
sc_capture_stay_awake(bool on) {
    if (!g_ready) {
        return;
    }
    SDL_Thread *t = SDL_CreateThread(stayawake_thread, "sc-awake",
                                     (void *) (intptr_t) (on ? 1 : 0));
    if (t) {
        SDL_DetachThread(t);
    }
}

bool
sc_capture_awake_is_on(void) {
    return SDL_GetAtomicInt(&g_awake) != 0;
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
    SDL_SetAtomicInt(&g_awake, 0);
    g_ready = true;

    // Read the device's real keep-awake state so the button starts correct.
    SDL_Thread *t = SDL_CreateThread(awake_query_thread, "sc-awake-q", NULL);
    if (t) {
        SDL_DetachThread(t);
    }
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
    g_ready = false;
}
