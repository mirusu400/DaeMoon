#include "backend_conformance.h"

#include "test.h"

#include <string.h>

/* Every case here corresponds to something core actually relies on. Where a
 * comment says "core relies on this", that is not decoration: it names the caller
 * that would break, so a backend author knows what they are about to change. */

typedef struct {
    const char        *want_path;
    unsigned long long want_size;
    int                found;
    int                count;
    int                stop_after;
} walk_t;

static int walk_cb(void *user, const char *path, unsigned long long size)
{
    walk_t *w = (walk_t *)user;

    w->count++;
    if (w->want_path != NULL && strcmp(path, w->want_path) == 0) {
        w->found = 1;
        w->want_size = size;
    }
    /* Paths are relative to the save root and use forward slashes. A backend
     * handing back an absolute path would send it straight into a package. */
    if (path[0] == '/' || strchr(path, '\\') != NULL) {
        printf("  entry path %s is not relative with forward slashes\n", path);
        return 1;
    }
    if (w->stop_after > 0 && w->count >= w->stop_after) {
        return 1; /* stop the walk */
    }
    return 0;
}

/* The entry the cases use, which a single file backend gets to choose. */
static const char *entry_of(const daemoon_backend_under_test_t *ut)
{
    return ut->entry_name != NULL ? ut->entry_name : "main.sav";
}

static daemoon_result_t write_file(const daemoon_backend_under_test_t *ut, daemoon_save_t *save,
                                   const char *path, const char *body)
{
    daemoon_stream_t *s = NULL;
    daemoon_result_t r;

    DAEMOON_TRY(ut->backend->open_file(ut->ctx, save, path, DAEMOON_OPEN_WRITE, &s));
    r = daemoon_stream_write(s, body, strlen(body));
    if (r != DAEMOON_OK) {
        (void)daemoon_stream_close(s);
        return r;
    }
    return daemoon_stream_close(s);
}

static daemoon_result_t read_file(const daemoon_backend_under_test_t *ut, daemoon_save_t *save,
                                  const char *path, char *out, size_t cap, size_t *out_len)
{
    daemoon_stream_t *s = NULL;
    size_t total = 0;
    daemoon_result_t r;

    DAEMOON_TRY(ut->backend->open_file(ut->ctx, save, path, DAEMOON_OPEN_READ, &s));
    for (;;) {
        size_t got = 0;
        r = daemoon_stream_read(s, out + total, cap - 1 - total, &got);
        if (r != DAEMOON_OK) {
            (void)daemoon_stream_close(s);
            return r;
        }
        if (got == 0) {
            break;
        }
        total += got;
        if (total >= cap - 1) {
            break;
        }
    }
    out[total] = '\0';
    if (out_len != NULL) {
        *out_len = total;
    }
    return daemoon_stream_close(s);
}

/* ---------------------------------------------------------------- the battery */

static void case_write_read_round_trip(const daemoon_backend_under_test_t *ut)
{
    daemoon_save_t *save = NULL;
    char buf[256];
    size_t len = 0;
    walk_t walk;

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));
    CHECK_OK(write_file(ut, save, entry_of(ut), "player data"));
    /* Nested paths exist in real saves. core never creates a directory itself, so
     * open_file for writing has to make whatever the path needs. */
    if (!ut->single_entry) {
        CHECK_OK(write_file(ut, save, "sub/dir/extra.bin", "nested"));
    }
    CHECK_OK(ut->backend->commit(ut->ctx, save));
    CHECK_OK(ut->backend->close_save(ut->ctx, save));

    /* Reopened read only, everything is there and unchanged. */
    CHECK_OK(ut->backend->open_save(ut->ctx, ut->title, &save));

    CHECK_OK(read_file(ut, save, entry_of(ut), buf, sizeof(buf), &len));
    CHECK_STR(buf, "player data");
    if (!ut->single_entry) {
        CHECK_OK(read_file(ut, save, "sub/dir/extra.bin", buf, sizeof(buf), &len));
        CHECK_STR(buf, "nested");
    }

    /* list_entries reports both, with the size core will put in the digest. A
     * size that disagrees with what a read returns makes daemoon_archive_pack
     * refuse, which is correct but looks like a corrupt save to the user. */
    memset(&walk, 0, sizeof(walk));
    walk.want_path = ut->single_entry ? entry_of(ut) : "sub/dir/extra.bin";
    CHECK_OK(ut->backend->list_entries(ut->ctx, save, walk_cb, &walk));
    CHECK_EQ_INT(walk.count, ut->single_entry ? 1 : 2);
    CHECK(walk.found);
    CHECK_EQ_INT(walk.want_size, ut->single_entry ? 11 : 6);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_empty_file(const daemoon_backend_under_test_t *ut)
{
    /* A zero length file is legal and appears in real saves. It still has to be
     * listed, because the digest covers its path and its length. */
    daemoon_save_t *save = NULL;
    walk_t walk;

    const char *name = ut->single_entry ? entry_of(ut) : "empty.bin";

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));
    CHECK_OK(write_file(ut, save, name, ""));
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    memset(&walk, 0, sizeof(walk));
    walk.want_path = name;
    CHECK_OK(ut->backend->list_entries(ut->ctx, save, walk_cb, &walk));
    CHECK(walk.found);
    CHECK_EQ_INT(walk.want_size, 0);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_overwrite_truncates(const daemoon_backend_under_test_t *ut)
{
    /* DAEMOON_OPEN_WRITE creates or truncates. Appending instead would leave the
     * tail of the previous save behind every restore, and the result would still
     * verify against nothing because the digest is computed after. */
    daemoon_save_t *save = NULL;
    char buf[256];
    size_t len = 0;

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));
    CHECK_OK(write_file(ut, save, entry_of(ut), "a much longer original value"));
    CHECK_OK(write_file(ut, save, entry_of(ut), "short"));
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    CHECK_OK(read_file(ut, save, entry_of(ut), buf, sizeof(buf), &len));
    CHECK_STR(buf, "short");
    CHECK_EQ_INT(len, 5);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_remove_all_clears_everything(const daemoon_backend_under_test_t *ut)
{
    /* daemoon_archive_unpack calls this before writing, so a file the incoming
     * package does not contain cannot survive a restore. Missing a nested file
     * here means the game sees a mixture of two saves. */
    daemoon_save_t *save = NULL;
    walk_t walk;

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(write_file(ut, save, entry_of(ut), "x"));
    if (!ut->single_entry) {
        CHECK_OK(write_file(ut, save, "a/one.bin", "x"));
        CHECK_OK(write_file(ut, save, "a/b/two.bin", "x"));
    }
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    CHECK_OK(ut->backend->remove_all(ut->ctx, save));

    memset(&walk, 0, sizeof(walk));
    CHECK_OK(ut->backend->list_entries(ut->ctx, save, walk_cb, &walk));
    CHECK_EQ_INT(walk.count, 0);

    CHECK_OK(ut->backend->commit(ut->ctx, save));
    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_missing_file_is_not_found(const daemoon_backend_under_test_t *ut)
{
    /* daemoon_sync_scan_local treats not_found as "no save yet" rather than a
     * failure, so a backend returning something else turns a first run into an
     * error the user cannot act on. */
    daemoon_save_t *save = NULL;
    daemoon_stream_t *s = NULL;

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    CHECK_RESULT(ut->backend->open_file(ut->ctx, save, "nothing-here.bin",
                                        DAEMOON_OPEN_READ, &s),
                 DAEMOON_ERR_NOT_FOUND);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_read_only_saves_refuse_writes(const daemoon_backend_under_test_t *ut)
{
    /* Scanning opens read only. If that handle could write, a bug in the scan
     * path would be a corrupted save rather than a wrong answer. */
    daemoon_save_t *save = NULL;
    daemoon_stream_t *s = NULL;

    /* Leave something to open. A backend whose save *is* one file has nothing to
     * open read only once the file is gone, and the previous case removed it. */
    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(write_file(ut, save, entry_of(ut), "present"));
    CHECK_OK(ut->backend->commit(ut->ctx, save));
    CHECK_OK(ut->backend->close_save(ut->ctx, save));

    save = NULL;
    CHECK_OK(ut->backend->open_save(ut->ctx, ut->title, &save));
    if (save == NULL) {
        return;
    }
    CHECK(ut->backend->open_file(ut->ctx, save, "should-not-appear.bin",
                                 DAEMOON_OPEN_WRITE, &s) != DAEMOON_OK);
    CHECK(ut->backend->remove_all(ut->ctx, save) != DAEMOON_OK);
    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_walk_stops_when_asked(const daemoon_backend_under_test_t *ut)
{
    /* daemoon_archive_pack stops the walk when it hits its entry limit, and a
     * backend that keeps going would write past the caller's table. */
    daemoon_save_t *save = NULL;
    walk_t walk;

    if (ut->single_entry) {
        /* Nothing to stop early on: there is one entry by construction. */
        return;
    }

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));
    CHECK_OK(write_file(ut, save, "one.bin", "1"));
    CHECK_OK(write_file(ut, save, "two.bin", "2"));
    CHECK_OK(write_file(ut, save, "three.bin", "3"));
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    memset(&walk, 0, sizeof(walk));
    walk.stop_after = 1;
    CHECK_OK(ut->backend->list_entries(ut->ctx, save, walk_cb, &walk));
    CHECK_EQ_INT(walk.count, 1);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_streaming_round_trip(const daemoon_backend_under_test_t *ut)
{
    /* Nothing in core sizes a buffer to a save, so a backend has to cope with a
     * write arriving in pieces and a read handing back short reads. */
    daemoon_save_t *save = NULL;
    daemoon_stream_t *s = NULL;
    unsigned char pattern[1024];
    unsigned char check[1024];
    size_t total = 0;
    size_t i;

    for (i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (unsigned char)(i * 31u + 7u);
    }

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(ut->backend->remove_all(ut->ctx, save));

    CHECK_OK(ut->backend->open_file(ut->ctx, save, entry_of(ut), DAEMOON_OPEN_WRITE, &s));
    for (i = 0; i < sizeof(pattern); i += 97) {
        size_t n = sizeof(pattern) - i;
        CHECK_OK(daemoon_stream_write(s, pattern + i, n < 97 ? n : 97));
    }
    CHECK_OK(daemoon_stream_close(s));
    CHECK_OK(ut->backend->commit(ut->ctx, save));

    CHECK_OK(ut->backend->open_file(ut->ctx, save, entry_of(ut), DAEMOON_OPEN_READ, &s));
    for (;;) {
        size_t got = 0;
        CHECK_OK(daemoon_stream_read(s, check + total, sizeof(check) - total, &got));
        if (got == 0) {
            break;
        }
        total += got;
        if (total >= sizeof(check)) {
            break;
        }
    }
    CHECK_OK(daemoon_stream_close(s));

    CHECK_EQ_INT(total, sizeof(pattern));
    CHECK_EQ_INT(memcmp(pattern, check, sizeof(pattern)), 0);

    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_saves_are_isolated(const daemoon_backend_under_test_t *ut)
{
    /* Two titles are two saves. On the Switch this also has to hold across
     * accounts, which is why an account is selected before any of this runs. */
    daemoon_save_t *a = NULL;
    daemoon_save_t *b = NULL;
    daemoon_stream_t *s = NULL;
    walk_t walk;

    if (ut->other == NULL) {
        return;
    }

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &a));
    CHECK_OK(ut->backend->remove_all(ut->ctx, a));
    CHECK_OK(write_file(ut, a, entry_of(ut), "a"));
    CHECK_OK(ut->backend->commit(ut->ctx, a));
    CHECK_OK(ut->backend->close_save(ut->ctx, a));

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->other, &b));
    CHECK_OK(ut->backend->remove_all(ut->ctx, b));
    CHECK_OK(ut->backend->commit(ut->ctx, b));

    memset(&walk, 0, sizeof(walk));
    CHECK_OK(ut->backend->list_entries(ut->ctx, b, walk_cb, &walk));
    CHECK_EQ_INT(walk.count, 0);
    CHECK_RESULT(ut->backend->open_file(ut->ctx, b, entry_of(ut), DAEMOON_OPEN_READ, &s),
                 DAEMOON_ERR_NOT_FOUND);


    CHECK_OK(ut->backend->close_save(ut->ctx, b));

    /* And the first save is still intact after all of that. */
    a = NULL;
    CHECK_OK(ut->backend->open_save(ut->ctx, ut->title, &a));
    if (a == NULL) {
        return;
    }
    memset(&walk, 0, sizeof(walk));
    walk.want_path = entry_of(ut);
    CHECK_OK(ut->backend->list_entries(ut->ctx, a, walk_cb, &walk));
    CHECK(walk.found);
    CHECK_OK(ut->backend->close_save(ut->ctx, a));
}

static void case_commit_is_reported(const daemoon_backend_under_test_t *ut)
{
    /* The result of commit is the difference between a save that exists and one
     * that does not. Every caller in core checks it; a backend that always returns
     * success is worse than one that fails honestly. */
    daemoon_save_t *save = NULL;

    CHECK_OK(ut->backend->open_save_write(ut->ctx, ut->title, &save));
    CHECK_OK(write_file(ut, save, entry_of(ut), "x"));
    CHECK_OK(ut->backend->commit(ut->ctx, save));
    /* Committing twice with nothing in between is not an error. */
    CHECK_OK(ut->backend->commit(ut->ctx, save));
    CHECK_OK(ut->backend->close_save(ut->ctx, save));
}

static void case_list_titles_is_consistent(const daemoon_backend_under_test_t *ut)
{
    /* daemoon_sync_title takes what this returns and uses id and platform as the
     * key for everything else, including the state file and the server request. */
    daemoon_title_t *titles = NULL;
    size_t count = 0;
    size_t i;
    int found = 0;

    CHECK_OK(ut->backend->list_titles(ut->ctx, &titles, &count));
    for (i = 0; i < count; ++i) {
        CHECK(titles[i].id[0] != '\0');
        CHECK(titles[i].platform != DAEMOON_PLATFORM_UNKNOWN);
        CHECK(titles[i].save_type != DAEMOON_SAVE_UNKNOWN);
        if (strcmp(titles[i].id, ut->title->id) == 0 &&
            titles[i].platform == ut->title->platform) {
            found = 1;
        }
    }
    CHECK(found);

    ut->backend->free_titles(ut->ctx, titles, count);

    /* Freeing an empty list is not a special case anywhere in core. */
    ut->backend->free_titles(ut->ctx, NULL, 0);
}

void daemoon_backend_conformance(const daemoon_backend_under_test_t *ut)
{
    printf("  backend conformance: %s\n", ut->name != NULL ? ut->name : "unnamed");

    if (ut->backend == NULL || ut->title == NULL) {
        CHECK(0);
        return;
    }

    case_write_read_round_trip(ut);
    case_empty_file(ut);
    case_overwrite_truncates(ut);
    case_remove_all_clears_everything(ut);
    case_missing_file_is_not_found(ut);
    case_read_only_saves_refuse_writes(ut);
    case_walk_stops_when_asked(ut);
    case_streaming_round_trip(ut);
    case_saves_are_isolated(ut);
    case_commit_is_reported(ut);
    case_list_titles_is_consistent(ut);
}
