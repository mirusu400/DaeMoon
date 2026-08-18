/* nds-bootstrap saves: plain .sav files on the SD card.
 *
 * Phase 2 uses this as the network testbed, and the reason is the whole design of
 * this file: there are no permissions to get wrong, no archive to commit, and no
 * service that can refuse a read. A save is one file. If the sync path corrupts
 * something here, it is the sync path.
 *
 * It is ordinary stdio, so the desktop tests run it directly rather than through a
 * stub. That is not an accident either: the least dangerous backend should be the
 * one that is easiest to test.
 *
 * The layout is TWiLightMenu's: ROMs in one directory, saves in another with the
 * same base name.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>
#include <daemoon/util/utf8.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The one entry inside an nds save. A .sav has no internal structure this project
 * needs to know about: it is the file, whole. */
#define NDS_ENTRY_NAME "save.sav"

/* The DS cartridge header, which is where a stable id comes from. Filenames on a
 * real card are whatever somebody typed - the ones on the console this was
 * written against have spaces and Hangul in them - and a title id has to be
 * stable and match the manifest schema. */
#define NDS_HEADER_TITLE_OFF  0x00
#define NDS_HEADER_TITLE_LEN  12
#define NDS_HEADER_CODE_OFF   0x0C
#define NDS_HEADER_CODE_LEN   4

struct daemoon_save {
    char path[DAEMOON_PATH_MAX * 2];
    int  writable;
};

typedef struct {
    daemoon_stream_t stream;
    FILE            *fp;
    int              writable;
} nds_file_t;

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

/* ------------------------------------------------------------------ streams */

static daemoon_result_t nds_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    nds_file_t *f = (nds_file_t *)ctx;
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

static daemoon_result_t nds_write(void *ctx, const void *buf, size_t len)
{
    nds_file_t *f = (nds_file_t *)ctx;

    if (fwrite(buf, 1, len, f->fp) != len) {
        return from_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t nds_seek(void *ctx, unsigned long long offset)
{
    nds_file_t *f = (nds_file_t *)ctx;

    if (fseek(f->fp, (long)offset, SEEK_SET) != 0) {
        return DAEMOON_ERR_IO_ERROR;
    }
    return DAEMOON_OK;
}

static daemoon_result_t nds_close(void *ctx)
{
    nds_file_t *f = (nds_file_t *)ctx;
    daemoon_result_t r = DAEMOON_OK;

    /* A full SD card surfaces here, not at write time, and for this backend the
     * close is the commit: there is no archive to flush afterwards. */
    if (f->writable && fflush(f->fp) != 0) {
        r = from_errno(errno);
    }
    if (fclose(f->fp) != 0 && r == DAEMOON_OK) {
        r = from_errno(errno);
    }
    free(f);
    return r;
}

/* ------------------------------------------------------------------- titles */

/* A title id the manifest schema will accept: uppercase, digits, underscore and
 * dash only. Built from the cartridge header rather than the filename. */
static void sanitise_id(const char *in, size_t in_len, daemoon_strbuf_t *sb)
{
    size_t i;

    for (i = 0; i < in_len && in[i] != '\0'; ++i) {
        char c = in[i];

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-') {
            daemoon_strbuf_addc(sb, c);
        } else {
            daemoon_strbuf_addc(sb, '_');
        }
    }
}

/* Reads the header of the ROM matching a save, for its game code and title.
 * Returns not_found when there is no ROM beside the save, which is a normal
 * state: somebody can copy a .sav on its own. */
static daemoon_result_t read_rom_header(const char *rom_dir, const char *base,
                                        char *out_code, char *out_title)
{
    char path[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;
    unsigned char header[NDS_HEADER_CODE_OFF + NDS_HEADER_CODE_LEN];
    FILE *fp;
    size_t got;

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, rom_dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, base);
    daemoon_strbuf_add(&sb, ".nds");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    got = fread(header, 1, sizeof(header), fp);
    fclose(fp);

    if (got < sizeof(header)) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    memcpy(out_code, header + NDS_HEADER_CODE_OFF, NDS_HEADER_CODE_LEN);
    out_code[NDS_HEADER_CODE_LEN] = '\0';
    memcpy(out_title, header + NDS_HEADER_TITLE_OFF, NDS_HEADER_TITLE_LEN);
    out_title[NDS_HEADER_TITLE_LEN] = '\0';
    return DAEMOON_OK;
}

static daemoon_result_t save_path_of(const daemoon_3ds_nds_ctx_t *c,
                                     const daemoon_title_t *t, char *out, size_t cap)
{
    daemoon_strbuf_t sb;

    /* The file name is carried in the title rather than rebuilt from the id: a
     * real card has names with spaces and Hangul in them, and the id is a
     * sanitised thing that cannot be turned back into one. */
    daemoon_strbuf_init(&sb, out, cap);
    daemoon_strbuf_add(&sb, c->save_dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, t->name);
    daemoon_strbuf_add(&sb, ".sav");
    return daemoon_strbuf_result(&sb);
}

static daemoon_result_t list_titles(void *vctx, daemoon_title_t **out, size_t *count)
{
    daemoon_3ds_nds_ctx_t *c = (daemoon_3ds_nds_ctx_t *)vctx;
    daemoon_title_t *titles;
    DIR *d;
    struct dirent *ent;
    size_t n = 0;

    *out = NULL;
    *count = 0;

    d = opendir(c->save_dir);
    if (d == NULL) {
        /* No save directory is not a failure: it is a console that has never run
         * a DS game. */
        return DAEMOON_OK;
    }

    titles = (daemoon_title_t *)calloc(DAEMOON_3DS_NDS_MAX_TITLES, sizeof(*titles));
    if (titles == NULL) {
        closedir(d);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    while (n < DAEMOON_3DS_NDS_MAX_TITLES && (ent = readdir(d)) != NULL) {
        daemoon_title_t *t = &titles[n];
        char base[DAEMOON_NAME_MAX];
        char code[NDS_HEADER_CODE_LEN + 1];
        char rom_title[NDS_HEADER_TITLE_LEN + 1];
        daemoon_strbuf_t sb;
        size_t len = strlen(ent->d_name);
        struct stat st;
        char full[DAEMOON_PATH_MAX * 2];

        if (len < 5 || strcmp(ent->d_name + len - 4, ".sav") != 0) {
            continue;
        }
        if (daemoon_strlcpy(base, sizeof(base), ent->d_name) != DAEMOON_OK) {
            continue;
        }
        base[len - 4] = '\0';

        memset(t, 0, sizeof(*t));
        t->platform = DAEMOON_PLATFORM_NDS;
        t->save_type = DAEMOON_SAVE_NDS;

        /* The name is the file's base name, because that is what has to be turned
         * back into a path. It is also what a person sees, and on a real card it
         * is the only thing that says which game this is. */
        (void)daemoon_strlcpy(t->name, sizeof(t->name), base);

        daemoon_strbuf_init(&sb, t->id, sizeof(t->id));
        if (read_rom_header(c->rom_dir, base, code, rom_title) == DAEMOON_OK) {
            /* Game code plus the cartridge's own title: stable across renames,
             * and readable in a manifest. */
            sanitise_id(code, NDS_HEADER_CODE_LEN, &sb);
            daemoon_strbuf_addc(&sb, '_');
            sanitise_id(rom_title, NDS_HEADER_TITLE_LEN, &sb);
        } else {
            /* A save with no ROM beside it. The name is all there is, so the id
             * comes from that and stops being stable across a rename - which is
             * worth knowing about rather than refusing to back the save up. */
            sanitise_id(base, strlen(base), &sb);
        }
        if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
            /* Too long for an id, which the manifest would reject later anyway. */
            continue;
        }
        if (strlen(t->id) < 4) {
            continue;
        }

        if (save_path_of(c, t, full, sizeof(full)) == DAEMOON_OK &&
            stat(full, &st) == 0) {
            t->has_save = 1;
            t->size_hint = (unsigned long long)st.st_size;
        }

        ++n;
    }
    closedir(d);

    *out = titles;
    *count = n;
    return DAEMOON_OK;
}

static void free_titles(void *vctx, daemoon_title_t *titles, size_t count)
{
    (void)vctx;
    (void)count;
    free(titles);
}

/* -------------------------------------------------------------------- saves */

static daemoon_result_t open_common(daemoon_3ds_nds_ctx_t *c, const daemoon_title_t *t,
                                    int writable, daemoon_save_t **out)
{
    daemoon_save_t *s = (daemoon_save_t *)calloc(1, sizeof(*s));
    struct stat st;

    if (s == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    if (save_path_of(c, t, s->path, sizeof(s->path)) != DAEMOON_OK) {
        free(s);
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    if (!writable && stat(s->path, &st) != 0) {
        free(s);
        return DAEMOON_ERR_NOT_FOUND;
    }
    s->writable = writable;
    *out = s;
    return DAEMOON_OK;
}

static daemoon_result_t open_save(void *vctx, const daemoon_title_t *t,
                                  daemoon_save_t **out)
{
    return open_common((daemoon_3ds_nds_ctx_t *)vctx, t, 0, out);
}

static daemoon_result_t open_save_write(void *vctx, const daemoon_title_t *t,
                                        daemoon_save_t **out)
{
    return open_common((daemoon_3ds_nds_ctx_t *)vctx, t, 1, out);
}

static daemoon_result_t close_save(void *vctx, daemoon_save_t *s)
{
    (void)vctx;
    free(s);
    return DAEMOON_OK;
}

static daemoon_result_t list_entries(void *vctx, daemoon_save_t *s, daemoon_entry_cb cb,
                                     void *user)
{
    struct stat st;

    (void)vctx;
    if (stat(s->path, &st) != 0) {
        /* No file is an empty save, and core refuses to package one of those. */
        return DAEMOON_OK;
    }
    (void)cb(user, NDS_ENTRY_NAME, (unsigned long long)st.st_size);
    return DAEMOON_OK;
}

static daemoon_result_t open_file(void *vctx, daemoon_save_t *s, const char *path,
                                  daemoon_open_mode_t mode, daemoon_stream_t **out)
{
    nds_file_t *f;
    FILE *fp;
    struct stat st;

    (void)vctx;
    /* One entry, one name. A package that carries anything else was not made from
     * an nds save, and writing it here would produce a file the game cannot read. */
    if (strcmp(path, NDS_ENTRY_NAME) != 0) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    if (mode == DAEMOON_OPEN_WRITE && !s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }

    fp = fopen(s->path, mode == DAEMOON_OPEN_WRITE ? "wb" : "rb");
    if (fp == NULL) {
        return from_errno(errno);
    }

    f = (nds_file_t *)calloc(1, sizeof(*f));
    if (f == NULL) {
        fclose(fp);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    f->fp = fp;
    f->writable = (mode == DAEMOON_OPEN_WRITE);
    f->stream.ctx = f;
    f->stream.close = nds_close;
    f->stream.seek = nds_seek;
    if (f->writable) {
        f->stream.write = nds_write;
    } else {
        f->stream.read = nds_read;
        if (stat(s->path, &st) == 0) {
            f->stream.size = (unsigned long long)st.st_size;
        }
    }

    *out = &f->stream;
    return DAEMOON_OK;
}

static daemoon_result_t remove_all(void *vctx, daemoon_save_t *s)
{
    (void)vctx;
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    /* The restore recreates it. Removing it first is what makes a restore of a
     * shorter save actually shorter rather than a shorter save with the tail of
     * the previous one still attached. */
    if (unlink(s->path) != 0 && errno != ENOENT) {
        return from_errno(errno);
    }
    return DAEMOON_OK;
}

static daemoon_result_t commit(void *vctx, daemoon_save_t *s)
{
    (void)vctx;
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    /* Nothing to do, and that is a property of this backend rather than an
     * omission: a .sav is persisted when the stream that wrote it was closed, and
     * that close already reported whether it worked. There is no archive here to
     * leave uncommitted, which is exactly why this is the phase that goes on the
     * network first. */
    return DAEMOON_OK;
}

const daemoon_save_backend_t daemoon_3ds_nds_backend = {
    list_titles,
    free_titles,
    open_save,
    open_save_write,
    list_entries,
    open_file,
    remove_all,
    commit,
    close_save,
    NULL, /* is_title_running: nds-bootstrap has exited by the time this runs */
    /* A .sav is the whole save. Nothing binds one to a console, which is the
     * reason Phase 2 syncs these first. */
    NULL, NULL, NULL
};
