# Working on a 3DS without touching the SD card

Taking the card out, copying, putting it back, booting: about a minute per
iteration, and every one of those minutes is spent on a build that writes to save
data. The tedium is not the problem. The problem is that tedium invites shortcuts,
and the shortcut here is testing on a save somebody cares about.

So everything goes over wifi.

## What the console needs

Two homebrew apps, both standard:

| App | Used for | Mode |
|---|---|---|
| [FBI](https://github.com/Steveice10/FBI) | installing the CIA | Remote Install → Receive URLs over the network |
| [ftpd](https://github.com/mtheall/ftpd) | pushing and pulling files | just run it |

Both listen on TCP 5000, and neither is running while the other is, so that is
not a conflict. Both show the console's IP on screen; that is the `HOST` below.

## Installing a build

```bash
make 3ds-install HOST=192.168.1.42
```

This serves the CIA from this machine on a free port, tells FBI to fetch it, and
stops serving as soon as it is done. FBI answers with a status byte, which is
checked: an install that failed does not report success, because being sent to the
console to test a build that is not there is worse than being told it failed.

## The unattended self test

```bash
make 3ds-selftest HOST=192.168.1.42
```

Installs, pushes the `AUTOTEST` flag to the card, waits for you to launch the app,
then pulls the result back and prints it:

```
target=000400000DAE0000 checks=93 failures=0
```

The one step that cannot be automated is launching the title. Nothing can start a
title on a 3DS remotely.

The suite runs against **this application's own save archive** and no other. It
clears what it is pointed at, so an unattended run that could pick a title
somebody plays would be indefensible. A build with `SAVEDATA_SIZE=0K` - the
shipped one - has no archive of its own and says so instead of running.

## Pointing the console at a server

Phase 2 onwards needs two settings a console has no keyboard for, so they come off
the SD card at `sdmc:/DaeMoon/config.txt`:

```
server = http://192.168.1.13:8080
token  = <from daemoonctl pair, or the pairing flow in Phase 4>
label  = 거실 3DS
ca_bundle = sdmc:/DaeMoon/cacert.pem
```

Lines of `key=value`, everything else ignored. Deliberately not JSON: this gets
edited on a phone sometimes, and a missing brace should not be why a console
cannot reach its own server. A trailing slash on the server is stripped, because
kept it becomes a double slash in every path built from it.

`daemoond` serves https itself when `DAEMOON_TLS_CERT` and `DAEMOON_TLS_KEY` are
set - both or neither, because half configured TLS is the case where somebody
believes they have it and does not. It is served by the binary rather than left to
a reverse proxy because "one binary plus one database file" stops being true the
moment the answer to "how do I get https" is "install nginx".

For a self hosted setup that usually means an operator's own CA:

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -days 365 -nodes \
  -keyout ca.key -out ca.pem -subj "/CN=DaeMoon self-hosted CA"
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=192.168.1.13"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -days 365 -sha256 -out server.pem \
  -extfile <(printf "subjectAltName=IP:192.168.1.13\nextendedKeyUsage=serverAuth\n")
tools/3ds-deploy.sh push 192.168.1.43 ca.pem DaeMoon/cacert.pem
```

Two traps in that certificate, both of which come back as `tls_error` and neither
of which says which one it was:

- The `subjectAltName` is not optional. A certificate with only a common name is
  rejected by anything written this decade, mbedtls included.
- **`IP:` alone is not enough.** curl hands mbedtls the host as a string and
  mbedtls matches it against `dNSName` entries, so a certificate for a bare IP
  address needs `DNS:192.168.1.13` as well as `IP:192.168.1.13`. A name is easier
  to live with, but a self hosted LAN server usually has neither DNS nor a name.

`net/failed` in `sdmc:/DaeMoon/trace.txt` carries curl's own sentence about the
failure, which is what tells those two apart. `tls_error` on its own does not.

`ca_bundle` is only needed for https. The console's own certificate store is from
2011 and fails against ordinary modern servers, which is why this build links
3ds-curl and 3ds-mbedtls rather than using httpc:C - but a bundle still has to come
from somewhere, and on a self hosted setup that is usually the operator's own CA.
Verification is never turned off.

From Phase 2 on the app can also write this file itself: **Settings** on the
bottom screen, with the software keyboard. The rules call that keyboard painful
and they are right, which is why Phase 4 is QR and device code pairing - but until
then the alternative is taking the card out, and not taking the card out is what
this whole workflow is for. What it writes is the same file in the same format, so
the two ways of setting a server do not diverge.

Push it with the tool rather than moving the card:

```bash
printf 'server = http://192.168.1.13:8080\ntoken = %s\n' "$DAEMOON_TOKEN" > /tmp/config.txt
tools/3ds-deploy.sh push 192.168.1.43 /tmp/config.txt DaeMoon/config.txt
```

## Files, either direction

```bash
tools/3ds-deploy.sh push 192.168.1.42 ./something sdmc/path/to/it
tools/3ds-deploy.sh pull 192.168.1.42 DaeMoon/backups/3ds_0004000000055D00_ab12cd34ef56.zip
```

Paths are relative to the SD card root, as ftpd presents it. Pulling a backup off
the card is how you check one on a desktop: `daemoonctl` reads the same packages,
so a backup made on hardware can be unpacked and inspected without the console.

## What each layer actually proves

| Command | Runs where | Says |
|---|---|---|
| `make core-test` | desktop, sanitizers | the logic is not obviously wrong |
| `make cia-verify` | desktop | the CIA carries the rights `app.rsf` asks for |
| `make emu-selftest` | emulator | it runs as an ARM binary through real libctru |
| `make 3ds-selftest` | **hardware** | the FS service behaves the way core assumes |

Only the last one counts for the roadmap. The first three exist so that when the
last one fails, the failure is about hardware and not about something that could
have been found here.

## Iterating on the UI only

`3dslink` sends a `.3dsx` to the Homebrew Launcher over the network, which is
faster than installing a CIA. It is fine for laying out a screen and useless for
anything else: a `.3dsx` cannot open another title's save archive, which is the
entire subject of this project.
