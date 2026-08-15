/* A libctru shaped surface backed by ordinary directories.
 *
 * Written to be strict in the places the real service is strict, because a stub
 * that is more forgiving than the thing it stands in for is worse than no stub:
 * it produces a green run and a backend that fails on hardware.
 *
 * Specifically:
 *   - the converters truncate and report the length they would have needed, which
 *     is the libctru contract and the source of a real bug in this backend
 *   - a read only handle refuses writes
 *   - writes go to an explicit offset, like FSFILE_Write, rather than a cursor
 *   - handles are counted, so a leak is visible
 */
#define _POSIX_C_SOURCE 200809L

#include "3ds.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STUB_MAX_HANDLES 64
#define STUB_MAX_TITLES  16
#define STUB_PATH_MAX    1024
/* Roots and names are bounded well below STUB_PATH_MAX so a join provably fits. */
#define STUB_ROOT_MAX    256
#define STUB_NAME_MAX    256

typedef struct {
    int   used;
    int   is_dir;
    FILE *fp;
    int   writable;

    /* Directory iteration state. Snapshotted at open, which is what a real
     * directory handle effectively gives you and what keeps a test from depending
     * on the order the host filesystem happens to return. */
    char (*entries)[256];
    u64  *sizes;
    u8   *is_subdir;
    size_t count;
    size_t next;
} stub_handle_t;

static char          g_root[STUB_ROOT_MAX];
static stub_handle_t g_handles[STUB_MAX_HANDLES];
static unsigned      g_commits;
static int           g_fail_next_commit;

static struct {
    u64  id;
    char product[24];
    int  used;
} g_titles[STUB_MAX_TITLES];

static u64 g_secure_values[STUB_MAX_TITLES];
static int g_secure_present[STUB_MAX_TITLES];

/* ---------------------------------------------------------------- controls */

void daemoon_stub_init(const char *root)
{
    (void)snprintf(g_root, sizeof(g_root), "%s", root);
    daemoon_stub_reset();
}

void daemoon_stub_reset(void)
{
    size_t i;

    for (i = 0; i < STUB_MAX_HANDLES; ++i) {
        if (g_handles[i].used && g_handles[i].fp != NULL) {
            fclose(g_handles[i].fp);
        }
        free(g_handles[i].entries);
        free(g_handles[i].sizes);
        free(g_handles[i].is_subdir);
        memset(&g_handles[i], 0, sizeof(g_handles[i]));
    }
    memset(g_titles, 0, sizeof(g_titles));
    memset(g_secure_values, 0, sizeof(g_secure_values));
    memset(g_secure_present, 0, sizeof(g_secure_present));
    g_commits = 0;
    g_fail_next_commit = 0;
}

void daemoon_stub_add_title(u64 title_id, const char *product_code)
{
    size_t i;

    for (i = 0; i < STUB_MAX_TITLES; ++i) {
        if (g_titles[i].used) {
            continue;
        }
        g_titles[i].used = 1;
        g_titles[i].id = title_id;
        (void)snprintf(g_titles[i].product, sizeof(g_titles[i].product), "%s",
                       product_code != NULL ? product_code : "");
        return;
    }
}

unsigned daemoon_stub_commits(void) { return g_commits; }

void daemoon_stub_fail_next_commit(void) { g_fail_next_commit = 1; }

int daemoon_stub_open_handles(void)
{
    int n = 0;
    size_t i;

    for (i = 0; i < STUB_MAX_HANDLES; ++i) {
        if (g_handles[i].used) {
            ++n;
        }
    }
    return n;
}

/* ----------------------------------------------------------------- helpers */

static Result stub_error(int summary, int description)
{
    return DAEMOON_STUB_RESULT(summary, description);
}

static stub_handle_t *handle_of(Handle h)
{
    if (h == 0 || h > STUB_MAX_HANDLES) {
        return NULL;
    }
    if (!g_handles[h - 1].used) {
        return NULL;
    }
    return &g_handles[h - 1];
}

static Handle handle_alloc(stub_handle_t **out)
{
    size_t i;

    for (i = 0; i < STUB_MAX_HANDLES; ++i) {
        if (g_handles[i].used) {
            continue;
        }
        memset(&g_handles[i], 0, sizeof(g_handles[i]));
        g_handles[i].used = 1;
        *out = &g_handles[i];
        return (Handle)(i + 1);
    }
    return 0;
}

static void handle_free(stub_handle_t *h)
{
    free(h->entries);
    free(h->sizes);
    free(h->is_subdir);
    memset(h, 0, sizeof(*h));
}

/* An archive is a directory under the root, named for the title id the binary
 * path carried. */
static void archive_dir(FS_Archive archive, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%.*s/%016llX", (int)sizeof(g_root) - 1, g_root,
                   (unsigned long long)archive);
}

/* The UTF-16 path a caller built, back into something openable. */
static int path_to_host(FS_Archive archive, FS_Path path, char *out, size_t cap)
{
    char rel[STUB_PATH_MAX];
    char dir[STUB_PATH_MAX];
    ssize_t n;

    if (path.type != PATH_UTF16 || path.data == NULL) {
        return -1;
    }
    n = utf16_to_utf8((u8 *)rel, (const u16 *)path.data, sizeof(rel) - 1);
    if (n < 0 || (size_t)n > sizeof(rel) - 1) {
        return -1;
    }
    rel[n] = '\0';

    archive_dir(archive, dir, sizeof(dir));
    (void)snprintf(out, cap, "%.*s%.*s", (int)(STUB_ROOT_MAX + 24), dir,
                   (int)STUB_NAME_MAX, rel);
    return 0;
}

/* ---------------------------------------------------------------- archives */

Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path)
{
    const u32 *words;
    char dir[STUB_PATH_MAX];
    struct stat st;
    u64 title_id;

    if (id != ARCHIVE_USER_SAVEDATA) {
        return stub_error(RS_NOTSUPPORTED, RD_INVALID_ARGUMENT);
    }
    if (path.type != PATH_BINARY || path.size != 12 || path.data == NULL) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }

    words = (const u32 *)path.data;
    title_id = ((u64)words[2] << 32) | (u64)words[1];

    *archive = (FS_Archive)title_id;
    archive_dir(*archive, dir, sizeof(dir));

    /* A title with no save archive is not found, which is what the backend has to
     * turn into "no save yet" rather than a failure. */
    if (stat(dir, &st) != 0) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }
    return 0;
}

Result FSUSER_CloseArchive(FS_Archive archive)
{
    (void)archive;
    return 0;
}

Result FSUSER_ControlArchive(FS_Archive archive, FS_ArchiveAction action, void *input,
                             u32 inputSize, void *output, u32 outputSize)
{
    (void)archive;
    (void)input;
    (void)inputSize;
    (void)output;
    (void)outputSize;

    if (action != ARCHIVE_ACTION_COMMIT_SAVE_DATA) {
        return stub_error(RS_NOTSUPPORTED, RD_INVALID_ARGUMENT);
    }
    if (g_fail_next_commit) {
        g_fail_next_commit = 0;
        return stub_error(RS_INVALIDSTATE, RD_NOT_AUTHORIZED);
    }
    ++g_commits;
    return 0;
}

/* ------------------------------------------------------------------- files */

Result FSUSER_OpenFile(Handle *out, FS_Archive archive, FS_Path path, u32 openFlags,
                       u32 attributes)
{
    char host[STUB_PATH_MAX];
    stub_handle_t *h;
    Handle handle;
    FILE *fp;

    (void)attributes;
    if (path_to_host(archive, path, host, sizeof(host)) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }

    if (openFlags & FS_OPEN_WRITE) {
        /* Real FS creates only when asked, and never creates the parents. */
        fp = fopen(host, "r+b");
        if (fp == NULL) {
            if (!(openFlags & FS_OPEN_CREATE)) {
                return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
            }
            fp = fopen(host, "w+b");
        }
    } else {
        fp = fopen(host, "rb");
    }
    if (fp == NULL) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }

    handle = handle_alloc(&h);
    if (handle == 0) {
        fclose(fp);
        return stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
    }
    h->fp = fp;
    h->writable = (openFlags & FS_OPEN_WRITE) ? 1 : 0;

    *out = handle;
    return 0;
}

Result FSFILE_Read(Handle handle, u32 *bytesRead, u64 offset, void *buffer, u32 size)
{
    stub_handle_t *h = handle_of(handle);
    size_t n;

    if (h == NULL || h->is_dir) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (fseek(h->fp, (long)offset, SEEK_SET) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    n = fread(buffer, 1, size, h->fp);
    *bytesRead = (u32)n;
    return 0;
}

Result FSFILE_Write(Handle handle, u32 *bytesWritten, u64 offset, const void *buffer,
                    u32 size, u32 flags)
{
    stub_handle_t *h = handle_of(handle);
    size_t n;

    (void)flags;
    if (h == NULL || h->is_dir) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (!h->writable) {
        return stub_error(RS_INVALIDARG, RD_NOT_AUTHORIZED);
    }
    if (fseek(h->fp, (long)offset, SEEK_SET) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    n = fwrite(buffer, 1, size, h->fp);
    *bytesWritten = (u32)n;
    return n == size ? 0 : stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
}

Result FSFILE_GetSize(Handle handle, u64 *size)
{
    stub_handle_t *h = handle_of(handle);
    long pos;

    if (h == NULL || h->is_dir) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (fseek(h->fp, 0, SEEK_END) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    pos = ftell(h->fp);
    if (pos < 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    *size = (u64)pos;
    return 0;
}

Result FSFILE_SetSize(Handle handle, u64 size)
{
    stub_handle_t *h = handle_of(handle);

    if (h == NULL || h->is_dir || !h->writable) {
        return stub_error(RS_INVALIDARG, RD_NOT_AUTHORIZED);
    }
    fflush(h->fp);
    if (ftruncate(fileno(h->fp), (off_t)size) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    return 0;
}

Result FSFILE_Flush(Handle handle)
{
    stub_handle_t *h = handle_of(handle);

    if (h == NULL || h->is_dir) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    return fflush(h->fp) == 0 ? 0 : stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
}

Result FSFILE_Close(Handle handle)
{
    stub_handle_t *h = handle_of(handle);

    if (h == NULL) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (h->fp != NULL) {
        fclose(h->fp);
    }
    handle_free(h);
    return 0;
}

Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path)
{
    char host[STUB_PATH_MAX];

    if (path_to_host(archive, path, host, sizeof(host)) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (unlink(host) != 0) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }
    return 0;
}

Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes)
{
    char host[STUB_PATH_MAX];

    (void)attributes;
    if (path_to_host(archive, path, host, sizeof(host)) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    if (mkdir(host, 0755) != 0) {
        if (errno == EEXIST) {
            return stub_error(RS_INVALIDSTATE, RD_ALREADY_EXISTS);
        }
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }
    return 0;
}

/* -------------------------------------------------------------- directories */

Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path)
{
    char host[STUB_PATH_MAX];
    stub_handle_t *h;
    Handle handle;
    DIR *d;
    struct dirent *ent;
    size_t cap = 16;

    if (path_to_host(archive, path, host, sizeof(host)) != 0) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    d = opendir(host);
    if (d == NULL) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }

    handle = handle_alloc(&h);
    if (handle == 0) {
        closedir(d);
        return stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
    }
    h->is_dir = 1;
    h->entries = calloc(cap, 256);
    h->sizes = calloc(cap, sizeof(u64));
    h->is_subdir = calloc(cap, 1);
    if (h->entries == NULL || h->sizes == NULL || h->is_subdir == NULL) {
        closedir(d);
        handle_free(h);
        return stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
    }

    while ((ent = readdir(d)) != NULL) {
        char child[STUB_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (h->count == cap) {
            size_t next = cap * 2;
            void *e = realloc(h->entries, next * 256);
            void *s = realloc(h->sizes, next * sizeof(u64));
            void *b = realloc(h->is_subdir, next);
            if (e == NULL || s == NULL || b == NULL) {
                free(e); free(s); free(b);
                closedir(d);
                handle_free(h);
                return stub_error(RS_OUTOFRESOURCE, RD_OUT_OF_MEMORY);
            }
            h->entries = e;
            h->sizes = s;
            h->is_subdir = b;
            cap = next;
        }

        (void)snprintf(h->entries[h->count], STUB_NAME_MAX, "%s", ent->d_name);
        (void)snprintf(child, sizeof(child), "%.*s/%.*s",
                       (int)(STUB_ROOT_MAX + 24), host, (int)STUB_NAME_MAX - 1,
                       ent->d_name);
        if (stat(child, &st) == 0) {
            h->sizes[h->count] = (u64)st.st_size;
            h->is_subdir[h->count] = S_ISDIR(st.st_mode) ? 1 : 0;
        }
        ++h->count;
    }
    closedir(d);

    *out = handle;
    return 0;
}

Result FSDIR_Read(Handle handle, u32 *entriesRead, u32 entryCount, FS_DirectoryEntry *entries)
{
    stub_handle_t *h = handle_of(handle);
    u32 produced = 0;

    if (h == NULL || !h->is_dir) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }

    while (produced < entryCount && h->next < h->count) {
        FS_DirectoryEntry *e = &entries[produced];
        ssize_t units;

        memset(e, 0, sizeof(*e));
        units = utf8_to_utf16(e->name, (const u8 *)h->entries[h->next],
                              sizeof(e->name) / sizeof(e->name[0]) - 1);
        if (units < 0) {
            return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
        }
        e->name[units] = 0;
        e->valid = 1;
        e->fileSize = h->sizes[h->next];
        e->attributes = h->is_subdir[h->next] ? FS_ATTRIBUTE_DIRECTORY : 0;

        ++h->next;
        ++produced;
    }

    *entriesRead = produced;
    return 0;
}

Result FSDIR_Close(Handle handle)
{
    stub_handle_t *h = handle_of(handle);

    if (h == NULL) {
        return stub_error(RS_INVALIDARG, RD_INVALID_ARGUMENT);
    }
    handle_free(h);
    return 0;
}

/* ------------------------------------------------------------------- other */

Result FSUSER_GetArchiveResource(FS_ArchiveResource *out, FS_SystemMediaType mediaType)
{
    (void)mediaType;
    out->sectorSize = 512;
    out->clusterSize = 32768;
    out->totalClusters = 1024;
    out->freeClusters = 512;
    return 0;
}

static int secure_slot(u32 unique_id)
{
    size_t i;

    for (i = 0; i < STUB_MAX_TITLES; ++i) {
        if (g_titles[i].used && (u32)((g_titles[i].id >> 8) & 0xffffffu) == unique_id) {
            return (int)i;
        }
    }
    return -1;
}

Result FSUSER_GetSaveDataSecureValue(bool *exists, u64 *value, FS_SecureValueSlot slot,
                                     u32 titleUniqueId, u8 titleVariation)
{
    int i = secure_slot(titleUniqueId);

    (void)slot;
    (void)titleVariation;
    if (i < 0) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }
    *exists = g_secure_present[i] ? true : false;
    *value = g_secure_values[i];
    return 0;
}

Result FSUSER_SetSaveDataSecureValue(u64 value, FS_SecureValueSlot slot, u32 titleUniqueId,
                                     u8 titleVariation)
{
    int i = secure_slot(titleUniqueId);

    (void)slot;
    (void)titleVariation;
    if (i < 0) {
        return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
    }
    g_secure_present[i] = 1;
    g_secure_values[i] = value;
    return 0;
}

Result AM_GetTitleCount(FS_MediaType mediatype, u32 *count)
{
    size_t i;
    u32 n = 0;

    (void)mediatype;
    for (i = 0; i < STUB_MAX_TITLES; ++i) {
        if (g_titles[i].used) {
            ++n;
        }
    }
    *count = n;
    return 0;
}

Result AM_GetTitleList(u32 *titlesRead, FS_MediaType mediatype, u32 titleCount, u64 *titleIds)
{
    size_t i;
    u32 n = 0;

    (void)mediatype;
    for (i = 0; i < STUB_MAX_TITLES && n < titleCount; ++i) {
        if (g_titles[i].used) {
            titleIds[n++] = g_titles[i].id;
        }
    }
    *titlesRead = n;
    return 0;
}

Result AM_GetTitleProductCode(FS_MediaType mediatype, u64 titleId, char *productCode)
{
    size_t i;

    (void)mediatype;
    for (i = 0; i < STUB_MAX_TITLES; ++i) {
        if (g_titles[i].used && g_titles[i].id == titleId) {
            memcpy(productCode, g_titles[i].product, sizeof(g_titles[i].product));
            return 0;
        }
    }
    return stub_error(RS_NOTFOUND, RD_NOT_FOUND);
}

/* ------------------------------------------------------------- conversions */

/* The libctru contract, including the part that matters: the buffer is filled up
 * to len and the return is what the input *would* have produced, so a caller that
 * does not compare the two gets a silently truncated string. */
ssize_t utf8_to_utf16(u16 *out, const u8 *in, size_t len)
{
    size_t produced = 0;
    size_t i = 0;

    while (in[i] != '\0') {
        u32 cp;
        size_t need;

        if (in[i] < 0x80) {
            cp = in[i];
            need = 1;
        } else if ((in[i] & 0xe0) == 0xc0) {
            cp = in[i] & 0x1fu;
            need = 2;
        } else if ((in[i] & 0xf0) == 0xe0) {
            cp = in[i] & 0x0fu;
            need = 3;
        } else if ((in[i] & 0xf8) == 0xf0) {
            cp = in[i] & 0x07u;
            need = 4;
        } else {
            return -1;
        }

        {
            size_t k;
            for (k = 1; k < need; ++k) {
                if ((in[i + k] & 0xc0) != 0x80) {
                    return -1;
                }
                cp = (cp << 6) | (u32)(in[i + k] & 0x3fu);
            }
        }
        i += need;

        if (cp >= 0x10000u) {
            u32 v = cp - 0x10000u;
            if (out != NULL && produced + 1 < len) {
                out[produced] = (u16)(0xd800u | (v >> 10));
                out[produced + 1] = (u16)(0xdc00u | (v & 0x3ffu));
            }
            produced += 2;
        } else {
            if (out != NULL && produced < len) {
                out[produced] = (u16)cp;
            }
            produced += 1;
        }
    }
    return (ssize_t)produced;
}

ssize_t utf16_to_utf8(u8 *out, const u16 *in, size_t len)
{
    size_t produced = 0;
    size_t i = 0;

    while (in[i] != 0) {
        u32 cp = in[i];
        size_t need;

        if (cp >= 0xd800u && cp <= 0xdbffu) {
            u32 lo = in[i + 1];
            if (lo < 0xdc00u || lo > 0xdfffu) {
                return -1;
            }
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
            i += 2;
        } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
            return -1;
        } else {
            i += 1;
        }

        need = cp < 0x80u ? 1 : cp < 0x800u ? 2 : cp < 0x10000u ? 3 : 4;
        if (out != NULL && produced + need <= len) {
            switch (need) {
            case 1:
                out[produced] = (u8)cp;
                break;
            case 2:
                out[produced] = (u8)(0xc0u | (cp >> 6));
                out[produced + 1] = (u8)(0x80u | (cp & 0x3fu));
                break;
            case 3:
                out[produced] = (u8)(0xe0u | (cp >> 12));
                out[produced + 1] = (u8)(0x80u | ((cp >> 6) & 0x3fu));
                out[produced + 2] = (u8)(0x80u | (cp & 0x3fu));
                break;
            default:
                out[produced] = (u8)(0xf0u | (cp >> 18));
                out[produced + 1] = (u8)(0x80u | ((cp >> 12) & 0x3fu));
                out[produced + 2] = (u8)(0x80u | ((cp >> 6) & 0x3fu));
                out[produced + 3] = (u8)(0x80u | (cp & 0x3fu));
                break;
            }
        }
        produced += need;
    }
    return (ssize_t)produced;
}
