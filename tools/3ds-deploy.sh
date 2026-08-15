#!/bin/sh
# Getting a build onto a 3DS without touching the SD card.
#
# Taking the card out, copying, putting it back and booting is a minute of
# fiddling per iteration, and the thing being iterated on writes to save data -
# so the temptation to skip a step is exactly the wrong temptation to have.
#
# Two channels, both over wifi:
#
#   install   FBI's "Remote Install -> Receive URLs over the network" (TCP 5000).
#             This host serves the CIA over HTTP for as long as it takes FBI to
#             fetch it, then stops.
#   push/pull ftpd's FTP server (TCP 5000). Used for the AUTOTEST flag and for
#             collecting results and backups afterwards.
#
# Both listeners use port 5000 and neither runs while the other does, which is
# fine because they are used at different moments.
#
# Usage:
#   tools/3ds-deploy.sh install  <ip>
#   tools/3ds-deploy.sh push-cia <ip>
#   tools/3ds-deploy.sh push     <ip> <local file> <remote path under sdmc>
#   tools/3ds-deploy.sh pull     <ip> <remote path under sdmc> [local file]
#   tools/3ds-deploy.sh selftest <ip>
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cmd="${1:-}"
ip="${2:-}"

usage() {
	sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

[ -n "$cmd" ] && [ -n "$ip" ] || usage

# The address the 3DS has to reach us on. Picking the interface that routes to the
# console beats guessing, and beats asking.
host_ip() {
	ip route get "$1" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1
}

ftp_url() {
	# ftpd serves the SD card at the FTP root.
	printf 'ftp://%s:5000/%s' "$ip" "${1#/}"
}

case "$cmd" in
install)
	cia="$root/platform/3ds/daemoon.cia"
	[ -f "$cia" ] || { echo "no CIA yet: run make docker-cia"; exit 1; }

	me=$(host_ip "$ip")
	[ -n "$me" ] || { echo "cannot work out which address $ip reaches me on"; exit 1; }

	# A port nobody else is on. Hardcoding one meant a leftover server from an
	# earlier run kept answering, serving a directory that no longer existed, and
	# the console got a 404 for a file this host was apparently serving.
	port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("", 0)); print(s.getsockname()[1]); s.close()')
	dir=$(mktemp -d)
	cp "$cia" "$dir/daemoon.cia"

	echo "serving $me:$port/daemoon.cia"
	# --directory rather than a subshell that cd's: this way $! is the server's own
	# pid, so the trap below actually stops it.
	python3 -m http.server "$port" --bind "$me" --directory "$dir" \
		>"$dir/http.log" 2>&1 &
	server=$!
	# Stopping the server is not optional: it is serving a file on the LAN.
	trap 'kill "$server" 2>/dev/null || true; rm -rf "$dir"' EXIT INT TERM
	sleep 1

	# If it did not come up, say so here rather than letting the console report a
	# 404 for a file this host is supposedly serving.
	if ! kill -0 "$server" 2>/dev/null; then
		echo "the local web server did not start:"
		sed 's/^/  /' "$dir/http.log"
		exit 1
	fi

	# If the console never fetches it, the log says whether it even asked.
	echo "telling FBI to fetch it"
	echo "  (on the 3DS: FBI -> Remote Install -> Receive URLs over the network)"
	if ! python3 - "$ip" "http://$me:$port/daemoon.cia" <<-'PY'
	import socket, struct, sys

	ip, url = sys.argv[1], sys.argv[2]
	payload = url.encode("ascii")

	try:
	    sock = socket.create_connection((ip, 5000), timeout=10)
	except ConnectionRefusedError:
	    # FBI leaves the listener after each install, so this is what a second
	    # install in a row looks like rather than anything being wrong.
	    print("nothing is listening on %s:5000." % ip)
	    print("On the console: FBI -> Remote Install -> Receive URLs over the network.")
	    print("FBI leaves that screen after every install, so it needs re-entering.")
	    raise SystemExit(1)
	except OSError as err:
	    print("cannot reach %s:5000: %s" % (ip, err))
	    raise SystemExit(1)

	try:
	    # FBI expects the length as a big endian u32, then the URLs, newline
	    # separated. It answers with a status byte once it has finished.
	    sock.sendall(struct.pack("!L", len(payload)))
	    sock.sendall(payload)
	    sock.settimeout(300)
	    reply = sock.recv(1)
	finally:
	    sock.close()

	# An empty reply means FBI closed the connection without answering, which is
	# what happens when it failed to fetch the file. Reporting that as a successful
	# install would send someone to the console to test a build that is not there.
	if not reply:
	    print("FBI closed the connection without answering: it did not install")
	    raise SystemExit(1)
	if reply[0] != 0:
	    print("FBI reported a problem installing:", reply[0])
	    raise SystemExit(1)
	print("installed")
	PY
	then
		echo
		echo "what the console asked this host for:"
		sed 's/^/  /' "$dir/http.log" 2>/dev/null || echo "  nothing"
		exit 1
	fi
	;;

push-cia)
	# When ftpd is the thing that is running, the CIA goes onto the card and FBI
	# installs it from there. Same result, one screen switch fewer.
	cia="$root/platform/3ds/daemoon.cia"
	[ -f "$cia" ] || { echo "no CIA yet: run make docker-cia"; exit 1; }
	curl -sS --ftp-create-dirs -T "$cia" "$(ftp_url "cias/daemoon.cia")"
	echo "put daemoon.cia in /cias on the card"
	echo "on the console: FBI -> SD -> cias -> daemoon.cia -> install"
	;;

push)
	local_file="${3:?usage: push <ip> <local file> <remote path>}"
	remote="${4:?usage: push <ip> <local file> <remote path>}"
	[ -f "$local_file" ] || { echo "no such file: $local_file"; exit 1; }
	# --ftp-create-dirs so a first run does not need the directory to exist.
	curl -sS --ftp-create-dirs -T "$local_file" "$(ftp_url "$remote")"
	echo "pushed $local_file -> $remote"
	;;

pull)
	remote="${3:?usage: pull <ip> <remote path> [local file]}"
	local_file="${4:-$(basename "$remote")}"
	curl -sS -o "$local_file" "$(ftp_url "$remote")"
	echo "pulled $remote -> $local_file"
	;;

selftest)
	# The whole loop, minus the one part a person has to do: pressing the button
	# that launches the app. Nothing can launch a title remotely.
	flag=$(mktemp)
	: > "$flag"

	echo "1. installing"
	sh "$0" install "$ip"

	echo
	echo "2. arming the unattended self test"
	echo "   (on the 3DS: close FBI, start ftpd)"
	printf '   press enter once ftpd is running: '
	read -r _
	sh "$0" push "$ip" "$flag" "DaeMoon/AUTOTEST"
	rm -f "$flag"

	echo
	echo "3. launch DaeMoon on the console."
	echo "   It runs the suite against its own save archive, writes the result,"
	echo "   and waits for A. Then close it and start ftpd again."
	printf '   press enter once ftpd is running: '
	read -r _

	out="$root/build/selftest.txt"
	mkdir -p "$root/build"
	if ! sh "$0" pull "$ip" "DaeMoon/selftest.txt" "$out"; then
		echo "no result on the card. The app did not get far enough to write one."
		exit 1
	fi

	echo
	cat "$out"
	if grep -q 'failures=0' "$out"; then
		echo
		echo "the backend conforms on hardware"
		echo "next: the parts a person has to watch, in docs/phase1-hardware.md"
	else
		echo
		echo "the backend does not conform on this console. Do not sync with it."
		exit 1
	fi
	;;

*)
	usage
	;;
esac
