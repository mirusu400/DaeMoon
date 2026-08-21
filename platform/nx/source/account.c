/* Which account's saves these are.
 *
 * The difference from the 3DS that the roadmap singles out. A Switch save is keyed by
 * `AccountUid` as well as by title, so "this console's save for that game" is not a
 * complete thought here: a different account is a different save, and syncing the
 * wrong one would upload somebody else's progress under this device's name.
 *
 * Preselected first. An application launched with a user already chosen - which is
 * what happens when the album or a title hands off - should not ask again. Otherwise
 * the system's own selector, because it is the one people recognise and it is the one
 * that draws profile pictures.
 */
#include "daemoon_nx.h"

#include <daemoon/util/strbuf.h>

#include <switch.h>

#include <stdio.h>
#include <string.h>

static void fill(daemoon_nx_account_t *out, AccountUid uid)
{
    AccountProfile profile;
    AccountProfileBase base;

    out->lower = uid.uid[0];
    out->upper = uid.uid[1];
    out->valid = accountUidIsValid(&uid);
    out->nickname[0] = '\0';

    if (!out->valid) {
        return;
    }
    /* The nickname is what the account is called on screen. Failing to read it is not
     * failing to select: the uid is what a save is keyed by, and a name is how a
     * person recognises which one was picked. */
    if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
            (void)daemoon_strlcpy(out->nickname, sizeof(out->nickname), base.nickname);
        }
        accountProfileClose(&profile);
    }
    if (out->nickname[0] == '\0') {
        (void)snprintf(out->nickname, sizeof(out->nickname), "%016llX",
                       (unsigned long long)uid.uid[0]);
    }
}

daemoon_result_t daemoon_nx_account_select(daemoon_nx_account_t *out)
{
    AccountUid uid = {0};
    Result rc;

    if (out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    memset(out, 0, sizeof(*out));

    rc = accountInitialize(AccountServiceType_Application);
    daemoon_nx_trace("account/init", R_SUCCEEDED(rc) ? "ok" : "failed");
    if (R_FAILED(rc)) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) {
        fill(out, uid);
        daemoon_nx_trace("account/preselected", out->nickname);
        return DAEMOON_OK;
    }

    /* The system selector. It is a library applet, so it needs the memory an
     * application has - which is the other half of why applet mode is refused at
     * startup rather than partway through.
     *
     * The settings are required, not optional. Passing NULL here is a data abort
     * at address zero inside the applet library, which is what this build did on
     * the first console it ever reached: `app/start`, `account/init ok`, and then
     * nothing. The header says [in] and never says it may be null; there is no
     * default to fall back on, so the caller supplies one. All zero is the plain
     * case - every user offered, no skip button, no linked account demanded. */
    {
        PselUserSelectionSettings settings;

        memset(&settings, 0, sizeof(settings));
        rc = pselShowUserSelector(&uid, &settings);
    }
    if (R_FAILED(rc) || !accountUidIsValid(&uid)) {
        daemoon_nx_trace("account/selector", "cancelled");
        return DAEMOON_ERR_USER_CANCELLED;
    }
    fill(out, uid);
    daemoon_nx_trace("account/selected", out->nickname);
    return DAEMOON_OK;
}
