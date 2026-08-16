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
// Nothing here is localized, and that is on purpose: the rules say the server
// returns codes and clients render text. This *is* a client - the only one written
// in Go - so it holds English sentences and the consoles hold the translations.
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
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/config"
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
	store *store.Store
	cfg   config.Config
	tpl   *template.Template
}

func New(st *store.Store, cfg config.Config) (*Server, error) {
	tpl, err := template.New("").Funcs(template.FuncMap{
		"bytes": humanBytes,
	}).ParseFS(assets, "templates/*.html")
	if err != nil {
		return nil, fmt.Errorf("parse templates: %w", err)
	}
	return &Server{store: st, cfg: cfg, tpl: tpl}, nil
}

func (s *Server) Routes() chi.Router {
	r := chi.NewRouter()

	r.Handle("/static/*", http.FileServer(http.FS(assets)))

	r.Get("/setup", s.getSetup)
	r.Post("/setup", s.postSetup)
	r.Get("/login", s.getLogin)
	r.Post("/login", s.postLogin)
	r.Post("/logout", s.postLogout)

	r.Group(func(r chi.Router) {
		r.Use(s.requireSession)
		r.Get("/", s.getDashboard)
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

func (s *Server) requireSession(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie(sessionCookie)
		if err != nil || cookie.Value == "" {
			s.redirectToLogin(w, r)
			return
		}
		user, err := s.store.SessionUser(r.Context(), hashToken(cookie.Value))
		if err != nil {
			// Expired or revoked. Clear the cookie so the browser stops sending a
			// value that will never work again.
			s.clearSession(w)
			s.redirectToLogin(w, r)
			return
		}
		ctx := context.WithValue(r.Context(), userKey, user)
		next.ServeHTTP(w, r.WithContext(ctx))
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
	Titles  int
}

type page struct {
	Title string
	Page  string // which sidebar entry is the current one
	User  store.User
	Error string
	Nav   nav
	Data  any
}

func (s *Server) render(w http.ResponseWriter, r *http.Request, name string, p page) {
	p.User = userOf(r)

	// A signed out page has no sidebar and nothing to count.
	if p.User.ID != "" {
		if devices, err := s.store.ListDevices(r.Context(), p.User.ID); err == nil {
			p.Nav.Devices = devices
		}
		if titles, err := s.store.ListTitles(r.Context(), p.User.ID, ""); err == nil {
			p.Nav.Titles = len(titles)
		}
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
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
	s.render(w, r, "setup.html", page{Title: "Set up DaeMoon"})
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
		s.render(w, r, "setup.html", page{Title: "Set up DaeMoon", Error: msg})
		return
	}

	hash, err := auth.HashPassword(password)
	if err != nil {
		s.render(w, r, "setup.html", page{Title: "Set up DaeMoon",
			Error: "The password could not be stored."})
		return
	}
	user, err := s.store.CreateUser(r.Context(), uuid.NewString(), username, hash, true)
	if err != nil {
		s.render(w, r, "setup.html", page{Title: "Set up DaeMoon",
			Error: "That name is taken."})
		return
	}
	s.startSession(w, r, user, "/")
}

func checkCredentials(username, password string) string {
	switch {
	case username == "":
		return "A name is required."
	case len(username) > 64:
		return "That name is too long."
	case strings.ContainsAny(username, " \t\n/"):
		return "A name cannot contain spaces or slashes."
	case len(password) < 8:
		// Short enough to type on a phone, long enough not to be a word. A self
		// hosted service on a home network is not the place for a policy nobody
		// can satisfy without a password manager.
		return "The password must be at least 8 characters."
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
	s.render(w, r, "login.html", page{Title: "Sign in"})
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
		s.render(w, r, "login.html", page{Title: "Sign in",
			Error: "That name and password do not match."})
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

func (s *Server) fail(w http.ResponseWriter, r *http.Request, err error, what string) {
	if errors.Is(err, store.ErrNotFound) {
		http.NotFound(w, r)
		return
	}
	slog.ErrorContext(r.Context(), what, "err", err)
	http.Error(w, what, http.StatusInternalServerError)
}
