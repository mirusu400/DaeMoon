#include "test.h"

#include <daemoon/i18n.h>
#include <daemoon/util/utf8.h>

#include <string.h>

TEST_CASE(every_string_resolves_in_every_language)
{
    /* The generator already refuses to emit a table with a missing key, so this is
     * the second line of defence: it proves the table the client actually links
     * against has no hole, in any language, at any id. */
    int lang;
    int id;

    for (lang = 0; lang < DAEMOON_LANG_COUNT; ++lang) {
        for (id = 0; id < DAEMOON_STR_COUNT; ++id) {
            const char *s = daemoon_str_in((daemoon_lang_t)lang, (daemoon_str_id_t)id);
            CHECK(s != NULL && s[0] != '\0');
            CHECK(daemoon_utf8_valid(s, strlen(s)));
        }
    }
}

TEST_CASE(out_of_range_ids_do_not_crash)
{
    CHECK(daemoon_str((daemoon_str_id_t)-1) != NULL);
    CHECK(daemoon_str((daemoon_str_id_t)DAEMOON_STR_COUNT) != NULL);
    CHECK(daemoon_str_in((daemoon_lang_t)99, DAEMOON_STR_BTN_OK) != NULL);
}

TEST_CASE(language_selection)
{
    daemoon_lang_t lang = DAEMOON_LANG_EN;

    CHECK_OK(daemoon_i18n_language_from_code("ko", &lang));
    CHECK_EQ_INT(lang, DAEMOON_LANG_KO);

    CHECK_OK(daemoon_i18n_language_from_code("ZH-hans", &lang));
    CHECK_EQ_INT(lang, DAEMOON_LANG_ZH_HANS);

    /* A console that only reports "zh" does not say which script. */
    CHECK_OK(daemoon_i18n_language_from_code("zh", &lang));
    CHECK_EQ_INT(lang, DAEMOON_LANG_ZH_HANS);
    CHECK_OK(daemoon_i18n_language_from_code("zh-TW", &lang));
    CHECK_EQ_INT(lang, DAEMOON_LANG_ZH_HANT);

    /* An unknown code leaves the previous choice alone rather than resetting it. */
    lang = DAEMOON_LANG_JA;
    CHECK_RESULT(daemoon_i18n_language_from_code("xx", &lang), DAEMOON_ERR_UNSUPPORTED);
    CHECK_EQ_INT(lang, DAEMOON_LANG_JA);

    daemoon_i18n_set_language(DAEMOON_LANG_KO);
    CHECK_EQ_INT(daemoon_i18n_language(), DAEMOON_LANG_KO);
    CHECK_STR(daemoon_str(DAEMOON_STR_BTN_YES), "예");

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
    CHECK_STR(daemoon_str(DAEMOON_STR_BTN_YES), "Yes");
}

TEST_CASE(language_names_are_native)
{
    /* The picker has to read natively whatever is currently selected. */
    daemoon_i18n_set_language(DAEMOON_LANG_EN);
    CHECK_STR(daemoon_lang_name(DAEMOON_LANG_KO), "한국어");
    CHECK_STR(daemoon_lang_name(DAEMOON_LANG_DE), "Deutsch");
    CHECK_STR(daemoon_lang_code(DAEMOON_LANG_ZH_HANT), "zh-Hant");
}

TEST_CASE(strf_substitutes_placeholders)
{
    char buf[256];
    const char *args[2];

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
    args[0] = "Pokemon";
    CHECK_OK(daemoon_strf(buf, sizeof(buf), DAEMOON_STR_SYNC_UP_TO_DATE, args, 1));
    CHECK_STR(buf, "Pokemon is already up to date.");

    args[0] = "1.2 MiB";
    args[1] = "Old 3DS XL";
    CHECK_OK(daemoon_strf(buf, sizeof(buf), DAEMOON_STR_CONFLICT_KEEP_LOCAL, args, 2));
    CHECK_STR(buf, "Keep this console's save (1.2 MiB, saved on Old 3DS XL)");
}

TEST_CASE(strf_reorders_for_the_target_language)
{
    /* The whole reason placeholders exist instead of concatenated fragments: the
     * arguments arrive in the same order and come out where the language wants
     * them. */
    char en[256];
    char ja[256];
    const char *args[2];

    args[0] = "512 KiB";
    args[1] = "Switch";

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
    CHECK_OK(daemoon_strf(en, sizeof(en), DAEMOON_STR_CONFLICT_KEEP_SERVER, args, 2));

    daemoon_i18n_set_language(DAEMOON_LANG_JA);
    CHECK_OK(daemoon_strf(ja, sizeof(ja), DAEMOON_STR_CONFLICT_KEEP_SERVER, args, 2));

    CHECK(strstr(en, "512 KiB") != NULL && strstr(en, "Switch") != NULL);
    CHECK(strstr(ja, "512 KiB") != NULL && strstr(ja, "Switch") != NULL);
    CHECK(strcmp(en, ja) != 0);

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
}

TEST_CASE(strf_missing_argument_stays_visible)
{
    char buf[256];

    /* Silently dropping the placeholder would produce a sentence with a hole in it
     * that nobody notices until a user reports it. */
    daemoon_i18n_set_language(DAEMOON_LANG_EN);
    CHECK_OK(daemoon_strf(buf, sizeof(buf), DAEMOON_STR_SYNC_UP_TO_DATE, NULL, 0));
    CHECK(strstr(buf, "{0}") != NULL);
}

TEST_CASE(strf_truncates_on_a_codepoint_boundary)
{
    char buf[12];
    const char *args[1];
    daemoon_result_t r;

    daemoon_i18n_set_language(DAEMOON_LANG_JA);
    args[0] = "ゼルダの伝説";
    r = daemoon_strf(buf, sizeof(buf), DAEMOON_STR_SYNC_UP_TO_DATE, args, 1);
    CHECK_RESULT(r, DAEMOON_ERR_BUFFER_TOO_SMALL);
    CHECK(daemoon_utf8_valid(buf, strlen(buf)));

    daemoon_i18n_set_language(DAEMOON_LANG_EN);
}

TEST_CASE(fmt_bytes)
{
    char buf[24];

    daemoon_fmt_bytes(buf, sizeof(buf), 0);
    CHECK_STR(buf, "0 B");
    daemoon_fmt_bytes(buf, sizeof(buf), 512);
    CHECK_STR(buf, "512 B");
    daemoon_fmt_bytes(buf, sizeof(buf), 1024);
    CHECK_STR(buf, "1.0 KiB");
    daemoon_fmt_bytes(buf, sizeof(buf), 1536);
    CHECK_STR(buf, "1.5 KiB");
    daemoon_fmt_bytes(buf, sizeof(buf), 64ull * 1024 * 1024);
    CHECK_STR(buf, "64.0 MiB");
}

TEST_CASE(error_codes_round_trip)
{
    /* shared/errors.json is the single source of truth for both sides, so a code
     * that goes out over the wire has to come back as the same result. */
    CHECK_STR(daemoon_result_code(DAEMOON_ERR_VERSION_CONFLICT), "version_conflict");
    CHECK_RESULT(daemoon_result_from_code("version_conflict", 0), DAEMOON_ERR_VERSION_CONFLICT);
    CHECK_RESULT(daemoon_result_from_code("checksum_mismatch", 17), DAEMOON_ERR_CHECKSUM_MISMATCH);

    /* An unknown code from a newer server must never look like success. */
    CHECK_RESULT(daemoon_result_from_code("something_new", 0), DAEMOON_ERR_INTERNAL_ERROR);
    CHECK_RESULT(daemoon_result_from_code(NULL, 0), DAEMOON_ERR_INTERNAL_ERROR);

    CHECK_RESULT(daemoon_result_from_http(200, NULL, 0), DAEMOON_OK);
    CHECK_RESULT(daemoon_result_from_http(409, NULL, 0), DAEMOON_ERR_VERSION_CONFLICT);
    CHECK_RESULT(daemoon_result_from_http(409, "version_conflict", 0),
                 DAEMOON_ERR_VERSION_CONFLICT);

    CHECK(daemoon_result_retryable(DAEMOON_ERR_TIMEOUT));
    CHECK(daemoon_result_retryable(DAEMOON_ERR_RATE_LIMITED));
    CHECK(!daemoon_result_retryable(DAEMOON_ERR_VERSION_CONFLICT));
}

TEST_CASE(every_error_has_text_in_every_language)
{
    int lang;

    for (lang = 0; lang < DAEMOON_LANG_COUNT; ++lang) {
        int code;
        for (code = 1; code < DAEMOON_ERR_COUNT_; ++code) {
            const char *text = daemoon_str_in((daemoon_lang_t)lang,
                                              daemoon_result_str_id((daemoon_result_t)code));
            CHECK(text != NULL && text[0] != '\0');
        }
    }
}

void test_i18n(void)
{
    printf("i18n\n");
    RUN(every_string_resolves_in_every_language);
    RUN(out_of_range_ids_do_not_crash);
    RUN(language_selection);
    RUN(language_names_are_native);
    RUN(strf_substitutes_placeholders);
    RUN(strf_reorders_for_the_target_language);
    RUN(strf_missing_argument_stays_visible);
    RUN(strf_truncates_on_a_codepoint_boundary);
    RUN(fmt_bytes);
    RUN(error_codes_round_trip);
    RUN(every_error_has_text_in_every_language);
}
