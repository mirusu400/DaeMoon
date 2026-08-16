/* Choosing which backup to restore, and getting rid of the ones that are not
 * wanted.
 *
 * This screen used to be printf into a text console, left over from before the UI
 * was drawn with citro2d. Once the console was gone the calls stayed, and the
 * result was not a blank list: consoleClear on a console that was never
 * initialised, and gfxSwapBuffers while citro3d owns the GPU, means the last
 * frame stays on screen and the input loop never gives up. The application looked
 * frozen, on the screen after "restore", which is the worst possible place for it
 * to look frozen. Nothing had been written by then - the freeze was before the
 * confirmation - but from the outside that is indistinguishable from a restore
 * that stopped halfway.
 *
 * So: drawn like everything else, and while it was being rewritten it grew the
 * thing the list was missing. A backup is named after its content digest, which
 * is right for storage and useless for a person, so each package's manifest is
 * read and the list says how big the save is, which console it came from, and
 * when. There is also a delete, because "version management" without one just
 * means the card fills up.
 *
 * On dates: created_at comes from the console clock, which the user can set, and
 * this project never decides anything from it. It is shown because a person
 * recognises their own backups by it, it is sorted by because a list has to be in
 * some order, and the screen says where it comes from. What it never does is
 * decide which save is newer - that is the server issued version, and for purely
 * local backups it is the user's call, which is why this screen exists at all.
 */
#include "daemoon_3ds.h"
#include "gfx.h"

#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/manifest.h>
#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/* Past this the rest are not shown. A title with more than thirty two backups of
 * one save has a different problem than this screen can solve. */
#define MAX_SHOWN 32

#define ROW_H     34.0f
#define ROWS_PAGE 6

typedef struct {
    char               name[96];
    /* From the package's own manifest. Read once when the list is built: opening
     * thirty two zips is already the slow part of this screen and doing it per
     * frame would make it unusable. */
    unsigned long long size;
    char               created_at[DAEMOON_TIMESTAMP_MAX];
    char               device_label[DAEMOON_LABEL_MAX];
    char               sha256[DAEMOON_SHA256_HEX];
    int                readable;
    /* Whether this package holds exactly what is on the console right now. A fact
     * about content rather than about clocks, so unlike the date it can be trusted
     * and is worth saying out loud: restoring it would change nothing. */
    int                is_current;
} backup_row_t;

/* ------------------------------------------------------------------ gathering */

static daemoon_result_t read_row(const daemoon_env_t *env, const char *dir,
                                 backup_row_t *row)
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
static void sort_rows(backup_row_t *rows, size_t count)
{
    size_t i;

    for (i = 1; i < count; ++i) {
        backup_row_t key = rows[i];
        size_t j = i;

        while (j > 0) {
            const backup_row_t *prev = &rows[j - 1];
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

static size_t gather(const daemoon_env_t *env, const char *dir,
                     const daemoon_title_t *title, const char *current_digest,
                     backup_row_t *rows)
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
    while (count < MAX_SHOWN && (ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        memset(&rows[count], 0, sizeof(rows[count]));
        (void)daemoon_strlcpy(rows[count].name, sizeof(rows[count].name), ent->d_name);
        ++count;
    }
    (void)closedir(d);

    for (i = 0; i < count; ++i) {
        (void)read_row(env, dir, &rows[i]);
        if (current_digest != NULL && current_digest[0] != '\0' && rows[i].readable) {
            rows[i].is_current = strcmp(rows[i].sha256, current_digest) == 0;
        }
    }
    sort_rows(rows, count);
    return count;
}

/* --------------------------------------------------------------------- drawing */

static void human_size(unsigned long long bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull) {
        (void)snprintf(out, cap, "%llu.%llu MB", bytes / (1024ull * 1024ull),
                       (bytes % (1024ull * 1024ull)) * 10ull / (1024ull * 1024ull));
    } else if (bytes >= 1024ull) {
        (void)snprintf(out, cap, "%llu KB", bytes / 1024ull);
    } else {
        (void)snprintf(out, cap, "%llu B", bytes);
    }
}

/* The digest, short. Two backups of the same title differ only here, so it is what
 * the file name is made of and what a bug report should carry. */
static void short_digest(const backup_row_t *row, char *out, size_t cap)
{
    if (row->readable) {
        (void)snprintf(out, cap, "%.12s", row->sha256);
        return;
    }
    (void)daemoon_strlcpy(out, cap, "unreadable");
}

static void draw_details(const backup_row_t *row, const daemoon_title_t *title)
{
    char line[160];
    char size[32];
    float y;

    daemoon_gfx_top();
    daemoon_gfx_rect(0.0f, 0.0f, GFX_TOP_W, 28.0f, GFX_ACCENT_D);
    {
        daemoon_str_ref_t ref;

        memset(&ref, 0, sizeof(ref));
        ref.id = DAEMOON_STR_BACKUP_LIST;
        ref.args[0] = title->name;
        ref.nargs = 1;
        (void)daemoon_strf(line, sizeof(line), ref.id, ref.args, ref.nargs);
    }
    daemoon_gfx_text_fit(10.0f, 6.0f, GFX_TOP_W - 20.0f, 0.55f, GFX_TEXT, line);

    y = 44.0f;
    if (row == NULL) {
        return;
    }

    human_size(row->size, size, sizeof(size));
    (void)snprintf(line, sizeof(line), "%s", row->readable ? size : "unreadable package");
    daemoon_gfx_text(14.0f, y, 0.6f, GFX_TEXT, line);
    y += 30.0f;

    if (row->readable) {
        (void)snprintf(line, sizeof(line), "%s   %s", row->created_at, row->device_label);
        daemoon_gfx_text(14.0f, y, 0.42f, GFX_TEXT_DIM, line);
        y += 22.0f;
    }

    short_digest(row, line, sizeof(line));
    daemoon_gfx_text(14.0f, y, 0.42f, GFX_TEXT_DIM, line);
    y += 26.0f;

    if (row->is_current) {
        daemoon_gfx_text(14.0f, y, 0.42f, GFX_OK,
                         daemoon_str(DAEMOON_STR_BACKUP_SAME_AS_CONSOLE));
        y += 22.0f;
    }

    /* Said on the screen the dates are on, not in a document nobody opens. */
    (void)daemoon_gfx_text_wrapped(14.0f, GFX_SCREEN_H - 52.0f, GFX_TOP_W - 28.0f, 0.36f,
                                   GFX_TEXT_DIM,
                                   daemoon_str(DAEMOON_STR_BACKUP_CLOCK_NOTE));
}

static void draw_list(const backup_row_t *rows, size_t count, size_t selected,
                      size_t scroll)
{
    size_t i;

    daemoon_gfx_bottom();
    for (i = 0; i < ROWS_PAGE; ++i) {
        size_t index = scroll + i;
        float y = 8.0f + (float)i * ROW_H;
        char size[32];
        char label[96];

        if (index >= count) {
            break;
        }
        if (index == selected) {
            daemoon_gfx_rect(6.0f, y, GFX_BOTTOM_W - 12.0f, ROW_H - 4.0f, GFX_ACCENT);
        } else {
            daemoon_gfx_rect(6.0f, y, GFX_BOTTOM_W - 12.0f, ROW_H - 4.0f, GFX_PANEL);
        }

        if (rows[index].readable) {
            human_size(rows[index].size, size, sizeof(size));
            (void)snprintf(label, sizeof(label), "%.10s   %s", rows[index].created_at,
                           size);
        } else {
            (void)daemoon_strlcpy(label, sizeof(label), "unreadable package");
        }
        daemoon_gfx_text_fit(12.0f, y + 4.0f, GFX_BOTTOM_W - 24.0f, 0.42f, GFX_TEXT,
                             label);

        short_digest(&rows[index], label, sizeof(label));
        daemoon_gfx_text(12.0f, y + 18.0f, 0.34f,
                         rows[index].is_current ? GFX_OK : GFX_TEXT_DIM, label);
    }

    daemoon_gfx_text(8.0f, GFX_SCREEN_H - 18.0f, 0.36f, GFX_TEXT_DIM,
                     "A restore   X delete   B back");
}

/* --------------------------------------------------------------------- picking */

static daemoon_result_t delete_row(const daemoon_env_t *env, const char *dir,
                                   const backup_row_t *row)
{
    char path[DAEMOON_PATH_MAX];
    daemoon_strbuf_t sb;
    daemoon_str_ref_t ask;

    memset(&ask, 0, sizeof(ask));
    ask.id = DAEMOON_STR_CONFIRM_DELETE_BACKUP;
    if (!env->ui->confirm(env->ui_ctx, &ask)) {
        return DAEMOON_ERR_USER_CANCELLED;
    }

    daemoon_strbuf_init(&sb, path, sizeof(path));
    daemoon_strbuf_add(&sb, dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, row->name);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    return env->fs->remove(env->fs_ctx, path);
}

daemoon_result_t daemoon_3ds_pick_backup(const daemoon_env_t *env, const char *dir,
                                         const daemoon_title_t *title,
                                         const char *current_digest, char *out,
                                         size_t cap)
{
    /* Thirty two of these is about twenty kilobytes. Static rather than on the
     * stack, which on this platform is small enough to care about. */
    static backup_row_t rows[MAX_SHOWN];
    size_t count;
    size_t selected = 0;
    size_t scroll = 0;

    count = gather(env, dir, title, current_digest, rows);
    if (count == 0) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    while (aptMainLoop()) {
        u32 down;

        hidScanInput();
        down = hidKeysDown();

        daemoon_gfx_frame_begin();
        draw_details(&rows[selected], title);
        draw_list(rows, count, selected, scroll);
        daemoon_gfx_frame_end();

        if (down & KEY_B) {
            return DAEMOON_ERR_USER_CANCELLED;
        }
        if (down & KEY_A) {
            break;
        }
        if (down & KEY_X) {
            /* The confirmation draws its own frames, so it happens between ours
             * rather than inside one. */
            if (delete_row(env, dir, &rows[selected]) == DAEMOON_OK) {
                if (env->ui->notify != NULL) {
                    daemoon_str_ref_t done;

                    memset(&done, 0, sizeof(done));
                    done.id = DAEMOON_STR_BACKUP_DELETED;
                    env->ui->notify(env->ui_ctx, &done);
                }
                count = gather(env, dir, title, current_digest, rows);
                if (count == 0) {
                    return DAEMOON_ERR_NOT_FOUND;
                }
                if (selected >= count) {
                    selected = count - 1;
                }
            }
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && selected + 1 < count) {
            ++selected;
        }
        if (selected < scroll) {
            scroll = selected;
        } else if (selected >= scroll + ROWS_PAGE) {
            scroll = selected - ROWS_PAGE + 1;
        }
    }

    {
        daemoon_strbuf_t sb;

        daemoon_strbuf_init(&sb, out, cap);
        daemoon_strbuf_add(&sb, dir);
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, rows[selected].name);
        return daemoon_strbuf_result(&sb);
    }
}
