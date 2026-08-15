/* Switch entry point. Phase 6.
 *
 * Same shape as the 3DS entry point: assemble a daemoon_env_t out of libnx and hand
 * it to core. Two things have to happen here before anything else, and both are
 * checked at startup rather than discovered halfway through a sync:
 *
 *   - an account has to be selected, because a save belongs to one AccountUid
 *   - applet mode has to be detected, because its memory limit will not fit a sync
 */
#include <switch.h>

#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/sync.h>

#include <stdio.h>

static unsigned char g_scratch[64 * 1024];
static daemoon_archive_ctx_t g_archive;

static void select_language(void)
{
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    daemoon_lang_t chosen = DAEMOON_LANG_EN;

    if (R_FAILED(setInitialize())) {
        return;
    }
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) &&
        R_SUCCEEDED(setMakeLanguage(code, &lang))) {
        switch (lang) {
        case SetLanguage_JA:     (void)daemoon_i18n_language_from_code("ja", &chosen); break;
        case SetLanguage_FR:     (void)daemoon_i18n_language_from_code("fr", &chosen); break;
        case SetLanguage_DE:     (void)daemoon_i18n_language_from_code("de", &chosen); break;
        case SetLanguage_ES:     (void)daemoon_i18n_language_from_code("es", &chosen); break;
        case SetLanguage_ZHCN:
        case SetLanguage_ZHHANS: (void)daemoon_i18n_language_from_code("zh-Hans", &chosen); break;
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANT: (void)daemoon_i18n_language_from_code("zh-Hant", &chosen); break;
        case SetLanguage_KO:     (void)daemoon_i18n_language_from_code("ko", &chosen); break;
        default:                 chosen = DAEMOON_LANG_EN; break;
        }
    }
    setExit();
    daemoon_i18n_set_language(chosen);
}

int main(int argc, char **argv)
{
    AppletType applet;

    (void)argc;
    (void)argv;

    consoleInit(NULL);
    select_language();

    g_archive.count = 0;
    (void)g_scratch;

    printf("%s\n\n", daemoon_str(DAEMOON_STR_APP_TITLE));

    applet = appletGetAppletType();
    if (applet != AppletType_Application && applet != AppletType_SystemApplication) {
        /* Limited memory. Say so now rather than failing partway through a sync. */
        printf("%s\n\n", daemoon_str(DAEMOON_STR_WARN_APPLET_MODE));
    }

    printf("Phase 6: the save, net and UI backends are not implemented yet.\n");
    printf("Press + to exit.\n");

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
