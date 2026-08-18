#!/bin/sh
# Phase 5 in an emulator: a console that boots, syncs, and gets out of the way.
#
# What hardware has to answer about this is narrow and specific: whether Luma's
# autoboot lands on this title, and whether exiting it puts somebody at the HOME
# menu. Everything else is ordinary code - the grace period, the network probe, the
# run over both libraries with a deferring policy, the report - and running that on a
# console to find out it works is a trip for nothing.
#
# So the emulator runs the whole thing as an ARM binary against a real daemoond. What
# is left for hardware is the two things about booting.
#
# Nothing here is a stub. If the report says a save went up, the server has it.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

azahar="${AZAHAR:-}"
if [ -z "$azahar" ]; then
	for candidate in \
		/tmp/squashfs-root/usr/bin/azahar \
		"$HOME/Applications/azahar" \
		"$(command -v azahar 2>/dev/null || true)"
	do
		if [ -n "$candidate" ] && [ -x "$candidate" ]; then
			azahar="$candidate"
			break
		fi
	done
fi
if [ -z "$azahar" ] || [ ! -x "$azahar" ]; then
	echo "no emulator found. Set AZAHAR=/path/to/azahar."
	echo "Skipping: this is a convenience, not a gate."
	exit 0
fi

sd="${AZAHAR_SDMC:-$HOME/.local/share/azahar-emu/sdmc}"
work=$(mktemp -d)
trap 'rm -rf "$work"; [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null || true' EXIT

port="${DAEMOON_PORT:-18081}"
base="http://127.0.0.1:$port"

echo "building"
make -C "$root" server >/dev/null
make -C "$root" docker-cia >/dev/null

echo "starting a server"
DAEMOON_DB="$work/daemoon.db" DAEMOON_ADDR="127.0.0.1:$port" \
	"$root/build/daemoond" >"$work/server.log" 2>&1 &
server_pid=$!

i=0
while ! curl -sS -o /dev/null "$base/healthz" 2>/dev/null; do
	i=$((i + 1))
	if [ "$i" -gt 100 ]; then
		echo "the server never listened"
		cat "$work/server.log"
		exit 1
	fi
	sleep 0.1
done

echo "setting up an account and asking for a pairing code"
curl -sS -c "$work/cj" -o /dev/null -X POST "$base/setup" \
	-d "username=emu&password=hunter2hunter2"
curl -sS -b "$work/cj" -o "$work/pair.html" -X POST "$base/pair" -d "platform=3ds"
code=$(sed -n 's/.*<p class="code">\([0-9]\{6\}\)<\/p>.*/\1/p' "$work/pair.html")
if [ -z "$code" ]; then
	echo "the panel did not produce a pairing code"
	exit 1
fi

echo "installing"
xvfb-run -a "$azahar" --install "$root/platform/3ds/daemoon.cia" >/dev/null 2>&1

app=$(ls -t "$sd/Nintendo 3DS/"*/*/title/00040000/0dae0000/content/*.app 2>/dev/null | head -1)
if [ -z "$app" ]; then
	echo "the title did not install where it was expected"
	exit 1
fi

mkdir -p "$sd/DaeMoon"
rm -f "$sd/DaeMoon/AUTOTEST" "$sd/DaeMoon/autopair.txt" \
      "$sd/DaeMoon/autosync.txt" "$sd/DaeMoon/config.txt"
rm -rf "$sd/DaeMoon/state"

# A DS save is the thing to sync: one plain file, no archive, no permissions. If the
# sync path mishandles it, it is the sync path.
mkdir -p "$sd/roms/nds/saves"
printf 'a save from last night' > "$sd/roms/nds/saves/DAEMOON-AUTOSYNC.sav"

# Pair first, through the same unattended path emu-pair uses. A sync at startup needs
# a token, and getting one is Phase 4's problem rather than this script's.
echo "pairing"
printf '%s' "DAEMOON|1|$base|$code" > "$sd/DaeMoon/AUTOPAIR"
timeout 120 xvfb-run -a "$azahar" "$app" >/dev/null 2>&1 || true
rm -f "$sd/DaeMoon/AUTOPAIR"
if ! grep -q 'pair=ok' "$sd/DaeMoon/autopair.txt" 2>/dev/null; then
	echo "the console did not pair, so there is nothing to sync with."
	echo "net=network_error means the emulator has no route to the host, which is an"
	echo "emulator setting rather than a finding about this code."
	cat "$sd/DaeMoon/autopair.txt" 2>/dev/null || true
	exit 1
fi

# Two settings, written the way a person would: the toggle in Settings, and the
# welcome having been through once. A console booting into a sync must not stop at an
# explanation, and the code skips it - but leaving this unset here would test that
# rather than the sync.
printf 'autosync = 1\nwelcomed = 1\n' >> "$sd/DaeMoon/config.txt"

echo "booting"
# No input is sent, so the grace period expires on its own - which is the case that
# matters: this has to run when nobody touches the console.
timeout 240 xvfb-run -a "$azahar" "$app" >/dev/null 2>&1 || true

result="$sd/DaeMoon/autosync.txt"
if [ ! -f "$result" ]; then
	echo "no report was written."
	echo "The run never reached the point where it writes one, which is either the"
	echo "toggle not being read or the grace period not ending."
	sed -n 's/^/trace: /p' "$sd/DaeMoon/trace.txt" 2>/dev/null | tail -20
	exit 1
fi
cat "$result"
echo

if ! grep -q 'network=ok' "$result"; then
	echo "the server was never reached. network_error here is an emulator route"
	echo "rather than a finding about this code."
	exit 1
fi
if ! grep -q 'uploaded=[1-9]' "$result"; then
	echo "nothing was uploaded, so a sync at startup did not sync anything."
	exit 1
fi

# And the server agrees. The report is the console's account; this is the other side.
if ! curl -sS -b "$work/cj" "$base/" | grep -q 'DAEMOON-AUTOSYNC'; then
	echo "the report says a save went up and the server has no title to show for it"
	exit 1
fi

# Nothing was deferred here, because only one side ever changed. A conflict left
# alone is the case core's tests cover, and it is the case this script cannot stage
# without a second device.
echo "a console booted, synced against a real server, and wrote down what it did"
echo "what is left for hardware: Luma autobooting this title, and exiting to HOME"
