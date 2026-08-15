/* The 3DS save backend.
 *
 * Implements core/include/daemoon/backend.h against libctru. Everything above this
 * file - packing, digests, conflict resolution - already runs on a desktop and is
 * tested there. What is new here is the only part that can lose someone's save.
 *
 * Two things are load bearing and easy to get quietly wrong:
 *
 *   - The archive is ARCHIVE_USER_SAVEDATA opened with a binary path carrying the
 *     media type and title id. That is what needs the FS rights in app.rsf, and it
 *     is why a .3dsx cannot do any of this.
 *   - Nothing is persisted until FSUSER_ControlArchive commits. A write that
 *     "succeeded" without a commit is gone at power off, and the user finds out
 *     the next time they launch the game.
 */
#include "daemoon_3ds.h"

#include <daemoon/archive.h>
#include <daemoon/util/strbuf.h>

#include <3ds.h>

#include <stdlib.h>
#include <string.h>

/* A save is a small tree. 3DS save archives are FAT, so paths are short and the
 * depth is shallow, but neither is guaranteed by anything. */
#define TDS_PATH_MAX  DAEMOON_PATH_MAX
#define TDS_UTF16_MAX (TDS_PATH_MAX + 1)

struct daemoon_save {
    FS_Archive archive;
    int        writable;
    int        open;
};

/* libctru results carry a summary. Anything that means "it is not there" has to
 * come back as not_found, because daemoon_sync_scan_local reads that as "no save
 * yet" rather than a failure the user has to act on. */
static daemoon_result_t from_result(Result res)
{
    if (R_SUCCEEDED(res)) {
        return DAEMOON_OK;
    }
    switch (R_SUMMARY(res)) {
    case RS_NOTFOUND:
        return DAEMOON_ERR_NOT_FOUND;
    case RS_OUTOFRESOURCE:
        return DAEMOON_ERR_OUT_OF_MEMORY;
    case RS_INVALIDARG:
    case RS_WRONGARG:
        return DAEMOON_ERR_INVALID_REQUEST;
    case RS_NOTSUPPORTED:
        return DAEMOON_ERR_UNSUPPORTED;
    default:
        break;
    }
    if (R_DESCRIPTION(res) == RD_NOT_FOUND) {
        return DAEMOON_ERR_NOT_FOUND;
    }
    return DAEMOON_FROM_BACKEND(res);
}

/* Paths inside the archive are UTF-16. Core speaks UTF-8 everywhere, so this is
 * the only place the two meet. */
static daemoon_result_t to_fs_path(const char *rel, u16 *out, size_t out_len, FS_Path *path)
{
    char joined[TDS_PATH_MAX + 2];
    daemoon_strbuf_t sb;
    ssize_t units;

    daemoon_strbuf_init(&sb, joined, sizeof(joined));
    daemoon_strbuf_addc(&sb, '/');
    daemoon_strbuf_add(&sb, rel);
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    units = utf8_to_utf16(out, (const uint8_t *)joined, out_len - 1);
    if (units < 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    /* libctru fills the buffer and reports what the input *would* have produced,
     * so a return larger than the buffer means it truncated and said nothing. A
     * truncated path is a different path: it would open, write to, or delete the
     * wrong file. */
    if ((size_t)units > out_len - 1) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    out[units] = 0;

    path->type = PATH_UTF16;
    path->size = (u32)((units + 1) * sizeof(u16));
    path->data = out;
    return DAEMOON_OK;
}

/* ------------------------------------------------------------------ streams */

typedef struct {
    daemoon_stream_t stream;
    Handle           handle;
    u64              offset;
    int              writable;
} tds_file_t;

static daemoon_result_t file_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    tds_file_t *f = (tds_file_t *)ctx;
    u32 got = 0;
    Result res;

    if (cap == 0) {
        *out_len = 0;
        return DAEMOON_OK;
    }
    res = FSFILE_Read(f->handle, &got, f->offset, buf, (u32)cap);
    if (R_FAILED(res)) {
        return from_result(res);
    }
    f->offset += got;
    *out_len = got;
    return DAEMOON_OK;
}

static daemoon_result_t file_write(void *ctx, const void *buf, size_t len)
{
    tds_file_t *f = (tds_file_t *)ctx;
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        u32 wrote = 0;
        /* FS_WRITE_FLUSH on every chunk. The commit is what persists the archive,
         * but leaving writes sitting in the service's buffers across a whole save
         * only widens the window where a crash loses half of it. */
        Result res = FSFILE_Write(f->handle, &wrote, f->offset, p + done, (u32)(len - done),
                                  FS_WRITE_FLUSH);
        if (R_FAILED(res)) {
            return from_result(res);
        }
        if (wrote == 0) {
            return DAEMOON_ERR_NO_SPACE;
        }
        f->offset += wrote;
        done += wrote;
    }
    return DAEMOON_OK;
}

static daemoon_result_t file_seek(void *ctx, unsigned long long offset)
{
    ((tds_file_t *)ctx)->offset = offset;
    return DAEMOON_OK;
}

static daemoon_result_t file_close(void *ctx)
{
    tds_file_t *f = (tds_file_t *)ctx;
    Result res = DAEMOON_OK;

    if (f->writable) {
        /* A full archive shows up here rather than at write time. */
        res = FSFILE_Flush(f->handle);
    }
    if (R_SUCCEEDED(res)) {
        res = FSFILE_Close(f->handle);
    } else {
        (void)FSFILE_Close(f->handle);
    }
    free(f);
    return from_result(res);
}

/* -------------------------------------------------------------------- saves */

static daemoon_result_t archive_for(const daemoon_title_t *t, FS_Archive *out)
{
    u64 title_id;
    u32 path[3];
    FS_Path binpath;

    DAEMOON_TRY(daemoon_3ds_parse_title_id(t->id, &title_id));

    path[0] = (u32)(t->size_hint == 0 ? MEDIATYPE_SD : (FS_MediaType)t->size_hint);
    path[1] = (u32)(title_id & 0xffffffffull);
    path[2] = (u32)(title_id >> 32);

    binpath.type = PATH_BINARY;
    binpath.size = sizeof(path);
    binpath.data = path;

    return from_result(FSUSER_OpenArchive(out, ARCHIVE_USER_SAVEDATA, binpath));
}

static daemoon_result_t open_common(const daemoon_title_t *t, int writable,
                                    daemoon_save_t **out)
{
    daemoon_save_t *s = (daemoon_save_t *)calloc(1, sizeof(*s));
    daemoon_result_t r;

    if (s == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    r = archive_for(t, &s->archive);
    if (r != DAEMOON_OK) {
        free(s);
        return r;
    }
    s->writable = writable;
    s->open = 1;
    *out = s;
    return DAEMOON_OK;
}

static daemoon_result_t open_save(void *ctx, const daemoon_title_t *t, daemoon_save_t **out)
{
    (void)ctx;
    return open_common(t, 0, out);
}

static daemoon_result_t open_save_write(void *ctx, const daemoon_title_t *t,
                                        daemoon_save_t **out)
{
    (void)ctx;
    return open_common(t, 1, out);
}

static daemoon_result_t close_save(void *ctx, daemoon_save_t *s)
{
    Result res;

    (void)ctx;
    if (s == NULL) {
        return DAEMOON_OK;
    }
    res = FSUSER_CloseArchive(s->archive);
    free(s);
    return from_result(res);
}

/* This is the call the whole project hinges on. Never skip it, never ignore what
 * it returns: without it nothing written above is persisted. */
static daemoon_result_t commit(void *ctx, daemoon_save_t *s)
{
    (void)ctx;
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }
    return from_result(FSUSER_ControlArchive(s->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
                                             NULL, 0, NULL, 0));
}

/* ------------------------------------------------------------------ walking */

/* Depth first, with paths relative to the save root and forward slashes, which is
 * what backend.h promises and what goes straight into a package. */
static daemoon_result_t walk(daemoon_save_t *s, const char *rel, daemoon_entry_cb cb,
                             void *user, int *stopped, int depth)
{
    u16 utf16[TDS_UTF16_MAX];
    FS_Path path;
    Handle dir = 0;
    FS_DirectoryEntry *entries;
    daemoon_result_t r;

    /* A save archive that nests this deep is not a save archive. Refusing beats
     * recursing until the stack gives out. */
    if (depth > 8) {
        return DAEMOON_ERR_ARCHIVE_ERROR;
    }

    DAEMOON_TRY(to_fs_path(rel, utf16, TDS_UTF16_MAX, &path));
    DAEMOON_TRY(from_result(FSUSER_OpenDirectory(&dir, s->archive, path)));

    /* One entry at a time. FS_DirectoryEntry is over half a kilobyte and the heap
     * here is small enough that a batch would be a real allocation. */
    entries = (FS_DirectoryEntry *)calloc(1, sizeof(*entries));
    if (entries == NULL) {
        (void)FSDIR_Close(dir);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    r = DAEMOON_OK;
    while (!*stopped) {
        char name[TDS_PATH_MAX];
        char child[TDS_PATH_MAX];
        daemoon_strbuf_t sb;
        u32 read = 0;
        ssize_t bytes;

        if (R_FAILED(FSDIR_Read(dir, &read, 1, entries)) || read == 0) {
            break;
        }

        bytes = utf16_to_utf8((uint8_t *)name, entries->name, sizeof(name) - 1);
        if (bytes < 0 || (size_t)bytes > sizeof(name) - 1) {
            /* Either a name this backend cannot represent, or one longer than the
             * buffer, which the converter truncates without saying so. Both would
             * go into a package as something else and come back as something else
             * again. */
            r = DAEMOON_ERR_ARCHIVE_ERROR;
            break;
        }
        name[bytes] = '\0';

        daemoon_strbuf_init(&sb, child, sizeof(child));
        if (rel[0] != '\0') {
            daemoon_strbuf_add(&sb, rel);
            daemoon_strbuf_addc(&sb, '/');
        }
        daemoon_strbuf_add(&sb, name);
        r = daemoon_strbuf_result(&sb);
        if (r != DAEMOON_OK) {
            break;
        }

        if (entries->attributes & FS_ATTRIBUTE_DIRECTORY) {
            r = walk(s, child, cb, user, stopped, depth + 1);
            if (r != DAEMOON_OK) {
                break;
            }
            continue;
        }

        if (cb(user, child, entries->fileSize) != 0) {
            *stopped = 1;
        }
    }

    free(entries);
    (void)FSDIR_Close(dir);
    return r;
}

static daemoon_result_t list_entries(void *ctx, daemoon_save_t *s, daemoon_entry_cb cb,
                                     void *user)
{
    int stopped = 0;

    (void)ctx;
    return walk(s, "", cb, user, &stopped, 0);
}

/* --------------------------------------------------------------------- files */

/* The archive has no mkdir -p, so a nested path needs its parents made one at a
 * time. core never creates a directory itself; the backend contract says this is
 * where it happens. */
static daemoon_result_t make_parents(daemoon_save_t *s, const char *rel)
{
    char partial[TDS_PATH_MAX];
    size_t i;
    size_t len = strlen(rel);

    if (len >= sizeof(partial)) {
        return DAEMOON_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(partial, rel, len + 1);

    for (i = 0; i < len; ++i) {
        u16 utf16[TDS_UTF16_MAX];
        FS_Path path;
        Result res;

        if (partial[i] != '/') {
            continue;
        }
        partial[i] = '\0';
        if (to_fs_path(partial, utf16, TDS_UTF16_MAX, &path) == DAEMOON_OK) {
            /* Whatever this returns is ignored on purpose.
             *
             * The common case is that the directory is already there, and the
             * result code for that is not the same everywhere: it differs between
             * the service and an emulator, and keying on one description made a
             * second write to an existing directory fail the whole operation.
             * Comparing against a list of "acceptable" codes is the same bet with
             * more steps.
             *
             * The open that follows is the honest test. If a directory really
             * could not be made, the file cannot be created either, and that error
             * is the one worth reporting. */
            res = FSUSER_CreateDirectory(s->archive, path, 0);
            (void)res;
        }
        partial[i] = '/';
    }
    return DAEMOON_OK;
}

static daemoon_result_t open_file(void *ctx, daemoon_save_t *s, const char *rel,
                                  daemoon_open_mode_t mode, daemoon_stream_t **out)
{
    u16 utf16[TDS_UTF16_MAX];
    FS_Path path;
    tds_file_t *f;
    Handle handle = 0;
    u32 flags;
    Result res;

    (void)ctx;
    if (mode == DAEMOON_OPEN_WRITE && !s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }

    if (mode == DAEMOON_OPEN_WRITE) {
        DAEMOON_TRY(make_parents(s, rel));
        flags = FS_OPEN_WRITE | FS_OPEN_CREATE;
    } else {
        flags = FS_OPEN_READ;
    }

    DAEMOON_TRY(to_fs_path(rel, utf16, TDS_UTF16_MAX, &path));
    res = FSUSER_OpenFile(&handle, s->archive, path, flags, 0);
    if (R_FAILED(res)) {
        return from_result(res);
    }

    if (mode == DAEMOON_OPEN_WRITE) {
        /* Create or truncate. Without this a shorter save leaves the tail of the
         * longer one behind, and the result verifies against nothing because the
         * digest is taken from what was written. */
        res = FSFILE_SetSize(handle, 0);
        if (R_FAILED(res)) {
            (void)FSFILE_Close(handle);
            return from_result(res);
        }
    }

    f = (tds_file_t *)calloc(1, sizeof(*f));
    if (f == NULL) {
        (void)FSFILE_Close(handle);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    f->handle = handle;
    f->writable = (mode == DAEMOON_OPEN_WRITE);
    f->stream.ctx = f;
    f->stream.close = file_close;
    f->stream.seek = file_seek;
    if (f->writable) {
        f->stream.write = file_write;
    } else {
        u64 size = 0;
        f->stream.read = file_read;
        if (R_SUCCEEDED(FSFILE_GetSize(handle, &size))) {
            f->stream.size = size;
        }
    }

    *out = &f->stream;
    return DAEMOON_OK;
}

/* Collecting the paths before deleting any of them, rather than deleting during
 * the walk. Removing an entry from a directory that has an open handle being read
 * is undefined on FAT: the walk can skip the entry after the one just deleted, and
 * a file left behind after remove_all is a file the game sees mixed into the save
 * that was just restored. */
typedef struct {
    char            (*paths)[DAEMOON_PATH_MAX];
    size_t            capacity;
    size_t            count;
    daemoon_result_t  err;
} collect_ctx_t;

static int collect_cb(void *user, const char *rel, unsigned long long size)
{
    collect_ctx_t *c = (collect_ctx_t *)user;

    (void)size;
    if (c->count >= c->capacity) {
        /* Refusing beats clearing most of a save: the caller aborts and nothing
         * has been written yet. */
        c->err = DAEMOON_ERR_ARCHIVE_ERROR;
        return 1;
    }
    if (daemoon_strlcpy(c->paths[c->count], DAEMOON_PATH_MAX, rel) != DAEMOON_OK) {
        c->err = DAEMOON_ERR_BUFFER_TOO_SMALL;
        return 1;
    }
    c->count++;
    return 0;
}

/* Called before a restore writes anything, so a file the incoming package does
 * not contain cannot survive. Directories are left: an empty one is harmless, and
 * removing them mid walk is where this kind of code usually goes wrong. */
static daemoon_result_t remove_all(void *ctx, daemoon_save_t *s)
{
    collect_ctx_t c;
    int stopped = 0;
    size_t i;
    daemoon_result_t r;

    (void)ctx;
    if (!s->writable) {
        return DAEMOON_ERR_FORBIDDEN;
    }

    c.capacity = DAEMOON_ARCHIVE_MAX_ENTRIES;
    c.count = 0;
    c.err = DAEMOON_OK;
    c.paths = calloc(c.capacity, DAEMOON_PATH_MAX);
    if (c.paths == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    r = walk(s, "", collect_cb, &c, &stopped, 0);
    if (r == DAEMOON_OK) {
        r = c.err;
    }

    for (i = 0; r == DAEMOON_OK && i < c.count; ++i) {
        u16 utf16[TDS_UTF16_MAX];
        FS_Path path;
        Result res;

        r = to_fs_path(c.paths[i], utf16, TDS_UTF16_MAX, &path);
        if (r != DAEMOON_OK) {
            break;
        }
        res = FSUSER_DeleteFile(s->archive, path);
        if (R_FAILED(res) && R_DESCRIPTION(res) != RD_NOT_FOUND) {
            r = from_result(res);
        }
    }

    free(c.paths);
    return r;
}

/* -------------------------------------------------------------------- titles */

static daemoon_result_t list_titles(void *ctx, daemoon_title_t **out, size_t *count)
{
    daemoon_3ds_save_ctx_t *c = (daemoon_3ds_save_ctx_t *)ctx;
    daemoon_title_t *titles = NULL;
    u64 *ids = NULL;
    u32 total = 0;
    u32 read = 0;
    u32 i;
    size_t n = 0;
    daemoon_result_t r;

    *out = NULL;
    *count = 0;

    r = from_result(AM_GetTitleCount(c->media, &total));
    if (r != DAEMOON_OK) {
        return r;
    }
    if (total == 0) {
        return DAEMOON_OK;
    }

    ids = (u64 *)calloc(total, sizeof(*ids));
    if (ids == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    r = from_result(AM_GetTitleList(&read, c->media, total, ids));
    if (r != DAEMOON_OK) {
        free(ids);
        return r;
    }

    titles = (daemoon_title_t *)calloc(read, sizeof(*titles));
    if (titles == NULL) {
        free(ids);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    for (i = 0; i < read; ++i) {
        daemoon_title_t *t = &titles[n];
        char product[24];

        /* Applications only. System titles, DLC and updates have no save of their
         * own worth touching, and reaching into them is how a console stops
         * booting. */
        if ((u32)(ids[i] >> 32) != 0x00040000u) {
            continue;
        }

        memset(t, 0, sizeof(*t));
        daemoon_3ds_format_title_id(ids[i], t->id, sizeof(t->id));
        t->platform = DAEMOON_PLATFORM_3DS;
        t->save_type = DAEMOON_SAVE_SAVEDATA;
        /* The media type is carried here so archive_for can reopen the same
         * archive later without another lookup. */
        t->size_hint = (unsigned long long)c->media;

        memset(product, 0, sizeof(product));
        if (R_SUCCEEDED(AM_GetTitleProductCode(c->media, ids[i], product))) {
            product[sizeof(product) - 1] = '\0';
        }
        if (product[0] != '\0') {
            (void)daemoon_strlcpy(t->name, sizeof(t->name), product);
        } else {
            (void)daemoon_strlcpy(t->name, sizeof(t->name), t->id);
        }

        /* Whether a save exists is answered by opening it, because that is the
         * question core actually asks and a title list cannot answer it. */
        {
            FS_Archive archive;
            if (archive_for(t, &archive) == DAEMOON_OK) {
                t->has_save = 1;
                (void)FSUSER_CloseArchive(archive);
            }
        }
        if (!t->has_save && c->only_with_saves) {
            continue;
        }

        /* Whether this title binds its save to the console is a Phase 1 question
         * that hardware has to answer. Until it is answered, every title is
         * treated as if it might, and the user is warned before a restore. */
        t->secure_value = 1;

        ++n;
    }

    free(ids);
    *out = titles;
    *count = n;
    return DAEMOON_OK;
}

static void free_titles(void *ctx, daemoon_title_t *titles, size_t count)
{
    (void)ctx;
    (void)count;
    free(titles);
}

const daemoon_save_backend_t daemoon_3ds_save_backend = {
    list_titles,
    free_titles,
    open_save,
    open_save_write,
    list_entries,
    open_file,
    remove_all,
    commit,
    close_save,
    NULL /* is_title_running: there is no reliable way to ask, so the UI warns */
};

/* --------------------------------------------------------- own archive only */

/* Creates this application's own save archive.
 *
 * A declared SaveDataSize does not create anything: the archive appears the first
 * time the title formats it. That is fine for the shipped app, which keeps
 * everything on the SD card and declares 0K, and it is what the unattended self
 * test needs, because the only archive it may destroy is one this title owns.
 *
 * It refuses any title id other than this one. Formatting somebody else's save is
 * not a thing this project does, and an unattended run must not be one command
 * away from it.
 */
daemoon_result_t daemoon_3ds_format_own_save(const daemoon_title_t *t, unsigned blocks)
{
    u64 title_id = 0;
    u64 own_id = 0;
    u32 path[3];
    FS_Path binpath;

    DAEMOON_TRY(daemoon_3ds_parse_title_id(t->id, &title_id));
    if (R_FAILED(APT_GetProgramID(&own_id))) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    if (title_id != own_id) {
        return DAEMOON_ERR_FORBIDDEN;
    }

    (void)path;
    (void)binpath;

    /* ARCHIVE_SAVEDATA with an empty path, not ARCHIVE_USER_SAVEDATA with a title
     * id: the format call only ever applies to the caller's own save, which is
     * also exactly the restriction this function wants. Asking to format anyone
     * else's is refused by the service, and refused above by the id check as
     * well, because relying on somebody else's error handling for that would be
     * careless. */
    return from_result(FSUSER_FormatSaveData(ARCHIVE_SAVEDATA, fsMakePath(PATH_EMPTY, ""),
                                             blocks, 16, 16, 8, 8, false));
}

/* ------------------------------------------------------------ secure values */

daemoon_result_t daemoon_3ds_read_secure_value(const daemoon_title_t *t,
                                               daemoon_3ds_secure_value_t *out)
{
    u64 title_id = 0;
    bool exists = false;
    u64 value = 0;
    Result res;

    DAEMOON_TRY(daemoon_3ds_parse_title_id(t->id, &title_id));

    memset(out, 0, sizeof(*out));
    res = FSUSER_GetSaveDataSecureValue(&exists, &value,
                                        SECUREVALUE_SLOT_SD,
                                        (u32)((title_id >> 8) & 0xffffffu),
                                        (u8)(title_id & 0xffu));
    if (R_FAILED(res)) {
        return from_result(res);
    }
    out->exists = exists ? 1 : 0;
    out->value = value;
    return DAEMOON_OK;
}

daemoon_result_t daemoon_3ds_write_secure_value(const daemoon_title_t *t,
                                                const daemoon_3ds_secure_value_t *value)
{
    u64 title_id = 0;

    DAEMOON_TRY(daemoon_3ds_parse_title_id(t->id, &title_id));
    if (!value->exists) {
        /* Nothing was recorded, so there is nothing to put back. Inventing one
         * would be worse than leaving whatever is there. */
        return DAEMOON_OK;
    }
    return from_result(FSUSER_SetSaveDataSecureValue(value->value,
                                                     SECUREVALUE_SLOT_SD,
                                                     (u32)((title_id >> 8) & 0xffffffu),
                                                     (u8)(title_id & 0xffu)));
}
