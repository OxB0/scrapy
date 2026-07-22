#ifndef SC_XAPK_H
#define SC_XAPK_H

#include "common.h"

#include <stdbool.h>

#include "util/intr.h"

// True if `path` looks like a bundled/split-APK archive we can install (by
// extension: .xapk, .apkm or .apks), as opposed to a plain .apk.
bool
sc_xapk_is_bundle(const char *path);

/**
 * Install a split-APK bundle (XAPK/APKM/APKS).
 *
 * The archive is a zip containing the base APK, its config splits and,
 * optionally, OBB expansion files under Android/obb/<package>/. The APKs are
 * installed together with `adb install-multiple` and any OBB files are pushed
 * to the device's shared storage.
 *
 * If `out_msg` is not NULL, `*out_msg` is set to a heap-allocated error summary
 * (to be freed by the caller) on failure, or NULL.
 *
 * Return true on success.
 */
bool
sc_xapk_install(struct sc_intr *intr, const char *serial, const char *path,
                char **out_msg);

#endif
