package web

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"
)

/* Installing on a 3DS by pointing its camera at this page.
 *
 * FBI reads a URL out of a QR code, downloads it and installs it, which is the
 * whole of what a person otherwise does with an SD card reader. The code has to
 * carry a URL a console can fetch, and the release assets are named after the
 * commit they were built from, so there is no fixed GitHub URL to put in one.
 *
 * So the code carries this server's own address and the redirect lives here. Two
 * things come out of that beyond the naming problem:
 *
 *   - The code stays short. A 3DS camera reading a code off a monitor is the
 *     marginal case this has to work in, and every character is more modules in
 *     the same square. `https://example.org/install/3ds.cia` is a fraction of a
 *     GitHub asset URL.
 *   - There is one place to change when the release layout moves, rather than a
 *     QR somebody has already printed.
 *
 * The redirect rather than streaming the file: the bytes are already on a CDN
 * that is better at serving them than a self hosted box on a home connection,
 * and FBI follows redirects.
 */

// releaseTTL is how long a resolved asset URL is reused. Short enough that a
// nightly published minutes ago is what gets installed, long enough that a page
// somebody is reading does not spend a GitHub API call per refresh.
const releaseTTL = 10 * time.Minute

// The unauthenticated GitHub API allows 60 requests an hour per address. One
// call per ten minutes leaves that alone even with several instances behind one
// address.
type releaseCache struct {
	mu  sync.Mutex
	url string
	at  time.Time
	// api is the endpoint to ask. A field rather than the constant inline so a
	// test can point it at a server it controls; empty means the real one.
	api string
}

// assetURLFor returns the download URL of the newest nightly asset whose name
// starts with prefix and ends with suffix.
//
// A stale answer is preferred to no answer: a release that resolved an hour ago
// still installs, and GitHub being unreachable for a minute should not take the
// install QR off a page.
func (c *releaseCache) assetURLFor(ctx context.Context, prefix, suffix string) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.url != "" && time.Since(c.at) < releaseTTL {
		return c.url, nil
	}

	api := c.api
	if api == "" {
		api = releaseAPI
	}
	url, err := resolveAsset(ctx, api, prefix, suffix)
	if err != nil {
		if c.url != "" {
			return c.url, nil
		}
		return "", err
	}
	c.url, c.at = url, time.Now()
	return url, nil
}

func resolveAsset(ctx context.Context, api, prefix, suffix string) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, api, nil)
	if err != nil {
		return "", fmt.Errorf("build release request: %w", err)
	}
	req.Header.Set("Accept", "application/vnd.github+json")

	// A timeout and a ceiling on both the wait and the body, like every other
	// request this binary makes.
	client := &http.Client{Timeout: 10 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", fmt.Errorf("fetch release: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("fetch release: github answered %s", resp.Status)
	}

	var release struct {
		Assets []struct {
			Name string `json:"name"`
			URL  string `json:"browser_download_url"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(nil, resp.Body, 1<<20)).Decode(&release); err != nil {
		return "", fmt.Errorf("decode release: %w", err)
	}

	for _, a := range release.Assets {
		if strings.HasPrefix(a.Name, prefix) && strings.HasSuffix(a.Name, suffix) && a.URL != "" {
			return a.URL, nil
		}
	}
	return "", fmt.Errorf("no asset named %s*%s in the nightly release", prefix, suffix)
}

// The one build a console can install this way. A .3dsx cannot reach another
// title's save archive, so there is nothing else here to offer a 3DS.
const (
	ciaPrefix = "daemoon-3ds-"
	ciaSuffix = ".cia"
	ciaPath   = "/install/3ds.cia"
)

// getInstallCIA sends a console to the current nightly CIA.
//
// A redirect, not a proxy: this is the URL inside the QR code, and what follows
// it is a download a CDN should serve.
func (s *Server) getInstallCIA(w http.ResponseWriter, r *http.Request) {
	url, err := s.releases.assetURLFor(r.Context(), ciaPrefix, ciaSuffix)
	if err != nil {
		// A console shows this to somebody holding it, so it says which side
		// failed rather than only that something did.
		s.fail(w, r, err, "could not find the current 3DS build")
		return
	}
	// Not cached at any hop: the whole point of this URL is that it means
	// whatever the newest build is.
	w.Header().Set("Cache-Control", "no-store")
	http.Redirect(w, r, url, http.StatusFound)
}

// getInstallQR draws the code a 3DS camera reads.
func (s *Server) getInstallQR(w http.ResponseWriter, r *http.Request) {
	writeQRSVG(w, r, requestOrigin(r)+ciaPath)
}

// installAvailable reports whether the QR is worth drawing at all.
//
// Asked while rendering the page, so an instance that cannot reach GitHub shows
// the release link it always showed instead of a code that would fail in
// somebody's hands after they had gone and fetched a console.
func (s *Server) installAvailable(ctx context.Context) bool {
	_, err := s.releases.assetURLFor(ctx, ciaPrefix, ciaSuffix)
	return err == nil
}
