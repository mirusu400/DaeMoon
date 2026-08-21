/* The pictures: a title's icon, and the selected account's profile picture.
 *
 * Both are JPEG, both come out of a system service, and neither is decoded here.
 * This file's whole job is to hand back the bytes the console stores, because the
 * thing that draws them already knows how to read a JPEG and this file has no
 * business knowing that a GPU exists - the same split `platform/3ds/source/icons.h`
 * describes, arrived at for the same reason.
 *
 * An icon is how a person recognises a game. The list was hex before it had names
 * and names before it had these, and each step made it possible to find the right
 * save without reading carefully.
 */
#include "daemoon_nx.h"

#include <switch.h>

#include <stdlib.h>
#include <string.h>

daemoon_result_t daemoon_nx_icon_load(unsigned long long app_id, unsigned char **out,
                                      size_t *out_len)
{
    NsApplicationControlData *data;
    unsigned char *copy;
    u64 size = 0;
    size_t icon_len;

    if (out == NULL || out_len == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    *out = NULL;
    *out_len = 0;

    /* Around 144 KiB. Heap rather than stack, the same rule save_backend.c reads
     * names under: this project has already had one data abort from a large frame
     * on a console. */
    data = (NsApplicationControlData *)calloc(1, sizeof(*data));
    if (data == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id,
                                             data, sizeof(*data), &size)) ||
        size <= sizeof(data->nacp)) {
        free(data);
        return DAEMOON_ERR_NOT_FOUND;
    }

    /* Everything past the nacp is the icon. The service reports one length for the
     * whole record, so this is the only way to know how much of the icon buffer was
     * actually written. */
    icon_len = (size_t)(size - sizeof(data->nacp));
    if (icon_len == 0 || icon_len > sizeof(data->icon)) {
        free(data);
        return DAEMOON_ERR_NOT_FOUND;
    }

    copy = (unsigned char *)malloc(icon_len);
    if (copy == NULL) {
        free(data);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, data->icon, icon_len);
    free(data);

    *out = copy;
    *out_len = icon_len;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_nx_account_image(const daemoon_nx_account_t *account,
                                          unsigned char **out, size_t *out_len)
{
    AccountProfile profile;
    AccountUid uid;
    unsigned char *buf;
    u32 want = 0;
    u32 got = 0;

    if (account == NULL || out == NULL || out_len == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    *out = NULL;
    *out_len = 0;
    if (!account->valid) {
        return DAEMOON_ERR_NOT_FOUND;
    }

    uid.uid[0] = account->lower;
    uid.uid[1] = account->upper;

    if (R_FAILED(accountGetProfile(&profile, uid))) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    /* Asked for rather than assumed: the size is a property of the picture the user
     * chose, and a fixed buffer would be either wasteful or one day too small. */
    if (R_FAILED(accountProfileGetImageSize(&profile, &want)) || want == 0) {
        accountProfileClose(&profile);
        return DAEMOON_ERR_NOT_FOUND;
    }
    buf = (unsigned char *)malloc(want);
    if (buf == NULL) {
        accountProfileClose(&profile);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    if (R_FAILED(accountProfileLoadImage(&profile, buf, want, &got)) || got == 0) {
        free(buf);
        accountProfileClose(&profile);
        return DAEMOON_ERR_NOT_FOUND;
    }
    accountProfileClose(&profile);

    *out = buf;
    *out_len = (size_t)got;
    return DAEMOON_OK;
}
