/* The device token, kept somewhere that does not come out of the console.
 *
 * It used to live in `sdmc:/DaeMoon/config.txt` beside the server address, which
 * made a lost SD card a live credential in somebody else's hands: whoever found it
 * could read a person's saves off the server and write over them. Revocation is the
 * answer to that and always has been, but revocation is a thing you have to notice
 * you need - and the card is out of the console by the time you do.
 *
 * So the token goes in this application's own save archive. That is not removable,
 * it is still a server issued credential that can be revoked and rotated, and it
 * involves no hardware id - which is the other way to get a stable identity and the
 * one the rules refuse, because a console id is not secret and follows a person
 * across services.
 *
 * Everything else stays in config.txt. A server address and a label are not secrets
 * and being able to edit them on a PC is most of why the file exists.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <string.h>

/* One line each, so the file can be read by anything that can read the archive and
 * there is no format to get wrong. */
#define SECRET_PATH "token"
#define SECRET_MAX  (DAEMOON_TOKEN_MAX + DAEMOON_DEVICE_ID_MAX + 8)

/* This title, as the save backend wants to see it. */
static daemoon_result_t own_title(daemoon_title_t *t)
{
    u64 program_id = 0;

    memset(t, 0, sizeof(*t));
    if (R_FAILED(APT_GetProgramID(&program_id))) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    daemoon_3ds_format_title_id(program_id, t->id, sizeof(t->id));
    t->platform = DAEMOON_PLATFORM_3DS;
    t->save_type = DAEMOON_SAVE_SAVEDATA;
    t->size_hint = 1; /* MEDIATYPE_SD */
    t->has_save = 1;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_3ds_secret_load(const daemoon_env_t *env, char *token,
                                         size_t token_len, char *device_id,
                                         size_t device_len)
{
    daemoon_title_t self;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    char buf[SECRET_MAX];
    size_t got = 0;
    daemoon_result_t r;
    char *nl;

    token[0] = '\0';
    device_id[0] = '\0';

    DAEMOON_TRY(own_title(&self));
    DAEMOON_TRY(daemoon_3ds_save_backend.open_save(NULL, &self, &save));

    r = daemoon_3ds_save_backend.open_file(NULL, save, SECRET_PATH,
                                           DAEMOON_OPEN_READ, &f);
    if (r == DAEMOON_OK) {
        r = daemoon_stream_read(f, buf, sizeof(buf) - 1, &got);
        (void)daemoon_stream_close(f);
    }
    (void)daemoon_3ds_save_backend.close_save(NULL, save);
    if (r != DAEMOON_OK) {
        return r;
    }

    buf[got] = '\0';
    nl = strchr(buf, '\n');
    if (nl != NULL) {
        *nl = '\0';
        (void)daemoon_strlcpy(device_id, device_len, nl + 1);
        {
            /* The id runs to the end or to another newline; a trailing one from an
             * editor should not become part of it. */
            char *end = strchr(device_id, '\n');

            if (end != NULL) {
                *end = '\0';
            }
        }
    }
    (void)daemoon_strlcpy(token, token_len, buf);

    (void)env;
    return token[0] != '\0' ? DAEMOON_OK : DAEMOON_ERR_NOT_FOUND;
}

daemoon_result_t daemoon_3ds_secret_save(const daemoon_env_t *env, const char *token,
                                         const char *device_id)
{
    daemoon_title_t self;
    daemoon_save_t *save = NULL;
    daemoon_stream_t *f = NULL;
    char buf[SECRET_MAX];
    daemoon_strbuf_t sb;
    daemoon_result_t r;

    (void)env;
    DAEMOON_TRY(own_title(&self));

    daemoon_strbuf_init(&sb, buf, sizeof(buf));
    daemoon_strbuf_add(&sb, token);
    daemoon_strbuf_addc(&sb, '\n');
    daemoon_strbuf_add(&sb, device_id);
    daemoon_strbuf_addc(&sb, '\n');
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    r = daemoon_3ds_save_backend.open_save_write(NULL, &self, &save);
    if (r == DAEMOON_ERR_NOT_FOUND) {
        /* A declared SaveDataSize does not create an archive; the title has to
         * format it once. Only ever its own - daemoon_3ds_format_own_save refuses
         * any other id, which is not a check worth doing without. */
        /* The same size the unattended self test formats, which is the only value
         * this has been seen to work at. A declared SaveDataSize is a ceiling, not
         * an archive. */
        DAEMOON_TRY(daemoon_3ds_format_own_save(&self, 128));
        r = daemoon_3ds_save_backend.open_save_write(NULL, &self, &save);
    }
    DAEMOON_TRY(r);

    r = daemoon_3ds_save_backend.open_file(NULL, save, SECRET_PATH,
                                           DAEMOON_OPEN_WRITE, &f);
    if (r == DAEMOON_OK) {
        r = daemoon_stream_write(f, buf, sb.len);
        {
            daemoon_result_t cr = daemoon_stream_close(f);

            if (r == DAEMOON_OK) {
                r = cr;
            }
        }
    }

    /* Without the commit nothing is persisted and the console finds out the next
     * time it is turned on. The same rule as every other write in this project. */
    if (r == DAEMOON_OK) {
        r = daemoon_3ds_save_backend.commit(NULL, save);
    }
    {
        daemoon_result_t cr = daemoon_3ds_save_backend.close_save(NULL, save);

        if (r == DAEMOON_OK) {
            r = cr;
        }
    }
    return r;
}
