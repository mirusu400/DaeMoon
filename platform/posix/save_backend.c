/* lstat, gmtime_r, getaddrinfo and readdir all need this before any include. */
#define _POSIX_C_SOURCE 200809L

/* A directory presented as a save archive.
 *
 * The commit call has nothing to do on a desktop, but it is still counted and can
 * still be made to fail: the rule that a save is not persisted until commit
 * succeeds is the one that costs a real save when it is broken, so the tests need a
 * way to break it on purpose. */
#include "daemoon_posix.h"
#include "posix_internal.h"

#include <daemoon/util/strbuf.h>

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct daemoon_save {
    daemoon_posix_save_ctx_t *ctx;
    char                      dir[DAEMOON_PATH_MAX * 2];
    int                       writable;
};

void daemoon_posix_save_init(daemoon_posix_save_ctx_t *ctx, const char *root)
{
    memset(ctx, 0, sizeof(*ctx));
    (void)daemoon_strlcpy(ctx->root, sizeof(ctx->root), root);
}

daemoon_result_t daemoon_posix_save_add_title(daemoon_posix_save_ctx_t *ctx, const char *title_id,
                                              const char *name, daemoon_platform_t platform,
                                              daemoon_save_type_t save_type)
{
    daemoon_title_t *t;
    char dir[DAEMOON_PATH_MAX * 2];
    struct stat st;

    if (ctx->ntitles >= DAEMOON_POSIX_MAX_TITLES) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }

    t = &ctx->titles[ctx->ntitles];
    memset(t, 0, sizeof(*t));
    DAEMOON_TRY(daemoon_strlcpy(t->id, sizeof(t->id), title_id));
    DAEMOON_TRY(daemoon_strlcpy(t->name, sizeof(t->name), name));
    t->platform = platform;
    t->save_type = save_type;

    DAEMOON_TRY(daemoon_posix_save_dir(ctx, t, dir, sizeof(dir)));
    t->has_save = (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;

    ctx->ntitles++;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_posix_save_dir(const daemoon_posix_save_ctx_t *ctx,
                                        const daemoon_title_t *title, char *buf, size_t cap)
{
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, buf, cap);
    daemoon_strbuf_add(&sb, ctx->root);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, daemoon_platform_name(title->platform));
    daemoon_strbuf_addc(&sb, '_');
    daemoon_strbuf_add(&sb, title->id);
    return daemoon_strbuf_result(&sb);
}

static daemoon_result_t list_titles(void *vctx, daemoon_title_t **out, size_t *count)
{
    daemoon_posix_save_ctx_t *ctx = (daemoon_posix_save_ctx_t *)vctx;
    daemoon_title_t *copy;
    size_t i;

    if (ctx->ntitles == 0) {
        *out = NULL;
        *count = 0;
        return DAEMOON_OK;
    }

    copy = (daemoon_title_t *)calloc(ctx->ntitles, sizeof(*copy));
    if (copy == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < ctx->ntitles; ++i) {
        char dir[DAEMOON_PATH_MAX * 2];
        struct stat st;

        copy[i] = ctx->titles[i];
        /* Recheck rather than trusting what was recorded at registration: a test
         * creates the directory after adding the title. */
        if (daemoon_posix_save_dir(ctx, &copy[i], dir, sizeof(dir)) == DAEMOON_OK) {
            copy[i].has_save = (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
        }
    }

    *out = copy;
    *count = ctx->ntitles;
    return DAEMOON_OK;
}

static void free_titles(void *vctx, daemoon_title_t *titles, size_t count)
{
    (void)vctx;
    (void)count;
    free(titles);
}

static daemoon_result_t open_common(daemoon_posix_save_ctx_t *ctx, const daemoon_title_t *t,
                                    int writable, daemoon_save_t **out)
{
    daemoon_save_t *s;
    struct stat st;

    s = (daemoon_save_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    s->ctx = ctx;
    s->writable = writable;

    if (daemoon_posix_save_dir(ctx, t, s->dir, sizeof(s->dir)) != DAEMOON_OK) {
        free(s);
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }

    if (writable) {
        daemoon_result_t r = daemoon_posix_mkdir_p(s->dir);
        if (r != DAEMOON_OK) {
            free(s);
            return r;
        }
    } else if (stat(s->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(s);
        return DAEMOON_ERR_NOT_FOUND;
    }

    ctx->opens++;
    *out = s;
    return DAEMOON_OK;
}

static daemoon_result_t open_save(void *vctx, const daemoon_title_t *t, daemoon_save_t **out)
{
    return open_common((daemoon_posix_save_ctx_t *)vctx, t, 0, out);
}

static daemoon_result_t open_save_write(void *vctx, const daemoon_title_t *t, daemoon_save_t **out)
{
    daemoon_posix_save_ctx_t *ctx = (daemoon_posix_save_ctx_t *)vctx;

    if (ctx->fail_open_write != DAEMOON_OK) {
        return ctx->fail_open_write;
    }
    return open_common(ctx, t, 1, out);
}

/* Depth first walk yielding files with paths relative to the save root. */
static daemoon_result_t walk(const char *base, const char *rel, daemoon_entry_cb cb, void *user,
                             int *stopped)
{
    char dirpath[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;
    DIR *d;
    struct dirent *ent;

    daemoon_strbuf_init(&sb, dirpath, sizeof(dirpath));
    daemoon_strbuf_add(&sb, base);
    if (rel[0] != '\0') {
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, rel);
    }
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    d = opendir(dirpath);
    if (d == NULL) {
        return daemoon_posix_errno(errno);
    }

    while (!*stopped && (ent = readdir(d)) != NULL) {
        char childrel[DAEMOON_PATH_MAX];
        char childabs[DAEMOON_PATH_MAX * 2];
        struct stat st;
        daemoon_strbuf_t rb;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        daemoon_strbuf_init(&rb, childrel, sizeof(childrel));
        if (rel[0] != '\0') {
            daemoon_strbuf_add(&rb, rel);
            daemoon_strbuf_addc(&rb, '/');
        }
        daemoon_strbuf_add(&rb, ent->d_name);
        if (daemoon_strbuf_result(&rb) != DAEMOON_OK) {
            closedir(d);
            return DAEMOON_ERR_BUFFER_TOO_SMALL;
        }

        daemoon_strbuf_init(&sb, childabs, sizeof(childabs));
        daemoon_strbuf_add(&sb, dirpath);
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, ent->d_name);
        if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
            closedir(d);
            return DAEMOON_ERR_BUFFER_TOO_SMALL;
        }

        if (lstat(childabs, &st) != 0) {
            closedir(d);
            return daemoon_posix_errno(errno);
        }
        if (S_ISDIR(st.st_mode)) {
            daemoon_result_t r = walk(base, childrel, cb, user, stopped);
            if (r != DAEMOON_OK) {
                closedir(d);
                return r;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            /* Symlinks and device nodes have no meaning inside a console save
             * archive, so they are not carried into a package. */
            continue;
        }
        if (cb(user, childrel, (unsigned long long)st.st_size) != 0) {
            *stopped = 1;
        }
    }

    closedir(d);
    return DAEMOON_OK;
}

static daemoon_result_t list_entries(void *vctx, daemoon_save_t *s, daemoon_entry_cb cb,
                                     void *user)
{
    int stopped = 0;
    (void)vctx;
    return walk(s->dir, "", cb, user, &stopped);
}

static daemoon_result_t open_file(void *vctx, daemoon_save_t *s, const char *path,
                                  daemoon_open_mode_t mode, daemoon_stream_t **out)
{
    char full[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;

    (void)vctx;
    if (mode == DAEMOON_OPEN_WRITE && !s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }

    daemoon_strbuf_init(&sb, full, sizeof(full));
    daemoon_strbuf_add(&sb, s->dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, path);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    return daemoon_posix_open_stream(full, mode, NULL, out);
}

static int unlink_cb(void *user, const char *path, unsigned long long size)
{
    daemoon_save_t *s = (daemoon_save_t *)user;
    char full[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;

    (void)size;
    daemoon_strbuf_init(&sb, full, sizeof(full));
    daemoon_strbuf_add(&sb, s->dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, path);
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return 1;
    }
    (void)unlink(full);
    return 0;
}

static daemoon_result_t remove_all(void *vctx, daemoon_save_t *s)
{
    int stopped = 0;

    (void)vctx;
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    /* Files only. Empty directories left behind are harmless and removing them
     * during a walk is where this kind of code usually goes wrong. */
    return walk(s->dir, "", unlink_cb, s, &stopped);
}

static daemoon_result_t commit(void *vctx, daemoon_save_t *s)
{
    daemoon_posix_save_ctx_t *ctx = (daemoon_posix_save_ctx_t *)vctx;

    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    if (ctx->fail_commit != DAEMOON_OK) {
        return ctx->fail_commit;
    }
    /* On a console this is FSUSER_ControlArchive or fsdevCommitDevice, and skipping
     * it loses everything that was just written. */
    ctx->commits++;
    return DAEMOON_OK;
}

static daemoon_result_t close_save(void *vctx, daemoon_save_t *s)
{
    (void)vctx;
    free(s);
    return DAEMOON_OK;
}

const daemoon_save_backend_t daemoon_posix_save_backend = {
    list_titles,
    free_titles,
    open_save,
    open_save_write,
    list_entries,
    open_file,
    remove_all,
    commit,
    close_save,
    NULL /* is_title_running: a desktop cannot have the title running */
};
