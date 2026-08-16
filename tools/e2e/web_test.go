package e2e

// The web panel and a real client, in the same room.
//
// Phase 4's whole point is that a token stops being something a person copies onto
// an SD card by hand. That claim spans three pieces written separately - a browser
// form, a QR payload, and a console's pairing call - and none of them proves it
// alone. This is where somebody logs in, asks for a code, and a client claims it.
//
// The camera is the only part of the flow that a desktop cannot stand in for. What
// it would read is checked in the C suite against quirc; what it would mean is
// checked by core's parser. This covers everything between.

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"regexp"
	"strings"
	"testing"
)

type panel struct {
	t    *testing.T
	base string
	http *http.Client
}

func newPanel(t *testing.T, e *env) *panel {
	t.Helper()

	jar, err := cookiejar.New(nil)
	if err != nil {
		t.Fatalf("cookie jar: %v", err)
	}
	return &panel{t: t, base: "http://" + e.addr, http: &http.Client{Jar: jar}}
}

func (p *panel) get(path string) (int, string) {
	p.t.Helper()

	resp, err := p.http.Get(p.base + path)
	if err != nil {
		p.t.Fatalf("GET %s: %v", path, err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

func (p *panel) post(path string, form url.Values) (int, string) {
	p.t.Helper()

	resp, err := p.http.PostForm(p.base+path, form)
	if err != nil {
		p.t.Fatalf("POST %s: %v", path, err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

var codeRe = regexp.MustCompile(`<p class="code">(\d{6})</p>`)

func TestPairingAConsoleFromTheWebPanel(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)

	// A fresh instance sends everything to setup, and setup is the only page that
	// can make an administrator.
	if status, body := p.get("/"); status != http.StatusOK ||
		!strings.Contains(body, "Set up DaeMoon") {
		t.Fatalf("a fresh instance did not offer setup: %d\n%s", status, body)
	}

	if status, body := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK || !strings.Contains(body, "Saves") {
		t.Fatalf("setup: %d\n%s", status, body)
	}

	// And once there is one, it stops being reachable. An instance with an account
	// must not offer another administrator to whoever asks.
	if status, body := p.get("/setup"); strings.Contains(body, "Create the account") {
		t.Fatalf("setup is still open: %d\n%s", status, body)
	}

	status, body := p.post("/pair", url.Values{"platform": {"3ds"}})
	if status != http.StatusOK {
		t.Fatalf("pair: %d\n%s", status, body)
	}
	m := codeRe.FindStringSubmatch(body)
	if m == nil {
		t.Fatalf("no pairing code on the page:\n%s", body)
	}
	code := m[1]

	// The QR the console would scan. Its contents are checked against quirc in the
	// C suite; here it only has to exist and carry the payload the parser expects.
	status, svg := p.get("/pair/" + code + "/qr.svg")
	if status != http.StatusOK || !strings.HasPrefix(svg, "<svg") {
		t.Fatalf("qr: %d\n%s", status, svg[:min(200, len(svg))])
	}

	// Still outstanding until something claims it.
	if _, s := p.get("/pair/" + code + "/status"); !strings.Contains(s, `"claimed":false`) {
		t.Fatalf("status before pairing: %s", s)
	}

	// A real client, claiming a code a browser made. This is the join the whole
	// phase is about.
	c := e.console("console", "living room 3DS")
	c.pair(code)

	if _, s := p.get("/pair/" + code + "/status"); !strings.Contains(s, `"claimed":true`) {
		t.Fatalf("status after pairing: %s", s)
	}

	// And the console is on the panel, under the name it gave.
	if _, body := p.get("/devices"); !strings.Contains(body, "living room 3DS") {
		t.Fatalf("the console is not listed:\n%s", body)
	}
}

// A save synced from a console has to be visible, and downloadable, from a
// browser. That is what makes the panel worth having over reading the database.
func TestASyncedSaveIsVisibleAndDownloadable(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)
	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}
	_, body := p.post("/pair", url.Values{"platform": {"3ds"}})
	code := codeRe.FindStringSubmatch(body)[1]

	c := e.console("console", "3DS")
	c.pair(code)
	c.writeSave(title3DS, "main.sav", "player data")
	c.mustRun("", "--yes", "sync")

	_, body = p.get("/")
	if !strings.Contains(body, "0004000000055D00") {
		t.Fatalf("the synced title is not on the dashboard:\n%s", body)
	}

	_, body = p.get("/titles/3ds/0004000000055D00")
	if !strings.Contains(body, "Download") {
		t.Fatalf("no download offered:\n%s", body)
	}

	status, blob := p.get("/titles/3ds/0004000000055D00/blob/1")
	if status != http.StatusOK {
		t.Fatalf("download: %d", status)
	}
	// A save package is a zip, and the browser gets exactly what the console would.
	if !strings.HasPrefix(blob, "PK") {
		t.Fatalf("the download is not a package: %.16q", blob)
	}
}

// Somebody else's session must not reach these pages at all, and a signed out
// browser must not either.
func TestThePanelRefusesWithoutASession(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)
	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	// A second browser, with no cookie.
	anon := newPanel(t, e)
	for _, path := range []string{"/", "/devices", "/pair", "/users"} {
		_, body := anon.get(path)
		if !strings.Contains(body, "Sign in") {
			t.Fatalf("%s was reachable without a session:\n%s", path, body)
		}
	}

	// And a wrong password does not become one, with the same message either way.
	_, body := anon.post("/login", url.Values{
		"username": {"mirusu"}, "password": {"wrong"},
	})
	if !strings.Contains(body, "do not match") {
		t.Fatalf("a wrong password did not fail:\n%s", body)
	}
	_, body = anon.post("/login", url.Values{
		"username": {"nobody"}, "password": {"wrong"},
	})
	if !strings.Contains(body, "do not match") {
		t.Fatalf("an unknown name gave a different answer:\n%s", body)
	}
}

// The last administrator cannot be removed. An instance with saves on it and
// nobody who can administer it is not a state worth being one click away from.
func TestTheLastAdministratorStays(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)
	if status, _ := p.post("/setup", url.Values{
		"username": {"admin"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	_, body := p.get("/users")
	id := regexp.MustCompile(`action="/users/([^/]+)/delete"`).FindStringSubmatch(body)
	if id == nil {
		t.Fatalf("no user row:\n%s", body)
	}
	if status, _ := p.post("/users/"+id[1]+"/delete", nil); status != http.StatusConflict {
		t.Fatalf("the last administrator was removable: %d", status)
	}

	// With a second administrator there is somebody left, so it is allowed.
	if status, _ := p.post("/users", url.Values{
		"username": {"second"}, "password": {"hunter2hunter2"}, "admin": {"on"},
	}); status != http.StatusOK {
		t.Fatalf("add: %d", status)
	}
	if status, _ := p.post("/users/"+id[1]+"/delete", nil); status != http.StatusOK {
		t.Fatalf("removing one of two administrators: %d", status)
	}
}

// Pairing a console twice leaves two live tokens and two rows a person cannot tell
// apart. Nobody but the console can connect them: the server sees two pairings, and
// the only thing that would tie them together is a hardware id, which this project
// will not send. So the console spends the old credential on itself.
//
// Checked here rather than only on a console because it is the part that can be:
// what the 3DS does is call the same endpoint with the same two values.
func TestPairingAgainRetiresTheOldToken(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)

	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	newCode := func() string {
		_, body := p.post("/pair", url.Values{"platform": {"3ds"}})
		m := codeRe.FindStringSubmatch(body)
		if m == nil {
			t.Fatalf("no pairing code:\n%s", body)
		}
		return m[1]
	}

	c := e.console("console", "3DS")
	c.pair(newCode())
	first, firstID := c.token, c.deviceID

	c.pair(newCode())
	second := c.token
	if second == first {
		t.Fatal("the second pairing reused the first token")
	}

	// What the console does next: retire the old one, authenticated as itself.
	e.revoke(first, firstID)

	// The old token is dead and the new one still works, which is the property that
	// matters - retiring the wrong one would lock the console out.
	c.token = first
	out, _ := c.run("", "list")
	if !strings.Contains(out, "device_revoked") {
		t.Fatalf("the retired token still works:\n%s", out)
	}
	c.token = second
	out, err := c.run("", "list")
	if err != nil || strings.Contains(out, "device_revoked") {
		t.Fatalf("the current token stopped working: %v\n%s", err, out)
	}

	// And the panel shows the history rather than two live consoles: two rows, one
	// of them marked.
	_, body := p.get("/devices")
	if n := strings.Count(body, `pill revoked`); n != 1 {
		t.Fatalf("expected exactly one revoked console, found %d:\n%s", n, body)
	}
	if n := strings.Count(body, `class="danger">Revoke`); n != 1 {
		t.Fatalf("expected exactly one console still live, found %d:\n%s", n, body)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

var _ = json.Marshal
