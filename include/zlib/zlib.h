/* zlib.h -- interface of the 'zlib' general purpose compression library
  version 1.3.1.2, December 8th, 2025

  Copyright (C) 1995-2025 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu


  The data format used by the zlib library is described by RFCs (Request for
  Comments) 1950 to 1952 at https://datatracker.ietf.org/doc/html/rfc1950
  (zlib format), rfc1951 (deflate format) and rfc1952 (gzip format).
*/

#ifndef ZLIB_H
#define ZLIB_H

#ifdef ZLIB_BUILD
#  include <zconf.h>
#else
# include "zconf.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ZLIB_VERSION "1.3.1.2-audit"
#define ZLIB_VERNUM 0x1312
#define ZLIB_VER_MAJOR 1
#define ZLIB_VER_MINOR 3
#define ZLIB_VER_REVISION 1
#define ZLIB_VER_SUBREVISION 2


typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void   (*free_func)(voidpf opaque, voidpf address);

struct internal_state;

typedef struct z_stream_s {
    z_const Bytef *next_in;     /* next input byte */
    uInt     avail_in;  /* number of bytes available at next_in */
    uLong    total_in;  /* total number of input bytes read so far */

    Bytef    *next_out; /* next output byte will go here */
    uInt     avail_out; /* remaining free space at next_out */
    uLong    total_out; /* total number of bytes output so far */

    z_const char *msg;  /* last error message, NULL if no error */
    struct internal_state FAR *state; /* not visible by applications */

    alloc_func zalloc;  /* used to allocate the internal state */
    free_func  zfree;   /* used to free the internal state */
    voidpf     opaque;  /* private data object passed to zalloc and zfree */

    int     data_type;  /* best guess about the data type: binary or text
                           for deflate, or the decoding state for inflate */
    uLong   adler;      /* Adler-32 or CRC-32 value of the uncompressed data */
    uLong   reserved;   /* reserved for future use */
} z_stream;

typedef z_stream FAR *z_streamp;

typedef struct gz_header_s {
    int     text;       /* true if compressed data believed to be text */
    uLong   time;       /* modification time */
    int     xflags;     /* extra flags (not used when writing a gzip file) */
    int     os;         /* operating system */
    Bytef   *extra;     /* pointer to extra field or Z_NULL if none */
    uInt    extra_len;  /* extra field length (valid if extra != Z_NULL) */
    uInt    extra_max;  /* space at extra (only when reading header) */
    Bytef   *name;      /* pointer to zero-terminated file name or Z_NULL */
    uInt    name_max;   /* space at name (only when reading header) */
    Bytef   *comment;   /* pointer to zero-terminated comment or Z_NULL */
    uInt    comm_max;   /* space at comment (only when reading header) */
    int     hcrc;       /* true if there was or will be a header crc */
    int     done;       /* true when done reading gzip header (not used
                           when writing a gzip file) */
} gz_header;

typedef gz_header FAR *gz_headerp;


                        /* constants */

#define Z_NO_FLUSH      0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH    2
#define Z_FULL_FLUSH    3
#define Z_FINISH        4
#define Z_BLOCK         5
#define Z_TREES         6
/* Allowed flush values; see deflate() and inflate() below for details */

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NEED_DICT     2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)
/* Return codes for the compression/decompression functions. Negative values
 * are errors, positive values are used for special but normal events.
 */

#define Z_NO_COMPRESSION         0
#define Z_BEST_SPEED             1
#define Z_BEST_COMPRESSION       9
#define Z_DEFAULT_COMPRESSION  (-1)
/* compression levels */

#define Z_FILTERED            1
#define Z_HUFFMAN_ONLY        2
#define Z_RLE                 3
#define Z_FIXED               4
#define Z_DEFAULT_STRATEGY    0
/* compression strategy; see deflateInit2() below for details */

#define Z_BINARY   0
#define Z_TEXT     1
#define Z_ASCII    Z_TEXT   /* for compatibility with 1.2.2 and earlier */
#define Z_UNKNOWN  2
/* Possible values of the data_type field for deflate() */

#define Z_DEFLATED   8
/* The deflate compression method (the only one supported in this version) */

#define Z_NULL  0  /* for initializing zalloc, zfree, opaque */

#define zlib_version zlibVersion()
/* for compatibility with versions < 1.0.2 */


                        /* basic functions */

ZEXTERN const char * ZEXPORT zlibVersion(void);
/* The application can compare zlibVersion and ZLIB_VERSION for consistency.
   If the first character differs, the library code actually used is not
   compatible with the zlib.h header file used by the application.  This check
   is automatically made by deflateInit and inflateInit.
 */



ZEXTERN int ZEXPORT deflate(z_streamp strm, int flush);


ZEXTERN int ZEXPORT deflateEnd(z_streamp strm);




ZEXTERN int ZEXPORT inflate(z_streamp strm, int flush);


ZEXTERN int ZEXPORT inflateEnd(z_streamp strm);


                        /* Advanced functions */



ZEXTERN int ZEXPORT deflateSetDictionary(z_streamp strm,
                                         const Bytef *dictionary,
                                         uInt  dictLength);

ZEXTERN int ZEXPORT deflateGetDictionary(z_streamp strm,
                                         Bytef *dictionary,
                                         uInt  *dictLength);

ZEXTERN int ZEXPORT deflateCopy(z_streamp dest,
                                z_streamp source);

ZEXTERN int ZEXPORT deflateReset(z_streamp strm);

ZEXTERN int ZEXPORT deflateParams(z_streamp strm,
                                  int level,
                                  int strategy);

ZEXTERN int ZEXPORT deflateTune(z_streamp strm,
                                int good_length,
                                int max_lazy,
                                int nice_length,
                                int max_chain);

ZEXTERN uLong ZEXPORT deflateBound(z_streamp strm, uLong sourceLen);
ZEXTERN z_size_t ZEXPORT deflateBound_z(z_streamp strm, z_size_t sourceLen);

ZEXTERN int ZEXPORT deflatePending(z_streamp strm,
                                   unsigned *pending,
                                   int *bits);

ZEXTERN int ZEXPORT deflateUsed(z_streamp strm,
                                int *bits);

ZEXTERN int ZEXPORT deflatePrime(z_streamp strm,
                                 int bits,
                                 int value);

ZEXTERN int ZEXPORT deflateSetHeader(z_streamp strm,
                                     gz_headerp head);


ZEXTERN int ZEXPORT inflateSetDictionary(z_streamp strm,
                                         const Bytef *dictionary,
                                         uInt  dictLength);

ZEXTERN int ZEXPORT inflateGetDictionary(z_streamp strm,
                                         Bytef *dictionary,
                                         uInt  *dictLength);

ZEXTERN int ZEXPORT inflateSync(z_streamp strm);

ZEXTERN int ZEXPORT inflateCopy(z_streamp dest,
                                z_streamp source);

ZEXTERN int ZEXPORT inflateReset(z_streamp strm);

ZEXTERN int ZEXPORT inflateReset2(z_streamp strm,
                                  int windowBits);

ZEXTERN int ZEXPORT inflatePrime(z_streamp strm,
                                 int bits,
                                 int value);

ZEXTERN long ZEXPORT inflateMark(z_streamp strm);

ZEXTERN int ZEXPORT inflateGetHeader(z_streamp strm,
                                     gz_headerp head);


typedef unsigned (*in_func)(void FAR *,
                            z_const unsigned char FAR * FAR *);
typedef int (*out_func)(void FAR *, unsigned char FAR *, unsigned);

ZEXTERN int ZEXPORT inflateBack(z_streamp strm,
                                in_func in, void FAR *in_desc,
                                out_func out, void FAR *out_desc);

ZEXTERN int ZEXPORT inflateBackEnd(z_streamp strm);

ZEXTERN uLong ZEXPORT zlibCompileFlags(void);
/* Return flags indicating compile-time options.

    Type sizes, two bits each, 00 = 16 bits, 01 = 32, 10 = 64, 11 = other:
     1.0: size of uInt
     3.2: size of uLong
     5.4: size of voidpf (pointer)
     7.6: size of z_off_t

    Compiler, assembler, and debug options:
     8: ZLIB_DEBUG
     9: ASMV or ASMINF -- use ASM code
     10: ZLIB_WINAPI -- exported functions use the WINAPI calling convention
     11: 0 (reserved)

    One-time table building (smaller code, but not thread-safe if true):
     12: BUILDFIXED -- build static block decoding tables when needed
     13: DYNAMIC_CRC_TABLE -- build CRC calculation tables when needed
     14,15: 0 (reserved)

    Library content (indicates missing functionality):
     16: NO_GZCOMPRESS -- gz* functions cannot compress (to avoid linking
                          deflate code when not needed)
     17: NO_GZIP -- deflate can't write gzip streams, and inflate can't detect
                    and decode gzip streams (to avoid linking crc code)
     18-19: 0 (reserved)

    Operation variations (changes in library functionality):
     20: PKZIP_BUG_WORKAROUND -- slightly more permissive inflate
     21: FASTEST -- deflate algorithm with only one, lowest compression level
     22,23: 0 (reserved)

    The sprintf variant used by gzprintf (all zeros is best):
     24: 0 = vs*, 1 = s* -- 1 means limited to 20 arguments after the format
     25: 0 = *nprintf, 1 = *printf -- 1 means gzprintf() is not secure!
     26: 0 = returns value, 1 = void -- 1 means inferred string length returned
     27: 0 = gzprintf() present, 1 = not -- 1 means gzprintf() returns an error

    Remainder:
     28-31: 0 (reserved)
 */

#ifndef Z_SOLO

                        /* utility functions */


ZEXTERN int ZEXPORT compress(Bytef *dest,   uLongf *destLen,
                             const Bytef *source, uLong sourceLen);

ZEXTERN int ZEXPORT compress2(Bytef *dest,   uLongf *destLen,
                              const Bytef *source, uLong sourceLen,
                              int level);

ZEXTERN uLong ZEXPORT compressBound(uLong sourceLen);
ZEXTERN z_size_t ZEXPORT compressBound_z(z_size_t sourceLen);

ZEXTERN int ZEXPORT uncompress(Bytef *dest,   uLongf *destLen,
                               const Bytef *source, uLong sourceLen);

ZEXTERN int ZEXPORT uncompress2(Bytef *dest,   uLongf *destLen,
                                const Bytef *source, uLong *sourceLen);

                        /* gzip file access functions */


typedef struct gzFile_s *gzFile;    /* semi-opaque gzip file descriptor */


ZEXTERN gzFile ZEXPORT gzdopen(int fd, const char *mode);

ZEXTERN int ZEXPORT gzbuffer(gzFile file, unsigned size);

ZEXTERN int ZEXPORT gzsetparams(gzFile file, int level, int strategy);

ZEXTERN int ZEXPORT gzread(gzFile file, voidp buf, unsigned len);

ZEXTERN z_size_t ZEXPORT gzfread(voidp buf, z_size_t size, z_size_t nitems,
                                 gzFile file);

ZEXTERN int ZEXPORT gzwrite(gzFile file, voidpc buf, unsigned len);

ZEXTERN z_size_t ZEXPORT gzfwrite(voidpc buf, z_size_t size,
                                  z_size_t nitems, gzFile file);

#if defined(STDC) || defined(Z_HAVE_STDARG_H)
ZEXTERN int ZEXPORTVA gzprintf(gzFile file, const char *format, ...);
#else
ZEXTERN int ZEXPORTVA gzprintf();
#endif

ZEXTERN int ZEXPORT gzputs(gzFile file, const char *s);

ZEXTERN char * ZEXPORT gzgets(gzFile file, char *buf, int len);

ZEXTERN int ZEXPORT gzputc(gzFile file, int c);

ZEXTERN int ZEXPORT gzgetc(gzFile file);

ZEXTERN int ZEXPORT gzungetc(int c, gzFile file);

ZEXTERN int ZEXPORT gzflush(gzFile file, int flush);


ZEXTERN int ZEXPORT gzrewind(gzFile file);



ZEXTERN int ZEXPORT gzeof(gzFile file);

ZEXTERN int ZEXPORT gzdirect(gzFile file);

ZEXTERN int ZEXPORT gzclose(gzFile file);

ZEXTERN int ZEXPORT gzclose_r(gzFile file);
ZEXTERN int ZEXPORT gzclose_w(gzFile file);

ZEXTERN const char * ZEXPORT gzerror(gzFile file, int *errnum);

ZEXTERN void ZEXPORT gzclearerr(gzFile file);

#endif /* !Z_SOLO */

                        /* checksum functions */


ZEXTERN uLong ZEXPORT adler32(uLong adler, const Bytef *buf, uInt len);

ZEXTERN uLong ZEXPORT adler32_z(uLong adler, const Bytef *buf,
                                z_size_t len);


ZEXTERN uLong ZEXPORT crc32(uLong crc, const Bytef *buf, uInt len);

ZEXTERN uLong ZEXPORT crc32_z(uLong crc, const Bytef *buf,
                              z_size_t len);



ZEXTERN uLong ZEXPORT crc32_combine_op(uLong crc1, uLong crc2, uLong op);


                        /* various hacks, don't look :) */

/* deflateInit and inflateInit are macros to allow checking the zlib version
 * and the compiler's view of z_stream:
 */
ZEXTERN int ZEXPORT deflateInit_(z_streamp strm, int level,
                                 const char *version, int stream_size);
ZEXTERN int ZEXPORT inflateInit_(z_streamp strm,
                                 const char *version, int stream_size);
ZEXTERN int ZEXPORT deflateInit2_(z_streamp strm, int  level, int  method,
                                  int windowBits, int memLevel,
                                  int strategy, const char *version,
                                  int stream_size);
ZEXTERN int ZEXPORT inflateInit2_(z_streamp strm, int  windowBits,
                                  const char *version, int stream_size);
ZEXTERN int ZEXPORT inflateBackInit_(z_streamp strm, int windowBits,
                                     unsigned char FAR *window,
                                     const char *version,
                                     int stream_size);
#ifdef Z_PREFIX_SET
#  define z_deflateInit(strm, level) \
          deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))
#  define z_inflateInit(strm) \
          inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#  define z_deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
          deflateInit2_((strm),(level),(method),(windowBits),(memLevel),\
                        (strategy), ZLIB_VERSION, (int)sizeof(z_stream))
#  define z_inflateInit2(strm, windowBits) \
          inflateInit2_((strm), (windowBits), ZLIB_VERSION, \
                        (int)sizeof(z_stream))
#  define z_inflateBackInit(strm, windowBits, window) \
          inflateBackInit_((strm), (windowBits), (window), \
                           ZLIB_VERSION, (int)sizeof(z_stream))
#else
#  define deflateInit(strm, level) \
          deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))
#  define inflateInit(strm) \
          inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#  define deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
          deflateInit2_((strm),(level),(method),(windowBits),(memLevel),\
                        (strategy), ZLIB_VERSION, (int)sizeof(z_stream))
#  define inflateInit2(strm, windowBits) \
          inflateInit2_((strm), (windowBits), ZLIB_VERSION, \
                        (int)sizeof(z_stream))
#  define inflateBackInit(strm, windowBits, window) \
          inflateBackInit_((strm), (windowBits), (window), \
                           ZLIB_VERSION, (int)sizeof(z_stream))
#endif

#ifndef Z_SOLO

/* gzgetc() macro and its supporting function and exposed data structure.  Note
 * that the real internal state is much larger than the exposed structure.
 * This abbreviated structure exposes just enough for the gzgetc() macro.  The
 * user should not mess with these exposed elements, since their names or
 * behavior could change in the future, perhaps even capriciously.  They can
 * only be used by the gzgetc() macro.  You have been warned.
 */
struct gzFile_s {
    unsigned have;
    unsigned char *next;
    z_off64_t pos;
};
ZEXTERN int ZEXPORT gzgetc_(gzFile file);       /* backward compatibility */
#ifdef Z_PREFIX_SET
#  undef z_gzgetc
#  define z_gzgetc(g) \
          ((g)->have ? ((g)->have--, (g)->pos++, *((g)->next)++) : (gzgetc)(g))
#else
#  define gzgetc(g) \
          ((g)->have ? ((g)->have--, (g)->pos++, *((g)->next)++) : (gzgetc)(g))
#endif

/* provide 64-bit offset functions if _LARGEFILE64_SOURCE defined, and/or
 * change the regular functions to 64 bits if _FILE_OFFSET_BITS is 64 (if
 * both are true, the application gets the *64 functions, and the regular
 * functions are changed to 64 bits) -- in case these are set on systems
 * without large file support, _LFS64_LARGEFILE must also be true
 */
#ifdef Z_LARGE64
   ZEXTERN gzFile ZEXPORT gzopen64(const char *, const char *);
   ZEXTERN z_off64_t ZEXPORT gzseek64(gzFile, z_off64_t, int);
   ZEXTERN z_off64_t ZEXPORT gztell64(gzFile);
   ZEXTERN z_off64_t ZEXPORT gzoffset64(gzFile);
   ZEXTERN uLong ZEXPORT adler32_combine64(uLong, uLong, z_off64_t);
   ZEXTERN uLong ZEXPORT crc32_combine64(uLong, uLong, z_off64_t);
   ZEXTERN uLong ZEXPORT crc32_combine_gen64(z_off64_t);
#endif

#if !defined(ZLIB_INTERNAL) && defined(Z_WANT64)
#  ifdef Z_PREFIX_SET
#    define z_gzopen z_gzopen64
#    define z_gzseek z_gzseek64
#    define z_gztell z_gztell64
#    define z_gzoffset z_gzoffset64
#    define z_adler32_combine z_adler32_combine64
#    define z_crc32_combine z_crc32_combine64
#    define z_crc32_combine_gen z_crc32_combine_gen64
#  else
#    define gzopen gzopen64
#    define gzseek gzseek64
#    define gztell gztell64
#    define gzoffset gzoffset64
#    define adler32_combine adler32_combine64
#    define crc32_combine crc32_combine64
#    define crc32_combine_gen crc32_combine_gen64
#  endif
#  ifndef Z_LARGE64
     ZEXTERN gzFile ZEXPORT gzopen64(const char *, const char *);
     ZEXTERN z_off_t ZEXPORT gzseek64(gzFile, z_off_t, int);
     ZEXTERN z_off_t ZEXPORT gztell64(gzFile);
     ZEXTERN z_off_t ZEXPORT gzoffset64(gzFile);
     ZEXTERN uLong ZEXPORT adler32_combine64(uLong, uLong, z_off64_t);
     ZEXTERN uLong ZEXPORT crc32_combine64(uLong, uLong, z_off64_t);
     ZEXTERN uLong ZEXPORT crc32_combine_gen64(z_off64_t);
#  endif
#else
   ZEXTERN gzFile ZEXPORT gzopen(const char *, const char *);
   ZEXTERN z_off_t ZEXPORT gzseek(gzFile, z_off_t, int);
   ZEXTERN z_off_t ZEXPORT gztell(gzFile);
   ZEXTERN z_off_t ZEXPORT gzoffset(gzFile);
   ZEXTERN uLong ZEXPORT adler32_combine(uLong, uLong, z_off_t);
   ZEXTERN uLong ZEXPORT crc32_combine(uLong, uLong, z_off_t);
   ZEXTERN uLong ZEXPORT crc32_combine_gen(z_off_t);
#endif

#else /* Z_SOLO */

   ZEXTERN uLong ZEXPORT adler32_combine(uLong, uLong, z_off_t);
   ZEXTERN uLong ZEXPORT crc32_combine(uLong, uLong, z_off_t);
   ZEXTERN uLong ZEXPORT crc32_combine_gen(z_off_t);

#endif /* !Z_SOLO */

/* undocumented functions */
ZEXTERN const char   * ZEXPORT zError(int);
ZEXTERN int            ZEXPORT inflateSyncPoint(z_streamp);
ZEXTERN const z_crc_t FAR * ZEXPORT get_crc_table(void);
ZEXTERN int            ZEXPORT inflateUndermine(z_streamp, int);
ZEXTERN int            ZEXPORT inflateValidate(z_streamp, int);
ZEXTERN unsigned long  ZEXPORT inflateCodesUsed(z_streamp);
ZEXTERN int            ZEXPORT inflateResetKeep(z_streamp);
ZEXTERN int            ZEXPORT deflateResetKeep(z_streamp);
#if defined(_WIN32) && !defined(Z_SOLO)
ZEXTERN gzFile         ZEXPORT gzopen_w(const wchar_t *path,
                                        const char *mode);
#endif
#if defined(STDC) || defined(Z_HAVE_STDARG_H)
#  ifndef Z_SOLO
ZEXTERN int            ZEXPORTVA gzvprintf(gzFile file,
                                           const char *format,
                                           va_list va);
#  endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZLIB_H */
