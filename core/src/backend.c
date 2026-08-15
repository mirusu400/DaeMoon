/* Helpers that sit on top of the platform interfaces: stream plumbing, the enum
 * spellings that go on the wire, and an up front check of an assembled environment.
 *
 * No platform header appears here or anywhere else under core/. */
#include <daemoon/backend.h>

#include <string.h>

const char *daemoon_platform_name(daemoon_platform_t p)
{
    switch (p) {
    case DAEMOON_PLATFORM_3DS: return "3ds";
    case DAEMOON_PLATFORM_NX:  return "nx";
    case DAEMOON_PLATFORM_NDS: return "nds";
    default:                   return "unknown";
    }
}

static int str_eq_n(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && memcmp(s, lit, n) == 0;
}

daemoon_platform_t daemoon_platform_parse(const char *s, size_t len)
{
    if (s == NULL) {
        return DAEMOON_PLATFORM_UNKNOWN;
    }
    if (len == 0) {
        len = strlen(s);
    }
    if (str_eq_n(s, len, "3ds")) {
        return DAEMOON_PLATFORM_3DS;
    }
    if (str_eq_n(s, len, "nx")) {
        return DAEMOON_PLATFORM_NX;
    }
    if (str_eq_n(s, len, "nds")) {
        return DAEMOON_PLATFORM_NDS;
    }
    return DAEMOON_PLATFORM_UNKNOWN;
}

const char *daemoon_save_type_name(daemoon_save_type_t t)
{
    switch (t) {
    case DAEMOON_SAVE_SAVEDATA: return "savedata";
    case DAEMOON_SAVE_EXTDATA:  return "extdata";
    case DAEMOON_SAVE_NDS:      return "nds";
    default:                    return "unknown";
    }
}

daemoon_save_type_t daemoon_save_type_parse(const char *s, size_t len)
{
    if (s == NULL) {
        return DAEMOON_SAVE_UNKNOWN;
    }
    if (len == 0) {
        len = strlen(s);
    }
    if (str_eq_n(s, len, "savedata")) {
        return DAEMOON_SAVE_SAVEDATA;
    }
    if (str_eq_n(s, len, "extdata")) {
        return DAEMOON_SAVE_EXTDATA;
    }
    if (str_eq_n(s, len, "nds")) {
        return DAEMOON_SAVE_NDS;
    }
    return DAEMOON_SAVE_UNKNOWN;
}

daemoon_result_t daemoon_stream_read(daemoon_stream_t *s, void *buf, size_t cap, size_t *out_len)
{
    if (s == NULL || s->read == NULL || out_len == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    *out_len = 0;
    return s->read(s->ctx, buf, cap, out_len);
}

daemoon_result_t daemoon_stream_read_full(daemoon_stream_t *s, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        size_t got = 0;
        DAEMOON_TRY(daemoon_stream_read(s, p + done, len - done, &got));
        if (got == 0) {
            /* Short read where a length was promised means the file is not what the
             * manifest says it is. */
            return DAEMOON_ERR_IO_ERROR;
        }
        done += got;
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_stream_write(daemoon_stream_t *s, const void *buf, size_t len)
{
    if (s == NULL || s->write == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (len == 0) {
        return DAEMOON_OK;
    }
    return s->write(s->ctx, buf, len);
}

daemoon_result_t daemoon_stream_seek(daemoon_stream_t *s, unsigned long long offset)
{
    if (s == NULL || s->seek == NULL) {
        return DAEMOON_ERR_UNSUPPORTED;
    }
    return s->seek(s->ctx, offset);
}

daemoon_result_t daemoon_stream_close(daemoon_stream_t *s)
{
    if (s == NULL) {
        return DAEMOON_OK;
    }
    if (s->close == NULL) {
        return DAEMOON_OK;
    }
    /* Never ignored: on a write stream this is where a full SD card surfaces. */
    return s->close(s->ctx);
}

daemoon_result_t daemoon_stream_copy(daemoon_stream_t *dst, daemoon_stream_t *src,
                                     void *scratch, size_t scratch_len,
                                     unsigned long long *out_copied)
{
    unsigned long long total = 0;

    if (dst == NULL || src == NULL || scratch == NULL || scratch_len == 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    for (;;) {
        size_t got = 0;
        DAEMOON_TRY(daemoon_stream_read(src, scratch, scratch_len, &got));
        if (got == 0) {
            break;
        }
        DAEMOON_TRY(daemoon_stream_write(dst, scratch, got));
        total += got;
    }

    if (out_copied != NULL) {
        *out_copied = total;
    }
    return DAEMOON_OK;
}

daemoon_result_t daemoon_env_validate(const daemoon_env_t *env)
{
    if (env == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->scratch == NULL || env->scratch_len < 4096) {
        /* Every copy in core goes through this buffer. Undersizing it would not
         * break correctness but would make SD card IO miserable, so it is a
         * configuration error rather than something to work around. */
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->save == NULL || env->fs == NULL || env->ui == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->save->list_titles == NULL || env->save->open_save == NULL ||
        env->save->open_save_write == NULL || env->save->list_entries == NULL ||
        env->save->open_file == NULL || env->save->remove_all == NULL ||
        env->save->commit == NULL || env->save->close_save == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->fs->open == NULL || env->fs->rename == NULL || env->fs->mkdir_p == NULL ||
        env->fs->remove == NULL || env->fs->exists == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->ui->confirm == NULL || env->ui->choose == NULL) {
        /* Without confirm there is no way to honour "destructive actions always ask",
         * and there is no force path to fall back to. */
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->work_dir == NULL || env->work_dir[0] == '\0') {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (env->device_label == NULL || env->device_label[0] == '\0') {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    return DAEMOON_OK;
}
