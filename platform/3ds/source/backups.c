/* Finding a backup to restore from.
 *
 * Backups are named after their content, which is what keeps two backups of an
 * unchanged save from piling up. It also means the newest is not obvious from the
 * name, and there is nothing to sort by that can be trusted: the console clock is
 * user settable, so a file's timestamp is a suggestion.
 *
 * So Phase 1 shows the list and the user points at one.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define MAX_SHOWN 32

static u32 wait_keys(u32 mask)
{
    while (aptMainLoop()) {
        u32 down;

        hidScanInput();
        down = hidKeysDown();
        if (down & mask) {
            return down;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}

daemoon_result_t daemoon_3ds_pick_backup(const char *dir, const daemoon_title_t *title,
                                         char *out, size_t cap)
{
    char prefix[DAEMOON_TITLE_ID_MAX + 16];
    char names[MAX_SHOWN][96];
    daemoon_strbuf_t sb;
    DIR *d;
    struct dirent *ent;
    size_t count = 0;
    size_t selected = 0;

    /* The same key the sync state and the staging path use: a title id is only
     * unique together with its platform. */
    daemoon_strbuf_init(&sb, prefix, sizeof(prefix));
    daemoon_strbuf_add(&sb, daemoon_platform_name(title->platform));
    daemoon_strbuf_addc(&sb, '_');
    daemoon_strbuf_add(&sb, title->id);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    d = opendir(dir);
    if (d == NULL) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    while (count < MAX_SHOWN && (ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        (void)daemoon_strlcpy(names[count], sizeof(names[count]), ent->d_name);
        ++count;
    }
    closedir(d);

    if (count == 0) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    for (;;) {
        size_t i;
        u32 down;

        consoleClear();
        printf("Restore which backup?\n\n");
        for (i = 0; i < count; ++i) {
            printf("%s %s\n", i == selected ? ">" : " ", names[i]);
        }
        printf("\n  up/down   (A) choose   (B) back\n");

        down = wait_keys(KEY_A | KEY_B | KEY_UP | KEY_DOWN);
        if (down == 0 || (down & KEY_B)) {
            return DAEMOON_ERR_USER_CANCELLED;
        }
        if (down & KEY_A) {
            break;
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && selected + 1 < count) {
            ++selected;
        }
    }

    daemoon_strbuf_init(&sb, out, cap);
    daemoon_strbuf_add(&sb, dir);
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, names[selected]);
    return daemoon_strbuf_result(&sb);
}
