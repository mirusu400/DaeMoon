/* lstat, gmtime_r, getaddrinfo and readdir all need this before any include. */
#define _POSIX_C_SOURCE 200809L

/* Local storage and the shared file stream used by both the fs and save backends. */
#include "daemoon_posix.h"
#include "posix_internal.h"

#include <daemoon/util/strbuf.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

daemoon_result_t daemoon_posix_errno(int e)
{
    switch (e) {
    case ENOENT:  return DAEMOON_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:   return DAEMOON_ERR_FORBIDDEN;
    case ENOSPC:
    case EDQUOT:  return DAEMOON_ERR_NO_SPACE;
    case ENOMEM:  return DAEMOON_ERR_OUT_OF_MEMORY;
    default:      return DAEMOON_ERR_IO_ERROR;
    }
}

/* ----------------------------------------------------------- file streams */

typedef struct {
    FILE            *fp;
    unsigned        *write_counter; /* optional, for test assertions */
    int              writable;
} posix_file_t;

static daemoon_result_t file_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    posix_file_t *f = (posix_file_t *)ctx;
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

static daemoon_result_t file_write(void *ctx, const void *buf, size_t len)
{
    posix_file_t *f = (posix_file_t *)ctx;

    if (fwrite(buf, 1, len, f->fp) != len) {
        return daemoon_posix_errno(errno);
    }
    if (f->write_counter != NULL) {
        (*f->write_counter)++;
    }
    return DAEMOON_OK;
}

static daemoon_result_t file_seek(void *ctx, unsigned long long offset)
{
    posix_file_t *f = (posix_file_t *)ctx;

    if (fseek(f->fp, (long)offset, SEEK_SET) != 0) {
        return DAEMOON_ERR_IO_ERROR;
    }
    return DAEMOON_OK;
}

/* The stream and its context are allocated together so a caller only has to close
 * the stream. */
typedef struct {
    daemoon_stream_t stream;
    posix_file_t     file;
} posix_stream_t;

static daemoon_result_t stream_close_owner(void *ctx)
{
    posix_stream_t *ps = (posix_stream_t *)ctx;
    daemoon_result_t r = DAEMOON_OK;

    /* A write only surfaces a full disk at flush time, so this result is part of
     * whether the write succeeded, not a formality. */
    if (ps->file.writable && fflush(ps->file.fp) != 0) {
        r = daemoon_posix_errno(errno);
    }
    if (fclose(ps->file.fp) != 0 && r == DAEMOON_OK) {
        r = daemoon_posix_errno(errno);
    }
    free(ps);
    return r;
}

static daemoon_result_t stream_read_owner(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    return file_read(&((posix_stream_t *)ctx)->file, buf, cap, out_len);
}

static daemoon_result_t stream_write_owner(void *ctx, const void *buf, size_t len)
{
    return file_write(&((posix_stream_t *)ctx)->file, buf, len);
}

static daemoon_result_t stream_seek_owner(void *ctx, unsigned long long offset)
{
    return file_seek(&((posix_stream_t *)ctx)->file, offset);
}

daemoon_result_t daemoon_posix_open_stream(const char *path, daemoon_open_mode_t mode,
                                           unsigned *write_counter, daemoon_stream_t **out)
{
    posix_stream_t *ps;
    FILE *fp;
    struct stat st;

    if (path == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    if (mode == DAEMOON_OPEN_WRITE) {
        DAEMOON_TRY(daemoon_posix_mkdir_parents(path));
    }

    fp = fopen(path, mode == DAEMOON_OPEN_WRITE ? "wb" : "rb");
    if (fp == NULL) {
        return daemoon_posix_errno(errno);
    }

    ps = (posix_stream_t *)calloc(1, sizeof(*ps));
    if (ps == NULL) {
        fclose(fp);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    ps->file.fp = fp;
    ps->file.writable = (mode == DAEMOON_OPEN_WRITE);
    ps->file.write_counter = write_counter;

    ps->stream.ctx = ps;
    ps->stream.close = stream_close_owner;
    ps->stream.seek = stream_seek_owner;
    if (mode == DAEMOON_OPEN_WRITE) {
        ps->stream.write = stream_write_owner;
    } else {
        ps->stream.read = stream_read_owner;
        if (stat(path, &st) == 0) {
            ps->stream.size = (unsigned long long)st.st_size;
        }
    }

    *out = &ps->stream;
    return DAEMOON_OK;
}

/* ---------------------------------------------------------------- helpers */

daemoon_result_t daemoon_posix_mkdir_p(const char *path)
{
    char tmp[DAEMOON_PATH_MAX * 2];
    size_t i, n;

    if (path == NULL || path[0] == '\0') {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(daemoon_strlcpy(tmp, sizeof(tmp), path));

    n = strlen(tmp);
    for (i = 1; i <= n; ++i) {
        if (tmp[i] != '/' && tmp[i] != '\0') {
            continue;
        }
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return daemoon_posix_errno(errno);
        }
        if (i < n) {
            tmp[i] = '/';
        }
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_posix_mkdir_parents(const char *path)
{
    char tmp[DAEMOON_PATH_MAX * 2];
    char *slash;

    DAEMOON_TRY(daemoon_strlcpy(tmp, sizeof(tmp), path));
    slash = strrchr(tmp, '/');
    if (slash == NULL || slash == tmp) {
        return DAEMOON_OK;
    }
    *slash = '\0';
    return daemoon_posix_mkdir_p(tmp);
}

daemoon_result_t daemoon_posix_rmtree(const char *path)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;

    if (path == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (lstat(path, &st) != 0) {
        return (errno == ENOENT) ? DAEMOON_OK : daemoon_posix_errno(errno);
    }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? DAEMOON_OK : daemoon_posix_errno(errno);
    }

    d = opendir(path);
    if (d == NULL) {
        return daemoon_posix_errno(errno);
    }
    while ((ent = readdir(d)) != NULL) {
        char child[DAEMOON_PATH_MAX * 2];
        daemoon_strbuf_t sb;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        daemoon_strbuf_init(&sb, child, sizeof(child));
        daemoon_strbuf_add(&sb, path);
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, ent->d_name);
        if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
            closedir(d);
            return DAEMOON_ERR_BUFFER_TOO_SMALL;
        }
        if (daemoon_posix_rmtree(child) != DAEMOON_OK) {
            closedir(d);
            return DAEMOON_ERR_IO_ERROR;
        }
    }
    closedir(d);

    return (rmdir(path) == 0) ? DAEMOON_OK : daemoon_posix_errno(errno);
}

daemoon_result_t daemoon_posix_clock_iso8601(void *ctx, char *buf, size_t cap)
{
    time_t now;
    struct tm tm_utc;

    (void)ctx;
    if (buf == NULL || cap < 21) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    now = time(NULL);
    if (gmtime_r(&now, &tm_utc) == NULL) {
        return DAEMOON_ERR_IO_ERROR;
    }
    if (strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    return DAEMOON_OK;
}

/* ------------------------------------------------------------- fs backend */

static daemoon_result_t fs_open(void *ctx, const char *path, daemoon_open_mode_t mode,
                                daemoon_stream_t **out)
{
    daemoon_posix_fs_ctx_t *fs = (daemoon_posix_fs_ctx_t *)ctx;

    if (mode == DAEMOON_OPEN_WRITE && fs != NULL && fs->fail_open_write != DAEMOON_OK) {
        return fs->fail_open_write;
    }
    return daemoon_posix_open_stream(path, mode, fs != NULL ? &fs->writes : NULL, out);
}

static daemoon_result_t fs_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (unlink(path) != 0) {
        return (errno == ENOENT) ? DAEMOON_OK : daemoon_posix_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t fs_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    DAEMOON_TRY(daemoon_posix_mkdir_parents(to));
    if (rename(from, to) != 0) {
        return daemoon_posix_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t fs_mkdir_p(void *ctx, const char *path)
{
    (void)ctx;
    return daemoon_posix_mkdir_p(path);
}

static int fs_exists(void *ctx, const char *path)
{
    struct stat st;
    (void)ctx;
    return stat(path, &st) == 0;
}

static daemoon_result_t fs_free_space(void *ctx, const char *path, unsigned long long *out_bytes)
{
    (void)ctx;
    (void)path;
    /* A desktop has orders of magnitude more room than any save, and reporting a
     * real figure here would need statvfs, which is not portable to the consoles
     * this interface exists for. */
    if (out_bytes != NULL) {
        *out_bytes = 0xffffffffull;
    }
    return DAEMOON_OK;
}

const daemoon_fs_backend_t daemoon_posix_fs_backend = {
    fs_open,
    fs_remove,
    fs_rename,
    fs_mkdir_p,
    fs_exists,
    fs_free_space
};
