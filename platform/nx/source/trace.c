/* One line per step, on the card.
 *
 * Opened and closed per line on purpose: this file exists to be readable after a
 * crash, and a buffered handle is a file that says nothing about the moment that
 * mattered. The 3DS build's most useful diagnostic by a wide margin - four rounds of
 * hardware debugging became one - so the Switch build has it from the first day
 * rather than after the first mystery.
 */
#include "daemoon_nx.h"

#include <stdio.h>

void daemoon_nx_trace(const char *step, const char *detail)
{
    FILE *fp;

    if (step == NULL) {
        return;
    }
    fp = fopen(DAEMOON_NX_TRACE_PATH, "ab");
    if (fp == NULL) {
        return;
    }
    if (detail != NULL) {
        (void)fprintf(fp, "%s\t%s\n", step, detail);
    } else {
        (void)fprintf(fp, "%s\n", step);
    }
    (void)fclose(fp);
}

/* The shared code has things worth recording and no business knowing where a Switch
 * keeps them. platform/common calls this name; this file decides the file. */
void daemoon_newlib_trace(const char *step, const char *detail)
{
    daemoon_nx_trace(step, detail);
}
