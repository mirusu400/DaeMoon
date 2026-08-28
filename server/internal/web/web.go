// Package web is the browser side: logging in, approving a console, and looking
// at what has been synced.
//
// It exists because the pairing flow was always described in terms of it - "log in
// on the web, display a QR, scan it with the console camera" - and until now there
// was nothing to log in to. A token had to be issued from a command line and copied
// onto an SD card by hand, which is the work this project exists to remove.
//
// Server rendered HTML with html/template, embedded in the binary. No build step,
// no framework, no JavaScript beyond the few lines that poll a pairing code. "One
// static binary plus one database file" stops being true the moment the answer to
// "how do I run the web side" involves npm.
//
// The rule that the server returns codes and clients render text still holds for
// the API. This is not the API: it is a client, the only one written in Go, and a
// client is the thing that renders. Its sentences come from shared/lang/ through
// internal/i18n, the same files the consoles read, so there is one place to add a
// string and CI still checks every language for it.
//
// Which means no user facing sentence should appear in this package or in its
// templates. A `page` carries a key and the template resolves it.
package web

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"embed"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"html/template"
	"io/fs"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/i18n"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

//go:embed templates/*.html static/*
var assets embed.FS

const (
	sessionCookie = "daemoon_session"
	// A week. Long enough that logging in is not a chore on a home network, short
	// enough that a forgotten browser stops being a way in.
	sessionTTL = 7 * 24 * time.Hour
)

type Server struct {
	store    *store.Store
	cfg      config.Config
	tpl      *template.Template
	releases *releaseCache
}

func New(st *store.Store, cfg config.Config) (*Server, error) {
	digests, err := hashAssets()
	if err != nil {
		return nil, err
	}
	tpl, err := template.New("").Funcs(template.FuncMap{
		"bytes": humanBytes,
		"asset": func(name string) string { return assetURL(digests, name) },
	}).ParseFS(assets, "templates/*.html")
	if err != nil {
		return nil, fmt.Errorf("parse templates: %w", err)
	}
	return &Server{store: st, cfg: cfg, tpl: tpl, releases: &releaseCache{}}, nil
}

/* Static files are addressed by what is in them.
 *
 * A deployment behind a CDN published new HTML against a stylesheet that had been
 * cached for four hours, and every page came out unstyled. Nothing was wrong with
 * either file: they were from different builds, and the one URL they share is what
 * let a cache pair them up that way.
 *
 * So the URL carries a digest of the contents. A build that changes a file changes
 * its URL, an unchanged file keeps the URL it had, and no cache anywhere has to be
 * asked to forget anything. It also means the answer to "the site looks broken
 * after a deploy" stops being "purge the cache and wait".
 *
 * Computed once at startup from the embedded files, so there is no build step and
 * nothing on disk to get out of step with the binary.
 */
func hashAssets() (map[string]string, error) {
	out := map[string]string{}
	err := fs.WalkDir(assets, "static", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, err := assets.ReadFile(path)
		if err != nil {
			return fmt.Errorf("read %s: %w", path, err)
		}
		sum := sha256.Sum256(b)
		out[strings.TrimPrefix(path, "static/")] = hex.EncodeToString(sum[:])[:10]
		return nil
	})
	if err != nil {
		return nil, fmt.Errorf("hash static assets: %w", err)
	}
	return out, nil
}

// assetURL is `{{asset "style.css"}}`. An unknown name is returned unversioned
// rather than dropped: a missing digest should cost a cache entry, not a
// stylesheet.
func assetURL(digests map[string]string, name string) string {
	if sum, ok := digests[name]; ok {
		return "/static/" + name + "?v=" + sum
	}
	return "/static/" + name
}

// staticCache decides how long a static file may be reused.
//
// A request that names a digest can be kept forever, because that URL can only
// ever mean those bytes. One that does not has to be revalidated, since the same
// URL will mean something else after the next deploy.
func staticCache(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("v") != "" {
			w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		} else {
			w.Header().Set("Cache-Control", "no-cache")
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) Routes() chi.Router {
	r := chi.NewRouter()

	r.Handle("/static/*", staticCache(http.FileServer(http.FS(assets))))
	r.Get("/robots.txt", s.getRobots)
	r.Get("/sitemap.xml", s.getSitemap)

	// The root is the one address somebody is given, so it answers for whoever
	// arrives at it rather than bouncing everybody to a sign in form. See getRoot.
	r.Get("/", s.getRoot)

	// No session on either: the thing reading the code is a console that has
	// never signed in to anything, and the thing following the URL is FBI.
	r.Get(ciaPath, s.getInstallCIA)
	r.Get("/install/3ds.qr.svg", s.getInstallQR)

	r.Get("/setup", s.getSetup)
	r.Post("/setup", s.postSetup)
	r.Get("/login", s.getLogin)
	r.Post("/login", s.postLogin)
	r.Get("/register", s.getRegister)
	r.Post("/register", s.postRegister)
	r.Post("/logout", s.postLogout)
	// Outside the session group: a signed out login page can be themed and, more to
	// the point, read. Somebody who cannot read the login screen cannot get past it
	// to change the language.
	r.Post("/theme", s.postTheme)
	r.Post("/lang", s.postLang)

	r.Group(func(r chi.Router) {
		r.Use(s.requireSession)
		r.Get("/devices", s.getDevices)
		r.Post("/devices/{id}/revoke", s.postRevokeDevice)
		r.Get("/pair", s.getPair)
		r.Post("/pair", s.postPair)
		r.Get("/pair/{code}/status", s.getPairStatus)
		r.Get("/pair/{code}/qr.svg", s.getPairQR)
		r.Get("/titles/{platform}/{tid}", s.getTitle)
		r.Get("/titles/{platform}/{tid}/blob/{version}", s.getTitleBlob)
		r.Get("/users", s.getUsers)
		r.Post("/users", s.postUsers)
		r.Post("/users/registration", s.postRegistration)
		r.Post("/users/{id}/delete", s.postDeleteUser)
	})

	return r
}

// ------------------------------------------------------------------ sessions

type ctxKey int

const userKey ctxKey = 1

func hashToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

func newSessionToken() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate session token: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

func userOf(r *http.Request) store.User {
	u, _ := r.Context().Value(userKey).(store.User)
	return u
}

// sessionUser resolves the session cookie, and reports whether there was one that
// still works. ok is false both for "not signed in" and for "signed in with a
// session that has expired or been revoked", because a page has the same thing to
// do about either.
func (s *Server) sessionUser(r *http.Request) (store.User, bool) {
	cookie, err := r.Cookie(sessionCookie)
	if err != nil || cookie.Value == "" {
		return store.User{}, false
	}
	user, err := s.store.SessionUser(r.Context(), hashToken(cookie.Value))
	if err != nil {
		return store.User{}, false
	}
	return user, true
}

func (s *Server) requireSession(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		user, ok := s.sessionUser(r)
		if !ok {
			// Clear whatever was sent, so a browser holding an expired value stops
			// sending one that will never work again.
			if c, err := r.Cookie(sessionCookie); err == nil && c.Value != "" {
				s.clearSession(w)
			}
			s.redirectToLogin(w, r)
			return
		}
		ctx := context.WithValue(r.Context(), userKey, user)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

// getRoot answers for whoever arrives at the one address somebody is handed.
//
// Three different people reach it and they want three different things: an
// instance with no account on it yet needs setting up, somebody signed in wants
// their saves, and everybody else is reading about this for the first time and
// needs to know what it is and where the console builds are.
//
// The third case is why this is not a redirect to the sign in form. The address a
// console is paired against is the address a person types into a browser, and a
// sign in form answers none of the questions they arrived with.
func (s *Server) getRoot(w http.ResponseWriter, r *http.Request) {
	if n, err := s.store.CountUsers(r.Context()); err == nil && n == 0 {
		http.Redirect(w, r, "/setup", http.StatusSeeOther)
		return
	}
	if user, ok := s.sessionUser(r); ok {
		s.getDashboard(w, r.WithContext(context.WithValue(r.Context(), userKey, user)))
		return
	}
	// Asked here rather than in render, so the panel's pages never wait on
	// GitHub to draw a screen that has nothing to do with it.
	s.render(w, r, "welcome.html", page{
		Title:     "web.welcome.title",
		Indexable: true,
		InstallQR: s.installAvailable(r.Context()),
	})
}

func (s *Server) redirectToLogin(w http.ResponseWriter, r *http.Request) {
	n, err := s.store.CountUsers(r.Context())
	if err == nil && n == 0 {
		http.Redirect(w, r, "/setup", http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

// secureCookie marks the cookie Secure only when the request arrived over TLS.
// Setting it unconditionally would break a plain http instance on a home network
// silently: the browser would accept the cookie and never send it back.
func (s *Server) setSession(w http.ResponseWriter, r *http.Request, token string) {
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookie,
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		Secure:   r.TLS != nil,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   int(sessionTTL / time.Second),
	})
}

func (s *Server) clearSession(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{
		Name: sessionCookie, Value: "", Path: "/", HttpOnly: true, MaxAge: -1,
	})
}

// ------------------------------------------------------------------ rendering

/* What every page needs regardless of what it is about.
 *
 * The sidebar names the consoles and counts the saves on every screen, so it
 * cannot come from whichever handler happens to be rendering. Filled once, here,
 * rather than by each handler remembering to. */
type nav struct {
	Devices []store.DeviceInfo
	// Live rather than all: the sidebar count answers "how many consoles do I
	// have", and a revoked one is a row in a history, not a console.
	LiveDevices int
	Titles      int
}

/* One appearance choice, as the sidebar draws it. Label is a key like everything
 * else the panel puts on a screen. */
type themeChoice struct {
	ID      string
	Label   string
	Current bool
}

/* One language, named in itself rather than in the language being left. */
type langChoice struct {
	ID      i18n.Lang
	Label   string
	Current bool
}

type page struct {
	// Title and Error are keys from shared/lang/, not sentences. A key with no
	// translation renders as itself, which is what lets a page be titled after a
	// title id.
	Title  string
	Page   string // which sidebar entry is the current one
	Path   string // where to come back to after changing something about the page
	Theme  string // "" for auto, which lets the stylesheet follow the system
	Themes []themeChoice
	Lang   i18n.Lang
	Langs  []langChoice
	User   store.User
	Error  string
	// SignUpOpen decides whether the login page offers a way to make an account.
	// A link to a page that would refuse is worse than no link.
	SignUpOpen bool
	Nav        nav
	Downloads  downloads
	// InstallQR decides whether the welcome page offers the scan to install code.
	// A code that cannot resolve to a build is worse than no code: somebody finds
	// that out holding a console, after going to fetch one.
	InstallQR bool
	// Only the public landing page belongs in a search index. Every account,
	// setup and panel page gets noindex from the shared document head.
	Indexable bool
	SEO       seoData
	Data      any
}

/* Where the builds are.
 *
 * The release page rather than a file. The assets are named after the commit they
 * were built from, so there is no stable file name to link to - and adding one
 * would put a `daemoon.cia` on somebody's card that cannot say which build it is,
 * which is the thing that naming was chosen to avoid.
 *
 * Constants rather than configuration. A self hosted instance runs somebody's own
 * server; it does not fork the software, and the one address the builds come from
 * is the same for every instance. A fork changes a line here.
 */
const (
	repoSlug   = "mirusu400/DaeMoon"
	repoURL    = "https://github.com/" + repoSlug
	buildsURL  = repoURL + "/releases/tag/nightly"
	releaseAPI = "https://api.github.com/repos/" + repoSlug + "/releases/tags/nightly"
)

type downloads struct {
	ThreeDS string
	Switch  string
	Server  string
	Source  string
}

// T resolves a key in the page's language. Templates call it as {{$.T "key"}},
// which is why it is a method and not a template function: a function would need
// the language passed at every call site.
func (p page) T(key string) string { return i18n.T(p.Lang, key) }

// Tf is T with {0}, {1} ... substituted.
func (p page) Tf(key string, args ...any) string { return i18n.Tf(p.Lang, key, args...) }

func (s *Server) render(w http.ResponseWriter, r *http.Request, name string, p page) {
	p.User = userOf(r)

	p.Path = r.URL.Path
	p.Theme = themeOf(r)
	p.Themes = themeChoices(p.Theme)
	p.Lang = i18n.Of(r)
	p.Langs = langChoices(p.Lang)
	if p.Indexable {
		p.SEO = landingSEO(p.Lang)
	}
	p.SignUpOpen = s.store.OpenRegistration(r.Context())
	p.Downloads = downloads{
		ThreeDS: buildsURL, Switch: buildsURL, Server: buildsURL, Source: repoURL,
	}

	// A signed out page has no sidebar and nothing to count.
	if p.User.ID != "" {
		if devices, err := s.store.ListDevices(r.Context(), p.User.ID); err == nil {
			p.Nav.Devices = devices
			for _, d := range devices {
				if !d.Revoked {
					p.Nav.LiveDevices++
				}
			}
		}
		if titles, err := s.store.ListTitles(r.Context(), p.User.ID, ""); err == nil {
			p.Nav.Titles = len(titles)
		}
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Language", string(p.Lang))
	if err := s.tpl.ExecuteTemplate(w, name, p); err != nil {
		// The status line is already out by the time a template fails partway, so
		// this can only be logged.
		slog.ErrorContext(r.Context(), "render", "template", name, "err", err)
	}
}

func humanBytes(n uint64) string {
	switch {
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MiB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.0f KiB", float64(n)/(1<<10))
	default:
		return fmt.Sprintf("%d B", n)
	}
}

// ------------------------------------------------------------------ theme

/* Appearance, in a cookie rather than in local storage.
 *
 * The panel is server rendered and this is the only piece of state a page needs
 * before it draws anything. Reading it from JavaScript would mean the page arrives
 * in one theme and changes to another a frame later, which is the flash every site
 * that does it has. A cookie is already on the request.
 *
 * It is not stored per account either. A theme is a property of the screen somebody
 * is looking at, not of who they are, and the same person on a phone and a desktop
 * can reasonably want different answers.
 */
const themeCookie = "daemoon_theme"

func themeOf(r *http.Request) string {
	c, err := r.Cookie(themeCookie)
	if err != nil {
		return ""
	}
	switch c.Value {
	case "dark", "light":
		return c.Value
	}
	return "" // auto
}

func themeChoices(current string) []themeChoice {
	return []themeChoice{
		{ID: "auto", Label: "web.theme.auto", Current: current == ""},
		{ID: "dark", Label: "web.theme.dark", Current: current == "dark"},
		{ID: "light", Label: "web.theme.light", Current: current == "light"},
	}
}

func (s *Server) postTheme(w http.ResponseWriter, r *http.Request) {
	value := r.FormValue("theme")
	cookie := &http.Cookie{
		Name: themeCookie, Path: "/", HttpOnly: false,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   365 * 24 * 60 * 60,
	}
	switch value {
	case "dark", "light":
		cookie.Value = value
	default:
		// Auto is the absence of a preference, so it is stored as one.
		cookie.MaxAge = -1
	}
	http.SetCookie(w, cookie)
	redirectBack(w, r)
}

// redirectBack returns to the page the form was on.
//
// Only a path from this site. A redirect target taken from a form field is
// somewhere to send somebody if it is not checked, and `//host` is a URL even
// though it starts with a slash.
func redirectBack(w http.ResponseWriter, r *http.Request) {
	back := r.FormValue("from")
	if !strings.HasPrefix(back, "/") || strings.HasPrefix(back, "//") {
		back = "/"
	}
	http.Redirect(w, r, back, http.StatusSeeOther)
}

// ------------------------------------------------------------------ language

/* The same shape as the theme, for the same reasons: a cookie, so the page is
 * rendered in the right language rather than translated after it arrives, and per
 * browser rather than per account, so the login screen can be read by somebody who
 * does not have an account open yet.
 *
 * Without a cookie the browser's Accept-Language decides, which is the case that
 * matters most: a Korean console owner opening this for the first time should not
 * have to find a language menu written in English.
 */
func langChoices(current i18n.Lang) []langChoice {
	out := make([]langChoice, 0, len(i18n.Order))
	for _, l := range i18n.Order {
		out = append(out, langChoice{ID: l, Label: i18n.Names[l], Current: l == current})
	}
	return out
}

func (s *Server) postLang(w http.ResponseWriter, r *http.Request) {
	cookie := &http.Cookie{
		Name: i18n.Cookie, Path: "/", HttpOnly: false,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   365 * 24 * 60 * 60,
	}
	if l := i18n.Lang(r.FormValue("lang")); i18n.Known(l) {
		cookie.Value = string(l)
	} else {
		// Anything else means "stop deciding for me", and the absence of the cookie
		// is what hands the choice back to Accept-Language.
		cookie.MaxAge = -1
	}
	http.SetCookie(w, cookie)
	redirectBack(w, r)
}

// ------------------------------------------------------------------ setup

func (s *Server) getSetup(w http.ResponseWriter, r *http.Request) {
	n, err := s.store.CountUsers(r.Context())
	if err != nil {
		http.Error(w, "database unavailable", http.StatusInternalServerError)
		return
	}
	if n > 0 {
		// Setup is not a page that stays reachable. An instance with an account on
		// it must not offer to make another administrator to whoever asks.
		http.Redirect(w, r, "/login", http.StatusSeeOther)
		return
	}
	s.render(w, r, "setup.html", page{Title: "web.setup.title"})
}

func (s *Server) postSetup(w http.ResponseWriter, r *http.Request) {
	n, err := s.store.CountUsers(r.Context())
	if err != nil || n > 0 {
		http.Redirect(w, r, "/login", http.StatusSeeOther)
		return
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := r.FormValue("password")
	if msg := checkCredentials(username, password); msg != "" {
		s.render(w, r, "setup.html", page{Title: "web.setup.title", Error: msg})
		return
	}

	hash, err := auth.HashPassword(password)
	if err != nil {
		s.render(w, r, "setup.html", page{Title: "web.setup.title",
			Error: "web.err.password_store"})
		return
	}
	user, err := s.store.CreateUser(r.Context(), uuid.NewString(), username, hash, true)
	if err != nil {
		s.render(w, r, "setup.html", page{Title: "web.setup.title",
			Error: "web.err.name_taken"})
		return
	}
	s.startSession(w, r, user, "/")
}

func checkCredentials(username, password string) string {
	switch {
	case username == "":
		return "web.err.name_required"
	case len(username) > 64:
		return "web.err.name_too_long"
	case strings.ContainsAny(username, " \t\n/"):
		return "web.err.name_chars"
	case len(password) < 8:
		// Short enough to type on a phone, long enough not to be a word. A self
		// hosted service on a home network is not the place for a policy nobody
		// can satisfy without a password manager.
		return "web.err.password_short"
	}
	return ""
}

func (s *Server) startSession(w http.ResponseWriter, r *http.Request, user store.User,
	to string) {
	token, err := newSessionToken()
	if err != nil {
		http.Error(w, "could not start a session", http.StatusInternalServerError)
		return
	}
	if err := s.store.CreateSession(r.Context(), hashToken(token), user.ID, sessionTTL); err != nil {
		http.Error(w, "could not start a session", http.StatusInternalServerError)
		return
	}
	s.setSession(w, r, token)
	http.Redirect(w, r, to, http.StatusSeeOther)
}

// ------------------------------------------------------------------ login

func (s *Server) getLogin(w http.ResponseWriter, r *http.Request) {
	if n, err := s.store.CountUsers(r.Context()); err == nil && n == 0 {
		http.Redirect(w, r, "/setup", http.StatusSeeOther)
		return
	}
	s.render(w, r, "login.html", page{Title: "web.login.title"})
}

func (s *Server) postLogin(w http.ResponseWriter, r *http.Request) {
	// Sessions nobody can use are cleared here rather than by a timer: a self
	// hosted instance can go weeks without a request, and a background sweeper
	// would be a goroutine whose whole job is deleting rows that already fail.
	if err := s.store.PurgeExpiredSessions(r.Context()); err != nil {
		slog.WarnContext(r.Context(), "purge sessions", "err", err)
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := r.FormValue("password")

	// One message and one path for every failure. A form that answers "no such
	// user" differently from "wrong password" is a list of who has an account, and
	// answering faster is the same leak told by a stopwatch.
	fail := func() {
		s.render(w, r, "login.html", page{Title: "web.login.title",
			Error: "web.login.failed"})
	}

	user, err := s.store.UserByName(r.Context(), username)
	if err != nil {
		// Still spend the time a real verification would, so an absent account and
		// a wrong password take the same length of time.
		_ = auth.VerifyPassword(dummyHash, password)
		fail()
		return
	}
	hash, err := s.store.PasswordHash(r.Context(), user.ID)
	if err != nil {
		_ = auth.VerifyPassword(dummyHash, password)
		fail()
		return
	}
	if err := auth.VerifyPassword(hash, password); err != nil {
		fail()
		return
	}
	s.startSession(w, r, user, "/")
}

// ------------------------------------------------------------------ sign up

/* Somebody who is not signed in, making their own account.
 *
 * Closed unless an administrator has opened it. That default is not caution for its
 * own sake: this is a save sync server, an open sign up page on an address that is
 * reachable from outside a home network is somewhere for anybody to put data, and
 * the person who installs this is not always the person who decides what the router
 * forwards. The People page has the switch and says what it means.
 *
 * A person who signs up is not an administrator, and the accounts are already
 * separated: ListDevices and ListTitles take a user id, so a new account starts with
 * nothing and can see nothing else.
 */
func (s *Server) getRegister(w http.ResponseWriter, r *http.Request) {
	// A fresh instance has a first account to make, and that is setup's job: it is
	// the page that grants administrator.
	if n, err := s.store.CountUsers(r.Context()); err == nil && n == 0 {
		http.Redirect(w, r, "/setup", http.StatusSeeOther)
		return
	}
	if !s.store.OpenRegistration(r.Context()) {
		// Rendered rather than hidden. Somebody sent here by a friend deserves to be
		// told what to ask for, and the page reveals nothing that /login does not.
		s.render(w, r, "register.html", page{Title: "web.register.title",
			Error: "web.register.closed"})
		return
	}
	s.render(w, r, "register.html", page{Title: "web.register.title"})
}

func (s *Server) postRegister(w http.ResponseWriter, r *http.Request) {
	// Checked again on the way in. The form being drawn is not permission: the
	// setting can change between a page loading and its button being pressed.
	if n, err := s.store.CountUsers(r.Context()); err == nil && n == 0 {
		http.Redirect(w, r, "/setup", http.StatusSeeOther)
		return
	}
	if !s.store.OpenRegistration(r.Context()) {
		s.render(w, r, "register.html", page{Title: "web.register.title",
			Error: "web.register.closed"})
		return
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := r.FormValue("password")
	fail := func(key string) {
		s.render(w, r, "register.html", page{Title: "web.register.title", Error: key})
	}
	if msg := checkCredentials(username, password); msg != "" {
		fail(msg)
		return
	}
	hash, err := auth.HashPassword(password)
	if err != nil {
		fail("web.err.password_store")
		return
	}
	user, err := s.store.CreateUser(r.Context(), uuid.NewString(), username, hash, false)
	if err != nil {
		fail("web.err.name_taken")
		return
	}
	slog.InfoContext(r.Context(), "account created", "who", username, "how", "sign up")
	s.startSession(w, r, user, "/")
}

// A real hash of a value nobody knows, so a login attempt against a name that does
// not exist costs the same as one that does.
var dummyHash = mustHash("daemoon-timing-equalizer")

func mustHash(s string) string {
	h, err := auth.HashPassword(s)
	if err != nil {
		panic(err)
	}
	return h
}

func (s *Server) postLogout(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie(sessionCookie); err == nil && cookie.Value != "" {
		if err := s.store.DeleteSession(r.Context(), hashToken(cookie.Value)); err != nil {
			slog.WarnContext(r.Context(), "delete session", "err", err)
		}
	}
	s.clearSession(w)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

// ------------------------------------------------------------------ errors

// denied refuses a request with a sentence somebody can read.
//
// These are the refusals a person reaches by using the panel - not being an
// administrator, deleting the last one - so they are translated. The `what` strings
// handed to fail below are not: those are internal failures, they are logged
// alongside the error that caused them, and a translated "database unavailable" is
// harder to search for without telling the reader anything more.
func (s *Server) denied(w http.ResponseWriter, r *http.Request, status int, key string) {
	http.Error(w, i18n.T(i18n.Of(r), key), status)
}

func (s *Server) fail(w http.ResponseWriter, r *http.Request, err error, what string) {
	if errors.Is(err, store.ErrNotFound) {
		http.NotFound(w, r)
		return
	}
	slog.ErrorContext(r.Context(), what, "err", err)
	http.Error(w, what, http.StatusInternalServerError)
}
