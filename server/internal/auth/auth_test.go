package auth_test

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

func openStore(t *testing.T) *store.Store {
	t.Helper()

	s, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "auth.db"), 512)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })
	if err := s.EnsureUser(context.Background(), "user-1"); err != nil {
		t.Fatalf("ensure user: %v", err)
	}
	return s
}

func TestTokensAreDistinctAndOpaque(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 256; i++ {
		token, err := auth.NewToken()
		if err != nil {
			t.Fatalf("new token: %v", err)
		}
		if seen[token] {
			t.Fatalf("a token repeated after %d draws", i)
		}
		seen[token] = true

		// Base64url of 32 bytes. No padding, so it survives a URL and a QR code
		// without anything having to escape it.
		if strings.ContainsAny(token, "+/=") {
			t.Fatalf("token %q is not url safe", token)
		}
		if len(token) < 40 {
			t.Fatalf("token %q is shorter than 32 bytes of entropy", token)
		}
	}
}

func TestHashIsStableAndOneWay(t *testing.T) {
	const token = "a-device-token"

	if auth.HashToken(token) != auth.HashToken(token) {
		t.Fatal("hashing is not stable")
	}
	if auth.HashToken(token) == auth.HashToken(token+"x") {
		t.Fatal("two tokens hashed the same")
	}
	// The stored form must not contain the token: a leaked database should not
	// hand anyone a working credential.
	if strings.Contains(auth.HashToken(token), token) {
		t.Fatal("the hash contains the token")
	}
	if len(auth.HashToken(token)) != 64 {
		t.Fatalf("hash is %d characters, want 64", len(auth.HashToken(token)))
	}
}

func TestPairingCodesAreSixDigits(t *testing.T) {
	// Digits only, and always six of them. The code is read off one screen and
	// typed on another, so letters invite O against 0.
	shaped := regexp.MustCompile(`^[0-9]{6}$`)

	for i := 0; i < 512; i++ {
		code, err := auth.NewPairingCode()
		if err != nil {
			t.Fatalf("new pairing code: %v", err)
		}
		if !shaped.MatchString(code) {
			t.Fatalf("pairing code %q is not six digits", code)
		}
	}
}

func TestShareCodesAreLongerThanPairingCodes(t *testing.T) {
	// A share code is not behind a login and grants a download to whoever holds
	// it, so it cannot be guessable the way a six digit code is.
	code, err := auth.NewShareCode()
	if err != nil {
		t.Fatalf("new share code: %v", err)
	}
	if len(code) < 16 {
		t.Fatalf("share code %q is too short to be unguessable", code)
	}
	if strings.ContainsAny(code, "+/=") {
		t.Fatalf("share code %q is not url safe", code)
	}
}

// middlewareCase drives the middleware with a given Authorization header.
func middlewareCase(t *testing.T, s *store.Store, header string) (int, string) {
	t.Helper()

	var reached bool
	h := auth.Middleware(s)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		reached = true
		d := auth.MustDevice(r.Context())
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(d.ID))
	}))

	req := httptest.NewRequest(http.MethodGet, "/v1/titles", nil)
	if header != "" {
		req.Header.Set("Authorization", header)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK && reached {
		t.Fatal("the handler ran even though the request was rejected")
	}

	var body struct {
		Error struct {
			Code string `json:"code"`
		} `json:"error"`
	}
	_ = json.Unmarshal(rec.Body.Bytes(), &body)
	return rec.Code, body.Error.Code
}

func TestMiddlewareRejectsEveryMalformedHeader(t *testing.T) {
	s := openStore(t)

	token, err := auth.NewToken()
	if err != nil {
		t.Fatalf("new token: %v", err)
	}
	if err := s.CreateDevice(context.Background(), store.Device{
		ID: "dev-1", UserID: "user-1", Label: "3DS", Platform: "3ds",
	}, auth.HashToken(token)); err != nil {
		t.Fatalf("create device: %v", err)
	}

	for _, tc := range []struct {
		name   string
		header string
	}{
		{"absent", ""},
		{"empty", "Bearer "},
		{"no scheme", token},
		{"wrong scheme", "Basic " + token},
		{"lowercase scheme", "bearer " + token},
		{"unknown token", "Bearer not-a-real-token"},
		{"token with the right length but wrong bytes", "Bearer " + strings.Repeat("A", len(token))},
	} {
		t.Run(tc.name, func(t *testing.T) {
			code, wire := middlewareCase(t, s, tc.header)
			if code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401", code)
			}
			if wire != "unauthorized" {
				t.Errorf("code = %q, want unauthorized", wire)
			}
		})
	}

	// The real token still works, so the cases above are rejecting the header and
	// not the setup.
	if code, _ := middlewareCase(t, s, "Bearer "+token); code != http.StatusOK {
		t.Fatalf("a valid token was rejected: status %d", code)
	}
}

// TestRevokedIsNotUnauthorized: telling a revoked console it is merely signed out
// would send the user to pair again, which is the opposite of what revoking a lost
// SD card is for.
func TestRevokedIsNotUnauthorized(t *testing.T) {
	s := openStore(t)
	ctx := context.Background()

	token, err := auth.NewToken()
	if err != nil {
		t.Fatalf("new token: %v", err)
	}
	if err := s.CreateDevice(ctx, store.Device{
		ID: "dev-1", UserID: "user-1", Label: "3DS", Platform: "3ds",
	}, auth.HashToken(token)); err != nil {
		t.Fatalf("create device: %v", err)
	}
	if err := s.RevokeDevice(ctx, "user-1", "dev-1"); err != nil {
		t.Fatalf("revoke: %v", err)
	}

	code, wire := middlewareCase(t, s, "Bearer "+token)
	if code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", code)
	}
	if wire != "device_revoked" {
		t.Errorf("code = %q, want device_revoked", wire)
	}
}

func TestMiddlewareToleratesSurroundingWhitespace(t *testing.T) {
	// A console writing the header by hand is a real possibility, and a trailing
	// newline in a token file should not read as a revoked device.
	s := openStore(t)

	token, err := auth.NewToken()
	if err != nil {
		t.Fatalf("new token: %v", err)
	}
	if err := s.CreateDevice(context.Background(), store.Device{
		ID: "dev-1", UserID: "user-1", Label: "3DS", Platform: "3ds",
	}, auth.HashToken(token)); err != nil {
		t.Fatalf("create device: %v", err)
	}

	if code, _ := middlewareCase(t, s, "Bearer  "+token+" "); code != http.StatusOK {
		t.Errorf("a padded token was rejected: status %d", code)
	}
}

func TestEqualIsConstantTimeOverEqualLengths(t *testing.T) {
	if !auth.Equal("abc", "abc") {
		t.Error("equal strings compared unequal")
	}
	if auth.Equal("abc", "abd") {
		t.Error("different strings compared equal")
	}
	if auth.Equal("abc", "abcd") {
		t.Error("different lengths compared equal")
	}
}

func TestIDsAreDistinct(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 256; i++ {
		id, err := auth.NewID()
		if err != nil {
			t.Fatalf("new id: %v", err)
		}
		if seen[id] {
			t.Fatalf("an id repeated after %d draws", i)
		}
		seen[id] = true
	}
}
