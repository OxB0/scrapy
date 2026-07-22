#include "util/zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "util/log.h"

// 64-bit file offsets: XAPKs (with bundled OBB data) routinely exceed 2 GB.
#ifdef _WIN32
# define sc_fseek(f, off, whence) _fseeki64((f), (off), (whence))
# define sc_ftell(f) _ftelli64(f)
#else
# define sc_fseek(f, off, whence) fseeko((f), (off), (whence))
# define sc_ftell(f) ftello(f)
#endif

#define SC_ZIP_EOCD_SIG 0x06054b50u // end of central directory record
#define SC_ZIP_CEN_SIG  0x02014b50u // central directory file header
#define SC_ZIP_LOC_SIG  0x04034b50u // local file header

struct sc_zip_entry {
    char *name;
    uint16_t method;        // 0 = stored, 8 = deflate
    uint32_t comp_size;
    uint32_t local_offset;  // offset of the local file header
};

struct sc_zip {
    FILE *file;
    struct sc_zip_entry *entries;
    size_t count;
};

static inline uint16_t
rd16(const uint8_t *p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static inline uint32_t
rd32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}

static void
sc_zip_free_entries(struct sc_zip *zip) {
    for (size_t i = 0; i < zip->count; ++i) {
        free(zip->entries[i].name);
    }
    free(zip->entries);
    zip->entries = NULL;
    zip->count = 0;
}

struct sc_zip *
sc_zip_open(const char *path) {
    struct sc_zip *zip = calloc(1, sizeof(*zip));
    if (!zip) {
        LOG_OOM();
        return NULL;
    }

    zip->file = fopen(path, "rb");
    if (!zip->file) {
        LOGE("Could not open archive: %s", path);
        free(zip);
        return NULL;
    }

    if (sc_fseek(zip->file, 0, SEEK_END) != 0) {
        goto error;
    }
    int64_t file_size = sc_ftell(zip->file);
    if (file_size < 22) { // minimum EOCD size
        LOGE("Archive too small to be a zip: %s", path);
        goto error;
    }

    // The EOCD is at the end, preceded by an optional comment of up to 65535
    // bytes. Scan the tail for its signature.
    size_t tail_len = 22 + 65535;
    if ((int64_t) tail_len > file_size) {
        tail_len = (size_t) file_size;
    }
    uint8_t *tail = malloc(tail_len);
    if (!tail) {
        LOG_OOM();
        goto error;
    }
    if (sc_fseek(zip->file, file_size - (int64_t) tail_len, SEEK_SET) != 0
            || fread(tail, 1, tail_len, zip->file) != tail_len) {
        free(tail);
        goto error;
    }

    long eocd = -1;
    for (long i = (long) tail_len - 22; i >= 0; --i) {
        if (rd32(tail + i) == SC_ZIP_EOCD_SIG) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        LOGE("No zip end-of-central-directory found: %s", path);
        free(tail);
        goto error;
    }

    uint16_t total = rd16(tail + eocd + 10);
    uint32_t cd_size = rd32(tail + eocd + 12);
    uint32_t cd_off = rd32(tail + eocd + 16);
    free(tail);

    if (cd_size == 0 || total == 0) {
        LOGE("Empty zip archive: %s", path);
        goto error;
    }

    uint8_t *cd = malloc(cd_size);
    if (!cd) {
        LOG_OOM();
        goto error;
    }
    if (sc_fseek(zip->file, cd_off, SEEK_SET) != 0
            || fread(cd, 1, cd_size, zip->file) != cd_size) {
        free(cd);
        goto error;
    }

    zip->entries = calloc(total, sizeof(*zip->entries));
    if (!zip->entries) {
        LOG_OOM();
        free(cd);
        goto error;
    }

    size_t p = 0;
    size_t n = 0;
    for (; n < total && p + 46 <= cd_size; ++n) {
        if (rd32(cd + p) != SC_ZIP_CEN_SIG) {
            break;
        }
        uint16_t method = rd16(cd + p + 10);
        uint32_t comp_size = rd32(cd + p + 20);
        uint16_t name_len = rd16(cd + p + 28);
        uint16_t extra_len = rd16(cd + p + 30);
        uint16_t comment_len = rd16(cd + p + 32);
        uint32_t local_off = rd32(cd + p + 42);
        if (p + 46 + name_len > cd_size) {
            break;
        }
        char *name = malloc(name_len + 1u);
        if (!name) {
            LOG_OOM();
            break;
        }
        memcpy(name, cd + p + 46, name_len);
        name[name_len] = '\0';

        zip->entries[n].name = name;
        zip->entries[n].method = method;
        zip->entries[n].comp_size = comp_size;
        zip->entries[n].local_offset = local_off;

        p += 46u + name_len + extra_len + comment_len;
    }
    free(cd);
    zip->count = n;

    if (zip->count == 0) {
        LOGE("Could not read any zip entry: %s", path);
        goto error;
    }

    return zip;

error:
    sc_zip_close(zip);
    return NULL;
}

void
sc_zip_close(struct sc_zip *zip) {
    if (!zip) {
        return;
    }
    if (zip->file) {
        fclose(zip->file);
    }
    sc_zip_free_entries(zip);
    free(zip);
}

size_t
sc_zip_count(struct sc_zip *zip) {
    return zip->count;
}

const char *
sc_zip_entry_name(struct sc_zip *zip, size_t i) {
    if (i >= zip->count) {
        return NULL;
    }
    return zip->entries[i].name;
}

// Copy `size` bytes straight from the archive stream to `out`.
static bool
sc_zip_copy_stored(FILE *in, FILE *out, uint32_t size) {
    uint8_t buf[65536];
    while (size > 0) {
        size_t want = size < sizeof(buf) ? size : sizeof(buf);
        size_t got = fread(buf, 1, want, in);
        if (got == 0) {
            return false;
        }
        if (fwrite(buf, 1, got, out) != got) {
            return false;
        }
        size -= (uint32_t) got;
    }
    return true;
}

// Inflate `comp_size` bytes of raw deflate data from `in` to `out`.
static bool
sc_zip_inflate(FILE *in, FILE *out, uint32_t comp_size) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    // Negative window bits: raw deflate, no zlib header (zip stores it raw).
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return false;
    }

    uint8_t inbuf[65536];
    uint8_t outbuf[65536];
    bool ok = true;
    int ret = Z_OK;
    uint32_t remaining = comp_size;

    while (remaining > 0 && ret != Z_STREAM_END) {
        size_t want = remaining < sizeof(inbuf) ? remaining : sizeof(inbuf);
        size_t got = fread(inbuf, 1, want, in);
        if (got == 0) {
            ok = false;
            break;
        }
        remaining -= (uint32_t) got;

        strm.next_in = inbuf;
        strm.avail_in = (uInt) got;
        do {
            strm.next_out = outbuf;
            strm.avail_out = sizeof(outbuf);
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                ok = false;
                break;
            }
            size_t have = sizeof(outbuf) - strm.avail_out;
            if (have && fwrite(outbuf, 1, have, out) != have) {
                ok = false;
                break;
            }
        } while (strm.avail_out == 0 && ok);
        if (!ok) {
            break;
        }
    }

    inflateEnd(&strm);
    return ok;
}

bool
sc_zip_extract(struct sc_zip *zip, size_t i, const char *dest_path) {
    if (i >= zip->count) {
        return false;
    }
    struct sc_zip_entry *e = &zip->entries[i];

    size_t name_len = strlen(e->name);
    if (name_len == 0 || e->name[name_len - 1] == '/') {
        return false; // directory entry
    }
    if (e->method != 0 && e->method != 8) {
        LOGE("Unsupported zip compression method %u for %s", e->method,
             e->name);
        return false;
    }

    // Read the local header to find where the entry data actually starts (its
    // name/extra field lengths may differ from the central directory).
    uint8_t lh[30];
    if (sc_fseek(zip->file, e->local_offset, SEEK_SET) != 0
            || fread(lh, 1, sizeof(lh), zip->file) != sizeof(lh)
            || rd32(lh) != SC_ZIP_LOC_SIG) {
        LOGE("Invalid local header for %s", e->name);
        return false;
    }
    uint16_t lh_name_len = rd16(lh + 26);
    uint16_t lh_extra_len = rd16(lh + 28);
    int64_t data_off = (int64_t) e->local_offset + 30 + lh_name_len
                     + lh_extra_len;
    if (sc_fseek(zip->file, data_off, SEEK_SET) != 0) {
        return false;
    }

    FILE *out = fopen(dest_path, "wb");
    if (!out) {
        LOGE("Could not create %s", dest_path);
        return false;
    }

    bool ok = e->method == 0
            ? sc_zip_copy_stored(zip->file, out, e->comp_size)
            : sc_zip_inflate(zip->file, out, e->comp_size);

    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        LOGE("Failed to extract %s", e->name);
        remove(dest_path);
    }
    return ok;
}
