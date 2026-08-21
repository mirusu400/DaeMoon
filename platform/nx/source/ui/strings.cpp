/* The string tables, as std::string.
 *
 * Thin on purpose. The rule is that a user visible sentence exists only in
 * shared/lang, reaches the code as a daemoon_str_id_t and is resolved here;
 * everything below is the resolution, not a place to add text. There is deliberately
 * no overload taking a const char *, because one would be all it takes for a literal
 * to end up on screen in English on a Korean console.
 */
#include "ui/dm_ui.hpp"

#include <cstring>

namespace dm
{

std::string str(daemoon_str_id_t id)
{
    /* Never NULL: a missing entry falls back to English and a missing English entry
     * falls back to the key name, so a gap in a translation is visible rather than a
     * crash. */
    return std::string(daemoon_str(id));
}

std::string strf(const daemoon_str_ref_t* ref)
{
    char buf[512];

    if (ref == nullptr)
        return std::string();

    /* Truncation is reported and ignored here: this is text on a screen, and half a
     * sentence is more use than none. daemoon_strf cuts on a UTF-8 boundary, so the
     * half that survives is still text. */
    (void)daemoon_strf(buf, sizeof(buf), ref->id, ref->args, ref->nargs);
    return std::string(buf);
}

std::string strf(daemoon_str_id_t id, const std::vector<std::string>& args)
{
    const char* pointers[DAEMOON_STR_MAX_ARGS];
    size_t n = args.size();
    char buf[512];

    if (n > DAEMOON_STR_MAX_ARGS)
        n = DAEMOON_STR_MAX_ARGS;
    for (size_t i = 0; i < n; ++i)
        pointers[i] = args[i].c_str();

    (void)daemoon_strf(buf, sizeof(buf), id, pointers, n);
    return std::string(buf);
}

} // namespace dm
