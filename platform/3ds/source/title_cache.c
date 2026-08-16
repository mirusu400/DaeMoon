/* What an SMDH costs, remembered on the SD card.
 *
 * A title's name and its icon come out of the same SMDH, and reading one means
 * opening the title's content and decrypting the front of it. That is the single
 * most expensive thing this application does, it is done once per title, and the
 * answer does not change until the title is updated or deleted.
 *
 * Before this file it was done twice per title - once by list_titles for the name
 * and again by the icon loader - and then again on every launch. A console with a
 * full library spent the whole loading screen re-deriving something it had already
 * derived.
 *
 * Two things are worth being deliberate about:
 *
 * A failed lookup is cached too. On a real console several titles have no readable
 * SMDH at all, and those are exactly the ones that cost the most to find out about,
 * because the failure comes after the open. Not caching the negative would leave
 * the slowest titles paying full price forever.
 *
 * Which means a bug in the lookup gets cached as well - and one has already
 * happened here, the read that had to start at offset zero. So the file carries a
 * format number, a build that changes the lookup bumps it, and the whole cache is
 * discarded rather than migrated. The Survey action ignores this file entirely and
 * always asks the hardware, because a diagnostic that reads yesterday's answer is
 * not a diagnostic.
 */
#include "daemoon_3ds.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bump when anything about the lookup changes: which slot a name is taken from,
 * how the icon is extracted, what counts as a failure. Old files are dropped, not
 * migrated - the data is a cache and re-deriving it costs one slow launch. */
#define CACHE_FORMAT 1

#define CACHE_MAGIC0 'D'
#define CACHE_MAGIC1 'M'
#define CACHE_MAGIC2 'T'
#define CACHE_MAGIC3 'C'

#define CACHE_MAX_ENTRIES 256

#define ENTRY_HAS_NAME  0x01u
#define ENTRY_HAS_ASCII 0x02u
#define ENTRY_HAS_ICON  0x04u
/* The lookup ran and came back with nothing. Kept apart from an empty record so a
 * miss and a known-absent SMDH are not the same thing. */
#define ENTRY_PROBED    0x08u

#define CACHE_NAME_MAX 128

/* One record, fixed size, written to the file exactly as it is held in memory.
 *
 * Fixed rather than packed: the file is read and written whole by the same build
 * that wrote it, a format mismatch throws it away, and a variable length record
 * would buy a few hundred kilobytes of SD card at the cost of the only property
 * that matters here, which is that a truncated file is obviously truncated.
 */
typedef struct {
    unsigned long long title_id;
    unsigned int       flags;
    /* The name as the title actually carries it, in whatever script that is. */
    char               name[CACHE_NAME_MAX];
    /* The best name the console's fallback renderer can draw, when the two differ.
     * Held separately so the cache does not depend on whether a font was found
     * this launch - that is a property of the console, not of the title. */
    char               ascii[CACHE_NAME_MAX];
    unsigned char      icon[DAEMOON_3DS_ICON_BYTES];
} cache_entry_t;

struct daemoon_3ds_title_cache {
    cache_entry_t *entries;
    size_t         count;
    /* Which entries were asked for since the last flush. Anything else is a title
     * that is no longer installed, and writing it back would grow the file
     * forever. */
    unsigned char *live;
    int            lang;
    int            dirty;
    unsigned       hits;
    unsigned       misses;
};

typedef struct {
    unsigned char magic[4];
    unsigned int  format;
    unsigned int  lang;
    unsigned int  count;
} cache_header_t;

/* ------------------------------------------------------------------- loading */

static void discard(daemoon_3ds_title_cache_t *c)
{
    c->count = 0;
    c->dirty = 1;
}

static void load_file(daemoon_3ds_title_cache_t *c, const char *path)
{
    cache_header_t header;
    FILE *fp = fopen(path, "rb");

    if (fp == NULL) {
        return;
    }
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        (void)fclose(fp);
        return;
    }
    if (header.magic[0] != CACHE_MAGIC0 || header.magic[1] != CACHE_MAGIC1 ||
        header.magic[2] != CACHE_MAGIC2 || header.magic[3] != CACHE_MAGIC3 ||
        header.format != CACHE_FORMAT) {
        /* A file this build does not understand is a file from a build whose
         * lookup was different. Throwing it away costs one slow launch; keeping it
         * would carry that build's bugs into this one. */
        (void)fclose(fp);
        return;
    }
    if (header.lang != (unsigned int)c->lang) {
        /* Names are chosen by a fallback chain that starts at the console's own
         * language, so a console whose language changed has a cache of names for
         * the language it used to be in. */
        (void)fclose(fp);
        return;
    }
    if (header.count > CACHE_MAX_ENTRIES) {
        (void)fclose(fp);
        return;
    }

    c->count = fread(c->entries, sizeof(c->entries[0]), header.count, fp);
    (void)fclose(fp);

    /* A short read is a truncated file - a card pulled during a write, most
     * likely. The records that did arrive are whole, because they are fixed size,
     * so they are kept and the rest are simply misses. */
    if (c->count != header.count) {
        c->dirty = 1;
    }
}

daemoon_result_t daemoon_3ds_cache_open(const char *path, int lang,
                                        daemoon_3ds_title_cache_t **out)
{
    daemoon_3ds_title_cache_t *c;

    *out = NULL;

    c = (daemoon_3ds_title_cache_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    /* One allocation for the whole table rather than growing it. The heap here
     * fragments badly and this lives for the entire run; a realloc of half a
     * megabyte partway through a loading screen is the wrong thing to do to it. */
    c->entries = (cache_entry_t *)calloc(CACHE_MAX_ENTRIES, sizeof(*c->entries));
    c->live = (unsigned char *)calloc(CACHE_MAX_ENTRIES, 1);
    if (c->entries == NULL || c->live == NULL) {
        free(c->entries);
        free(c->live);
        free(c);
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    c->lang = lang;

    if (path != NULL) {
        load_file(c, path);
    }

    *out = c;
    return DAEMOON_OK;
}

void daemoon_3ds_cache_close(daemoon_3ds_title_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->entries);
    free(c->live);
    free(c);
}

/* -------------------------------------------------------------------- lookup */

static int is_ascii_name(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    for (; *p != '\0'; ++p) {
        if (*p < 0x20 || *p > 0x7e) {
            return 0;
        }
    }
    return 1;
}

static cache_entry_t *find(daemoon_3ds_title_cache_t *c, unsigned long long tid)
{
    size_t i;

    for (i = 0; i < c->count; ++i) {
        if (c->entries[i].title_id == tid) {
            c->live[i] = 1;
            return &c->entries[i];
        }
    }
    return NULL;
}

/* Reads the SMDH once and fills a record from it. Both names and the icon come out
 * of the same read, which is the whole point: this is the expensive call. */
static cache_entry_t *probe(daemoon_3ds_title_cache_t *c, int media,
                            unsigned long long tid)
{
    /* Fourteen kilobytes, once per title, and not on a stack the UI also uses. */
    static unsigned char smdh[DAEMOON_3DS_SMDH_SIZE];
    cache_entry_t *e;
    size_t slot;

    if (c->count >= CACHE_MAX_ENTRIES) {
        /* Past the table the lookups still work, they just are not remembered.
         * Refusing to answer would be worse than being slow. */
        return NULL;
    }
    slot = c->count++;
    e = &c->entries[slot];
    memset(e, 0, sizeof(*e));
    e->title_id = tid;
    e->flags = ENTRY_PROBED;
    c->live[slot] = 1;
    c->dirty = 1;
    ++c->misses;

    if (daemoon_3ds_smdh_load(media, tid, smdh) != DAEMOON_OK) {
        return e;
    }

    if (daemoon_3ds_smdh_name(smdh, c->lang, 0, e->name, sizeof(e->name)) ==
        DAEMOON_OK) {
        e->flags |= ENTRY_HAS_NAME;
    }
    /* Asked for separately rather than filtered from the one above: with the ASCII
     * constraint the fallback chain walks past the slot it would otherwise stop
     * at, so a title with a Korean name and an English one further down gives two
     * different answers, and both are worth having. */
    if (daemoon_3ds_smdh_name(smdh, c->lang, DAEMOON_3DS_NAME_ASCII, e->ascii,
                              sizeof(e->ascii)) == DAEMOON_OK) {
        e->flags |= ENTRY_HAS_ASCII;
    }

    memcpy(e->icon, smdh + DAEMOON_3DS_SMDH_ICON_OFF, sizeof(e->icon));
    e->flags |= ENTRY_HAS_ICON;
    return e;
}

static cache_entry_t *entry_for(daemoon_3ds_title_cache_t *c, int media,
                                unsigned long long tid)
{
    cache_entry_t *e = find(c, tid);

    if (e != NULL) {
        ++c->hits;
        return e;
    }
    return probe(c, media, tid);
}

daemoon_result_t daemoon_3ds_cache_name(daemoon_3ds_title_cache_t *c, int media,
                                        unsigned long long tid, unsigned flags,
                                        char *out, size_t cap)
{
    cache_entry_t *e;

    out[0] = '\0';
    if (c == NULL) {
        /* Callers that run without a cache - the conformance build, the survey -
         * go to daemoon_3ds_title_name directly, because only they know which
         * language to ask for. Guessing one here would put a name on screen in a
         * language nobody selected. */
        return DAEMOON_ERR_INVALID_REQUEST;
    }

    e = entry_for(c, media, tid);
    if (e == NULL) {
        /* Past the table: still answer, just do not remember it. */
        return daemoon_3ds_title_name(media, tid, c->lang, flags, out, cap);
    }

    if (flags & DAEMOON_3DS_NAME_ASCII) {
        if (e->flags & ENTRY_HAS_ASCII) {
            return daemoon_strlcpy(out, cap, e->ascii);
        }
        /* The unrestricted name might happen to be drawable anyway - a Japanese
         * console's copy of an English game, say. */
        if ((e->flags & ENTRY_HAS_NAME) && is_ascii_name(e->name)) {
            return daemoon_strlcpy(out, cap, e->name);
        }
        return DAEMOON_ERR_UNSUPPORTED;
    }

    if (e->flags & ENTRY_HAS_NAME) {
        return daemoon_strlcpy(out, cap, e->name);
    }
    return DAEMOON_ERR_NOT_FOUND;
}

const void *daemoon_3ds_cache_icon(daemoon_3ds_title_cache_t *c, int media,
                                   unsigned long long tid)
{
    cache_entry_t *e;

    if (c == NULL) {
        return NULL;
    }
    e = entry_for(c, media, tid);
    if (e == NULL || !(e->flags & ENTRY_HAS_ICON)) {
        return NULL;
    }
    return e->icon;
}

void daemoon_3ds_cache_forget(daemoon_3ds_title_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    discard(c);
    memset(c->live, 0, CACHE_MAX_ENTRIES);
}

unsigned daemoon_3ds_cache_hits(const daemoon_3ds_title_cache_t *c)
{
    return c == NULL ? 0u : c->hits;
}

unsigned daemoon_3ds_cache_misses(const daemoon_3ds_title_cache_t *c)
{
    return c == NULL ? 0u : c->misses;
}

/* ------------------------------------------------------------------- writing */

daemoon_result_t daemoon_3ds_cache_flush(daemoon_3ds_title_cache_t *c, const char *path)
{
    char temp[DAEMOON_PATH_MAX];
    cache_header_t header;
    daemoon_strbuf_t sb;
    FILE *fp;
    size_t i;
    size_t written = 0;
    unsigned int live_count = 0;

    if (c == NULL) {
        return DAEMOON_OK;
    }

    /* Titles that were not asked about are titles that are no longer installed.
     * Dropping them here is the only pruning this file gets, and without it a
     * console that has had a hundred games deleted still carries them. */
    for (i = 0; i < c->count; ++i) {
        if (c->live[i]) {
            ++live_count;
        }
    }

    if (c->count > 0 && live_count == 0) {
        /* Nothing was asked about at all. On a console that means the title list
         * could not be read, not that every game was uninstalled between two
         * launches, and throwing away a good cache on that evidence would make a
         * transient AM failure cost the next launch as well. */
        return DAEMOON_OK;
    }
    if (!c->dirty && live_count == c->count) {
        return DAEMOON_OK;
    }

    daemoon_strbuf_init(&sb, temp, sizeof(temp));
    daemoon_strbuf_add(&sb, path);
    daemoon_strbuf_add(&sb, ".tmp");
    DAEMOON_TRY(daemoon_strbuf_result(&sb));

    fp = fopen(temp, "wb");
    if (fp == NULL) {
        return DAEMOON_ERR_IO_ERROR;
    }

    header.magic[0] = CACHE_MAGIC0;
    header.magic[1] = CACHE_MAGIC1;
    header.magic[2] = CACHE_MAGIC2;
    header.magic[3] = CACHE_MAGIC3;
    header.format = CACHE_FORMAT;
    header.lang = (unsigned int)c->lang;
    header.count = live_count;

    if (fwrite(&header, sizeof(header), 1, fp) != 1) {
        (void)fclose(fp);
        (void)remove(temp);
        return DAEMOON_ERR_IO_ERROR;
    }
    for (i = 0; i < c->count; ++i) {
        if (!c->live[i]) {
            continue;
        }
        if (fwrite(&c->entries[i], sizeof(c->entries[i]), 1, fp) != 1) {
            (void)fclose(fp);
            (void)remove(temp);
            return DAEMOON_ERR_IO_ERROR;
        }
        ++written;
    }
    if (fclose(fp) != 0 || written != live_count) {
        (void)remove(temp);
        return DAEMOON_ERR_IO_ERROR;
    }

    /* Write then swap, the same rule the save path follows. A cache is not save
     * data and losing one costs nothing, but a half written file is read by the
     * next launch and there is no reason to make that possible. */
    (void)remove(path);
    if (rename(temp, path) != 0) {
        (void)remove(temp);
        return DAEMOON_ERR_IO_ERROR;
    }

    c->dirty = 0;
    return DAEMOON_OK;
}
