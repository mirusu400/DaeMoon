package turnstile

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestVerifyAcceptsARegisterToken(t *testing.T) {
	var sawSecret, sawToken bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			t.Errorf("method = %s", r.Method)
		}
		if got := r.Header.Get("Content-Type"); got != "application/x-www-form-urlencoded" {
			t.Errorf("Content-Type = %q", got)
		}
		if err := r.ParseForm(); err != nil {
			t.Errorf("ParseForm: %v", err)
		}
		sawSecret = r.Form.Get("secret") == "server-secret"
		sawToken = r.Form.Get("response") == "browser-token"
		if _, err := w.Write([]byte(`{"success":true,"action":"register"}`)); err != nil {
			t.Errorf("write response: %v", err)
		}
	}))
	defer srv.Close()

	c := New("server-secret")
	c.endpoint = srv.URL
	c.http = srv.Client()
	if err := c.Verify(context.Background(), "browser-token"); err != nil {
		t.Fatalf("Verify: %v", err)
	}
	if !sawSecret || !sawToken {
		t.Fatalf("Siteverify form = secret:%v token:%v", sawSecret, sawToken)
	}
}

func TestVerifyRejectsInvalidAndWrongActionTokens(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
	}{
		{"challenge rejected", `{"success":false,"error-codes":["invalid-input-response"]}`},
		{"wrong action", `{"success":true,"action":"login"}`},
		{"missing action", `{"success":true}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				if _, err := w.Write([]byte(tc.body)); err != nil {
					t.Errorf("write response: %v", err)
				}
			}))
			defer srv.Close()

			c := New("server-secret")
			c.endpoint = srv.URL
			c.http = srv.Client()
			if err := c.Verify(context.Background(), "browser-token"); !errors.Is(err, ErrRejected) {
				t.Fatalf("Verify error = %v, want ErrRejected", err)
			}
		})
	}
}

func TestVerifyRejectsMissingAndOversizedTokensWithoutANetworkCall(t *testing.T) {
	calls := 0
	srv := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		calls++
	}))
	defer srv.Close()

	c := New("server-secret")
	c.endpoint = srv.URL
	c.http = srv.Client()
	for _, token := range []string{"", "   ", strings.Repeat("x", maxTokenLength+1)} {
		if err := c.Verify(context.Background(), token); !errors.Is(err, ErrRejected) {
			t.Errorf("Verify(%d bytes) = %v, want ErrRejected", len(token), err)
		}
	}
	if calls != 0 {
		t.Fatalf("Siteverify calls = %d, want 0", calls)
	}
}

func TestVerifyFailsClosedOnAServiceError(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		body   string
	}{
		{"HTTP error", http.StatusBadGateway, `{}`},
		{"invalid JSON", http.StatusOK, `{`},
		{"oversized response", http.StatusOK, strings.Repeat("x", maxResponseBytes+1)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(tc.status)
				if _, err := w.Write([]byte(tc.body)); err != nil {
					t.Errorf("write response: %v", err)
				}
			}))
			defer srv.Close()

			c := New("server-secret")
			c.endpoint = srv.URL
			c.http = srv.Client()
			if err := c.Verify(context.Background(), "browser-token"); err == nil {
				t.Fatal("Verify accepted a service failure")
			} else if errors.Is(err, ErrRejected) {
				t.Fatalf("Verify error = %v, want a service error", err)
			}
		})
	}
}
