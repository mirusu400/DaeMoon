package web

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/i18n"
	"github.com/mirusu400/DaeMoon/server/internal/store"
	"github.com/mirusu400/DaeMoon/server/internal/turnstile"
)

type fakeCaptcha struct {
	err   error
	token string
	calls int
}

func (f *fakeCaptcha) Verify(_ context.Context, token string) error {
	f.calls++
	f.token = token
	return f.err
}

func openRegistrationServer(t *testing.T, withTurnstile bool) (*Server, *store.Store) {
	t.Helper()
	cfg := config.Default()
	cfg.Database = filepath.Join(t.TempDir(), "register.db")
	if withTurnstile {
		cfg.TurnstileSiteKey = "public-site-key"
		cfg.TurnstileSecretKey = "must-not-appear-in-html"
	}

	st, err := store.Open(context.Background(), cfg.Database, cfg.BlobChunkSize)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() {
		if err := st.Close(); err != nil {
			t.Errorf("close store: %v", err)
		}
	})
	if _, err := st.CreateUser(context.Background(), "admin", "admin", "hash", true); err != nil {
		t.Fatalf("create administrator: %v", err)
	}
	if err := st.SetOpenRegistration(context.Background(), true); err != nil {
		t.Fatalf("open registration: %v", err)
	}

	s, err := New(st, cfg)
	if err != nil {
		t.Fatalf("new web server: %v", err)
	}
	return s, st
}

func registerRequest(token string) *http.Request {
	form := url.Values{
		"username":              {"new-user"},
		"password":              {"long-enough-password"},
		"cf-turnstile-response": {token},
	}
	r := httptest.NewRequest(http.MethodPost, "/register", strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	return r
}

func TestRegisterPageRendersOnlyThePublicTurnstileKey(t *testing.T) {
	s, _ := openRegistrationServer(t, true)
	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/register", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	body := rec.Body.String()
	for _, want := range []string{
		"https://challenges.cloudflare.com/turnstile/v0/api.js",
		`data-sitekey="public-site-key"`,
		`data-action="register"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("register page does not contain %q", want)
		}
	}
	if strings.Contains(body, "must-not-appear-in-html") {
		t.Error("Turnstile secret was rendered into the page")
	}
}

func TestRegisterRequiresAnAcceptedTurnstileToken(t *testing.T) {
	s, st := openRegistrationServer(t, true)
	fake := &fakeCaptcha{err: turnstile.ErrRejected}
	s.captcha = fake
	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, registerRequest("rejected-token"))

	if fake.calls != 1 || fake.token != "rejected-token" {
		t.Fatalf("captcha calls = %d, token = %q", fake.calls, fake.token)
	}
	if !strings.Contains(rec.Body.String(), i18n.T(i18n.Default, "web.err.captcha")) {
		t.Error("register page does not explain the failed security check")
	}
	if n, err := st.CountUsers(context.Background()); err != nil {
		t.Fatalf("count users: %v", err)
	} else if n != 1 {
		t.Fatalf("users = %d, rejected captcha created an account", n)
	}
}

func TestRegisterCreatesAnAccountAfterTurnstileAccepts(t *testing.T) {
	s, st := openRegistrationServer(t, true)
	fake := &fakeCaptcha{}
	s.captcha = fake
	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, registerRequest("accepted-token"))

	if rec.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if fake.calls != 1 || fake.token != "accepted-token" {
		t.Fatalf("captcha calls = %d, token = %q", fake.calls, fake.token)
	}
	if _, err := st.UserByName(context.Background(), "new-user"); err != nil {
		t.Fatalf("new account was not created: %v", err)
	}
}

func TestRegisterStaysUsableWhenTurnstileIsNotConfigured(t *testing.T) {
	s, st := openRegistrationServer(t, false)
	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, registerRequest(""))

	if rec.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if _, err := st.UserByName(context.Background(), "new-user"); err != nil {
		t.Fatalf("new account was not created: %v", err)
	}
}

func TestRegisterFailsClosedWhenTurnstileIsUnavailable(t *testing.T) {
	s, st := openRegistrationServer(t, true)
	s.captcha = &fakeCaptcha{err: errors.New("network unavailable")}
	rec := httptest.NewRecorder()
	s.Routes().ServeHTTP(rec, registerRequest("token"))

	if n, err := st.CountUsers(context.Background()); err != nil {
		t.Fatalf("count users: %v", err)
	} else if n != 1 {
		t.Fatalf("users = %d, service failure created an account", n)
	}
}
