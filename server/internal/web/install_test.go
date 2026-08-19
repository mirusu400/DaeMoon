package web

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

const nightlyJSON = `{"assets":[
  {"name":"SHA256SUMS","browser_download_url":"https://example.test/SHA256SUMS"},
  {"name":"daemoon-switch-abc1234.nro","browser_download_url":"https://example.test/nro"},
  {"name":"daemoon-3ds-abc1234.cia","browser_download_url":"https://example.test/cia"},
  {"name":"daemoond-linux-amd64-abc1234","browser_download_url":"https://example.test/server"}
]}`

func fakeGitHub(t *testing.T, body string, status int, hits *int) *releaseCache {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits != nil {
			*hits++
		}
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	}))
	t.Cleanup(srv.Close)
	return &releaseCache{api: srv.URL}
}

// The release carries four assets and only one of them is a CIA. Picking by
// position, or by "the first thing that ends in .cia", is how somebody ends up
// installing SHA256SUMS.
func TestTheCIAIsPickedOutOfTheRelease(t *testing.T) {
	c := fakeGitHub(t, nightlyJSON, http.StatusOK, nil)
	got, err := c.assetURLFor(context.Background(), ciaPrefix, ciaSuffix)
	if err != nil {
		t.Fatal(err)
	}
	if got != "https://example.test/cia" {
		t.Errorf("resolved %q", got)
	}
}

func TestAReleaseWithNoCIAIsAnError(t *testing.T) {
	c := fakeGitHub(t, `{"assets":[{"name":"SHA256SUMS","browser_download_url":"https://x.test/s"}]}`,
		http.StatusOK, nil)
	if _, err := c.assetURLFor(context.Background(), ciaPrefix, ciaSuffix); err == nil {
		t.Fatal("a release with no CIA in it resolved to something")
	}
}

// One call per ten minutes is what keeps this inside the unauthenticated rate
// limit, so the caching is the feature and not an optimisation.
func TestTheAnswerIsCached(t *testing.T) {
	hits := 0
	c := fakeGitHub(t, nightlyJSON, http.StatusOK, &hits)
	for i := 0; i < 5; i++ {
		if _, err := c.assetURLFor(context.Background(), ciaPrefix, ciaSuffix); err != nil {
			t.Fatal(err)
		}
	}
	if hits != 1 {
		t.Errorf("asked GitHub %d times for five lookups", hits)
	}
}

// A page that has resolved once must not lose its install code because GitHub
// answered 502 while somebody was reading it.
func TestAStaleAnswerBeatsNoAnswer(t *testing.T) {
	fail := false
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if fail {
			w.WriteHeader(http.StatusBadGateway)
			return
		}
		_, _ = w.Write([]byte(nightlyJSON))
	}))
	defer srv.Close()

	c := &releaseCache{api: srv.URL}
	if _, err := c.assetURLFor(context.Background(), ciaPrefix, ciaSuffix); err != nil {
		t.Fatal(err)
	}
	fail = true
	c.at = time.Now().Add(-2 * releaseTTL) // expire it

	got, err := c.assetURLFor(context.Background(), ciaPrefix, ciaSuffix)
	if err != nil {
		t.Fatalf("a working instance lost its build URL to one bad response: %v", err)
	}
	if got != "https://example.test/cia" {
		t.Errorf("resolved %q", got)
	}
}

// What FBI actually gets: a redirect to the file, and a code holding a URL
// short enough for a console camera to read off a screen.
func TestTheInstallURLRedirectsToTheBuild(t *testing.T) {
	s := &Server{releases: fakeGitHub(t, nightlyJSON, http.StatusOK, nil)}

	rec := httptest.NewRecorder()
	s.getInstallCIA(rec, httptest.NewRequest(http.MethodGet, ciaPath, nil))

	if rec.Code != http.StatusFound {
		t.Fatalf("status %d, want 302", rec.Code)
	}
	if got := rec.Header().Get("Location"); got != "https://example.test/cia" {
		t.Errorf("Location = %q", got)
	}
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("Cache-Control = %q; this URL means whatever the newest build is", got)
	}
}

func TestTheQRHoldsThisServersOwnInstallURL(t *testing.T) {
	s := &Server{}
	req := httptest.NewRequest(http.MethodGet, "/install/3ds.qr.svg", nil)
	req.Host = "daemoon.example"
	req.Header.Set("X-Forwarded-Proto", "https")

	rec := httptest.NewRecorder()
	s.getInstallQR(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "image/svg+xml" {
		t.Errorf("Content-Type = %q", ct)
	}
	if !strings.HasPrefix(rec.Body.String(), "<svg") {
		t.Errorf("body does not start with <svg")
	}

	// The payload itself, checked where it is built rather than by decoding the
	// picture: TLS in front means the scheme has to come from the header.
	if got := requestOrigin(req) + ciaPath; got != "https://daemoon.example/install/3ds.cia" {
		t.Errorf("QR would carry %q", got)
	}
}
