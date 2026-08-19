package web

import (
	"fmt"
	"log/slog"
	"net/http"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/pkgfmt"
	"github.com/mirusu400/DaeMoon/server/internal/qr"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

// ------------------------------------------------------------------ dashboard

type dashboardData struct {
	Titles  []store.TitleSummary
	Devices []store.DeviceInfo
	// Counted here rather than in the template: a template that can do arithmetic
	// is a template that will.
	LiveDevices int
	Versions    int
	Stored      uint64
}

func (s *Server) getDashboard(w http.ResponseWriter, r *http.Request) {
	user := userOf(r)

	titles, err := s.store.ListTitles(r.Context(), user.ID, "")
	if err != nil {
		s.fail(w, r, err, "could not list titles")
		return
	}
	devices, err := s.store.ListDevices(r.Context(), user.ID)
	if err != nil {
		s.fail(w, r, err, "could not list devices")
		return
	}
	data := dashboardData{Titles: titles, Devices: devices}
	for _, d := range devices {
		if !d.Revoked {
			data.LiveDevices++
		}
	}
	// Every version of every title, and what they take up. Cheap at the scale a
	// self hosted instance runs at, and it is the number somebody actually wants
	// when they wonder whether this is filling a disk.
	for _, t := range titles {
		versions, err := s.store.ListVersions(r.Context(), user.ID, t.Platform, t.TitleID)
		if err != nil {
			continue
		}
		data.Versions += len(versions)
		for _, v := range versions {
			data.Stored += v.Size
		}
	}

	s.render(w, r, "dashboard.html", page{
		Title: "web.nav.saves",
		Page:  "saves",
		Data:  data,
	})
}

// ------------------------------------------------------------------ devices

func (s *Server) getDevices(w http.ResponseWriter, r *http.Request) {
	devices, err := s.store.ListDevices(r.Context(), userOf(r).ID)
	if err != nil {
		s.fail(w, r, err, "could not list devices")
		return
	}
	s.render(w, r, "devices.html", page{Title: "web.nav.consoles", Page: "devices", Data: devices})
}

func (s *Server) postRevokeDevice(w http.ResponseWriter, r *http.Request) {
	// Scoped to the session's own user, so a device id from somebody else's
	// instance is a 404 rather than a revocation.
	err := s.store.RevokeDevice(r.Context(), userOf(r).ID, chi.URLParam(r, "id"))
	if err != nil {
		s.fail(w, r, err, "could not revoke that console")
		return
	}
	http.Redirect(w, r, "/devices", http.StatusSeeOther)
}

// ------------------------------------------------------------------ pairing

type pairData struct {
	Code     string
	QRTarget string
	Platform string
}

func (s *Server) getPair(w http.ResponseWriter, r *http.Request) {
	s.render(w, r, "pair.html", page{Title: "web.nav.add_console", Page: "pair"})
}

// postPair mints a pairing code and marks it approved in the same step.
//
// The roadmap describes two flows and this is where they meet: the console shows a
// code and somebody approves it from a browser, or the browser shows a code and
// the console scans it. Both end at POST /v1/devices/pair with a code. Approving
// on creation is what makes the QR direction work - the person is already logged
// in and looking at the screen, so there is nobody left to ask.
func (s *Server) postPair(w http.ResponseWriter, r *http.Request) {
	user := userOf(r)
	platform := r.FormValue("platform")
	if !pkgfmt.Platform(platform).Valid() {
		platform = string(pkgfmt.Platform3DS)
	}

	code, err := auth.NewPairingCode()
	if err != nil {
		s.fail(w, r, err, "could not create a pairing code")
		return
	}
	if err := s.store.CreatePairing(r.Context(), code, user.ID, true,
		s.cfg.PairingTTL); err != nil {
		s.fail(w, r, err, "could not create a pairing code")
		return
	}

	s.render(w, r, "pair.html", page{
		Title: "web.nav.add_console",
		Page:  "pair",
		Data: pairData{
			Code:     code,
			QRTarget: s.pairPayload(r, code),
			Platform: platform,
		},
	})
}

// pairPayload is what the QR holds: a tag, a server address, and a code, separated
// by vertical bars.
//
// Not a URL. The only thing that reads this is a 3DS, and the parser on that side
// is C with a fixed buffer - percent decoding and query string order are work with
// nothing to show for it. A bar cannot appear unencoded in a URL and a pairing code
// is six digits, so splitting on it is unambiguous. The leading tag and version let
// a later format be told apart from this one rather than mis-parsed as it.
//
// The address is in the payload because a console that can scan a code should not
// also need somebody to type a URL into it, which is the entire point of scanning
// one. Scheme and host come from the request, so an instance reached at
// 192.168.1.13:8443 hands out that address rather than whatever was configured once
// and then moved.
func (s *Server) pairPayload(r *http.Request, code string) string {
	return fmt.Sprintf("DAEMOON|1|%s|%s", requestOrigin(r), code)
}

// requestOrigin is the scheme and host this instance was actually reached at.
//
// Taken from the request rather than from configuration, so an instance reached
// at 192.168.1.13:8443 hands out that address rather than whatever was set once
// and then moved. X-Forwarded-Proto because TLS is usually terminated in front.
func requestOrigin(r *http.Request) string {
	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	return scheme + "://" + r.Host
}

// getPairStatus is polled by the page while somebody carries a console across the
// room. It reports whether the code has been claimed, which is when the console
// has a token and the page can stop.
func (s *Server) getPairStatus(w http.ResponseWriter, r *http.Request) {
	devices, err := s.store.ListDevices(r.Context(), userOf(r).ID)
	if err != nil {
		s.fail(w, r, err, "could not check the pairing")
		return
	}
	// A claimed code is deleted by ClaimPairing, so "gone" means "used". Anything
	// else - still waiting, or expired - leaves the page as it is.
	claimed := !s.store.PairingExists(r.Context(), chi.URLParam(r, "code"))

	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"claimed":%t,"devices":%d}`, claimed, len(devices))
}

func (s *Server) getPairQR(w http.ResponseWriter, r *http.Request) {
	code := chi.URLParam(r, "code")
	if !auth.ValidPairingCode(code) {
		http.NotFound(w, r)
		return
	}

	writeQRSVG(w, r, s.pairPayload(r, code))
}

// writeQRSVG draws one code.
//
// SVG rather than a raster: no image encoder, no size to choose, and it scales to
// whatever the browser gives it - which matters when the thing reading it is a
// console camera being held at arm's length.
func writeQRSVG(w http.ResponseWriter, r *http.Request, payload string) {
	img, err := qr.Encode([]byte(payload))
	if err != nil {
		http.Error(w, "could not draw the code", http.StatusInternalServerError)
		return
	}

	const quiet = 4
	side := img.Size + quiet*2

	w.Header().Set("Content-Type", "image/svg+xml")
	w.Header().Set("Cache-Control", "no-store")
	var b strings.Builder
	fmt.Fprintf(&b, `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" `+
		`shape-rendering="crispEdges" width="512" height="512">`, side, side)
	fmt.Fprintf(&b, `<rect width="%d" height="%d" fill="#fff"/>`, side, side)
	b.WriteString(`<path fill="#000" d="`)
	for y := 0; y < img.Size; y++ {
		for x := 0; x < img.Size; x++ {
			if img.At(x, y) {
				fmt.Fprintf(&b, "M%d %dh1v1h-1z", x+quiet, y+quiet)
			}
		}
	}
	b.WriteString(`"/></svg>`)
	_, _ = w.Write([]byte(b.String()))
}

// ------------------------------------------------------------------ titles

type titleData struct {
	Platform  string
	TitleID   string
	TitleName string
	Versions  []store.VersionMeta
}

func (s *Server) getTitle(w http.ResponseWriter, r *http.Request) {
	platform := chi.URLParam(r, "platform")
	tid := chi.URLParam(r, "tid")
	if !pkgfmt.Platform(platform).Valid() {
		http.NotFound(w, r)
		return
	}

	versions, err := s.store.ListVersions(r.Context(), userOf(r).ID, platform, tid)
	if err != nil {
		s.fail(w, r, err, "could not list versions")
		return
	}
	// The name is on the title row, not on a version, so it comes from the list.
	name := ""
	if titles, err := s.store.ListTitles(r.Context(), userOf(r).ID, platform); err == nil {
		for _, t := range titles {
			if t.TitleID == tid {
				name = t.TitleName
				break
			}
		}
	}

	s.render(w, r, "title.html", page{
		Title: tid,
		Page:  "saves",
		Data: titleData{
			Platform: platform, TitleID: tid, TitleName: name, Versions: versions,
		},
	})
}

// getTitleBlob hands a save package to a browser. The same bytes the console
// downloads, which is what makes this useful: a backup pulled out here can be
// unpacked with daemoonctl or with any zip tool.
func (s *Server) getTitleBlob(w http.ResponseWriter, r *http.Request) {
	platform := chi.URLParam(r, "platform")
	tid := chi.URLParam(r, "tid")
	version, err := strconv.ParseUint(chi.URLParam(r, "version"), 10, 32)
	if err != nil || !pkgfmt.Platform(platform).Valid() {
		http.NotFound(w, r)
		return
	}

	meta, err := s.store.Version(r.Context(), userOf(r).ID, platform, tid, uint32(version))
	if err != nil {
		s.fail(w, r, err, "could not read that version")
		return
	}

	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition",
		fmt.Sprintf(`attachment; filename="%s_%s_v%d.zip"`, platform, tid, meta.Version))
	w.Header().Set("X-DaeMoon-SHA256", meta.SHA256)
	if err := s.store.WriteBlobTo(r.Context(), meta, w); err != nil {
		// The headers are already out; a truncated download is all that can be
		// signalled from here, and the log is where the reason lives.
		s.fail(w, r, err, "could not stream that version")
	}
}

// ------------------------------------------------------------------ users

func (s *Server) getUsers(w http.ResponseWriter, r *http.Request) {
	if !userOf(r).IsAdmin {
		s.denied(w, r, http.StatusForbidden, "web.people.admin_only")
		return
	}
	s.renderUsers(w, r, "")
}

/* What the People page shows: everybody, and the one switch that decides whether
 * anybody else can arrive without being added here by hand. */
type usersView struct {
	Users      []store.User
	OpenSignUp bool
}

func (s *Server) renderUsers(w http.ResponseWriter, r *http.Request, errKey string) {
	users, err := s.store.ListUsers(r.Context())
	if err != nil {
		s.fail(w, r, err, "could not list users")
		return
	}
	s.render(w, r, "users.html", page{Title: "web.nav.people", Page: "users",
		Error: errKey,
		Data:  usersView{Users: users, OpenSignUp: s.store.OpenRegistration(r.Context())}})
}

// postRegistration opens or closes the sign up page.
//
// A form with an explicit value rather than a toggle that flips whatever it finds:
// two tabs open on this page must not be able to turn sign up back on by pressing
// the button that said "close" when it was drawn.
func (s *Server) postRegistration(w http.ResponseWriter, r *http.Request) {
	if !userOf(r).IsAdmin {
		s.denied(w, r, http.StatusForbidden, "web.people.admin_only")
		return
	}
	open := r.FormValue("open") == "1"
	if err := s.store.SetOpenRegistration(r.Context(), open); err != nil {
		s.fail(w, r, err, "could not change the sign up setting")
		return
	}
	slog.InfoContext(r.Context(), "open registration", "open", open, "by", userOf(r).Username)
	http.Redirect(w, r, "/users", http.StatusSeeOther)
}

func (s *Server) postUsers(w http.ResponseWriter, r *http.Request) {
	if !userOf(r).IsAdmin {
		s.denied(w, r, http.StatusForbidden, "web.people.admin_only")
		return
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := r.FormValue("password")
	isAdmin := r.FormValue("admin") == "on"

	show := func(msg string) { s.renderUsers(w, r, msg) }

	if msg := checkCredentials(username, password); msg != "" {
		show(msg)
		return
	}
	hash, err := auth.HashPassword(password)
	if err != nil {
		show("web.err.password_store")
		return
	}
	if _, err := s.store.CreateUser(r.Context(), uuid.NewString(), username, hash,
		isAdmin); err != nil {
		show("web.err.name_taken")
		return
	}
	http.Redirect(w, r, "/users", http.StatusSeeOther)
}

func (s *Server) postDeleteUser(w http.ResponseWriter, r *http.Request) {
	me := userOf(r)
	if !me.IsAdmin {
		s.denied(w, r, http.StatusForbidden, "web.people.admin_only")
		return
	}

	id := chi.URLParam(r, "id")
	target, err := s.store.UserByID(r.Context(), id)
	if err != nil {
		s.fail(w, r, err, "no such person")
		return
	}

	// The last administrator cannot be removed, whether by themselves or by
	// another. An instance with saves on it and nobody who can administer it is
	// not a state worth being one click away from.
	if target.IsAdmin {
		n, err := s.store.CountAdmins(r.Context())
		if err != nil {
			s.fail(w, r, err, "could not count administrators")
			return
		}
		if n <= 1 {
			s.denied(w, r, http.StatusConflict, "web.people.last_admin")
			return
		}
	}

	if err := s.store.DeleteUser(r.Context(), id); err != nil {
		s.fail(w, r, err, "could not remove that person")
		return
	}
	http.Redirect(w, r, "/users", http.StatusSeeOther)
}
