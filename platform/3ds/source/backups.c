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

/* --------------------------------------------------------------------- drawing */

/* The digest, short. Two backups of the same title differ only here, so it is what
 * the file name is made of and what a bug report should carry. */
static void short_digest(const daemoon_3ds_backup_row_t *row, char *out, size_t cap)
{
    if (row->readable) {
        (void)snprintf(out, cap, "%.12s", row->sha256);
        return;
    }
    (void)daemoon_strlcpy(out, cap, "unreadable");
}

static void draw_details(const daemoon_3ds_backup_row_t *row, const daemoon_title_t *title)
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

    daemoon_3ds_backup_size_text(row->size, size, sizeof(size));
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

static void draw_list(const daemoon_3ds_backup_row_t *rows, size_t count, size_t selected,
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
            daemoon_3ds_backup_size_text(rows[index].size, size, sizeof(size));
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

/* Draws both halves of this screen, with no input, for the unattended test.
 *
 * The list itself is covered on a desktop. What is not, and what has now cost two
 * trips to a console, is the drawing: real citro2d, a real font, and strings whose
 * length depends on which language the console is in. This runs it against rows
 * that cover the cases the layout has to survive - a package that could not be
 * read, one that matches the console, one with nothing in it - so an emulator can
 * fault instead of somebody's afternoon.
 */
void daemoon_3ds_pick_backup_render_check(const daemoon_title_t *title, unsigned frames)
{
    daemoon_3ds_backup_row_t rows[3];
    unsigned f;

    memset(rows, 0, sizeof(rows));
    (void)daemoon_strlcpy(rows[0].name, sizeof(rows[0].name),
                          "3ds_0004000000055D00_0123456789ab.zip");
    rows[0].size = 967;
    (void)daemoon_strlcpy(rows[0].created_at, sizeof(rows[0].created_at),
                          "1970-01-01T00:00:00Z");
    (void)daemoon_strlcpy(rows[0].device_label, sizeof(rows[0].device_label), "3DS");
    (void)daemoon_strlcpy(rows[0].sha256, sizeof(rows[0].sha256),
                          "0123456789abcdef0123456789abcdef"
                          "0123456789abcdef0123456789abcdef");
    rows[0].readable = 1;
    rows[0].is_current = 1;

    rows[1] = rows[0];
    rows[1].is_current = 0;
    rows[1].size = 5ull * 1024ull * 1024ull;

    /* Everything blank, which is what a package that could not be read leaves
     * behind. Nothing here may treat an empty digest as a string to print past. */
    (void)daemoon_strlcpy(rows[2].name, sizeof(rows[2].name), "3ds_broken.zip");

    for (f = 0; f < frames; ++f) {
        size_t selected = f % (sizeof(rows) / sizeof(rows[0]));

        daemoon_gfx_frame_begin();
        draw_details(&rows[selected], title);
        draw_list(rows, sizeof(rows) / sizeof(rows[0]), selected, 0);
        daemoon_gfx_frame_end();
    }
}

/* --------------------------------------------------------------------- picking */

static daemoon_result_t delete_row(const daemoon_env_t *env, const char *dir,
                                   const daemoon_3ds_backup_row_t *row)
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
    static daemoon_3ds_backup_row_t rows[MAX_SHOWN];
    size_t count;
    size_t selected = 0;
    size_t scroll = 0;
    int chosen = 0;
    int drew = 0;

    daemoon_3ds_trace("pick/gather", dir);
    count = daemoon_3ds_backup_list(env, dir, title, current_digest, rows, MAX_SHOWN);
    daemoon_3ds_trace_uint("pick/rows", (unsigned long long)count);
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
        if (!drew) {
            drew = 1;
            daemoon_3ds_trace("pick/drew", NULL);
        }

        if (down & KEY_B) {
            return DAEMOON_ERR_USER_CANCELLED;
        }
        if (down & KEY_A) {
            chosen = 1;
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
                count = daemoon_3ds_backup_list(env, dir, title, current_digest, rows, MAX_SHOWN);
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

    if (!chosen) {
        /* aptMainLoop said the application is going away - HOME, or the power
         * button. Falling out of the loop is not a choice, and treating it as one
         * would start a restore while the system is trying to close us. */
        daemoon_3ds_trace("pick/apt-exit", NULL);
        return DAEMOON_ERR_USER_CANCELLED;
    }

    {
        daemoon_strbuf_t sb;

        daemoon_strbuf_init(&sb, out, cap);
        daemoon_strbuf_add(&sb, dir);
        daemoon_strbuf_addc(&sb, '/');
        daemoon_strbuf_add(&sb, rows[selected].name);
        daemoon_3ds_trace("pick/chose", rows[selected].name);
        return daemoon_strbuf_result(&sb);
    }
}
