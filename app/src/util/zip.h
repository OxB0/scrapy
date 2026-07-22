#ifndef SC_ZIP_H
#define SC_ZIP_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

// Minimal read-only ZIP reader (store + deflate), enough to unpack XAPK/APKM
// bundles. It reads the central directory up-front and streams each entry to
// disk on demand, so large archives are not held in memory.
struct sc_zip;

// Open a ZIP archive for reading. Returns NULL on error.
struct sc_zip *
sc_zip_open(const char *path);

void
sc_zip_close(struct sc_zip *zip);

// Number of entries in the archive.
size_t
sc_zip_count(struct sc_zip *zip);

// In-archive path of entry `i` (always forward-slash separated), or NULL if out
// of range. The pointer stays valid until sc_zip_close().
const char *
sc_zip_entry_name(struct sc_zip *zip, size_t i);

// Extract entry `i` to `dest_path` on the local filesystem. Returns false on
// error (including if the entry is a directory or uses an unsupported
// compression method).
bool
sc_zip_extract(struct sc_zip *zip, size_t i, const char *dest_path);

#endif
