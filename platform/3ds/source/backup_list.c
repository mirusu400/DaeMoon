/* Which backups exist for a title, and what each one says about itself.
 *
 * Kept apart from the screen that shows them so it can be run on a desktop. The
 * first version of this was written straight into the drawing code and shipped to
 * a console without a test, which is exactly the order this project is supposed to
 * do things in and did not.
 *
 * A backup is named after its content digest - right for storage, useless for a
 * person - so the metadata comes out of each package's own manifest. That means
 * opening up to thirty two zips, and every one of them is a file a card reader or
 * a half finished write could have damaged. None of that may take the application
 * down: an unreadable package is a row that says so, because it is still a file
 * taking up space and the user still has to be able to delete it.
 */
#include "daemoon_3ds.h"

#include <daemoon/archive.h>
#include <daemoon/manifest.h>
#include <daemoon/util/strbuf.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

void daemoon_3ds_backup_size_text(unsigned long long bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull) {
        (void)snprintf(out, cap, "%lu.%lu MB", (unsigned long)(bytes / (1024ull * 1024ull)),
                       (unsigned long)((bytes % (1024ull * 1024ull)) * 10ull /
                                       (1024ull * 1024ull)));
    } else if (bytes >= 1024ull) {
        (void)snprintf(out, cap, "%lu KB", (unsigned long)(bytes / 1024ull));
    } else {
        (void)snprintf(out, cap, "%lu B", (unsigned long)bytes);
    }
}

/* ------------------------------------------------------------------ gathering */

static daemoon_result_t read_row(const daemoon_env_t *env, const char *dir,
                                 daemoon_3ds_backup_row_t *row)
{
    char path[DAEMOON_PATH_MAX];
    daemoon_strbuf_t sb;
    daemoon_stream_t *pkg = NULL;
    daemoon_manifest_t m;
    daemoon_result_t r;

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, row->name);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    DAEMOON_TRY(env->fs->open(env->fs_ctx, path, DAEMOON_OPEN_READ, &pkg));
    r = daemoon_archive_read_manifest(pkg, &m);
    (void)daemoon_stream_close(pkg);
    if (r != DAEMOON_OK) {
        return r;
    }

    row->size = m.size;
    (void)daemoon_strlcpy(row->created_at, sizeof(row->created_at), m.created_at);
    (void)daemoon_strlcpy(row->device_label, sizeof(row->device_label), m.device_label);
    (void)daemoon_strlcpy(row->sha256, sizeof(row->sha256), m.sha256);
    row->readable = 1;
    return DAEMOON_OK;
}

/* Newest first, by the string, which works because the timestamps are ISO 8601 and
 * fixed width. A package whose manifest could not be read sorts to the bottom
 * rather than being hidden: it is still a file taking up space and the user should
 * be able to delete it. */
static void sort_rows(daemoon_3ds_backup_row_t *rows, size_t count)
{
    size_t i;

    for (i = 1; i < count; ++i) {
        daemoon_3ds_backup_row_t key = rows[i];
        size_t j = i;

        while (j > 0) {
            const daemoon_3ds_backup_row_t *prev = &rows[j - 1];
            int worse;

            if (prev->readable != key.readable) {
                worse = !prev->readable;
            } else {
                worse = strcmp(prev->created_at, key.created_at) < 0;
            }
            if (!worse) {
                break;
            }
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = key;
    }
}

size_t daemoon_3ds_backup_list(const daemoon_env_t *env, const char *dir,
                               const daemoon_title_t *title, const char *current_digest,
                               daemoon_3ds_backup_row_t *rows, size_t cap)
{
    char prefix[DAEMOON_TITLE_ID_MAX + 16];
    daemoon_strbuf_t sb;
    DIR *d;
    struct dirent *ent;
    size_t count = 0;
    size_t i;

    /* The same key the sync state and the staging path use: a title id is only
     * unique together with its platform. */
    daemoon_strbuf_init(&sb, prefix, sizeof(prefix));
    daemoon_strbuf_add(&sb, daemoon_platform_name(title->platform));
    daemoon_strbuf_addc(&sb, '_');
    daemoon_strbuf_add(&sb, title->id);
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return 0;
    }

    d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    while (count < cap && (ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        memset(&rows[count], 0, sizeof(rows[count]));
        (void)daemoon_strlcpy(rows[count].name, sizeof(rows[count].name), ent->d_name);
        ++count;
    }
    (void)closedir(d);

    for (i = 0; i < count; ++i) {
        daemoon_3ds_trace("pick/manifest", rows[i].name);
        (void)read_row(env, dir, &rows[i]);
        if (current_digest != NULL && current_digest[0] != '\0' && rows[i].readable) {
            rows[i].is_current = strcmp(rows[i].sha256, current_digest) == 0;
        }
    }
    sort_rows(rows, count);
    return count;
}

