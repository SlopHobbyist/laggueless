#ifndef ME_ARCHIVE_7Z_H
#define ME_ARCHIVE_7Z_H

/* .7z extraction for Cores > Download Cores, on the LZMA SDK (src/lzma).
   Decodes as it writes, so memory stays at the decoder dictionaries (tens of
   MB) however large the archive's solid block is. Supports what 7-Zip
   writes by default: LZMA / LZMA2 / stored, alone or behind BCJ2. */

#include <stddef.h>

/* Called now and then from the extracting thread. */
typedef void (*me_7z_progress)(void *ctx, unsigned long long done_bytes,
                               unsigned long long total_bytes);

typedef enum { ME_7Z_OK = 0, ME_7Z_FAILED, ME_7Z_CANCELLED } me_7z_result;

/* Extract every file in `archive` into `dest_dir` (which must exist),
   dropping the archive's folders: "a\b\c.dll" lands as dest_dir\c.dll.
   Each file is written beside its final name and checked against its CRC
   before it replaces what is there. A file that can't be replaced because
   it's in use (a loaded core) is renamed to "<name>.old" first; any that
   still can't be are counted in *in_use and left as they were.
   Stops early when *cancel becomes nonzero. On failure `err` says why. */
me_7z_result me_7z_extract_flat(const char *archive, const char *dest_dir,
                                me_7z_progress progress, void *ctx,
                                volatile long *cancel,
                                int *extracted, int *in_use,
                                char *err, size_t err_sz);

#endif
