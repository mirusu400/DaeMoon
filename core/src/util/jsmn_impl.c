/* The one translation unit that compiles jsmn. Every other file includes it with
 * JSMN_HEADER defined and gets declarations only.
 *
 * jsmn is token based and allocates nothing, which is why it is here instead of
 * cJSON: the 3DS heap has no room for a DOM built out of a save manifest, and no
 * room for the fragmentation that would come with one.
 *
 * JSMN_PARENT_LINKS makes each token record its parent, which turns "find this key
 * in this object" into a linear scan instead of a recursive walk. */
#define JSMN_PARENT_LINKS
#include <jsmn/jsmn.h>
