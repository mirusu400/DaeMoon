/* SD card storage: backups, staging and the per title sync state.
 *
 * Deliberately separate from the save backend. A backup has to be writable when
 * the save archive cannot even be opened, which is precisely the situation where
 * it matters most.
 *
 * devkitARM mounts the SD card as a newlib device, so this is ordinary stdio and
 * dirent. It is still its own file rather than a reuse of platform/posix, because
 * that one is built for a desktop and quietly depends on things (lstat, symlinks)
 * that mean nothing here.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static daemoon_result_t from_errno(int e)
{
    switch (e) {
    case ENOENT: return DAEMOON_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:  return DAEMOON_ERR_FORBIDDEN;
    case ENOSPC: return DAEMOON_ERR_NO_SPACE;
    case ENOMEM: return DAEMOON_ERR_OUT_OF_MEMORY;
    default:     return DAEMOON_ERR_IO_ERROR;
    }
}

typedef struct {
    daemoon_stream_t stream;
    FILE            *fp;
    int              writable;
} sd_file_t;

static daemoon_result_t sd_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    sd_file_t *f = (sd_file_t *)ctx;
    size_t n;

    if (cap == 0) {
        *out_len = 0;
        return DAEMOON_OK;
    }
    n = fread(buf, 1, cap, f->fp);
    if (n == 0 && ferror(f->fp)) {
        return DAEMOON_ERR_IO_ERROR;
    }
    *out_len = n;
    return DAEMOON_OK;
}

static daemoon_result_t sd_write(void *ctx, const void *buf, size_t len)
{
    sd_file_t *f = (sd_file_t *)ctx;

    if (fwrite(buf, 1, len, f->fp) != len) {
        return from_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t sd_seek(void *ctx, unsigned long long offset)
{
    sd_file_t *f = (sd_file_t *)ctx;

    if (fseek(f->fp, (long)offset, SEEK_SET) != 0) {
        return DAEMOON_ERR_IO_ERROR;
    }
    return DAEMOON_OK;
}

static daemoon_result_t sd_close(void *ctx)
{
    sd_file_t *f = (sd_file_t *)ctx;
    daemoon_result_t r = DAEMOON_OK;

    /* A full SD card surfaces at flush, not at write. This result is part of
     * whether a backup exists. */
    if (f->writable && fflush(f->fp) != 0) {
        r = from_errno(errno);
    }
    if (fclose(f->fp) != 0 && r == DAEMOON_OK) {
        r = from_errno(errno);
    }
    free(f);
    return r;
}

static daemoon_result_t sd_mkdir_p(const char *path)
{
    char tmp[DAEMOON_PATH_MAX * 2];
    size_t i, n;

    DAEMOON_TRY(daemoon_strlcpy(tmp, sizeof(tmp), path));
    n = strlen(tmp);

    /* Skip the "sdmc:/" prefix: mkdir on the device root is neither needed nor
     * meaningful. */
    for (i = 1; i <= n; ++i) {
        if (tmp[i] != '/' && tmp[i] != '\0') {
            continue;
        }
        if (i > 0 && tmp[i - 1] == ':') {
            continue;
        }
        tmp[i] = '\0';
        if (strchr(tmp, '/') != NULL && mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            return from_errno(errno);
        }
        if (i < n) {
            tmp[i] = '/';
        }
    }
    return DAEMOON_OK;
}

static daemoon_result_t sd_mkdir_parents(const char *path)
{
    char tmp[DAEMOON_PATH_MAX * 2];
    char *slash;

    DAEMOON_TRY(daemoon_strlcpy(tmp, sizeof(tmp), path));
    slash = strrchr(tmp, '/');
    if (slash == NULL) {
        return DAEMOON_OK;
    }
    *slash = '\0';
    return sd_mkdir_p(tmp);
}

static daemoon_result_t fs_open(void *ctx, const char *path, daemoon_open_mode_t mode,
                                daemoon_stream_t **out)
{
    sd_file_t *f;
    FILE *fp;
    struct stat st;

    (void)ctx;
    if (mode == DAEMOON_OPEN_WRITE) {
        DAEMOON_TRY(sd_mkdir_parents(path));
    }

    fp = fopen(path, mode == DAEMOON_OPEN_WRITE ? "wb" : "rb");
    if (fp == NULL) {
        return from_errno(errno);
    }

    f = (sd_file_t *)calloc(1, sizeof(*f));
    if (f == NULL) {
        fclose(fp);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    f->fp = fp;
    f->writable = (mode == DAEMOON_OPEN_WRITE);
    f->stream.ctx = f;
    f->stream.close = sd_close;
    f->stream.seek = sd_seek;
    if (f->writable) {
        f->stream.write = sd_write;
    } else {
        f->stream.read = sd_read;
        if (stat(path, &st) == 0) {
            f->stream.size = (unsigned long long)st.st_size;
        }
    }

    *out = &f->stream;
    return DAEMOON_OK;
}

static daemoon_result_t fs_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (unlink(path) != 0 && errno != ENOENT) {
        return from_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t fs_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    DAEMOON_TRY(sd_mkdir_parents(to));
    /* FAT rename does not replace, so the destination goes first. This is the
     * "write to a temp path, then swap" step, and on this filesystem the swap is
     * not atomic: the window is one unlink wide. A partly written file is never
     * what is left, because the temp file is complete before this runs. */
    (void)unlink(to);
    if (rename(from, to) != 0) {
        return from_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t fs_mkdir_p_entry(void *ctx, const char *path)
{
    (void)ctx;
    return sd_mkdir_p(path);
}

static int fs_exists(void *ctx, const char *path)
{
    struct stat st;

    (void)ctx;
    return stat(path, &st) == 0;
}

static daemoon_result_t fs_free_space(void *ctx, const char *path, unsigned long long *out)
{
    FS_ArchiveResource resource;

    (void)ctx;
    (void)path;
    if (out == NULL) {
        return DAEMOON_OK;
    }
    *out = 0;
    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        /* Not knowing is survivable: the caller only uses this to refuse a restore
         * that obviously cannot fit. */
        return DAEMOON_OK;
    }
    *out = (unsigned long long)resource.freeClusters *
           (unsigned long long)resource.clusterSize;
    return DAEMOON_OK;
}

const daemoon_fs_backend_t daemoon_3ds_fs_backend = {
    fs_open,
    fs_remove,
    fs_rename,
    fs_mkdir_p_entry,
    fs_exists,
    fs_free_space
};
