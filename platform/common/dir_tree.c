/* Walking and clearing a directory tree, over newlib stdio.
 *
 * A Switch save is a mounted filesystem: once fsdevMountSaveData has run, the save is
 * an ordinary directory and the two things a save backend has to do to it - list every
 * file, and clear it before a restore - are the same tree walk either console needs.
 *
 * It lives here rather than in the Switch backend because it is not about the Switch.
 * Nothing in this file may include a platform header.
 */
#include "daemoon_newlib.h"

#include <daemoon/util/strbuf.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Deep enough for any save this project has met and shallow enough that the
 * recursion below cannot run a console out of stack. A save is a game's own data;
 * one that nests further than this is a bug report, not a case to support. */
#define DIR_TREE_MAX_DEPTH 8

static int is_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* Builds "<base>/<name>", or "<name>" when base is empty.
 *
 * Relative paths with forward slashes are what daemoon_entry_cb is specified to
 * receive, and they are also what goes into a package - so this is the one place the
 * separator is decided. */
static daemoon_result_t join(char *out, size_t cap, const char *base, const char *name)
{
    daemoon_strbuf_t sb;

    daemoon_strbuf_init(&sb, out, cap);
    if (base != NULL && base[0] != '\0') {
        daemoon_strbuf_add(&sb, base);
        daemoon_strbuf_addc(&sb, '/');
    }
    daemoon_strbuf_add(&sb, name);
    return daemoon_strbuf_result(&sb);
}

static daemoon_result_t walk(const char *root, const char *rel, int depth,
                             daemoon_entry_cb cb, void *user, int *stopped)
{
    char abs[DAEMOON_PATH_MAX * 2];
    DIR *dir;
    struct dirent *ent;
    daemoon_result_t r;

    if (depth > DIR_TREE_MAX_DEPTH) {
        return DAEMOON_ERR_UNSUPPORTED;
    }
    DAEMOON_TRY(join(abs, sizeof(abs), root, rel));

    dir = opendir(abs);
    if (dir == NULL) {
        return errno == ENOENT ? DAEMOON_ERR_NOT_FOUND : DAEMOON_ERR_IO_ERROR;
    }

    r = DAEMOON_OK;
    while ((ent = readdir(dir)) != NULL) {
        char childrel[DAEMOON_PATH_MAX];
        char childabs[DAEMOON_PATH_MAX * 2];
        struct stat st;

        if (is_dot(ent->d_name)) {
            continue;
        }
        r = join(childrel, sizeof(childrel), rel, ent->d_name);
        if (r != DAEMOON_OK) {
            break;
        }
        r = join(childabs, sizeof(childabs), root, childrel);
        if (r != DAEMOON_OK) {
            break;
        }
        /* stat rather than d_type: newlib fills d_type in on some devices and not
         * others, and a directory taken for a file is a save that packs as garbage. */
        if (stat(childabs, &st) != 0) {
            r = DAEMOON_ERR_IO_ERROR;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            r = walk(root, childrel, depth + 1, cb, user, stopped);
            if (r != DAEMOON_OK || *stopped) {
                break;
            }
            continue;
        }
        if (cb(user, childrel, (unsigned long long)st.st_size) != 0) {
            *stopped = 1;
            break;
        }
    }
    (void)closedir(dir);
    return r;
}

daemoon_result_t daemoon_dir_walk(const char *root, daemoon_entry_cb cb, void *user)
{
    int stopped = 0;

    if (root == NULL || cb == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    return walk(root, "", 0, cb, user, &stopped);
}

static daemoon_result_t clear(const char *root, const char *rel, int depth)
{
    char abs[DAEMOON_PATH_MAX * 2];
    DIR *dir;
    struct dirent *ent;
    daemoon_result_t r;

    if (depth > DIR_TREE_MAX_DEPTH) {
        return DAEMOON_ERR_UNSUPPORTED;
    }
    DAEMOON_TRY(join(abs, sizeof(abs), root, rel));

    dir = opendir(abs);
    if (dir == NULL) {
        return errno == ENOENT ? DAEMOON_OK : DAEMOON_ERR_IO_ERROR;
    }

    r = DAEMOON_OK;
    while ((ent = readdir(dir)) != NULL) {
        char childrel[DAEMOON_PATH_MAX];
        char childabs[DAEMOON_PATH_MAX * 2];
        struct stat st;

        if (is_dot(ent->d_name)) {
            continue;
        }
        r = join(childrel, sizeof(childrel), rel, ent->d_name);
        if (r != DAEMOON_OK) {
            break;
        }
        r = join(childabs, sizeof(childabs), root, childrel);
        if (r != DAEMOON_OK) {
            break;
        }
        if (stat(childabs, &st) != 0) {
            r = DAEMOON_ERR_IO_ERROR;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            r = clear(root, childrel, depth + 1);
            if (r != DAEMOON_OK) {
                break;
            }
            /* The directory itself goes after its contents. Reopening the parent
             * would be the alternative, and readdir behaviour while the directory is
             * being modified is not something to rely on across two toolchains. */
            if (rmdir(childabs) != 0) {
                r = DAEMOON_ERR_IO_ERROR;
                break;
            }
            continue;
        }
        if (remove(childabs) != 0) {
            r = DAEMOON_ERR_IO_ERROR;
            break;
        }
    }
    (void)closedir(dir);
    return r;
}

daemoon_result_t daemoon_dir_remove_all(const char *root)
{
    if (root == NULL) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    /* The root stays; its contents go. A save archive is the thing being cleared, and
     * removing it would be removing the save rather than emptying it. */
    return clear(root, "", 0);
}
