#include "archive_7z.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzma/7z.h"
#include "lzma/7zCrc.h"
#include "lzma/7zFile.h"
#include "lzma/Bcj2.h"
#include "lzma/Lzma2Dec.h"
#include "lzma/LzmaDec.h"

/* The SDK's own extractor (SzArEx_Extract) decodes a whole solid block into
   one buffer; RetroArch_cores.7z is a single ~2 GB block. So we use the SDK
   to read the archive's header and its decoders, and run the folder
   ourselves: each coder decodes into a small buffer as its consumer needs
   more, and file contents stream straight to disk.

   A folder is either one coder, or the layout 7-Zip uses for executables:
   three coders (LZMA2 main stream, LZMA call and jump streams) plus a stored
   range-coder stream, recombined by BCJ2. The layout is the one the SDK's
   7zDec.c accepts (CheckSupportedFolder). */

#define k_Copy  0
#define k_LZMA2 0x21
#define k_LZMA  0x30101
#define k_BCJ2  0x303011B

#define IN_BUF  (1 << 18)   /* per packed stream */
#define SUB_BUF (1 << 18)   /* per BCJ2 input stream; must be a multiple of 4 */
#define OUT_BUF (1 << 20)

static void *sz_alloc(ISzAllocPtr p, size_t size) { (void)p; return size ? malloc(size) : NULL; }
static void  sz_free(ISzAllocPtr p, void *a)      { (void)p; free(a); }
static const ISzAlloc g_alloc = { sz_alloc, sz_free };

/* ---- one packed stream: raw bytes at a range of the archive -------------- */
typedef struct {
    CSzFile *file;
    UInt64   pos, left;   /* next archive offset to read, bytes not read yet */
    Byte    *buf;
    size_t   cur, len;
} pack_in;

static void pack_init(pack_in *p, CSzFile *file, UInt64 pos, UInt64 size, Byte *buf) {
    p->file = file;
    p->pos  = pos;
    p->left = size;
    p->buf  = buf;
    p->cur  = p->len = 0;
}

/* Refill the buffer once it's used up. Streams interleave, so every read
   seeks first. */
static SRes pack_fill(pack_in *p) {
    if (p->cur < p->len || p->left == 0) return SZ_OK;
    size_t n = p->left < IN_BUF ? (size_t)p->left : IN_BUF;
    Int64 pos = (Int64)p->pos;
    if (File_Seek(p->file, &pos, SZ_SEEK_SET) != 0) return SZ_ERROR_READ;
    if (File_Read(p->file, p->buf, &n) != 0) return SZ_ERROR_READ;
    if (n == 0) return SZ_ERROR_INPUT_EOF;
    p->pos  += n;
    p->left -= n;
    p->cur   = 0;
    p->len   = n;
    return SZ_OK;
}

/* ---- one coder: a packed stream, decoded -------------------------------- */
typedef struct {
    UInt32    method;
    int       allocated;
    CLzmaDec  lzma;
    CLzma2Dec lzma2;
    pack_in   in;
    UInt64    left;       /* output not produced yet */
} coder_in;

static SRes coder_open(coder_in *c, const CSzCoderInfo *info, const Byte *props, UInt64 size) {
    c->method = info->MethodID;
    c->left   = size;
    if (info->NumStreams != 1) return SZ_ERROR_UNSUPPORTED;
    switch (c->method) {
        case k_Copy:
            return SZ_OK;
        case k_LZMA:
            LzmaDec_CONSTRUCT(&c->lzma)
            RINOK(LzmaDec_Allocate(&c->lzma, props, info->PropsSize, &g_alloc))
            LzmaDec_Init(&c->lzma);
            c->allocated = 1;
            return SZ_OK;
        case k_LZMA2:
            if (info->PropsSize != 1) return SZ_ERROR_UNSUPPORTED;
            Lzma2Dec_CONSTRUCT(&c->lzma2)
            RINOK(Lzma2Dec_Allocate(&c->lzma2, props[0], &g_alloc))
            Lzma2Dec_Init(&c->lzma2);
            c->allocated = 1;
            return SZ_OK;
    }
    return SZ_ERROR_UNSUPPORTED;
}

static void coder_close(coder_in *c) {
    if (!c->allocated) return;
    if (c->method == k_LZMA) LzmaDec_Free(&c->lzma, &g_alloc);
    else                     Lzma2Dec_Free(&c->lzma2, &g_alloc);
    c->allocated = 0;
}

/* Up to `want` bytes of output; fewer only at the end of the stream. */
static SRes coder_read(coder_in *c, Byte *dst, size_t want, size_t *got) {
    if (want > c->left) want = (size_t)c->left;
    size_t done = 0;
    while (done < want) {
        RINOK(pack_fill(&c->in))
        const Byte *src = c->in.buf + c->in.cur;
        SizeT in_len  = c->in.len - c->in.cur;
        SizeT out_len = want - done;
        if (c->method == k_Copy) {
            if (in_len == 0) return SZ_ERROR_INPUT_EOF;
            if (out_len > in_len) out_len = in_len;
            memcpy(dst + done, src, out_len);
            in_len = out_len;
        } else {
            ELzmaStatus status;
            SRes r = c->method == k_LZMA
                ? LzmaDec_DecodeToBuf(&c->lzma, dst + done, &out_len, src, &in_len,
                                      LZMA_FINISH_ANY, &status)
                : Lzma2Dec_DecodeToBuf(&c->lzma2, dst + done, &out_len, src, &in_len,
                                       LZMA_FINISH_ANY, &status);
            if (r != SZ_OK) return r;
            if (in_len == 0 && out_len == 0) return SZ_ERROR_DATA;  /* ended early */
        }
        c->in.cur += in_len;
        done += out_len;
    }
    c->left -= done;
    *got = done;
    return SZ_OK;
}

/* ---- one folder (solid block) ------------------------------------------- */
typedef struct {
    int      bcj2;
    coder_in c[3];        /* the coder, or BCJ2's: [0] jump, [1] call, [2] main */
    pack_in  rc;          /* BCJ2's range-coder stream (stored) */
    Byte    *sub[BCJ2_NUM_STREAMS];
    CBcj2Dec dec;
    Byte    *mem;
} folder_in;

/* Coder ci of the BCJ2 layout: its packed stream and the BCJ2 input it feeds. */
static const unsigned k_bcj2_pack[3]   = { 3, 2, 0 };
static const unsigned k_bcj2_stream[3] = { BCJ2_STREAM_JUMP, BCJ2_STREAM_CALL, BCJ2_STREAM_MAIN };

static int is_bcj2_layout(const CSzFolder *f) {
    return f->NumCoders == 4
        && f->Coders[3].MethodID == k_BCJ2 && f->Coders[3].NumStreams == 4
        && f->NumPackStreams == 4
        && f->PackStreams[0] == 2 && f->PackStreams[1] == 6
        && f->PackStreams[2] == 1 && f->PackStreams[3] == 0
        && f->NumBonds == 3
        && f->Bonds[0].InIndex == 5 && f->Bonds[0].OutIndex == 0
        && f->Bonds[1].InIndex == 4 && f->Bonds[1].OutIndex == 1
        && f->Bonds[2].InIndex == 3 && f->Bonds[2].OutIndex == 2;
}

static void folder_close(folder_in *f) {
    for (int i = 0; i < 3; i++) coder_close(&f->c[i]);
    free(f->mem);
    memset(f, 0, sizeof(*f));
}

static SRes folder_open(folder_in *f, const CSzArEx *db, CSzFile *file, UInt32 fi) {
    const CSzAr *ar = &db->db;
    const Byte *data = ar->CodersData + ar->FoCodersOffsets[fi];
    CSzData sd = { data, ar->FoCodersOffsets[fi + 1] - ar->FoCodersOffsets[fi] };
    CSzFolder fo;
    RINOK(SzGetNextFolderItem(&fo, &sd))
    const UInt64 *unpack = &ar->CoderUnpackSizes[ar->FoToCoderUnpackSizes[fi]];
    const UInt64 *pack   = ar->PackPositions + ar->FoStartPackStreamIndex[fi];
    const UInt64 base    = db->dataPos;

    memset(f, 0, sizeof(*f));
    if (fo.NumCoders == 1) {
        if (fo.NumPackStreams != 1 || fo.PackStreams[0] != 0 || fo.NumBonds != 0)
            return SZ_ERROR_UNSUPPORTED;
        if (!(f->mem = (Byte *)malloc(IN_BUF))) return SZ_ERROR_MEM;
        pack_init(&f->c[0].in, file, base + pack[0], pack[1] - pack[0], f->mem);
        return coder_open(&f->c[0], &fo.Coders[0], data + fo.Coders[0].PropsOffset, unpack[0]);
    }
    if (!is_bcj2_layout(&fo)) return SZ_ERROR_UNSUPPORTED;
    /* Call and jump streams are whole 32-bit addresses. */
    if ((unpack[0] & 3) || (unpack[1] & 3)) return SZ_ERROR_DATA;

    f->bcj2 = 1;
    if (!(f->mem = (Byte *)malloc(4 * (size_t)IN_BUF + 3 * (size_t)SUB_BUF))) return SZ_ERROR_MEM;
    Byte *m = f->mem;
    for (int ci = 0; ci < 3; ci++) {
        unsigned p = k_bcj2_pack[ci];
        pack_init(&f->c[ci].in, file, base + pack[p], pack[p + 1] - pack[p], m);
        m += IN_BUF;
        f->sub[k_bcj2_stream[ci]] = m;
        m += SUB_BUF;
        RINOK(coder_open(&f->c[ci], &fo.Coders[ci], data + fo.Coders[ci].PropsOffset, unpack[ci]))
    }
    pack_init(&f->rc, file, base + pack[1], pack[2] - pack[1], m);
    f->sub[BCJ2_STREAM_RC] = m;

    Bcj2Dec_Init(&f->dec);
    for (int s = 0; s < BCJ2_NUM_STREAMS; s++) f->dec.bufs[s] = f->dec.lims[s] = f->sub[s];
    return SZ_OK;
}

/* Exactly `want` bytes of the folder's output. */
static SRes folder_read(folder_in *f, Byte *dst, size_t want) {
    if (!f->bcj2) {
        size_t got;
        RINOK(coder_read(&f->c[0], dst, want, &got))
        return got == want ? SZ_OK : SZ_ERROR_DATA;
    }
    CBcj2Dec *d = &f->dec;
    d->dest    = dst;
    d->destLim = dst + want;
    for (;;) {
        RINOK(Bcj2Dec_Decode(d))
        if (d->dest == d->destLim) return SZ_OK;
        /* Otherwise it stopped for more of input stream `state`, which it
           has used up. */
        unsigned s = d->state;
        size_t n;
        if (s >= BCJ2_NUM_STREAMS) return SZ_ERROR_DATA;
        if (s == BCJ2_STREAM_RC) {
            RINOK(pack_fill(&f->rc))
            n = f->rc.len - f->rc.cur;
            d->bufs[s] = f->rc.buf + f->rc.cur;
            f->rc.cur  = f->rc.len;
        } else {
            int ci = s == BCJ2_STREAM_MAIN ? 2 : s == BCJ2_STREAM_CALL ? 1 : 0;
            RINOK(coder_read(&f->c[ci], f->sub[s], SUB_BUF, &n))
            d->bufs[s] = f->sub[s];
        }
        d->lims[s] = d->bufs[s] + n;
        if (n == 0) return SZ_ERROR_DATA;   /* an input ran out before the output */
    }
}

/* ---- files ---------------------------------------------------------------- */
/* The last path component, if it's usable as a file name. */
static int flat_name(const WCHAR *path, WCHAR *out, size_t out_n) {
    const WCHAR *base = path;
    for (const WCHAR *p = path; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    size_t n = wcslen(base);
    if (n == 0 || n >= out_n || wcscmp(base, L".") == 0 || wcscmp(base, L"..") == 0) return 0;
    for (const WCHAR *p = base; *p; p++)
        if (*p < 32 || wcschr(L"<>:\"|?*", *p)) return 0;
    wcscpy(out, base);
    return 1;
}

/* Leftovers from replacing cores that were loaded (see replace_file). */
static void remove_old_files(const WCHAR *dir) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    if (_snwprintf(pattern, MAX_PATH, L"%ls*.old", dir) < 0) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (_snwprintf(path, MAX_PATH, L"%ls%ls", dir, fd.cFileName) >= 0) DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Move `tmp` over `path`. A loaded DLL can't be overwritten or deleted, but
   it can be renamed out of the way. Returns 0 if `path` is left as it was. */
static int replace_file(const WCHAR *tmp, const WCHAR *path) {
    if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    WCHAR old[MAX_PATH];
    if (_snwprintf(old, MAX_PATH, L"%ls.old", path) >= 0) {
        DeleteFileW(old);
        if (MoveFileExW(path, old, 0)) {
            if (MoveFileExW(tmp, path, 0)) return 1;
            MoveFileExW(old, path, 0);
        }
    }
    DeleteFileW(tmp);
    return 0;
}

static const char *sres_text(SRes r) {
    switch (r) {
        case SZ_ERROR_DATA:
        case SZ_ERROR_CRC:
        case SZ_ERROR_INPUT_EOF:
        case SZ_ERROR_ARCHIVE:
        case SZ_ERROR_NO_ARCHIVE:  return "The archive is damaged or incomplete.";
        case SZ_ERROR_UNSUPPORTED: return "The archive uses a compression method laggueless can't unpack.";
        case SZ_ERROR_MEM:         return "Out of memory.";
        case SZ_ERROR_READ:        return "Could not read the archive.";
        default:                   return "Could not unpack the archive.";
    }
}

me_7z_result me_7z_extract_flat(const char *archive, const char *dest_dir,
                                me_7z_progress progress, void *ctx,
                                volatile long *cancel,
                                int *extracted, int *in_use,
                                char *err, size_t err_sz) {
    static int crc_ready;
    if (!crc_ready) { CrcGenerateTable(); crc_ready = 1; }
    *extracted = *in_use = 0;
    err[0] = '\0';

    WCHAR dir[MAX_PATH];
    int dn = MultiByteToWideChar(CP_ACP, 0, dest_dir, -1, dir, MAX_PATH - 1);
    if (dn <= 1) { snprintf(err, err_sz, "Bad folder: %s", dest_dir); return ME_7Z_FAILED; }
    if (dir[dn - 2] != L'\\') { dir[dn - 1] = L'\\'; dir[dn] = 0; }
    remove_old_files(dir);

    CFileInStream fin;
    File_Construct(&fin.file);
    if (InFile_Open(&fin.file, archive) != 0) {
        snprintf(err, err_sz, "Could not open %s", archive);
        return ME_7Z_FAILED;
    }
    FileInStream_CreateVTable(&fin);
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf        = (Byte *)malloc(IN_BUF);
    look.bufSize    = IN_BUF;
    look.realStream = &fin.vt;
    LookToRead2_INIT(&look)

    CSzArEx db;
    SzArEx_Init(&db);
    Byte  *out = (Byte *)malloc(OUT_BUF);
    UInt16 *name16 = NULL;
    folder_in fo;
    memset(&fo, 0, sizeof(fo));
    UInt32 cur_folder = (UInt32)-1;
    UInt64 folder_pos = 0;   /* output of cur_folder consumed so far */
    me_7z_result result = ME_7Z_OK;
    HANDLE hf = INVALID_HANDLE_VALUE;
    WCHAR tmp[MAX_PATH] = L"";

    SRes res = (!look.buf || !out) ? SZ_ERROR_MEM
                                   : SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK) {
        snprintf(err, err_sz, "%s", sres_text(res));
        result = ME_7Z_FAILED;
        goto done;
    }

    UInt64 total = 0, written = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++)
        if (!SzArEx_IsDir(&db, i)) total += SzArEx_GetFileSize(&db, i);

    for (UInt32 i = 0; i < db.NumFiles && result == ME_7Z_OK; i++) {
        if (SzArEx_IsDir(&db, i)) continue;
        UInt64 size = SzArEx_GetFileSize(&db, i);

        size_t n16 = SzArEx_GetFileNameUtf16(&db, i, NULL);
        free(name16);
        if (!(name16 = (UInt16 *)malloc(n16 * sizeof(UInt16)))) { res = SZ_ERROR_MEM; break; }
        SzArEx_GetFileNameUtf16(&db, i, name16);
        WCHAR name[MAX_PATH], path[MAX_PATH];
        int usable = flat_name((const WCHAR *)name16, name, 200) &&
                     _snwprintf(path, MAX_PATH, L"%ls%ls", dir, name) > 0 &&
                     _snwprintf(tmp, MAX_PATH, L"%ls.part", path) > 0;

        /* Line the folder up with the start of this file's data. */
        if (size > 0) {
            UInt32 fi = db.FileToFolder[i];
            if (fi != cur_folder) {
                folder_close(&fo);
                cur_folder = fi;
                folder_pos = 0;
                if ((res = folder_open(&fo, &db, &fin.file, fi)) != SZ_OK) break;
            }
            UInt64 start = db.UnpackPositions[i] - db.UnpackPositions[db.FolderToFile[fi]];
            if (start < folder_pos) { res = SZ_ERROR_DATA; break; }
            while (folder_pos < start) {
                size_t n = start - folder_pos < OUT_BUF ? (size_t)(start - folder_pos) : OUT_BUF;
                if ((res = folder_read(&fo, out, n)) != SZ_OK) break;
                folder_pos += n;
            }
            if (res != SZ_OK) break;
        }

        if (usable) {
            hf = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) {
                snprintf(err, err_sz, "Could not write to %s (error %lu).", dest_dir, GetLastError());
                result = ME_7Z_FAILED;
                break;
            }
        }
        UInt32 crc = CRC_INIT_VAL;
        for (UInt64 left = size; left > 0; ) {
            if (cancel && *cancel) { result = ME_7Z_CANCELLED; break; }
            size_t n = left < OUT_BUF ? (size_t)left : OUT_BUF;
            if ((res = folder_read(&fo, out, n)) != SZ_OK) break;
            folder_pos += n;
            left -= n;
            written += n;
            crc = CrcUpdate(crc, out, n);
            DWORD wrote = 0;
            if (usable && (!WriteFile(hf, out, (DWORD)n, &wrote, NULL) || wrote != n)) {
                snprintf(err, err_sz, "Could not write to %s (error %lu). Is the disk full?",
                         dest_dir, GetLastError());
                result = ME_7Z_FAILED;
                break;
            }
            if (progress) progress(ctx, written, total);
        }
        if (hf != INVALID_HANDLE_VALUE) { CloseHandle(hf); hf = INVALID_HANDLE_VALUE; }
        if (res == SZ_OK && result == ME_7Z_OK && SzBitWithVals_Check(&db.CRCs, i) &&
            CRC_GET_DIGEST(crc) != db.CRCs.Vals[i])
            res = SZ_ERROR_CRC;
        if (res != SZ_OK || result != ME_7Z_OK) {
            if (usable) DeleteFileW(tmp);
            break;
        }
        if (!usable) continue;
        if (replace_file(tmp, path)) (*extracted)++;
        else                         (*in_use)++;
    }
    if (res != SZ_OK && result == ME_7Z_OK) {
        snprintf(err, err_sz, "%s", sres_text(res));
        result = ME_7Z_FAILED;
    }

done:
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    folder_close(&fo);
    free(name16);
    free(out);
    SzArEx_Free(&db, &g_alloc);
    free(look.buf);
    File_Close(&fin.file);
    return result;
}
