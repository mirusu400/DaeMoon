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

// getIn is get with an Accept-Language, which is what a browser that has never been
// told anything sends.
func (p *panel) getIn(path, accept string) (int, string) {
	p.t.Helper()

	req, err := http.NewRequest(http.MethodGet, p.base+path, nil)
	if err != nil {
		p.t.Fatalf("GET %s: %v", path, err)
	}
	req.Header.Set("Accept-Language", accept)
	resp, err := p.http.Do(req)
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
		!strings.Contains(body, "Set up this instance") {
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

// Pairing a console twice must leave one console.
//
// Every pairing used to mint a device, so a console paired three times was three
// live credentials and three rows nobody could tell apart - which is exactly what a
// console reported. Nothing but the console can connect them: the server sees three
// pairings, and the only thing that would tie them to one machine is a hardware id,
// which is not secret, follows a person across services, and the rules refuse.
//
// So the console proves it. It presents the token it holds, and the server rotates
// that device rather than creating another. The pairing code says a person
// approved; the bearer says which console is asking; both are required, or anybody
// with a code could take over an existing device.
func TestPairingAgainRotatesTheSameDevice(t *testing.T) {
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
	if c.deviceID != firstID {
		t.Fatalf("a second pairing made a new device: %s then %s", firstID, c.deviceID)
	}
	if c.token == first {
		t.Fatal("the token was not rotated")
	}

	// The old one is dead the moment the new one is issued - not later, and not
	// only if something remembers to clean up.
	c.token = first
	if out, _ := c.run("", "list"); !strings.Contains(out, "unauthorized") &&
		!strings.Contains(out, "device_revoked") {
		t.Fatalf("the old token still works:\n%s", out)
	}
	c.token = c.deviceID // deliberately wrong, to prove the check is not vacuous
	if out, _ := c.run("", "list"); !strings.Contains(out, "unauthorized") {
		t.Fatalf("a nonsense token was accepted:\n%s", out)
	}

	// One console, one row, still live.
	//
	// Matched on the words the page shows rather than on a class name: the panel
	// was restyled once already and a test that breaks on a colour change is a test
	// that gets deleted rather than read.
	_, body := p.get("/devices")
	if n := strings.Count(body, ">live<"); n != 1 {
		t.Fatalf("expected one live console, found %d:\n%s", n, body)
	}
	if strings.Contains(body, ">revoked<") {
		t.Fatalf("rotation left a revoked row behind:\n%s", body)
	}
}

// A pairing code alone must not be enough to take over somebody's console. The
// bearer is what says which console is asking, and a wrong one makes a new device
// rather than seizing an existing one.
func TestAStrangerWithACodeGetsTheirOwnDevice(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)

	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}
	first := e.console("first", "living room 3DS")
	_, body := p.post("/pair", url.Values{"platform": {"3ds"}})
	first.pair(codeRe.FindStringSubmatch(body)[1])

	other := e.console("other", "somebody else")
	other.token = "not-a-real-token"
	_, body = p.post("/pair", url.Values{"platform": {"3ds"}})
	other.pair(codeRe.FindStringSubmatch(body)[1])

	if other.deviceID == first.deviceID {
		t.Fatal("an unrecognised token took over an existing device")
	}
	// And the first console is untouched.
	if out, err := first.run("", "list"); err != nil ||
		strings.Contains(out, "unauthorized") {
		t.Fatalf("the first console lost its token: %v\n%s", err, out)
	}
}

// People is for administrators. The nav link is hidden from everybody else, but a
// hidden link is not a check - the three handlers behind it are, and this is what
// says so.
func TestPeopleIsAdministratorsOnly(t *testing.T) {
	e := start(t)
	admin := newPanel(t, e)

	if status, _ := admin.post("/setup", url.Values{
		"username": {"admin"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}
	if status, _ := admin.post("/users", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("add a member: %d", status)
	}

	member := newPanel(t, e)
	if status, body := member.post("/login", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK || strings.Contains(body, "do not match") {
		t.Fatalf("member could not sign in: %d\n%s", status, body)
	}

	// Signed in, and the page is not offered.
	if _, body := member.get("/"); strings.Contains(body, `href="/users"`) {
		t.Fatalf("the People link was shown to a member:\n%s", body)
	}

	// And asking for it directly is refused, which is the part that matters.
	if status, _ := member.get("/users"); status != http.StatusForbidden {
		t.Fatalf("GET /users as a member: %d, want 403", status)
	}
	if status, _ := member.post("/users", url.Values{
		"username": {"sneaked"}, "password": {"hunter2hunter2"}, "admin": {"on"},
	}); status != http.StatusForbidden {
		t.Fatalf("POST /users as a member: %d, want 403", status)
	}

	_, body := admin.get("/users")
	id := regexp.MustCompile(`action="/users/([^/]+)/delete"`).FindStringSubmatch(body)
	if id == nil {
		t.Fatalf("no user row:\n%s", body)
	}
	if status, _ := member.post("/users/"+id[1]+"/delete", nil); status != http.StatusForbidden {
		t.Fatalf("a member deleted somebody: %d, want 403", status)
	}
}

// The appearance is a cookie, so it survives a reload and is not somebody's
// account setting - the same person on a phone and a desktop can want different
// answers.
func TestAppearanceIsRememberedPerBrowser(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)

	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	// Auto by default: no attribute, so the stylesheet follows the system.
	if _, body := p.get("/"); strings.Contains(body, "data-theme=") {
		t.Fatalf("a fresh browser was not on auto:\n%s", body)
	}

	if status, _ := p.post("/theme", url.Values{"theme": {"light"}, "from": {"/devices"}}); status != http.StatusOK {
		t.Fatalf("set light: %d", status)
	}
	if _, body := p.get("/"); !strings.Contains(body, `data-theme="light"`) {
		t.Fatalf("light was not kept:\n%s", body)
	}

	if _, _ = p.post("/theme", url.Values{"theme": {"auto"}, "from": {"/"}}); true {
		if _, body := p.get("/"); strings.Contains(body, "data-theme=") {
			t.Fatalf("auto did not clear the choice:\n%s", body)
		}
	}

	// A redirect target that came out of a form field is somewhere to send
	// somebody, so only paths on this site are honoured.
	if status, _ := p.post("/theme", url.Values{
		"theme": {"dark"}, "from": {"//example.invalid/"},
	}); status != http.StatusOK {
		t.Fatalf("offsite redirect: %d", status)
	}
	if _, body := p.get("/"); !strings.Contains(body, `data-theme="dark"`) {
		t.Fatalf("dark was not kept:\n%s", body)
	}
}

func TestThePanelSpeaksKorean(t *testing.T) {
	e := start(t)
	p := newPanel(t, e)

	// Before there is an account, and therefore before there is anywhere to store a
	// preference. A console owner opening this for the first time gets their own
	// language from the browser, and the setup page is the first thing they read.
	if _, body := p.getIn("/setup", "ko-KR,ko;q=0.9,en;q=0.8"); !strings.Contains(body, "이 인스턴스 설정") ||
		!strings.Contains(body, `<html lang="ko"`) {
		t.Fatalf("the browser's language was not honoured:\n%s", body)
	}
	// And a browser that asks for something this panel has no file for still gets a
	// page it can read.
	if _, body := p.getIn("/setup", "is-IS"); !strings.Contains(body, "Set up this instance") {
		t.Fatalf("an unknown language did not fall back to English:\n%s", body)
	}

	if status, _ := p.post("/setup", url.Values{
		"username": {"mirusu"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	// An explicit choice outranks the header and survives the next request.
	if status, _ := p.post("/lang", url.Values{"lang": {"ko"}, "from": {"/devices"}}); status != http.StatusOK {
		t.Fatalf("set ko: %d", status)
	}
	_, body := p.getIn("/", "en-US")
	for _, want := range []string{`<html lang="ko"`, "세이브", "본체", "사용자", "로그아웃"} {
		if !strings.Contains(body, want) {
			t.Fatalf("Korean panel is missing %q:\n%s", want, body)
		}
	}
	// The count line is a template with three numbers in it, and Korean puts them
	// in a different order than English does.
	if !strings.Contains(body, "본체 0개 중 0개 사용 중") {
		t.Fatalf("the substituted subtitle is not there:\n%s", body)
	}
	if strings.Contains(body, "{0}") || strings.Contains(body, "web.nav.") {
		t.Fatalf("a placeholder or a key reached the page:\n%s", body)
	}

	// Every page, not only the one the switch was on.
	for _, path := range []string{"/devices", "/pair", "/users"} {
		if _, body := p.get(path); strings.Contains(body, "web.") ||
			!strings.Contains(body, `<html lang="ko"`) {
			t.Fatalf("%s was not Korean:\n%s", path, body)
		}
	}

	// A refusal somebody can reach by using the panel is translated too, and it is
	// reached by a member rather than by the administrator asking for it.
	if status, _ := p.post("/users", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("add a member: %d", status)
	}
	member := newPanel(t, e)
	if status, _ := member.post("/login", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("member could not sign in: %d", status)
	}
	if status, _ := member.post("/lang", url.Values{"lang": {"ko"}, "from": {"/"}}); status != http.StatusOK {
		t.Fatalf("member set ko: %d", status)
	}
	status, body := member.get("/users")
	if status != http.StatusForbidden || !strings.Contains(body, "관리자만") {
		t.Fatalf("the refusal was not in Korean: %d %q", status, body)
	}
	// The same refusal, from the same handler, reads in English for a browser that
	// asked for English. One handler, two languages, chosen per request.
	english := newPanel(t, e)
	if status, _ := english.post("/login", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("second member sign in: %d", status)
	}
	if status, body := english.getIn("/users", "en-US"); status != http.StatusForbidden ||
		!strings.Contains(body, "Administrators only") {
		t.Fatalf("the refusal was not in English: %d %q", status, body)
	}

	// Handing the choice back means the browser decides again.
	if status, _ := p.post("/lang", url.Values{"lang": {"auto"}, "from": {"/"}}); status != http.StatusOK {
		t.Fatalf("clear lang: %d", status)
	}
	if _, body := p.getIn("/", "ja"); !strings.Contains(body, `<html lang="ja"`) {
		t.Fatalf("clearing the choice did not restore the header:\n%s", body)
	}

	// And a value from a form field is not trusted to be a language.
	if status, _ := p.post("/lang", url.Values{"lang": {"../../etc"}, "from": {"//example.invalid/"}}); status != http.StatusOK {
		t.Fatalf("nonsense language: %d", status)
	}
	if _, body := p.getIn("/", "en"); !strings.Contains(body, `<html lang="en"`) {
		t.Fatalf("nonsense was stored:\n%s", body)
	}
}

// Signing up is closed until an administrator opens it, and this is the whole
// gate: the page, the switch, and what an account made through it can see.
//
// The default matters more than the feature. This is a save sync server; an open
// sign up page on an address a router forwards is somewhere for anybody to put
// data, and the person who installs it is not always the person who decided what
// that router does.
func TestSigningUpIsClosedUntilItIsOpened(t *testing.T) {
	e := start(t)
	admin := newPanel(t, e)

	// Before there is anybody, /register is setup's job: setup is the page that
	// grants administrator, and it must stay the only one.
	if _, body := admin.get("/register"); !strings.Contains(body, "Set up this instance") {
		t.Fatalf("a fresh instance did not send /register to setup:\n%s", body)
	}
	if status, _ := admin.post("/setup", url.Values{
		"username": {"admin"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("setup: %d", status)
	}

	// Closed by default, and the login page does not offer a link to a page that
	// would refuse.
	stranger := newPanel(t, e)
	if _, body := stranger.get("/login"); strings.Contains(body, `href="/register"`) {
		t.Fatalf("a closed instance advertised sign up:\n%s", body)
	}
	if _, body := stranger.get("/register"); !strings.Contains(body, "not accepting new accounts") ||
		strings.Contains(body, `action="/register"`) {
		t.Fatalf("the closed page was not closed:\n%s", body)
	}
	// And the form being absent is not the check. Posting anyway is.
	if _, body := stranger.post("/register", url.Values{
		"username": {"sneak"}, "password": {"hunter2hunter2"},
	}); !strings.Contains(body, "not accepting new accounts") {
		t.Fatalf("a POST got through a closed sign up:\n%s", body)
	}
	if status, _ := stranger.get("/"); status != http.StatusOK {
		t.Fatalf("stranger request: %d", status)
	}
	if _, body := admin.get("/users"); strings.Contains(body, "sneak") {
		t.Fatalf("the refused account was created anyway:\n%s", body)
	}

	// A member cannot open it either. The switch is the administrator's.
	if status, _ := admin.post("/users", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("add a member: %d", status)
	}
	member := newPanel(t, e)
	if status, _ := member.post("/login", url.Values{
		"username": {"member"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK {
		t.Fatalf("member sign in: %d", status)
	}
	if status, _ := member.post("/users/registration", url.Values{"open": {"1"}}); status != http.StatusForbidden {
		t.Fatalf("a member opened sign up: %d", status)
	}

	// The administrator opens it.
	if status, body := admin.post("/users/registration", url.Values{"open": {"1"}}); status != http.StatusOK ||
		!strings.Contains(body, "may create an account") {
		t.Fatalf("open sign up: %d\n%s", status, body)
	}
	if _, body := stranger.get("/login"); !strings.Contains(body, `href="/register"`) {
		t.Fatalf("an open instance did not offer sign up:\n%s", body)
	}
	if status, body := stranger.post("/register", url.Values{
		"username": {"friend"}, "password": {"hunter2hunter2"},
	}); status != http.StatusOK || !strings.Contains(body, "Saves") {
		t.Fatalf("sign up: %d\n%s", status, body)
	}

	// Signed in as itself, not as an administrator, and holding nothing.
	if _, body := stranger.get("/"); !strings.Contains(body, "friend") ||
		strings.Contains(body, `href="/users"`) {
		t.Fatalf("a new account was not a plain member:\n%s", body)
	}
	if status, _ := stranger.get("/users"); status != http.StatusForbidden {
		t.Fatalf("a new account reached People: %d", status)
	}
	if _, body := stranger.get("/devices"); !strings.Contains(body, "No consoles yet") {
		t.Fatalf("a new account saw somebody else's consoles:\n%s", body)
	}

	// The same name twice is refused rather than taking over the first.
	other := newPanel(t, e)
	if _, body := other.post("/register", url.Values{
		"username": {"friend"}, "password": {"hunter2hunter2"},
	}); !strings.Contains(body, "name is taken") {
		t.Fatalf("a duplicate name was accepted:\n%s", body)
	}

	// And closing it again shuts the door without touching the account behind it.
	if status, _ := admin.post("/users/registration", url.Values{"open": {"0"}}); status != http.StatusOK {
		t.Fatalf("close sign up: %d", status)
	}
	if _, body := other.post("/register", url.Values{
		"username": {"late"}, "password": {"hunter2hunter2"},
	}); !strings.Contains(body, "not accepting new accounts") {
		t.Fatalf("sign up stayed open after being closed:\n%s", body)
	}
	if status, body := stranger.get("/"); status != http.StatusOK || !strings.Contains(body, "friend") {
		t.Fatalf("closing sign up disturbed an existing account: %d\n%s", status, body)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

var _ = json.Marshal
