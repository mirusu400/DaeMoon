#!/bin/sh
# Reads a built CIA back and checks that the rights app.rsf asks for are actually
# in it.
#
# A .3dsx cannot reach another title's save archive; the exheader is what makes
# the CIA different, and until now the only way to find out whether makerom put
# the right bits in it was to install it on a console. This turns that into a
# build failure.
#
# It cannot tell you whether the *set* is sufficient. That is a hardware question
# and docs/phase1-hardware.md is how it gets answered.
set -eu

cia="${1:?usage: verify-cia.sh <file.cia>}"
info=$(ctrtool --info "$cia" 2>/dev/null)

fail=0

# Whole lines only. A substring match let "cam:u" satisfy a requirement for
# "am:u", which hid the fact that am:u was missing and would have turned into an
# empty title list on hardware that looked like a code bug.
require_line() {
	if printf '%s' "$info" | sed 's/[[:space:]]*$//' | grep -qxF "$1"; then
		printf '  ok       %s\n' "$1"
	else
		printf '  MISSING  %s\n' "$1"
		fail=1
	fi
}

require_service() { require_line " > $1"; }

echo "filesystem rights"
# Reaching another title's save archive, and the SD card for backups.
require_line ' > Category System Application'
require_line ' > Direct SDMC'
require_line ' > Direct SDMC (Write Only)'

echo "services"
require_service 'fs:USER'    # the save archives
require_service 'am:u'       # title enumeration; without it the list is empty
require_service 'cfg:u'      # the system language, for the default UI language
require_service 'hid:USER'   # the menu
require_service 'ssl:C'      # Phase 2 onwards
require_service 'soc:U'
require_service 'cam:u'      # Phase 4 QR pairing

echo "kernel"
# Spacing inside the kernel flags block is ctrtool's, so this one is matched as a
# pattern rather than a literal line.
if printf '%s' "$info" | grep -qE '^ > Memory Type: +APPLICATION$'; then
	printf '  ok        > Memory Type: APPLICATION\n'
else
	printf '  MISSING   > Memory Type: APPLICATION\n'
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo
	echo "The CIA does not carry what app.rsf asks for."
	echo "Installing it would fail on hardware in a way that looks like a code bug."
	exit 1
fi

echo
echo "the CIA carries the rights app.rsf asks for"
