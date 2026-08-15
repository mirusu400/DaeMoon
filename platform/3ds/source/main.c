/* 3DS entry point. Phase 1.
 *
 * Everything below the platform layer already exists and is tested: this file only
 * has to assemble a daemoon_env_t out of libctru and hand it to core. The backend
 * implementations (save_backend.c, net_backend.c, ui_backend.c) land with Phase 1,
 * against a CIA, on hardware.
 *
 * The scratch buffer is static rather than malloc'd. The 3DS heap fragments badly
 * and this one lives for the whole run, so there is nothing to gain by putting it
 * on the heap and a slow leak of headroom to lose.
 */
#include <3ds.h>

#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/sync.h>

#include <stdio.h>

static unsigned char g_scratch[64 * 1024];
static daemoon_archive_ctx_t g_archive;

/* Console setting first, user choice on top of it once settings exist. */
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
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    if (R_FAILED(cfguInit())) {
        printf("cfg:u unavailable\n");
    } else {
        select_language();
    }

    g_archive.count = 0;
    (void)g_scratch;

    printf("%s\n\n", daemoon_str(DAEMOON_STR_APP_TITLE));
    printf("Phase 1: the save, net and UI backends are not implemented yet.\n");
    printf("Press START to exit.\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    cfguExit();
    gfxExit();
    return 0;
}
