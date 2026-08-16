#!/bin/sh
# How much stack each function in core/ and platform/3ds/ needs.
#
# A 3DS thread stack is measured in tens of kilobytes. A desktop one is eight
# megabytes, so a function that puts a large structure in a local runs fine under
# every test in this repository and takes a console down the first time it is
# reached.
#
# That is not hypothetical. daemoon_archive_verify and daemoon_archive_unpack each
# held a daemoon_archive_ctx_t - fifty kilobytes - in a local. Both are on the
# restore path, both passed every desktop test, and the first restore ever run on
# hardware died in the prologue of the function they call: a data abort writing
# just below the stack pointer, into a section that is not mapped at all.
#
# So the size is checked rather than remembered. Nothing here needs a console or a
# cross compiler: the structures are the same size on a desktop, which is the part
# that goes wrong.
set -eu

ROOT=${1:-.}
LIMIT=${STACK_LIMIT:-8192}
CC=${CC:-cc}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Optimised, because that is how the console builds and -O0 inflates every frame
# past any threshold worth setting.
CFLAGS="-std=c11 -O2 -fstack-usage -w
        -I$ROOT/core/include -I$ROOT/vendor -I$ROOT/platform/3ds/source
        -I$ROOT/tools/test/ctru_stub -I$ROOT/tools/test -I$ROOT/platform/posix"

for f in "$ROOT"/core/src/*.c "$ROOT"/core/src/util/*.c "$ROOT"/platform/3ds/source/*.c; do
    case "$f" in
    # Needs citro2d, which is not here and draws nothing worth measuring.
    *"/gfx.c"|*"/icons.c"|*"/ui_backend.c"|*"/backups.c"|*"/main.c"|*"/net_backend.c")
        continue
        ;;
    esac
    # shellcheck disable=SC2086
    $CC $CFLAGS -c "$f" -o "$WORK/$(basename "$f").o" 2>/dev/null || continue
done

mv ./*.su "$WORK"/ 2>/dev/null || true
if ! ls "$WORK"/*.su >/dev/null 2>&1; then
    echo "stack-check: nothing was measured, which is not a pass" >&2
    exit 1
fi

over=$(awk -F'\t' -v limit="$LIMIT" '$2 > limit { print $2 "\t" $1 }' "$WORK"/*.su | sort -nr)

if [ -n "$over" ]; then
    echo "stack-check: over ${LIMIT} bytes of stack in one frame:" >&2
    echo "$over" >&2
    echo "" >&2
    echo "A console's stack does not have room for this. Take the buffer from the" >&2
    echo "caller, the way daemoon_archive_pack and daemoon_archive_hash_save do." >&2
    exit 1
fi

worst=$(awk -F'\t' '{ print $2 "\t" $1 }' "$WORK"/*.su | sort -nr | head -1)
printf 'stack-check: nothing over %s bytes. Worst: %s\n' "$LIMIT" "$worst"
