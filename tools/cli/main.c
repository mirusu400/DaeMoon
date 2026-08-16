/* daemoonctl - the desktop client.
 *
 * It links the same core/ that a 3DS CIA and a Switch NRO link, wired to the posix
 * backend instead of libctru or libnx. Nothing in core can tell the difference,
 * which is the whole point of platform/posix existing: the entire sync path,
 * including conflict resolution and the restore ordering, runs against a real
 * daemoond on a build machine before anyone opens a console.
 *
 * It is a development tool. It speaks plain HTTP and has no TLS.
 */
#define _POSIX_C_SOURCE 200809L

#include "daemoon_posix.h"

#include <daemoon/api.h>
#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/sync.h>
#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

/* Core never allocates a copy buffer, so the app owns one. 64 KiB is generous on a
 * desktop and is the same order as what a console build would hand it. */
static unsigned char g_scratch[64 * 1024];

typedef struct {
    daemoon_posix_save_ctx_t save;
    daemoon_posix_fs_ctx_t   fs;
    daemoon_posix_ui_ctx_t   ui;
    daemoon_posix_net_ctx_t  net;
    daemoon_env_t            env;
    daemoon_archive_ctx_t    actx;
} app_t;

/* ------------------------------------------------------------ interactive ui */

static void print_str(const daemoon_str_ref_t *ref)
{
    char text[512];

    (void)daemoon_strf(text, sizeof(text), ref->id, ref->args, ref->nargs);
    printf("%s\n", text);
}

static int read_yes_no(void)
{
    char line[16];

    printf("  [y/N] ");
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return 0;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

static int cli_confirm(void *ctx, const daemoon_str_ref_t *msg)
{
    (void)ctx;
    print_str(msg);
    return read_yes_no();
}

static void cli_progress(void *ctx, const daemoon_str_ref_t *label, int pct)
{
    char text[512];

    (void)ctx;
    (void)daemoon_strf(text, sizeof(text), label->id, label->args, label->nargs);
    if (pct < 0) {
        printf("  %s...\n", text);
    } else {
        printf("  %s... %d%%\n", text, pct);
    }
}

static int cli_choose(void *ctx, const daemoon_str_ref_t *msg, const daemoon_str_ref_t *opts,
                      size_t n)
{
    char line[16];
    size_t i;
    long pick;

    (void)ctx;
    print_str(msg);
    for (i = 0; i < n; ++i) {
        char text[512];
        (void)daemoon_strf(text, sizeof(text), opts[i].id, opts[i].args, opts[i].nargs);
        printf("  %zu) %s\n", i + 1, text);
    }
    printf("  choose [1-%zu, anything else cancels]: ", n);
    fflush(stdout);

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return -1;
    }
    pick = strtol(line, NULL, 10);
    if (pick < 1 || (size_t)pick > n) {
        return -1;
    }
    return (int)(pick - 1);
}

static void cli_notify(void *ctx, const daemoon_str_ref_t *msg)
{
    (void)ctx;
    printf("! ");
    print_str(msg);
}

static const daemoon_ui_backend_t cli_ui_backend = {
    cli_confirm,
    cli_progress,
    cli_choose,
    cli_notify
};

/* --------------------------------------------------------------------- setup */

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static void app_init(app_t *app, const char *saves_dir, const char *work_dir, int interactive)
{
    memset(app, 0, sizeof(*app));

    daemoon_posix_save_init(&app->save, saves_dir);
    daemoon_posix_ui_init(&app->ui);
    daemoon_posix_net_init(&app->net);

    app->env.save = &daemoon_posix_save_backend;
    app->env.fs = &daemoon_posix_fs_backend;
    app->env.net = &daemoon_posix_net_backend;
    app->env.ui = interactive ? &cli_ui_backend : &daemoon_posix_ui_backend;

    app->env.save_ctx = &app->save;
    app->env.fs_ctx = &app->fs;
    app->env.net_ctx = &app->net;
    app->env.ui_ctx = &app->ui;

    app->env.clock_iso8601 = daemoon_posix_clock_iso8601;
    app->env.server_url = env_or("DAEMOON_SERVER", "http://127.0.0.1:8080");
    app->env.token = env_or("DAEMOON_TOKEN", NULL);
    app->env.device_label = env_or("DAEMOON_LABEL", "desktop");
    app->env.work_dir = work_dir;
    app->env.scratch = g_scratch;
    app->env.scratch_len = sizeof(g_scratch);
}

/* Registers every directory under the saves root as a title. The name encodes the
 * platform, matching what the posix backend writes: <platform>_<title id>. */
static daemoon_result_t discover_titles(app_t *app, const char *saves_dir)
{
    /* The backend already knows how to enumerate; this only has to name them. A
     * console gets this list from the system, not from a directory listing. */
    static const struct {
        const char        *id;
        daemoon_platform_t platform;
    } known[] = {
        { "0004000000055D00", DAEMOON_PLATFORM_3DS },
        { "0100000000010000", DAEMOON_PLATFORM_NX  },
        { "ADAE_POKEMON",     DAEMOON_PLATFORM_NDS }
    };
    size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        daemoon_save_type_t st = (known[i].platform == DAEMOON_PLATFORM_NDS)
                                     ? DAEMOON_SAVE_NDS
                                     : DAEMOON_SAVE_SAVEDATA;
        DAEMOON_TRY(daemoon_posix_save_add_title(&app->save, known[i].id, known[i].id,
                                                 known[i].platform, st));
    }

    /* Whatever else is in the saves directory.
     *
     * A console gets its list from the system and a desktop's nearest equivalent
     * is this directory, so anything shaped like <platform>_<title id> counts.
     * Without it this tool can only ever act on three ids somebody typed into a
     * source file, which is exactly what stopped it standing in for a second
     * device when a conflict needed making. */
    {
        DIR *d = opendir(saves_dir);
        struct dirent *ent;

        if (d == NULL) {
            return DAEMOON_OK;
        }
        while ((ent = readdir(d)) != NULL) {
            const char *sep = strchr(ent->d_name, '_');
            daemoon_platform_t platform;
            size_t seen;
            int already = 0;

            if (sep == NULL || sep == ent->d_name || sep[1] == '\0') {
                continue;
            }
            platform = daemoon_platform_parse(ent->d_name, (size_t)(sep - ent->d_name));
            if (platform == DAEMOON_PLATFORM_UNKNOWN) {
                continue;
            }
            for (seen = 0; seen < sizeof(known) / sizeof(known[0]); ++seen) {
                if (strcmp(known[seen].id, sep + 1) == 0) {
                    already = 1;
                    break;
                }
            }
            if (already) {
                continue;
            }
            (void)daemoon_posix_save_add_title(&app->save, sep + 1, sep + 1, platform,
                                               platform == DAEMOON_PLATFORM_NDS
                                                   ? DAEMOON_SAVE_NDS
                                                   : DAEMOON_SAVE_SAVEDATA);
        }
        (void)closedir(d);
    }
    return DAEMOON_OK;
}

static void report(const char *what, daemoon_result_t r)
{
    if (r == DAEMOON_OK) {
        printf("%s: ok\n", what);
        return;
    }
    /* The wire code plus the text the client would show. Both, because one is for
     * the person reading a log and the other is what a user would actually see. */
    printf("%s: %s (%s)\n", what, daemoon_result_code(r),
           daemoon_str(daemoon_result_str_id(r)));
}

/* ------------------------------------------------------------------ commands */

static int cmd_list(app_t *app)
{
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    daemoon_result_t r;

    r = app->env.save->list_titles(app->env.save_ctx, &titles, &count);
    if (r != DAEMOON_OK) {
        report("list", r);
        return 1;
    }

    for (i = 0; i < count; ++i) {
        daemoon_local_state_t local;
        daemoon_remote_meta_t remote;
        char local_size[24] = "-";

        memset(&remote, 0, sizeof(remote));
        r = daemoon_sync_scan_local(&app->env, &app->actx, &titles[i], &local);
        if (r != DAEMOON_OK) {
            printf("%-6s %-20s scan failed: %s\n", daemoon_platform_name(titles[i].platform),
                   titles[i].id, daemoon_result_code(r));
            continue;
        }
        if (local.has_save) {
            daemoon_fmt_bytes(local_size, sizeof(local_size), local.size);
        }

        r = daemoon_api_get_latest(&app->env, titles[i].platform, titles[i].id, &remote);
        if (r != DAEMOON_OK) {
            printf("%-6s %-20s local %-10s server unreachable: %s\n",
                   daemoon_platform_name(titles[i].platform), titles[i].id, local_size,
                   daemoon_result_code(r));
            continue;
        }

        printf("%-6s %-20s local %-10s base v%-4u server v%-4u %s\n",
               daemoon_platform_name(titles[i].platform), titles[i].id, local_size,
               local.base_version, remote.exists ? remote.latest_version : 0u,
               daemoon_str(daemoon_sync_action_str(daemoon_sync_decide(&local, &remote))));
    }

    app->env.save->free_titles(app->env.save_ctx, titles, count);
    return 0;
}

static int cmd_sync(app_t *app, const char *only_title)
{
    daemoon_title_t *titles = NULL;
    daemoon_sync_stats_t stats;
    size_t count = 0;
    size_t i;
    int failures = 0;
    daemoon_result_t r;

    memset(&stats, 0, sizeof(stats));

    r = app->env.save->list_titles(app->env.save_ctx, &titles, &count);
    if (r != DAEMOON_OK) {
        report("list", r);
        return 1;
    }

    for (i = 0; i < count; ++i) {
        if (only_title != NULL && strcmp(only_title, titles[i].id) != 0) {
            continue;
        }
        if (!titles[i].has_save) {
            continue;
        }
        printf("== %s %s\n", daemoon_platform_name(titles[i].platform), titles[i].id);
        r = daemoon_sync_title(&app->env, &app->actx, &titles[i], &stats);
        report("   sync", r);
        if (r != DAEMOON_OK && r != DAEMOON_ERR_USER_CANCELLED) {
            ++failures;
        }
    }

    app->env.save->free_titles(app->env.save_ctx, titles, count);

    printf("uploaded %u, downloaded %u, skipped %u, conflicts %u, failed %u\n",
           stats.uploaded, stats.downloaded, stats.skipped, stats.conflicts, stats.failed);
    return failures == 0 ? 0 : 1;
}

static int cmd_backup(app_t *app, const char *only_title)
{
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int failures = 0;
    daemoon_result_t r;

    r = app->env.save->list_titles(app->env.save_ctx, &titles, &count);
    if (r != DAEMOON_OK) {
        report("list", r);
        return 1;
    }

    for (i = 0; i < count; ++i) {
        char path[512];

        if (only_title != NULL && strcmp(only_title, titles[i].id) != 0) {
            continue;
        }
        if (!titles[i].has_save) {
            continue;
        }
        r = daemoon_sync_backup_local(&app->env, &app->actx, &titles[i], path, sizeof(path));
        if (r == DAEMOON_OK) {
            printf("%s -> %s\n", titles[i].id, path);
        } else {
            report(titles[i].id, r);
            ++failures;
        }
    }

    app->env.save->free_titles(app->env.save_ctx, titles, count);
    return failures == 0 ? 0 : 1;
}

static int cmd_pair(app_t *app, const char *code)
{
    char token[DAEMOON_TOKEN_MAX];
    char device_id[DAEMOON_DEVICE_ID_MAX];
    daemoon_result_t r;

    r = daemoon_api_pair(&app->env, "device_code", code, app->env.device_label,
                         DAEMOON_PLATFORM_3DS, token, sizeof(token), device_id,
                         sizeof(device_id));
    if (r != DAEMOON_OK) {
        report("pair", r);
        return 1;
    }

    /* On a console this goes to the SD card. Here it is printed so the caller can
     * put it in the environment; it is never written anywhere by this tool. */
    printf("device_id: %s\nexport DAEMOON_TOKEN=%s\n", device_id, token);
    return 0;
}

static void usage(void)
{
    printf(
        "daemoonctl - desktop DaeMoon client (development tool, no TLS)\n"
        "\n"
        "usage: daemoonctl [options] <command> [title id]\n"
        "\n"
        "commands:\n"
        "  list              show what each title would do\n"
        "  sync              run the sync for every title, or one\n"
        "  backup            back up locally, no server involved\n"
        "  pair <code>       exchange a pairing code for a token\n"
        "\n"
        "options:\n"
        "  --saves DIR       directory holding <platform>_<title id>/ (default ./saves)\n"
        "  --work DIR        backups, staging and sync state (default ./daemoon-work)\n"
        "  --lang CODE       en, ko, ja, zh-Hans, zh-Hant, es, fr, de\n"
        "  --yes             answer every prompt yes, for scripted runs\n"
        "\n"
        "environment:\n"
        "  DAEMOON_SERVER    default http://127.0.0.1:8080\n"
        "  DAEMOON_TOKEN     device token from `daemoonctl pair`\n"
        "  DAEMOON_LABEL     device label shown in conflict dialogs\n");
}

int main(int argc, char **argv)
{
    app_t app;
    const char *saves = "./saves";
    const char *work = "./daemoon-work";
    const char *command = NULL;
    const char *arg = NULL;
    daemoon_lang_t lang = DAEMOON_LANG_EN;
    int interactive = 1;
    int i;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) {
            saves = argv[++i];
        } else if (strcmp(argv[i], "--work") == 0 && i + 1 < argc) {
            work = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            if (daemoon_i18n_language_from_code(argv[++i], &lang) != DAEMOON_OK) {
                printf("unknown language %s, keeping English\n", argv[i]);
            }
        } else if (strcmp(argv[i], "--yes") == 0) {
            interactive = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (command == NULL) {
            command = argv[i];
        } else if (arg == NULL) {
            arg = argv[i];
        }
    }

    if (command == NULL) {
        usage();
        return 2;
    }

    daemoon_i18n_set_language(lang);
    app_init(&app, saves, work, interactive);

    if (discover_titles(&app, saves) != DAEMOON_OK) {
        printf("could not register titles\n");
        return 1;
    }

    if (strcmp(command, "pair") == 0) {
        if (arg == NULL) {
            usage();
            return 2;
        }
        return cmd_pair(&app, arg);
    }

    if (app.env.token == NULL && strcmp(command, "backup") != 0) {
        printf("no token: run `daemoonctl pair <code>` first, or use `backup`\n");
        return 2;
    }

    if (strcmp(command, "list") == 0) {
        rc = cmd_list(&app);
    } else if (strcmp(command, "sync") == 0) {
        rc = cmd_sync(&app, arg);
    } else if (strcmp(command, "backup") == 0) {
        rc = cmd_backup(&app, arg);
    } else {
        usage();
        rc = 2;
    }
    return rc;
}
