/* zutil.c -- target dependent utility functions for the compression library
 * Simplified for MWOS kernel: 64-bit, no libc, custom kmalloc/kfree.
 */

#include "zutil.h"
#include "memory.h"

/* ---------------- Error messages ---------------- */

z_const char * const z_errmsg[10] = {
    (z_const char *)"need dictionary",     /* Z_NEED_DICT       2  */
    (z_const char *)"stream end",          /* Z_STREAM_END      1  */
    (z_const char *)"",                    /* Z_OK              0  */
    (z_const char *)"file error",          /* Z_ERRNO         (-1) */
    (z_const char *)"stream error",        /* Z_STREAM_ERROR  (-2) */
    (z_const char *)"data error",          /* Z_DATA_ERROR    (-3) */
    (z_const char *)"insufficient memory", /* Z_MEM_ERROR     (-4) */
    (z_const char *)"buffer error",        /* Z_BUF_ERROR     (-5) */
    (z_const char *)"incompatible version",/* Z_VERSION_ERROR (-6) */
    (z_const char *)""
};

const char * ZEXPORT zlibVersion(void) {
    return ZLIB_VERSION;
}

uLong ZEXPORT zlibCompileFlags(void) {
    uLong flags = 0;

    switch ((int)(sizeof(uInt))) {
    case 2:     break;
    case 4:     flags += 1;     break;
    case 8:     flags += 2;     break;
    default:    flags += 3;
    }
    switch ((int)(sizeof(uLong))) {
    case 2:     break;
    case 4:     flags += 1 << 2;        break;
    case 8:     flags += 2 << 2;        break;
    default:    flags += 3 << 2;
    }
    switch ((int)(sizeof(voidpf))) {
    case 2:     break;
    case 4:     flags += 1 << 4;        break;
    case 8:     flags += 2 << 4;        break;
    default:    flags += 3 << 4;
    }
    switch ((int)(sizeof(z_off_t))) {
    case 2:     break;
    case 4:     flags += 1 << 6;        break;
    case 8:     flags += 2 << 6;        break;
    default:    flags += 3 << 6;
    }

    return flags;
}

/* exported to allow conversion of error code to string for compress() and
 * uncompress()
 */
const char * ZEXPORT zError(int err) {
    return ERR_MSG(err);
}

/* ---------------- Memory helpers ---------------- 

void ZLIB_INTERNAL zmemcpy(Bytef* dest, const Bytef* source, uInt len) {
    if (len == 0) return;
    do {
        *dest++ = *source++;
    } while (--len != 0);
}

int ZLIB_INTERNAL zmemcmp(const Bytef* s1, const Bytef* s2, uInt len) {
    uInt j;

    for (j = 0; j < len; j++) {
        if (s1[j] != s2[j])
            return 2 * (s1[j] > s2[j]) - 1;
    }
    return 0;
}

void ZLIB_INTERNAL zmemzero(Bytef* dest, uInt len) {
    if (len == 0) return;
    do {
        *dest++ = 0;
    } while (--len != 0);
}

/* ---------------- Allocator for inflate ---------------- */


voidpf ZLIB_INTERNAL zcalloc(voidpf opaque, unsigned items, unsigned size)
{
    (void)opaque;

    ulg bsize = (ulg)items * size;
    return (voidpf)kmalloc(bsize);
}

void ZLIB_INTERNAL zcfree(voidpf opaque, voidpf ptr)
{
    (void)opaque;

    if (ptr) {
        kfree(ptr);
    }
}
