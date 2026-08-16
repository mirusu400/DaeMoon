#!/bin/sh
# How much stack the main thread actually gets.
#
# Not what app.rsf says. `StackSize` in the RSF goes into the exheader and is not
# what the main thread ends up running on: libctru carries a weak __stacksize__
# whose default is 32 KiB, and that is the number that decides whether a call
# chain fits.
#
# It has already decided once. The first restore run on hardware died in a data
# abort writing just below the stack pointer, because one function wanted 50 KiB
# for a single frame - and the ceiling it hit was this default. The frame is gone
# and tools/stack-check.sh keeps frames small, but that only helps if the stack is
# the size this build thinks it is. Nothing at runtime reports it until it runs
# out, so it is read out of the binary here.
set -eu

ELF=${1:-platform/3ds/daemoon.elf}
MIN=${STACK_MIN:-$((64 * 1024))}
PREFIX=${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi

addr=$("$PREFIX-nm" "$ELF" | awk '$3 == "__stacksize__" { print $1 }')
if [ -z "$addr" ]; then
	echo "stack: __stacksize__ not found in $ELF" >&2
	exit 1
fi

end=$(printf '0x%x' $((0x$addr + 4)))
# objdump prints the four bytes little endian; put them back in order.
bytes=$("$PREFIX-objdump" -s -j .data --start-address="0x$addr" --stop-address="$end" "$ELF" |
	awk '/^ /{ print $2; exit }')
if [ ${#bytes} -ne 8 ]; then
	echo "stack: could not read __stacksize__ out of $ELF" >&2
	exit 1
fi
size=$((0x$(echo "$bytes" | cut -c7-8)$(echo "$bytes" | cut -c5-6)$(echo "$bytes" | cut -c3-4)$(echo "$bytes" | cut -c1-2)))

if [ "$size" -lt "$MIN" ]; then
	echo "stack: __stacksize__ is $size bytes, want at least $MIN" >&2
	echo "" >&2
	echo "libctru's default is 32 KiB. This build links curl and mbedtls, and a TLS" >&2
	echo "handshake alone is tens of kilobytes of stack. Set __stacksize__ in main.c." >&2
	exit 1
fi

echo "stack: __stacksize__ = $size bytes"
