#include "xapk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
# include <direct.h>
#else
# include <sys/stat.h>
# include <unistd.h>
#endif

#include "adb/adb.h"
#include "util/file.h"
#include "util/log.h"
#include "util/zip.h"

// Where OBB expansion files live inside the archive and on the device.
#define SC_XAPK_OBB_PREFIX "Android/obb/"
#define SC_XAPK_DEVICE_STORAGE "/sdcard/"

static bool
sc_xapk_mkdir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return true;
    }
#else
    if (mkdir(path, 0700) == 0) {
        return true;
    }
#endif
    return errno == EEXIST;
}

static void
sc_xapk_rmdir(const char *path) {
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

// Fill `buf` with the OS temporary directory, without a trailing separator.
static bool
sc_xapk_tmp_base(char *buf, size_t len) {
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD r = GetTempPathA(sizeof(tmp), tmp);
    if (r == 0 || r >= sizeof(tmp)) {
        return false;
    }
    // GetTempPath keeps the trailing backslash; drop it.
    while (r > 0 && (tmp[r - 1] == '\\' || tmp[r - 1] == '/')) {
        tmp[--r] = '\0';
    }
    return (size_t) snprintf(buf, len, "%s", tmp) < len;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) {
        tmp = "/tmp";
    }
    return (size_t) snprintf(buf, len, "%s", tmp) < len;
#endif
}

// Last path component of an in-archive (forward-slash) name.
static const char *
sc_xapk_basename(const char *name) {
    const char *base = name;
    for (const char *p = name; *p; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base;
}

static inline char
sc_xapk_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static bool
sc_xapk_ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lsuf = strlen(suffix);
    if (lsuf > ls) {
        return false;
    }
    const char *tail = s + ls - lsuf;
    for (size_t i = 0; i < lsuf; ++i) {
        if (sc_xapk_lower(tail[i]) != sc_xapk_lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool
sc_xapk_is_bundle(const char *path) {
    return sc_xapk_ends_with_ci(path, ".xapk")
        || sc_xapk_ends_with_ci(path, ".apkm")
        || sc_xapk_ends_with_ci(path, ".apks");
}

// Reject archive names that could escape the extraction/target directory.
static bool
sc_xapk_name_is_safe(const char *name) {
    if (name[0] == '/' || name[0] == '\\') {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (p[0] == '.' && p[1] == '.'
                && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')
                && (p == name || p[-1] == '/' || p[-1] == '\\')) {
            return false;
        }
    }
    return true;
}

struct sc_xapk_files {
    char **paths;       // heap paths, freed by the caller
    char **remotes;     // device paths (OBB only), NULL for APKs; freed too
    size_t count;
    size_t cap;
};

static bool
sc_xapk_files_push(struct sc_xapk_files *f, char *path, char *remote) {
    if (f->count == f->cap) {
        return false;
    }
    f->paths[f->count] = path;
    f->remotes[f->count] = remote;
    ++f->count;
    return true;
}

static void
sc_xapk_files_clear(struct sc_xapk_files *apks, struct sc_xapk_files *obbs,
                    const char *tmp_dir) {
    for (size_t i = 0; i < apks->count; ++i) {
        remove(apks->paths[i]);
        free(apks->paths[i]);
        free(apks->remotes[i]);
    }
    for (size_t i = 0; i < obbs->count; ++i) {
        remove(obbs->paths[i]);
        free(obbs->paths[i]);
        free(obbs->remotes[i]);
    }
    if (tmp_dir) {
        sc_xapk_rmdir(tmp_dir);
    }
}

static void
sc_xapk_set_msg(char **out_msg, const char *msg) {
    if (out_msg) {
        free(*out_msg);
        *out_msg = strdup(msg);
    }
}

bool
sc_xapk_install(struct sc_intr *intr, const char *serial, const char *path,
                char **out_msg) {
    if (out_msg) {
        *out_msg = NULL;
    }

    struct sc_zip *zip = sc_zip_open(path);
    if (!zip) {
        sc_xapk_set_msg(out_msg, "not a valid archive");
        return false;
    }

    size_t total = sc_zip_count(zip);

    // Create a private extraction directory in the OS temp folder.
    char tmp_base[512];
    char tmp_dir[512];
    if (!sc_xapk_tmp_base(tmp_base, sizeof(tmp_base))
            || (size_t) snprintf(tmp_dir, sizeof(tmp_dir), "%s%cscrapy-xapk",
                                 tmp_base, SC_PATH_SEPARATOR) >= sizeof(tmp_dir)
            || !sc_xapk_mkdir(tmp_dir)) {
        LOGE("Could not create XAPK extraction directory");
        sc_xapk_set_msg(out_msg, "could not create temp folder");
        sc_zip_close(zip);
        return false;
    }

    struct sc_xapk_files apks = {
        .paths = calloc(total, sizeof(char *)),
        .remotes = calloc(total, sizeof(char *)),
        .count = 0,
        .cap = total,
    };
    struct sc_xapk_files obbs = {
        .paths = calloc(total, sizeof(char *)),
        .remotes = calloc(total, sizeof(char *)),
        .count = 0,
        .cap = total,
    };
    bool ok = apks.paths && apks.remotes && obbs.paths && obbs.remotes;

    for (size_t i = 0; ok && i < total; ++i) {
        const char *name = sc_zip_entry_name(zip, i);
        bool is_apk = sc_xapk_ends_with_ci(name, ".apk");
        bool is_obb = sc_xapk_ends_with_ci(name, ".obb");
        if ((!is_apk && !is_obb) || !sc_xapk_name_is_safe(name)) {
            continue;
        }

        // Extract to a flat file named after the archive entry's basename.
        const char *base = sc_xapk_basename(name);
        char *local = sc_file_build_path(tmp_dir, base);
        if (!local) {
            ok = false;
            break;
        }
        if (!sc_zip_extract(zip, i, local)) {
            free(local);
            ok = false;
            break;
        }

        char *remote = NULL;
        struct sc_xapk_files *bucket = &apks;
        if (is_obb) {
            // Push OBB files to their original in-archive location under the
            // device's shared storage (adb push creates missing parents).
            size_t rlen = strlen(SC_XAPK_DEVICE_STORAGE) + strlen(name) + 1;
            remote = malloc(rlen);
            if (!remote) {
                free(local);
                ok = false;
                break;
            }
            snprintf(remote, rlen, "%s%s", SC_XAPK_DEVICE_STORAGE, name);
            bucket = &obbs;
        }

        if (!sc_xapk_files_push(bucket, local, remote)) {
            free(local);
            free(remote);
            ok = false;
            break;
        }
    }

    sc_zip_close(zip);

    if (!ok) {
        sc_xapk_set_msg(out_msg, "could not unpack archive");
        sc_xapk_files_clear(&apks, &obbs, tmp_dir);
        free(apks.paths); free(apks.remotes);
        free(obbs.paths); free(obbs.remotes);
        return false;
    }

    if (apks.count == 0) {
        LOGE("No APK found in %s", path);
        sc_xapk_set_msg(out_msg, "no APK in archive");
        sc_xapk_files_clear(&apks, &obbs, tmp_dir);
        free(apks.paths); free(apks.remotes);
        free(obbs.paths); free(obbs.remotes);
        return false;
    }

    LOGI("Installing %u APK(s) from %s...", (unsigned) apks.count, path);
    char *detail = NULL;
    ok = sc_adb_install_multiple(intr, serial,
                                 (const char *const *) apks.paths, apks.count,
                                 0, &detail);
    if (!ok) {
        sc_xapk_set_msg(out_msg, detail && *detail ? detail : "install failed");
    }

    // Push OBB expansion files only once the package itself is installed.
    for (size_t i = 0; ok && i < obbs.count; ++i) {
        LOGI("Pushing expansion file to %s...", obbs.remotes[i]);
        free(detail);
        detail = NULL;
        if (!sc_adb_push(intr, serial, obbs.paths[i], obbs.remotes[i], 0,
                         &detail)) {
            LOGE("Failed to push %s", obbs.remotes[i]);
            sc_xapk_set_msg(out_msg, "installed, but data files failed");
            ok = false;
        }
    }

    free(detail);
    sc_xapk_files_clear(&apks, &obbs, tmp_dir);
    free(apks.paths); free(apks.remotes);
    free(obbs.paths); free(obbs.remotes);
    return ok;
}
