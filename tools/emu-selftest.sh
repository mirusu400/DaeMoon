#!/bin/sh
# Runs the backend conformance suite inside an emulator, unattended.
#
# What this is worth: the code runs as a real ARM binary, through real libctru,
# against a real save archive, with the real CIA permissions applied. That is a
# great deal more than the desktop stub can say, and it has already caught a bug
# the stub could not - a result code for "directory already exists" that differs
# between implementations, which failed every write into an existing directory.
#
# What it is not: proof that the app works on hardware. Emulators do not reproduce
# real save archive behaviour, which is why docs/phase1-hardware.md exists and why
# the roadmap says Phase 1 is not done until a console has run it. Treat a pass
# here as "worth taking to hardware", not as "finished".
#
# The target is this application's own save archive and never another title's.
#
#   AZAHAR=/path/to/azahar tools/emu-selftest.sh
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

azahar="${AZAHAR:-}"
if [ -z "$azahar" ]; then
	for candidate in \
		/tmp/squashfs-root/usr/bin/azahar \
		"$HOME/Applications/azahar" \
		"$(command -v azahar 2>/dev/null || true)" \
		"$(command -v citra-qt 2>/dev/null || true)"
	do
		if [ -n "$candidate" ] && [ -x "$candidate" ]; then
			azahar="$candidate"
			break
		fi
	done
fi

if [ -z "$azahar" ] || [ ! -x "$azahar" ]; then
	echo "no emulator found. Set AZAHAR to one, or skip this: it is a"
	echo "convenience ahead of hardware, not a gate."
	exit 0
fi

sd="${AZAHAR_SDMC:-$HOME/.local/share/azahar-emu/sdmc}"
cia="$root/platform/3ds/daemoon.cia"

# The shipped app declares no save archive of its own. The self test needs one it
# owns, because the only archive it may destroy unattended is its own.
echo "building a test CIA with its own save archive"
make -C "$root" docker-cia SAVEDATA_SIZE=128K >/dev/null

echo "installing"
xvfb-run -a "$azahar" --install "$cia" >/dev/null 2>&1

mkdir -p "$sd/DaeMoon"
: > "$sd/DaeMoon/AUTOTEST"
rm -f "$sd/DaeMoon/selftest.txt"

app=$(ls -t "$sd/Nintendo 3DS/"*/*/title/00040000/0dae0000/content/*.app 2>/dev/null | head -1)
if [ -z "$app" ]; then
	echo "the title did not install where it was expected"
	exit 1
fi

echo "running"
# The app exits by itself after writing the result; the timeout is for the case
# where it does not, which is itself a finding.
timeout 120 xvfb-run -a "$azahar" "$app" >/dev/null 2>&1 || true

result="$sd/DaeMoon/selftest.txt"
if [ ! -f "$result" ]; then
	echo "no result was written. The app did not get far enough to run the suite."
	echo "Look at the emulator log for what stopped it."
	exit 1
fi

cat "$result"

if grep -q 'failures=0' "$result"; then
	echo
	echo "the backend conforms, running as a real binary against a real archive"
	echo "hardware is still the thing that decides. See docs/phase1-hardware.md."
	exit 0
fi

echo
echo "the backend does not conform. Do not take this build to hardware."
exit 1
