#define _POSIX_C_SOURCE 200809L

#include "fixture_env.h"

#include "posix_internal.h"
#include "test.h"

#include <daemoon/util/strbuf.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned char g_scratch[64 * 1024];

int fixture_open(fixture_t *f, const char *tag)
{
    daemoon_strbuf_t sb;

    memset(f, 0, sizeof(*f));
    if (daemoon_test_tempdir(f->root, sizeof(f->root), tag) != 0) {
        return -1;
    }

    daemoon_strbuf_init(&sb, f->saves, sizeof(f->saves));
    daemoon_strbuf_add(&sb, f->root);
    daemoon_strbuf_add(&sb, "/saves");
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return -1;
    }

    daemoon_strbuf_init(&sb, f->work, sizeof(f->work));
    daemoon_strbuf_add(&sb, f->root);
    daemoon_strbuf_add(&sb, "/work");
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return -1;
    }

    daemoon_posix_save_init(&f->save, f->saves);
    daemoon_posix_ui_init(&f->ui);
    fake_server_init(&f->server);

    f->env.save = &daemoon_posix_save_backend;
    f->env.fs = &daemoon_posix_fs_backend;
    f->env.ui = &daemoon_posix_ui_backend;
    f->env.net = &fake_server_net_backend;
    f->env.save_ctx = &f->save;
    f->env.fs_ctx = &f->fs;
    f->env.ui_ctx = &f->ui;
    f->env.net_ctx = &f->server;
    f->env.clock_iso8601 = daemoon_posix_clock_iso8601;
    f->env.server_url = "http://fake.invalid";
    f->env.token = "test-token";
    f->env.device_label = "test console";
    f->env.work_dir = f->work;
    f->env.scratch = g_scratch;
    f->env.scratch_len = sizeof(g_scratch);

    return 0;
}

void fixture_close(fixture_t *f)
{
    fake_server_free(&f->server);
    (void)daemoon_posix_rmtree(f->root);
}

const daemoon_title_t *fixture_add_title(fixture_t *f, const char *title_id,
                                         daemoon_platform_t platform)
{
    if (daemoon_posix_save_add_title(&f->save, title_id, "Test Title", platform,
                                     DAEMOON_SAVE_SAVEDATA) != DAEMOON_OK) {
        return NULL;
    }
    return &f->save.titles[f->save.ntitles - 1];
}

static int save_file_path(fixture_t *f, const daemoon_title_t *t, const char *rel, char *buf,
                          size_t cap)
{
    char dir[320];
    daemoon_strbuf_t sb;

    if (daemoon_posix_save_dir(&f->save, t, dir, sizeof(dir)) != DAEMOON_OK) {
        return -1;
    }
    daemoon_strbuf_init(&sb, buf, cap);
    daemoon_strbuf_add(&sb, dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, rel);
    return daemoon_strbuf_result(&sb) == DAEMOON_OK ? 0 : -1;
}

int fixture_write_save_file(fixture_t *f, const daemoon_title_t *t, const char *rel,
                            const char *content)
{
    char path[640];
    FILE *fp;

    if (save_file_path(f, t, rel, path, sizeof(path)) != 0) {
        return -1;
    }
    if (daemoon_posix_mkdir_parents(path) != DAEMOON_OK) {
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return 0;
}

int fixture_read_save_file(fixture_t *f, const daemoon_title_t *t, const char *rel, char *buf,
                           size_t cap)
{
    char path[640];
    FILE *fp;
    size_t n;

    if (save_file_path(f, t, rel, path, sizeof(path)) != 0) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return 0;
}

int fixture_save_file_exists(fixture_t *f, const daemoon_title_t *t, const char *rel)
{
    char path[640];
    struct stat st;

    if (save_file_path(f, t, rel, path, sizeof(path)) != 0) {
        return 0;
    }
    return stat(path, &st) == 0;
}

size_t fixture_backup_count(fixture_t *f)
{
    char dir[400];
    daemoon_strbuf_t sb;
    DIR *d;
    struct dirent *ent;
    size_t n = 0;

    daemoon_strbuf_init(&sb, dir, sizeof(dir));
    daemoon_strbuf_add(&sb, f->work);
    daemoon_strbuf_add(&sb, "/backups");
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return 0;
    }

    d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        ++n;
    }
    closedir(d);
    return n;
}

daemoon_result_t fixture_pack_blob(fixture_t *f, const daemoon_title_t *t,
                                   unsigned int parent_version, unsigned char **out,
                                   size_t *out_len)
{
    char path[400];
    daemoon_strbuf_t sb;
    daemoon_manifest_t m;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *pkg = NULL;
    FILE *fp;
    long size;
    unsigned char *buf;
    daemoon_result_t r;

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, f->root);
    daemoon_strbuf_add(&sb, "/blob-staging.zip");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    daemoon_manifest_init(&m);
    m.platform = t->platform;
    m.save_type = t->save_type;
    m.parent_version = parent_version;
    DAEMOON_TRY(daemoon_strlcpy(m.title_id, sizeof(m.title_id), t->id));
    DAEMOON_TRY(daemoon_strlcpy(m.device_label, sizeof(m.device_label), "other console"));
    DAEMOON_TRY(daemoon_strlcpy(m.created_at, sizeof(m.created_at), "2026-01-01T00:00:00Z"));

    DAEMOON_TRY(f->env.save->open_save(f->env.save_ctx, t, &save));
    r = f->env.fs->open(f->env.fs_ctx, path, DAEMOON_OPEN_WRITE, &pkg);
    if (r != DAEMOON_OK) {
        (void)f->env.save->close_save(f->env.save_ctx, save);
        return r;
    }
    r = daemoon_archive_pack(&f->env, &f->actx, save, &m, pkg);
    if (r == DAEMOON_OK) {
        r = daemoon_stream_close(pkg);
    } else {
        (void)daemoon_stream_close(pkg);
    }
    (void)f->env.save->close_save(f->env.save_ctx, save);
    if (r != DAEMOON_OK) {
        return r;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return DAEMOON_ERR_IO_ERROR;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(fp);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return DAEMOON_ERR_IO_ERROR;
    }
    fclose(fp);
    (void)remove(path);

    *out = buf;
    *out_len = (size_t)size;
    return DAEMOON_OK;
}
