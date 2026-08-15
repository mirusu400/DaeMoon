/* 3DS entry point. Phase 1: local backup and restore, no server involved.
 *
 * Everything below the platform layer already exists and is tested on a desktop.
 * This file assembles a daemoon_env_t out of libctru and drives it from a menu.
 *
 * The scratch buffer is static rather than malloc'd. The 3DS heap fragments badly,
 * this one lives for the whole run, and there is nothing to gain from putting it
 * on the heap and a slow loss of headroom to lose.
 */
#include "daemoon_3ds.h"

#include "../../../tools/test/backend_conformance.h"
#include "../../../tools/test/test.h"

#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/sync.h>
#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_scratch[64 * 1024];
static daemoon_archive_ctx_t g_archive;

static daemoon_3ds_save_ctx_t g_save_ctx;
static daemoon_3ds_ui_ctx_t   g_ui_ctx;
static daemoon_env_t          g_env;

static daemoon_title_t *g_titles;
static size_t           g_title_count;

/* --------------------------------------------------------------------- i18n */

static void select_language(void)
{
    u8 code = 0;
    daemoon_lang_t lang = DAEMOON_LANG_EN;

    if (R_FAILED(CFGU_GetSystemLanguage(&code))) {
        return;
    }
    switch (code) {
    case CFG_LANGUAGE_JP: (void)daemoon_i18n_language_from_code("ja", &lang); break;
    case CFG_LANGUAGE_FR: (void)daemoon_i18n_language_from_code("fr", &lang); break;
    case CFG_LANGUAGE_DE: (void)daemoon_i18n_language_from_code("de", &lang); break;
    case CFG_LANGUAGE_ES: (void)daemoon_i18n_language_from_code("es", &lang); break;
    case CFG_LANGUAGE_ZH: (void)daemoon_i18n_language_from_code("zh-Hans", &lang); break;
    case CFG_LANGUAGE_KO: (void)daemoon_i18n_language_from_code("ko", &lang); break;
    case CFG_LANGUAGE_TW: (void)daemoon_i18n_language_from_code("zh-Hant", &lang); break;
    default:              lang = DAEMOON_LANG_EN; break;
    }
    daemoon_i18n_set_language(lang);

    /* The SMDH title index happens to be the console's own language numbering, so
     * a title's name comes back in the language the HOME menu shows it in. */
    g_save_ctx.smdh_language = (int)code;
}

/* -------------------------------------------------------------------- input */

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

static void pause_for_a(void)
{
    printf("\n  (A) continue\n");
    (void)wait_keys(KEY_A);
}

static void report(const char *what, daemoon_result_t r)
{
    if (r == DAEMOON_OK) {
        printf("%s: ok\n", what);
        return;
    }
    /* The wire code and the translated text. One is for whoever reads a photo of
     * the screen in a bug report, the other is for the person holding the console. */
    printf("%s: %s\n  %s\n", what, daemoon_result_code(r),
           daemoon_str(daemoon_result_str_id(r)));
}

/* ------------------------------------------------------------------- titles */

static daemoon_result_t reload_titles(void)
{
    if (g_titles != NULL) {
        g_env.save->free_titles(g_env.save_ctx, g_titles, g_title_count);
        g_titles = NULL;
        g_title_count = 0;
    }
    return g_env.save->list_titles(g_env.save_ctx, &g_titles, &g_title_count);
}

/* Returns the selected index, or -1. */
static int pick_title(const char *prompt)
{
    size_t selected = 0;
    size_t page = 0;
    const size_t per_page = 20;

    if (g_title_count == 0) {
        consoleClear();
        printf("No titles with save data were found.\n");
        pause_for_a();
        return -1;
    }

    for (;;) {
        size_t i;
        u32 down;

        page = selected / per_page;
        consoleClear();
        printf("%s\n\n", prompt);
        for (i = page * per_page; i < g_title_count && i < (page + 1) * per_page; ++i) {
            printf("%s %s  %s\n", i == selected ? ">" : " ", g_titles[i].id,
                   g_titles[i].name);
        }
        printf("\n  up/down select   (A) choose   (B) back\n");

        down = wait_keys(KEY_A | KEY_B | KEY_UP | KEY_DOWN | KEY_L | KEY_R);
        if (down == 0 || (down & KEY_B)) {
            return -1;
        }
        if (down & KEY_A) {
            return (int)selected;
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && selected + 1 < g_title_count) {
            ++selected;
        }
        if ((down & KEY_L) && selected >= per_page) {
            selected -= per_page;
        }
        if ((down & KEY_R) && selected + per_page < g_title_count) {
            selected += per_page;
        }
    }
}

/* ------------------------------------------------------------------ actions */

static void action_backup(void)
{
    char path[DAEMOON_PATH_MAX * 2];
    int index = pick_title("Back up which title?");
    daemoon_result_t r;

    if (index < 0) {
        return;
    }

    consoleClear();
    printf("%s\n%s\n\n", g_titles[index].id, g_titles[index].name);

    r = daemoon_sync_backup_local(&g_env, &g_archive, &g_titles[index], path, sizeof(path));
    report("backup", r);
    if (r == DAEMOON_OK) {
        printf("  %s\n", path);
    }
    pause_for_a();
}

/* Lists what is in the backup directory for a title and restores one. */
static void action_restore(void)
{
    char dir[DAEMOON_PATH_MAX * 2];
    char pick[DAEMOON_PATH_MAX * 2];
    daemoon_strbuf_t sb;
    int index = pick_title("Restore into which title?");
    daemoon_result_t r;

    if (index < 0) {
        return;
    }

    /* Backups are named after their content, so the newest is not obvious from the
     * name and there is nothing to sort by that can be trusted. Phase 1 restores
     * the one the user points at, and the list is the directory. */
    daemoon_strbuf_init(&sb, dir, sizeof(dir));
    daemoon_strbuf_add(&sb, DAEMOON_3DS_WORK_DIR);
    daemoon_strbuf_add(&sb, "/backups");
    if (daemoon_strbuf_result(&sb) != DAEMOON_OK) {
        return;
    }

    if (daemoon_3ds_pick_backup(dir, &g_titles[index], pick, sizeof(pick)) != DAEMOON_OK) {
        consoleClear();
        printf("No backup for this title yet.\n");
        pause_for_a();
        return;
    }

    consoleClear();
    printf("%s\n%s\n\n", g_titles[index].id, g_titles[index].name);
    printf("Restoring from:\n  %s\n\n", pick);

    /* The secure value is read before anything is written and put back after. A
     * title that binds its save to this console will otherwise treat the restored
     * save as corrupt and delete it, which is the failure this phase exists to
     * understand. */
    {
        daemoon_3ds_secure_value_t secure;
        daemoon_result_t sr = daemoon_3ds_read_secure_value(&g_titles[index], &secure);

        if (sr == DAEMOON_OK && secure.exists) {
            printf("secure value present: %016llX\n", (unsigned long long)secure.value);
        } else if (sr == DAEMOON_OK) {
            printf("no secure value for this title\n");
        } else {
            printf("secure value unreadable: %s\n", daemoon_result_code(sr));
        }

        r = daemoon_sync_restore_package(&g_env, &g_archive, &g_titles[index], pick);
        report("restore", r);

        if (r == DAEMOON_OK && sr == DAEMOON_OK && secure.exists) {
            daemoon_result_t wr = daemoon_3ds_write_secure_value(&g_titles[index], &secure);
            report("secure value restored", wr);
        }
    }
    pause_for_a();
}

static void action_secure_value(void)
{
    int index = pick_title("Show the secure value of which title?");
    daemoon_3ds_secure_value_t secure;
    daemoon_result_t r;

    if (index < 0) {
        return;
    }

    consoleClear();
    printf("%s\n%s\n\n", g_titles[index].id, g_titles[index].name);

    r = daemoon_3ds_read_secure_value(&g_titles[index], &secure);
    if (r != DAEMOON_OK) {
        report("secure value", r);
    } else if (secure.exists) {
        printf("secure value: %016llX\n", (unsigned long long)secure.value);
        printf("\nThis title ties its save to this console.\n");
        printf("A save from another console may be deleted by the game.\n");
    } else {
        printf("no secure value\n\nA save from another console should be safe here,\n");
        printf("as far as this mechanism is concerned.\n");
    }
    pause_for_a();
}

/* Reads every title once and writes what it found to the SD card.
 *
 * Read only: it opens save archives, counts what is in them, and asks for each
 * title's secure value. Nothing is written to any archive.
 *
 * It exists because the two open questions of this phase are answered by data
 * about a whole console, and reading that off a screen one title at a time is how
 * a survey turns into three titles and a guess. The file can be pulled off the
 * card with tools/3ds-deploy.sh and read on a desktop. */
static int survey_count_cb(void *user, const char *path, unsigned long long size)
{
    unsigned long long *total = (unsigned long long *)user;

    (void)path;
    *total += size;
    return 0;
}

static void action_survey(void)
{
    daemoon_stream_t *out = NULL;
    daemoon_result_t r;
    size_t i;

    consoleClear();
    printf("Survey\n\n");
    printf("Reads every title: save size and secure value.\n");
    printf("Nothing is written to any save.\n\n");
    printf("  (A) run   (B) back\n");
    if (!(wait_keys(KEY_A | KEY_B) & KEY_A)) {
        return;
    }

    consoleClear();
    printf("surveying %u titles...\n", (unsigned)g_title_count);

    r = g_env.fs->open(g_env.fs_ctx, DAEMOON_3DS_WORK_DIR "/survey.txt",
                       DAEMOON_OPEN_WRITE, &out);
    if (r != DAEMOON_OK) {
        report("survey", r);
        pause_for_a();
        return;
    }

    for (i = 0; i < g_title_count; ++i) {
        daemoon_title_t *t = &g_titles[i];
        daemoon_3ds_secure_value_t secure;
        daemoon_save_t *save = NULL;
        unsigned long long bytes = 0;
        char line[320];
        daemoon_strbuf_t sb;
        daemoon_result_t sr;
        daemoon_result_t or_;

        memset(&secure, 0, sizeof(secure));
        sr = daemoon_3ds_read_secure_value(t, &secure);

        or_ = g_env.save->open_save(g_env.save_ctx, t, &save);
        if (or_ == DAEMOON_OK) {
            (void)g_env.save->list_entries(g_env.save_ctx, save, survey_count_cb, &bytes);
            (void)g_env.save->close_save(g_env.save_ctx, save);
        }

        daemoon_strbuf_init(&sb, line, sizeof(line));
        daemoon_strbuf_add(&sb, t->id);
        daemoon_strbuf_add(&sb, "\tsave=");
        daemoon_strbuf_add(&sb, or_ == DAEMOON_OK ? "yes" : daemoon_result_code(or_));
        daemoon_strbuf_add(&sb, "\tbytes=");
        daemoon_strbuf_add_uint(&sb, bytes);
        daemoon_strbuf_add(&sb, "\tsecure=");
        if (sr != DAEMOON_OK) {
            daemoon_strbuf_add(&sb, daemoon_result_code(sr));
        } else if (secure.exists) {
            /* The value itself, because whether two titles share one and whether
             * it survives a restore are both questions about the number. */
            char hex[17];
            int k;
            static const char digits[] = "0123456789ABCDEF";
            for (k = 0; k < 16; ++k) {
                hex[k] = digits[(secure.value >> ((15 - k) * 4)) & 0xf];
            }
            hex[16] = '\0';
            daemoon_strbuf_add(&sb, hex);
        } else {
            daemoon_strbuf_add(&sb, "none");
        }
        daemoon_strbuf_add(&sb, "\t");
        daemoon_strbuf_add(&sb, t->name);
        daemoon_strbuf_addc(&sb, '\n');

        (void)daemoon_stream_write(out, line, sb.len);

        printf("%u/%u\r", (unsigned)(i + 1), (unsigned)g_title_count);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    r = daemoon_stream_close(out);
    printf("\n");
    report("survey", r);
    printf("  %s\n", DAEMOON_3DS_WORK_DIR "/survey.txt");
    pause_for_a();
}

/* The conformance suite writes, clears and commits a real save archive. It exists
 * to prove this backend behaves the way core is entitled to assume, and running it
 * on a title someone plays would be indefensible. */
static void action_self_test(void)
{
    int index;
    int before;
    char path[DAEMOON_PATH_MAX * 2];
    daemoon_backend_under_test_t ut;

    consoleClear();
    printf("Backend self test\n\n");
    printf("This DESTROYS the save of the title you pick.\n");
    printf("It writes files, clears the archive and commits.\n\n");
    printf("Use a dummy title. Never one you play.\n");
    printf("A backup is made first, but do not rely on it.\n");
    printf("\n  (A) continue   (B) back\n");
    if (!(wait_keys(KEY_A | KEY_B) & KEY_A)) {
        return;
    }

    index = pick_title("Self test on which title? Its save WILL be destroyed.");
    if (index < 0) {
        return;
    }

    consoleClear();
    printf("%s\n%s\n\n", g_titles[index].id, g_titles[index].name);
    printf("Last chance. This wipes the save above.\n");
    printf("\n  (A) run   (B) back\n");
    if (!(wait_keys(KEY_A | KEY_B) & KEY_A)) {
        return;
    }

    consoleClear();
    printf("backing up first...\n");
    report("backup", daemoon_sync_backup_local(&g_env, &g_archive, &g_titles[index],
                                               path, sizeof(path)));

    memset(&ut, 0, sizeof(ut));
    ut.name = "3ds";
    ut.backend = &daemoon_3ds_save_backend;
    ut.ctx = &g_save_ctx;
    ut.title = &g_titles[index];
    ut.other = NULL; /* a second title would be a second save to destroy */
    ut.scratch = g_scratch;
    ut.scratch_len = sizeof(g_scratch);

    before = daemoon_test_failures;
    daemoon_backend_conformance(&ut);

    printf("\n%d checks, %d failures\n", daemoon_test_checks,
           daemoon_test_failures - before);
    if (daemoon_test_failures == before) {
        printf("this backend behaves the way core assumes\n");
    } else {
        printf("DO NOT sync with this build\n");
    }
    pause_for_a();
}

/* ---------------------------------------------------------------- autotest */

/* Runs the conformance suite unattended and writes the result to the SD card.
 *
 * It exists so the backend can be exercised somewhere other than a person's
 * hands: an emulator, or a console left to run while someone else reads the
 * result afterwards. Reading a pass or a fail off a file beats reading it off a
 * photograph of a screen.
 *
 * It targets **this application's own save archive** and nothing else. The suite
 * clears whatever it is pointed at, and an unattended run that could pick a title
 * someone plays would be indefensible. A build with SAVEDATA_SIZE=0K has no such
 * archive, so the run reports that and does nothing, which is the right answer
 * for the shipped app.
 */
static void run_autotest(void)
{
    daemoon_backend_under_test_t ut;
    daemoon_title_t self;
    daemoon_stream_t *out = NULL;
    char line[256];
    daemoon_strbuf_t sb;
    u64 program_id = 0;
    int failures;

    consoleClear();
    printf("autotest: running unattended\n");

    memset(&self, 0, sizeof(self));
    if (R_FAILED(APT_GetProgramID(&program_id))) {
        printf("autotest: cannot read this title's own id\n");
        return;
    }
    daemoon_3ds_format_title_id(program_id, self.id, sizeof(self.id));
    (void)daemoon_strlcpy(self.name, sizeof(self.name), "DaeMoon itself");
    self.platform = DAEMOON_PLATFORM_3DS;
    self.save_type = DAEMOON_SAVE_SAVEDATA;
    self.size_hint = (unsigned long long)MEDIATYPE_SD;
    self.has_save = 1;

    printf("autotest: target %s\n", self.id);

    /* The archive does not exist until this title formats it once. A build with
     * SAVEDATA_SIZE=0K has none to format, and says so rather than pretending. */
    {
        daemoon_save_t *probe = NULL;
        daemoon_result_t r = daemoon_3ds_save_backend.open_save(&g_save_ctx, &self, &probe);

        if (r == DAEMOON_OK) {
            (void)daemoon_3ds_save_backend.close_save(&g_save_ctx, probe);
        } else {
            printf("autotest: no archive yet, formatting this title's own\n");
            r = daemoon_3ds_format_own_save(&self, 128);
            report("autotest: format", r);
            if (r != DAEMOON_OK) {
                printf("autotest: build with SAVEDATA_SIZE=128K to run this\n");
                (void)wait_keys(KEY_A | KEY_START);
                return;
            }
        }
    }

    memset(&ut, 0, sizeof(ut));
    ut.name = "3ds";
    ut.backend = &daemoon_3ds_save_backend;
    ut.ctx = &g_save_ctx;
    ut.title = &self;
    ut.other = NULL;
    ut.scratch = g_scratch;
    ut.scratch_len = sizeof(g_scratch);

    failures = daemoon_test_failures;
    daemoon_backend_conformance(&ut);
    failures = daemoon_test_failures - failures;

    daemoon_strbuf_init(&sb, line, sizeof(line));
    daemoon_strbuf_add(&sb, "target=");
    daemoon_strbuf_add(&sb, self.id);
    daemoon_strbuf_add(&sb, " checks=");
    daemoon_strbuf_add_uint(&sb, (unsigned long long)daemoon_test_checks);
    daemoon_strbuf_add(&sb, " failures=");
    daemoon_strbuf_add_uint(&sb, (unsigned long long)failures);
    if (failures > 0 && daemoon_test_last_failure[0] != '\0') {
        daemoon_strbuf_add(&sb, "\nfirst: ");
        daemoon_strbuf_add(&sb, daemoon_test_last_failure);
    }
    daemoon_strbuf_addc(&sb, '\n');

    printf("%s", line);

    if (g_env.fs->open(g_env.fs_ctx, DAEMOON_3DS_WORK_DIR "/selftest.txt",
                       DAEMOON_OPEN_WRITE, &out) == DAEMOON_OK) {
        (void)daemoon_stream_write(out, line, sb.len);
        (void)daemoon_stream_close(out);
        printf("autotest: wrote %s\n", DAEMOON_3DS_WORK_DIR "/selftest.txt");
    } else {
        printf("autotest: could not write the result file\n");
    }

    /* Leave the screen up long enough to be photographed if anyone is watching. */
    (void)wait_keys(KEY_A | KEY_START);
}

/* ---------------------------------------------------------------------- main */

static void draw_menu(size_t selected)
{
    static const char *const items[] = {
        "Back up a save",
        "Restore a save from a backup",
        "Show a title's secure value",
        "Survey every title, write it to the SD card",
        "Run the backend self test (destroys a save)",
        "Exit"
    };
    size_t i;

    consoleClear();
    printf("%s  -  Phase 1\n", daemoon_str(DAEMOON_STR_APP_TITLE));
    printf("%s\n", DAEMOON_3DS_WORK_DIR);
    printf("%u titles with save data\n\n", (unsigned)g_title_count);

    for (i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        printf("%s %s\n", i == selected ? ">" : " ", items[i]);
    }
    printf("\n  up/down   (A) select   START exit\n");
}

int main(void)
{
    size_t selected = 0;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    g_save_ctx.smdh_language = 1;
    if (R_SUCCEEDED(cfguInit())) {
        select_language();
    }
    if (R_FAILED(amInit())) {
        printf("am:u unavailable. Title enumeration will not work.\n");
        printf("This build has to be installed as a CIA.\n");
    }

    g_save_ctx.media = 1; /* MEDIATYPE_SD */
    g_save_ctx.only_with_saves = 1;
    g_save_ctx.smdh_language = 1; /* English, until the console says otherwise */
    daemoon_3ds_ui_init(&g_ui_ctx);
    g_archive.count = 0;

    memset(&g_env, 0, sizeof(g_env));
    g_env.save = &daemoon_3ds_save_backend;
    g_env.fs = &daemoon_3ds_fs_backend;
    g_env.ui = &daemoon_3ds_ui_backend;
    g_env.net = NULL; /* Phase 2 */
    g_env.save_ctx = &g_save_ctx;
    g_env.fs_ctx = NULL;
    g_env.ui_ctx = &g_ui_ctx;
    g_env.device_label = "3DS";
    g_env.work_dir = DAEMOON_3DS_WORK_DIR;
    g_env.scratch = g_scratch;
    g_env.scratch_len = sizeof(g_scratch);

    report("titles", reload_titles());

    /* A flag file rather than a menu entry, so an emulator or an unattended
     * console can run the suite and leave the answer behind. */
    if (g_env.fs->exists(g_env.fs_ctx, DAEMOON_3DS_WORK_DIR "/AUTOTEST")) {
        run_autotest();
        goto done;
    }

    while (aptMainLoop()) {
        u32 down;

        draw_menu(selected);
        down = wait_keys(KEY_A | KEY_UP | KEY_DOWN | KEY_START);
        if (down == 0 || (down & KEY_START)) {
            break;
        }
        if ((down & KEY_UP) && selected > 0) {
            --selected;
        }
        if ((down & KEY_DOWN) && selected < 5) {
            ++selected;
        }
        if (!(down & KEY_A)) {
            continue;
        }

        switch (selected) {
        case 0: action_backup(); break;
        case 1: action_restore(); break;
        case 2: action_secure_value(); break;
        case 3: action_survey(); break;
        case 4: action_self_test(); (void)reload_titles(); break;
        default:
            aptSetChainloader(0, 0);
            goto done;
        }
    }

done:
    if (g_titles != NULL) {
        g_env.save->free_titles(g_env.save_ctx, g_titles, g_title_count);
    }
    amExit();
    cfguExit();
    gfxExit();
    return 0;
}
