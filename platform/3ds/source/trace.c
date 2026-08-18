/* Breadcrumbs, for the failures a console will not describe.
 *
 * When this application dies on hardware there are three ways to find out why,
 * and two of them are bad. Reading it off the screen needs the screen to still be
 * there. A Luma exception dump needs the fault to be one Luma catches, and needs
 * the addresses to be matched back to a build - which works, and cost a round trip
 * to the console every time it was wrong about which build was installed.
 *
 * The third is to write down where the code got to. That is what this is: one line
 * per step, opened and closed around each write so the line is on the card before
 * the next thing has a chance to fail. It is the same lesson the SMDH lookup
 * taught - one variable carrying two steps could not say which of them failed, and
 * four trips to a console went into the wrong half of the problem.
 *
 * Cheap enough to leave on: a few writes per user action, never per frame.
 */
#include "daemoon_3ds.h"

#include <stdio.h>

void daemoon_3ds_trace(const char *step, const char *detail)
{
    FILE *fp = fopen(DAEMOON_3DS_TRACE_PATH, "ab");

    if (fp == NULL) {
        return;
    }
    (void)fputs(step, fp);
    if (detail != NULL) {
        (void)fputc('\t', fp);
        (void)fputs(detail, fp);
    }
    (void)fputc('\n', fp);
    /* Closed rather than flushed: a flushed FILE still has the SD write ahead of
     * it, and the whole point is that the line survives whatever happens next. */
    (void)fclose(fp);
}

void daemoon_3ds_trace_uint(const char *step, unsigned long long value)
{
    char text[24];

    (void)snprintf(text, sizeof(text), "%lu", (unsigned long)value);
    daemoon_3ds_trace(step, text);
}

/* The shared code has things worth recording and no business knowing where a 3DS
 * keeps them. platform/common calls this name; this file decides the file. */
void daemoon_newlib_trace(const char *step, const char *detail)
{
    daemoon_3ds_trace(step, detail);
}
