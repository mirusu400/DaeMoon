#include "fixture_json.h"

#include "../../core/src/util/json.h"

#include <string.h>

#define FIXTURE_MAX_TOKENS 512

/* The fixture is small and read a handful of times, so it is reparsed per call
 * rather than cached. Simpler is worth more than fast in a test helper. */
static int load(const char *json, size_t len, jsmntok_t *toks, int *ntok, int *cases_arr)
{
    if (daemoon_json_parse(json, len, toks, FIXTURE_MAX_TOKENS, ntok) != DAEMOON_OK) {
        return -1;
    }
    *cases_arr = daemoon_json_find(json, toks, *ntok, 0, "cases");
    if (*cases_arr < 0 || toks[*cases_arr].type != JSMN_ARRAY) {
        return -1;
    }
    return 0;
}

/* Index of the nth direct child of an array token. */
static int nth_child(const jsmntok_t *toks, int ntok, int parent, size_t n)
{
    int i;
    size_t seen = 0;

    for (i = parent + 1; i < ntok; ++i) {
        if (toks[i].parent != parent) {
            continue;
        }
        if (seen == n) {
            return i;
        }
        ++seen;
    }
    return -1;
}

static size_t count_children(const jsmntok_t *toks, int ntok, int parent)
{
    int i;
    size_t n = 0;

    for (i = parent + 1; i < ntok; ++i) {
        if (toks[i].parent == parent) {
            ++n;
        }
    }
    return n;
}

size_t fixture_digest_case_count(const char *json, size_t len)
{
    jsmntok_t toks[FIXTURE_MAX_TOKENS];
    int ntok = 0;
    int cases_arr = -1;

    if (load(json, len, toks, &ntok, &cases_arr) != 0) {
        return 0;
    }
    return count_children(toks, ntok, cases_arr);
}

int fixture_digest_case(const char *json, size_t len, size_t i, char *name, size_t name_cap,
                        char *sha256, unsigned long long *out_size, size_t *out_nentries)
{
    jsmntok_t toks[FIXTURE_MAX_TOKENS];
    int ntok = 0;
    int cases_arr = -1;
    int obj;
    int entries;

    if (load(json, len, toks, &ntok, &cases_arr) != 0) {
        return -1;
    }
    obj = nth_child(toks, ntok, cases_arr, i);
    if (obj < 0) {
        return -1;
    }
    if (daemoon_json_get_str(json, toks, ntok, obj, "name", name, name_cap) != DAEMOON_OK) {
        return -1;
    }
    if (daemoon_json_get_str(json, toks, ntok, obj, "sha256", sha256, 65) != DAEMOON_OK) {
        return -1;
    }
    if (daemoon_json_get_uint(json, toks, ntok, obj, "size", out_size) != DAEMOON_OK) {
        return -1;
    }

    entries = daemoon_json_find(json, toks, ntok, obj, "entries");
    *out_nentries = (entries < 0) ? 0 : count_children(toks, ntok, entries);
    return 0;
}

int fixture_digest_entry(const char *json, size_t len, size_t i, size_t e, char *path,
                         size_t path_cap, char *content, size_t content_cap)
{
    jsmntok_t toks[FIXTURE_MAX_TOKENS];
    int ntok = 0;
    int cases_arr = -1;
    int obj;
    int entries;
    int entry;

    if (load(json, len, toks, &ntok, &cases_arr) != 0) {
        return -1;
    }
    obj = nth_child(toks, ntok, cases_arr, i);
    if (obj < 0) {
        return -1;
    }
    entries = daemoon_json_find(json, toks, ntok, obj, "entries");
    if (entries < 0) {
        return -1;
    }
    entry = nth_child(toks, ntok, entries, e);
    if (entry < 0) {
        return -1;
    }
    if (daemoon_json_get_str(json, toks, ntok, entry, "path", path, path_cap) != DAEMOON_OK) {
        return -1;
    }
    /* An empty string is a legitimate case: a zero length file still contributes
     * its path and its length to the digest. */
    if (daemoon_json_get_str(json, toks, ntok, entry, "content_utf8", content, content_cap) !=
        DAEMOON_OK) {
        content[0] = '\0';
    }
    return 0;
}
