#!/bin/sh
# Pairing a console with a server, in an emulator.
#
# The QR path has one step a desktop cannot stand in for: whether a 3DS camera can
# read a code off a monitor. Everything either side of it is ordinary code - the
# payload parser, the network backend, the pairing call, the token write - and
# running that on hardware to find out it works is a trip to a console for nothing.
#
# So the emulator does the whole flow with the camera replaced by a file holding
# exactly what a scan would have produced. What is left for hardware is optics.
#
# This starts a real daemoond, sets it up through the real web panel, and asks it
# for a real pairing code. Nothing here is a stub: if the console comes back with a
# token, the server issued it.
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

# The emulator reaches the host as an ordinary process would, so a loopback
# address is what a console on the same machine sees.
port="${DAEMOON_PORT:-18080}"
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

# Exactly what the camera would have read. The payload is built here rather than
# scraped so a change to the format shows up as a parse failure on the console
# rather than as a silently skipped test.
payload="DAEMOON|1|$base|$code"

echo "installing"
xvfb-run -a "$azahar" --install "$root/platform/3ds/daemoon.cia" >/dev/null 2>&1

mkdir -p "$sd/DaeMoon"
printf '%s' "$payload" > "$sd/DaeMoon/AUTOPAIR"
# The other unattended mode is checked first, and a flag left behind by an earlier
# run silently turns this into that one. Found exactly that way.
rm -f "$sd/DaeMoon/AUTOTEST" "$sd/DaeMoon/autopair.txt" "$sd/DaeMoon/config.txt"

app=$(ls -t "$sd/Nintendo 3DS/"*/*/title/00040000/0dae0000/content/*.app 2>/dev/null | head -1)
if [ -z "$app" ]; then
	echo "the title did not install where it was expected"
	exit 1
fi

echo "pairing"
timeout 120 xvfb-run -a "$azahar" "$app" >/dev/null 2>&1 || true
rm -f "$sd/DaeMoon/AUTOPAIR"

result="$sd/DaeMoon/autopair.txt"
if [ ! -f "$result" ]; then
	echo "no result was written. The app did not get as far as the pairing call."
	exit 1
fi
cat "$result"

if ! grep -q 'pair=ok' "$result"; then
	echo
	echo "the console did not get a token."
	echo "net=network_error means the emulator has no route to the host, which is"
	echo "an emulator setting rather than a finding about this code."
	exit 1
fi
if ! grep -q 'save=ok' "$result"; then
	echo
	echo "a token was issued and not written to the card, which loses it at power off."
	exit 1
fi

# And the server agrees: a device exists, under the name the console gave.
if ! curl -sS -b "$work/cj" "$base/devices" | grep -q '3DS'; then
	echo
	echo "the server issued a token and has no device to show for it"
	exit 1
fi

echo
echo "the console paired itself against a real server, as an ARM binary"
echo "what is left for hardware is whether the camera can read a screen"
