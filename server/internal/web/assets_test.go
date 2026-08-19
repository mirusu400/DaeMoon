package web

import (
	"io/fs"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"
)

// A deploy behind a CDN served new HTML against a stylesheet that had been cached
// for four hours, and every page came out unstyled. The fix is that an asset's URL
// carries a digest of its contents, so these two say what that is worth: a
// template that goes back to a bare path would silently reintroduce it.

var staticRefRe = regexp.MustCompile(`["'(]/static/[^"')]+`)

func TestTemplatesAskForAssetsByDigest(t *testing.T) {
	files, err := assets.ReadDir("templates")
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		raw, err := assets.ReadFile("templates/" + f.Name())
		if err != nil {
			t.Fatal(err)
		}
		for _, m := range staticRefRe.FindAllString(string(raw), -1) {
			t.Errorf("%s: %q is a bare static path; use {{asset \"...\"}} so a cache "+
				"cannot pair new HTML with an old file", f.Name(), strings.TrimLeft(m, `"'(`))
		}
	}
}

func TestEveryAssetHasADigestAndItChangesWithTheBytes(t *testing.T) {
	digests, err := hashAssets()
	if err != nil {
		t.Fatal(err)
	}

	n := 0
	err = fs.WalkDir(assets, "static", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		name := strings.TrimPrefix(path, "static/")
		if digests[name] == "" {
			t.Errorf("no digest for %s", name)
		}
		n++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if n == 0 {
		t.Fatal("no static files were walked at all")
	}

	// The whole point is that two different files cannot share a URL.
	seen := map[string]string{}
	for name, sum := range digests {
		if other, dup := seen[sum]; dup {
			t.Errorf("%s and %s hash to the same digest %s", name, other, sum)
		}
		seen[sum] = name
	}

	if got := assetURL(digests, "style.css"); !strings.HasPrefix(got, "/static/style.css?v=") {
		t.Errorf("assetURL(style.css) = %q", got)
	}
	// An asset nobody hashed still has to render as a usable URL.
	if got := assetURL(digests, "nope.css"); got != "/static/nope.css" {
		t.Errorf("assetURL(nope.css) = %q", got)
	}
}

func TestOnlyADigestedURLIsCachedForever(t *testing.T) {
	h := staticCache(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))

	for _, c := range []struct{ url, want string }{
		{"/static/style.css?v=abc123", "public, max-age=31536000, immutable"},
		{"/static/style.css", "no-cache"},
		// An empty v is not a digest, and the file behind it still moves.
		{"/static/style.css?v=", "no-cache"},
	} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, c.url, nil))
		if got := rec.Header().Get("Cache-Control"); got != c.want {
			t.Errorf("%s: Cache-Control = %q, want %q", c.url, got, c.want)
		}
	}
}
