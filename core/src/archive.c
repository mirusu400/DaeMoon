#include <daemoon/archive.h>

#include <daemoon/util/sha256.h>
#include <daemoon/util/strbuf.h>

#include <miniz/miniz.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ path safety */

/* A package can come from anywhere, including a share code typed in by someone
 * else, so an entry path is treated as hostile input. Anything that could escape
 * the save root or confuse a platform filesystem is refused and the whole package
 * is rejected rather than partially extracted. */
static int path_is_safe(const char *p)
{
    size_t i, n;

    if (p == NULL) {
        return 0;
    }
    n = strlen(p);
    if (n == 0 || n >= DAEMOON_PATH_MAX) {
        return 0;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return 0;
    }
    if (strstr(p, "..") != NULL) {
        return 0;
    }
    if (strchr(p, '\\') != NULL || strchr(p, ':') != NULL) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7f) {
            return 0;
        }
    }
    /* "a//b" and a trailing slash both mean the writer produced something odd. */
    if (strstr(p, "//") != NULL || p[n - 1] == '/') {
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------- miniz IO over streams */

typedef struct {
    daemoon_stream_t  *s;
    unsigned long long ofs;
    daemoon_result_t   err;
} zip_io_t;

static size_t zip_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    zip_io_t *io = (zip_io_t *)opaque;

    if (io->err != DAEMOON_OK) {
        return 0;
    }
    /* miniz documents its zip writer output as streamable: file_ofs only ever
     * advances by n. Anything else would mean this build of miniz changed, so it is
     * an error rather than something to paper over with a seek. */
    if (file_ofs != io->ofs) {
        io->err = DAEMOON_ERR_ARCHIVE_ERROR;
        return 0;
    }
    io->err = daemoon_stream_write(io->s, buf, n);
    if (io->err != DAEMOON_OK) {
        return 0;
    }
    io->ofs += n;
    return n;
}

static size_t zip_read_cb(void *opaque, mz_uint64 file_ofs, void *buf, size_t n)
{
    zip_io_t *io = (zip_io_t *)opaque;
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;

    if (io->err != DAEMOON_OK) {
        return 0;
    }
    if (file_ofs != io->ofs) {
        io->err = daemoon_stream_seek(io->s, file_ofs);
        if (io->err != DAEMOON_OK) {
            return 0;
        }
        io->ofs = file_ofs;
    }
    while (done < n) {
        size_t got = 0;
        io->err = daemoon_stream_read(io->s, p + done, n - done, &got);
        if (io->err != DAEMOON_OK) {
            return done;
        }
        if (got == 0) {
            break; /* short read: miniz treats this as a truncated archive */
        }
        done += got;
    }
    io->ofs += done;
    return done;
}

/* ------------------------------------------------------------ entry tables */

static int entry_cmp(const void *a, const void *b)
{
    const daemoon_archive_entry_t *ea = (const daemoon_archive_entry_t *)a;
    const daemoon_archive_entry_t *eb = (const daemoon_archive_entry_t *)b;
    /* Raw byte order, not locale order: the digest has to be reproducible on a
     * console, on a build machine and inside the Go tests. */
    return strcmp(ea->path, eb->path);
}

typedef struct {
    daemoon_archive_ctx_t *ctx;
    daemoon_result_t       err;
} collect_t;

static int collect_entry(void *user, const char *path, unsigned long long size)
{
    collect_t *c = (collect_t *)user;

    if (!path_is_safe(path)) {
        c->err = DAEMOON_ERR_ARCHIVE_ERROR;
        return 1;
    }
    if (c->ctx->count >= DAEMOON_ARCHIVE_MAX_ENTRIES) {
        /* Refuse rather than truncate. A package missing files would restore a save
         * that looks fine and is not. */
        c->err = DAEMOON_ERR_ARCHIVE_ERROR;
        return 1;
    }
    if (daemoon_strlcpy(c->ctx->entries[c->ctx->count].path, DAEMOON_PATH_MAX, path) != DAEMOON_OK) {
        c->err = DAEMOON_ERR_ARCHIVE_ERROR;
        return 1;
    }
    c->ctx->entries[c->ctx->count].size = size;
    c->ctx->count++;
    return 0;
}

/* The per entry header that goes into the payload digest, as documented in
 * archive.h: path, one NUL, then the size as eight big endian bytes. Length
 * prefixing keeps "ab" + "c" from hashing the same as "a" + "bc". */
static void hash_entry_header(daemoon_sha256_t *h, const char *path, unsigned long long size)
{
    unsigned char be[8];
    int i;

    daemoon_sha256_update(h, path, strlen(path));
    daemoon_sha256_update(h, "", 1);
    for (i = 0; i < 8; ++i) {
        be[i] = (unsigned char)((size >> (56 - i * 8)) & 0xffu);
    }
    daemoon_sha256_update(h, be, sizeof(be));
}

/* ------------------------------------------------------------------- pack */

typedef struct {
    const daemoon_env_t *env;
    daemoon_save_t      *save;
    daemoon_stream_t    *file;
    unsigned long long   read_total;
    daemoon_sha256_t    *hash;
    daemoon_result_t     err;
} pack_src_t;

static size_t pack_read_cb(void *opaque, mz_uint64 file_ofs, void *buf, size_t n)
{
    pack_src_t *src = (pack_src_t *)opaque;
    size_t got = 0;

    (void)file_ofs; /* miniz reads a streamed entry strictly in order */

    if (src->err != DAEMOON_OK) {
        return 0;
    }
    src->err = daemoon_stream_read(src->file, buf, n, &got);
    if (src->err != DAEMOON_OK) {
        return 0;
    }
    /* Hash on the way past: the file is read once, not once for the digest and
     * again for the zip. */
    daemoon_sha256_update(src->hash, buf, got);
    src->read_total += got;
    return got;
}

daemoon_result_t daemoon_archive_pack(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                      daemoon_save_t *save, daemoon_manifest_t *m,
                                      daemoon_stream_t *out)
{
    mz_zip_archive zip;
    zip_io_t io;
    collect_t collect;
    daemoon_sha256_t hash;
    unsigned char digest[DAEMOON_SHA256_DIGEST_LEN];
    char manifest_json[DAEMOON_MANIFEST_MAX_BYTES];
    char name[DAEMOON_PATH_MAX + sizeof(DAEMOON_ARCHIVE_PAYLOAD_DIR)];
    size_t manifest_len = 0;
    size_t i;
    daemoon_result_t r;

    if (env == NULL || ctx == NULL || save == NULL || m == NULL || out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    ctx->count = 0;
    collect.ctx = ctx;
    collect.err = DAEMOON_OK;
    DAEMOON_TRY(env->save->list_entries(env->save_ctx, save, collect_entry, &collect));
    DAEMOON_TRY(collect.err);

    qsort(ctx->entries, ctx->count, sizeof(ctx->entries[0]), entry_cmp);

    memset(&zip, 0, sizeof(zip));
    io.s = out;
    io.ofs = 0;
    io.err = DAEMOON_OK;
    zip.m_pWrite = zip_write_cb;
    zip.m_pIO_opaque = &io;
    if (!mz_zip_writer_init_v2(&zip, 0, 0)) {
        return DAEMOON_ERR_ARCHIVE_ERROR;
    }

    daemoon_sha256_init(&hash);
    m->size = 0;

    for (i = 0; i < ctx->count; ++i) {
        pack_src_t src;
        daemoon_strbuf_t sb;

        daemoon_strbuf_init(&sb, name, sizeof(name));
        daemoon_strbuf_add(&sb, DAEMOON_ARCHIVE_PAYLOAD_DIR);
        daemoon_strbuf_add(&sb, ctx->entries[i].path);
        r = daemoon_strbuf_result(&sb);
        if (r != DAEMOON_OK) {
            goto fail;
        }

        hash_entry_header(&hash, ctx->entries[i].path, ctx->entries[i].size);

        src.env = env;
        src.save = save;
        src.file = NULL;
        src.read_total = 0;
        src.hash = &hash;
        src.err = DAEMOON_OK;

        r = env->save->open_file(env->save_ctx, save, ctx->entries[i].path,
                                 DAEMOON_OPEN_READ, &src.file);
        if (r != DAEMOON_OK) {
            goto fail;
        }

        if (!mz_zip_writer_add_read_buf_callback(&zip, name, pack_read_cb, &src,
                                                 ctx->entries[i].size, NULL, NULL, 0,
                                                 MZ_DEFAULT_COMPRESSION, NULL, 0, NULL, 0)) {
            r = (src.err != DAEMOON_OK) ? src.err
                : (io.err != DAEMOON_OK) ? io.err
                                         : DAEMOON_ERR_ARCHIVE_ERROR;
            (void)daemoon_stream_close(src.file);
            goto fail;
        }

        r = daemoon_stream_close(src.file);
        if (r != DAEMOON_OK) {
            goto fail;
        }
        if (src.read_total != ctx->entries[i].size) {
            /* The save changed between the listing and the read, which means a game
             * is holding the archive. Refuse: a package assembled from a moving save
             * is not a save. */
            r = DAEMOON_ERR_IO_ERROR;
            goto fail;
        }
        m->size += src.read_total;
    }

    daemoon_sha256_final(&hash, digest);
    daemoon_sha256_hex(digest, m->sha256);

    /* manifest.json goes in last so the digest can be computed in the same pass
     * that writes the payload. Zip has no ordering requirement and readers here go
     * through the central directory. */
    r = daemoon_manifest_write(m, manifest_json, sizeof(manifest_json), &manifest_len);
    if (r != DAEMOON_OK) {
        goto fail;
    }
    if (!mz_zip_writer_add_mem(&zip, DAEMOON_ARCHIVE_MANIFEST_PATH, manifest_json, manifest_len,
                               MZ_DEFAULT_COMPRESSION)) {
        r = (io.err != DAEMOON_OK) ? io.err : DAEMOON_ERR_ARCHIVE_ERROR;
        goto fail;
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        r = (io.err != DAEMOON_OK) ? io.err : DAEMOON_ERR_ARCHIVE_ERROR;
        goto fail;
    }
    mz_zip_writer_end(&zip);
    return io.err;

fail:
    mz_zip_writer_end(&zip);
    return r;
}

daemoon_result_t daemoon_archive_hash_save(const daemoon_env_t *env, daemoon_archive_ctx_t *ctx,
                                           daemoon_save_t *save, char *out_hex,
                                           unsigned long long *out_size)
{
    daemoon_sha256_t hash;
    unsigned char digest[DAEMOON_SHA256_DIGEST_LEN];
    collect_t collect;
    unsigned long long total = 0;
    size_t i;

    if (env == NULL || ctx == NULL || save == NULL || out_hex == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    ctx->count = 0;
    collect.ctx = ctx;
    collect.err = DAEMOON_OK;
    DAEMOON_TRY(env->save->list_entries(env->save_ctx, save, collect_entry, &collect));
    DAEMOON_TRY(collect.err);

    qsort(ctx->entries, ctx->count, sizeof(ctx->entries[0]), entry_cmp);

    daemoon_sha256_init(&hash);
    for (i = 0; i < ctx->count; ++i) {
        daemoon_stream_t *f = NULL;
        unsigned long long got_total = 0;

        hash_entry_header(&hash, ctx->entries[i].path, ctx->entries[i].size);

        DAEMOON_TRY(env->save->open_file(env->save_ctx, save, ctx->entries[i].path,
                                         DAEMOON_OPEN_READ, &f));
        for (;;) {
            size_t got = 0;
            daemoon_result_t r = daemoon_stream_read(f, env->scratch, env->scratch_len, &got);
            if (r != DAEMOON_OK) {
                (void)daemoon_stream_close(f);
                return r;
            }
            if (got == 0) {
                break;
            }
            daemoon_sha256_update(&hash, env->scratch, got);
            got_total += got;
        }
        DAEMOON_TRY(daemoon_stream_close(f));

        if (got_total != ctx->entries[i].size) {
            return DAEMOON_ERR_IO_ERROR;
        }
        total += got_total;
    }

    daemoon_sha256_final(&hash, digest);
    daemoon_sha256_hex(digest, out_hex);
    if (out_size != NULL) {
        *out_size = total;
    }
    return DAEMOON_OK;
}

/* ------------------------------------------------------------------ reader */

typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         len;
    int            overflow;
} membuf_t;

static size_t mem_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    membuf_t *mb = (membuf_t *)opaque;

    (void)file_ofs;
    if (mb->len + n > mb->cap) {
        mb->overflow = 1;
        return 0;
    }
    memcpy(mb->buf + mb->len, buf, n);
    mb->len += n;
    return n;
}

static daemoon_result_t reader_open(mz_zip_archive *zip, zip_io_t *io, daemoon_stream_t *pkg)
{
    if (pkg == NULL || pkg->read == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    if (pkg->seek == NULL) {
        /* The central directory lives at the end of a zip, so a package is always
         * staged as a file before it is read. A socket cannot be read this way. */
        return DAEMOON_ERR_UNSUPPORTED;
    }
    if (pkg->size == 0) {
        return DAEMOON_ERR_ARCHIVE_ERROR;
    }

    memset(zip, 0, sizeof(*zip));
    io->s = pkg;
    io->ofs = 0;
    io->err = DAEMOON_OK;
    DAEMOON_TRY(daemoon_stream_seek(pkg, 0));

    zip->m_pRead = zip_read_cb;
    zip->m_pIO_opaque = io;
    if (!mz_zip_reader_init(zip, pkg->size, 0)) {
        return (io->err != DAEMOON_OK) ? io->err : DAEMOON_ERR_ARCHIVE_ERROR;
    }
    return DAEMOON_OK;
}

static daemoon_result_t read_manifest_from(mz_zip_archive *zip, zip_io_t *io,
                                           daemoon_manifest_t *out)
{
    membuf_t mb;
    char json[DAEMOON_MANIFEST_MAX_BYTES];
    int idx;

    idx = mz_zip_reader_locate_file(zip, DAEMOON_ARCHIVE_MANIFEST_PATH, NULL,
                                    MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (idx < 0) {
        return DAEMOON_ERR_INVALID_MANIFEST;
    }

    mb.buf = (unsigned char *)json;
    mb.cap = sizeof(json);
    mb.len = 0;
    mb.overflow = 0;
    if (!mz_zip_reader_extract_to_callback(zip, (mz_uint)idx, mem_write_cb, &mb, 0)) {
        if (mb.overflow) {
            return DAEMOON_ERR_INVALID_MANIFEST;
        }
        return (io->err != DAEMOON_OK) ? io->err : DAEMOON_ERR_ARCHIVE_ERROR;
    }

    return daemoon_manifest_parse(json, mb.len, out);
}

daemoon_result_t daemoon_archive_read_manifest(daemoon_stream_t *pkg, daemoon_manifest_t *out)
{
    mz_zip_archive zip;
    zip_io_t io;
    daemoon_result_t r;

    if (out == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(reader_open(&zip, &io, pkg));
    r = read_manifest_from(&zip, &io, out);
    mz_zip_reader_end(&zip);
    return r;
}

/* Fill ctx with the payload entries of an open package, sorted. */
static daemoon_result_t collect_payload(mz_zip_archive *zip, daemoon_archive_ctx_t *ctx)
{
    mz_uint n = mz_zip_reader_get_num_files(zip);
    mz_uint i;
    const size_t prefix = sizeof(DAEMOON_ARCHIVE_PAYLOAD_DIR) - 1;

    ctx->count = 0;
    for (i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        const char *rel;

        if (!mz_zip_reader_file_stat(zip, i, &st)) {
            return DAEMOON_ERR_ARCHIVE_ERROR;
        }
        if (st.m_is_directory) {
            continue;
        }
        if (strcmp(st.m_filename, DAEMOON_ARCHIVE_MANIFEST_PATH) == 0) {
            continue;
        }
        if (strncmp(st.m_filename, DAEMOON_ARCHIVE_PAYLOAD_DIR, prefix) != 0) {
            /* An entry that is neither the manifest nor payload means this package
             * was made by something else. Refuse it. */
            return DAEMOON_ERR_ARCHIVE_ERROR;
        }
        rel = st.m_filename + prefix;
        if (!path_is_safe(rel)) {
            return DAEMOON_ERR_ARCHIVE_ERROR;
        }
        if (ctx->count >= DAEMOON_ARCHIVE_MAX_ENTRIES) {
            return DAEMOON_ERR_ARCHIVE_ERROR;
        }
        if (daemoon_strlcpy(ctx->entries[ctx->count].path, DAEMOON_PATH_MAX, rel) != DAEMOON_OK) {
            return DAEMOON_ERR_ARCHIVE_ERROR;
        }
        ctx->entries[ctx->count].size = st.m_uncomp_size;
        ctx->count++;
    }

    qsort(ctx->entries, ctx->count, sizeof(ctx->entries[0]), entry_cmp);
    return DAEMOON_OK;
}

typedef struct {
    daemoon_sha256_t  *hash;
    unsigned long long len;
} hash_sink_t;

static size_t hash_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    hash_sink_t *hs = (hash_sink_t *)opaque;
    (void)file_ofs;
    daemoon_sha256_update(hs->hash, buf, n);
    hs->len += n;
    return n;
}

daemoon_result_t daemoon_archive_verify(const daemoon_env_t *env, daemoon_stream_t *pkg,
                                        const daemoon_manifest_t *m)
{
    mz_zip_archive zip;
    zip_io_t io;
    daemoon_archive_ctx_t ctx;
    daemoon_sha256_t hash;
    unsigned char digest[DAEMOON_SHA256_DIGEST_LEN];
    char hex[DAEMOON_SHA256_HEX];
    unsigned long long total = 0;
    size_t i;
    daemoon_result_t r;

    (void)env;
    if (m == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(reader_open(&zip, &io, pkg));

    r = collect_payload(&zip, &ctx);
    if (r != DAEMOON_OK) {
        goto done;
    }

    daemoon_sha256_init(&hash);
    for (i = 0; i < ctx.count; ++i) {
        hash_sink_t hs;
        int idx;
        char name[DAEMOON_PATH_MAX + sizeof(DAEMOON_ARCHIVE_PAYLOAD_DIR)];
        daemoon_strbuf_t sb;

        daemoon_strbuf_init(&sb, name, sizeof(name));
        daemoon_strbuf_add(&sb, DAEMOON_ARCHIVE_PAYLOAD_DIR);
        daemoon_strbuf_add(&sb, ctx.entries[i].path);
        r = daemoon_strbuf_result(&sb);
        if (r != DAEMOON_OK) {
            goto done;
        }

        idx = mz_zip_reader_locate_file(&zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
        if (idx < 0) {
            r = DAEMOON_ERR_ARCHIVE_ERROR;
            goto done;
        }

        hash_entry_header(&hash, ctx.entries[i].path, ctx.entries[i].size);

        hs.hash = &hash;
        hs.len = 0;
        if (!mz_zip_reader_extract_to_callback(&zip, (mz_uint)idx, hash_write_cb, &hs, 0)) {
            r = (io.err != DAEMOON_OK) ? io.err : DAEMOON_ERR_ARCHIVE_ERROR;
            goto done;
        }
        if (hs.len != ctx.entries[i].size) {
            r = DAEMOON_ERR_ARCHIVE_ERROR;
            goto done;
        }
        total += hs.len;
    }

    daemoon_sha256_final(&hash, digest);
    daemoon_sha256_hex(digest, hex);

    if (!daemoon_sha256_hex_equal(hex, m->sha256) || total != m->size) {
        r = DAEMOON_ERR_CHECKSUM_MISMATCH;
        goto done;
    }
    r = DAEMOON_OK;

done:
    mz_zip_reader_end(&zip);
    return r;
}

/* ----------------------------------------------------------------- unpack */

typedef struct {
    daemoon_stream_t *out;
    daemoon_result_t  err;
} extract_sink_t;

static size_t extract_write_cb(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    extract_sink_t *es = (extract_sink_t *)opaque;

    (void)file_ofs;
    if (es->err != DAEMOON_OK) {
        return 0;
    }
    es->err = daemoon_stream_write(es->out, buf, n);
    return (es->err == DAEMOON_OK) ? n : 0;
}

daemoon_result_t daemoon_archive_unpack(const daemoon_env_t *env, daemoon_stream_t *pkg,
                                        daemoon_save_t *save)
{
    mz_zip_archive zip;
    zip_io_t io;
    daemoon_archive_ctx_t ctx;
    size_t i;
    daemoon_result_t r;

    if (env == NULL || save == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    DAEMOON_TRY(reader_open(&zip, &io, pkg));

    r = collect_payload(&zip, &ctx);
    if (r != DAEMOON_OK) {
        goto done;
    }

    /* Clear first: a file the package does not contain must not survive a restore,
     * or the game sees a mixture of two saves. */
    r = env->save->remove_all(env->save_ctx, save);
    if (r != DAEMOON_OK) {
        goto done;
    }

    for (i = 0; i < ctx.count; ++i) {
        extract_sink_t es;
        char name[DAEMOON_PATH_MAX + sizeof(DAEMOON_ARCHIVE_PAYLOAD_DIR)];
        daemoon_strbuf_t sb;
        int idx;

        daemoon_strbuf_init(&sb, name, sizeof(name));
        daemoon_strbuf_add(&sb, DAEMOON_ARCHIVE_PAYLOAD_DIR);
        daemoon_strbuf_add(&sb, ctx.entries[i].path);
        r = daemoon_strbuf_result(&sb);
        if (r != DAEMOON_OK) {
            goto done;
        }

        idx = mz_zip_reader_locate_file(&zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
        if (idx < 0) {
            r = DAEMOON_ERR_ARCHIVE_ERROR;
            goto done;
        }

        es.out = NULL;
        es.err = DAEMOON_OK;
        /* The backend creates any missing parent directories. */
        r = env->save->open_file(env->save_ctx, save, ctx.entries[i].path,
                                 DAEMOON_OPEN_WRITE, &es.out);
        if (r != DAEMOON_OK) {
            goto done;
        }

        if (!mz_zip_reader_extract_to_callback(&zip, (mz_uint)idx, extract_write_cb, &es, 0)) {
            r = (es.err != DAEMOON_OK) ? es.err
                : (io.err != DAEMOON_OK) ? io.err
                                         : DAEMOON_ERR_ARCHIVE_ERROR;
            (void)daemoon_stream_close(es.out);
            goto done;
        }

        r = daemoon_stream_close(es.out);
        if (r != DAEMOON_OK) {
            goto done;
        }
    }
    r = DAEMOON_OK;

done:
    mz_zip_reader_end(&zip);
    return r;
}
