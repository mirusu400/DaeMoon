/* What the first run says, and when it says it.
 *
 * Separate from welcome.c, which draws it. Everything here is a decision - whether
 * the screens are due, which page comes next, what each choice is called - and none
 * of it touches citro2d or a service, so `make core-test` runs it on a desktop.
 * The drawing is the part that needs a console; the order of the pages is not.
 */
#include "daemoon_3ds.h"

/* The three pages, in order. What it does, what it will not do to a save, and when
 * it is safe to use.
 *
 * Three because the third is the one somebody has to have read: syncing while a
 * game holds its archive open is the way to lose a save with this application
 * working exactly as designed, and there is no code that can prevent it. The other
 * two are what the reader came for, and a page they will not read is worse placed
 * before the one they must.
 */
static const daemoon_str_id_t k_pages[] = {
    DAEMOON_STR_WELCOME_WHAT,
    DAEMOON_STR_WELCOME_SAFE,
    DAEMOON_STR_WELCOME_WHEN
};

static const daemoon_str_id_t k_choice_labels[] = {
    DAEMOON_STR_WELCOME_OPT_QR,
    DAEMOON_STR_WELCOME_OPT_CODE,
    DAEMOON_STR_WELCOME_OPT_LATER
};

static const daemoon_str_id_t k_choice_hints[] = {
    DAEMOON_STR_WELCOME_OPT_QR_HINT,
    DAEMOON_STR_WELCOME_OPT_CODE_HINT,
    DAEMOON_STR_WELCOME_OPT_LATER_HINT
};

size_t daemoon_3ds_welcome_pages(void)
{
    return sizeof(k_pages) / sizeof(k_pages[0]);
}

daemoon_str_id_t daemoon_3ds_welcome_page(size_t index)
{
    if (index >= daemoon_3ds_welcome_pages()) {
        return DAEMOON_STR_WELCOME_WHAT;
    }
    return k_pages[index];
}

size_t daemoon_3ds_welcome_choices(void)
{
    return sizeof(k_choice_labels) / sizeof(k_choice_labels[0]);
}

daemoon_str_id_t daemoon_3ds_welcome_choice_label(size_t index)
{
    if (index >= daemoon_3ds_welcome_choices()) {
        return DAEMOON_STR_WELCOME_OPT_LATER;
    }
    return k_choice_labels[index];
}

daemoon_str_id_t daemoon_3ds_welcome_choice_hint(size_t index)
{
    if (index >= daemoon_3ds_welcome_choices()) {
        return DAEMOON_STR_WELCOME_OPT_LATER_HINT;
    }
    return k_choice_hints[index];
}

/* Due exactly once, and not deduced from anything else.
 *
 * The tempting shortcut is "no server configured means a new console", and it is
 * wrong in the direction that annoys: somebody who chose Not now has read the
 * screens and has no server, and would be shown them again at every launch. A
 * console that is paired and then unpaired is the same case from the other side.
 *
 * So the flag records that the screens happened rather than guessing from state
 * they might have produced. An SD card carried to a second console shows them
 * again, which is right - it is a different console, and the pairing is per
 * console.
 */
int daemoon_3ds_welcome_needed(const daemoon_3ds_config_t *cfg)
{
    return cfg != NULL && !cfg->welcomed;
}
