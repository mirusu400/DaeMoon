package pkgfmt_test

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/pkgfmt"
)

// repoRoot walks up to the directory holding shared/, so the fixtures the C tests
// read are the fixtures these tests read.
func repoRoot(t *testing.T) string {
	t.Helper()

	dir, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "shared", "errors.json")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	t.Fatal("cannot find the repository root from the test working directory")
	return ""
}

func readFixture(t *testing.T, name string) []byte {
	t.Helper()

	b, err := os.ReadFile(filepath.Join(repoRoot(t), "shared", "fixtures", name))
	if err != nil {
		t.Fatalf("read fixture %s: %v", name, err)
	}
	return b
}

type digestFixture struct {
	Cases []struct {
		Name    string `json:"name"`
		Entries []struct {
			Path    string `json:"path"`
			Content string `json:"content_utf8"`
		} `json:"entries"`
		SHA256 string `json:"sha256"`
		Size   uint64 `json:"size"`
	} `json:"cases"`
}

// TestDigestMatchesSharedVectors is the other half of the C test of the same name.
// The expected values in the fixture came from a third implementation, so passing
// here and passing there means the client and the server hash a save identically.
// That is the one thing that cannot be caught later: a mismatch shows up as a
// restore that refuses to run, on someone's console, with a correct save in hand.
func TestDigestMatchesSharedVectors(t *testing.T) {
	var fx digestFixture
	if err := json.Unmarshal(readFixture(t, "payload_digest.json"), &fx); err != nil {
		t.Fatalf("parse payload_digest.json: %v", err)
	}
	if len(fx.Cases) == 0 {
		t.Fatal("payload_digest.json has no cases")
	}

	for _, c := range fx.Cases {
		t.Run(c.Name, func(t *testing.T) {
			content := map[string]string{}
			entries := make([]pkgfmt.Entry, 0, len(c.Entries))
			for _, e := range c.Entries {
				content[e.Path] = e.Content
				entries = append(entries, pkgfmt.Entry{
					Path: e.Path,
					Size: uint64(len(e.Content)),
				})
			}

			got, total, err := pkgfmt.Digest(entries, func(e pkgfmt.Entry) (io.ReadCloser, error) {
				return io.NopCloser(strings.NewReader(content[e.Path])), nil
			})
			if err != nil {
				t.Fatalf("digest: %v", err)
			}
			if got != c.SHA256 {
				t.Errorf("digest = %s, want %s", got, c.SHA256)
			}
			if total != c.Size {
				t.Errorf("size = %d, want %d", total, c.Size)
			}
		})
	}
}

// TestDigestIgnoresInputOrder pins the sort: two consoles listing the same save in
// different directory order have to produce the same digest, or every sync would
// look like a conflict.
func TestDigestIgnoresInputOrder(t *testing.T) {
	content := map[string]string{"b.bin": "second", "a.bin": "first", "c/d.bin": "third"}
	open := func(e pkgfmt.Entry) (io.ReadCloser, error) {
		return io.NopCloser(strings.NewReader(content[e.Path])), nil
	}

	forward := []pkgfmt.Entry{
		{Path: "a.bin", Size: 5}, {Path: "b.bin", Size: 6}, {Path: "c/d.bin", Size: 5},
	}
	reversed := []pkgfmt.Entry{
		{Path: "c/d.bin", Size: 5}, {Path: "b.bin", Size: 6}, {Path: "a.bin", Size: 5},
	}

	a, _, err := pkgfmt.Digest(forward, open)
	if err != nil {
		t.Fatalf("digest forward: %v", err)
	}
	b, _, err := pkgfmt.Digest(reversed, open)
	if err != nil {
		t.Fatalf("digest reversed: %v", err)
	}
	if a != b {
		t.Errorf("order changed the digest: %s vs %s", a, b)
	}
}

// TestDigestIsLengthPrefixed guards the delimiter. Without the size in the stream,
// two different sets of files could hash the same.
func TestDigestIsLengthPrefixed(t *testing.T) {
	one := []pkgfmt.Entry{{Path: "a", Size: 2}, {Path: "b", Size: 1}}
	two := []pkgfmt.Entry{{Path: "a", Size: 1}, {Path: "b", Size: 2}}
	content := map[string][]string{
		"one": {"xy", "z"},
		"two": {"x", "yz"},
	}

	mk := func(key string) func(pkgfmt.Entry) (io.ReadCloser, error) {
		i := 0
		return func(pkgfmt.Entry) (io.ReadCloser, error) {
			s := content[key][i]
			i++
			return io.NopCloser(strings.NewReader(s)), nil
		}
	}

	a, _, err := pkgfmt.Digest(one, mk("one"))
	if err != nil {
		t.Fatalf("digest one: %v", err)
	}
	b, _, err := pkgfmt.Digest(two, mk("two"))
	if err != nil {
		t.Fatalf("digest two: %v", err)
	}
	if a == b {
		t.Error("different payloads produced the same digest")
	}
}

func TestParseManifestFixtures(t *testing.T) {
	t.Run("valid", func(t *testing.T) {
		m, err := pkgfmt.ParseManifest(readFixture(t, "manifest_valid.json"))
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		if m.Platform != pkgfmt.Platform3DS || m.TitleID != "0004000000055D00" {
			t.Errorf("unexpected identity: %+v", m)
		}
		if m.Version != 42 || m.Parent() != 41 {
			t.Errorf("version %d parent %d, want 42 and 41", m.Version, m.Parent())
		}
	})

	t.Run("first upload", func(t *testing.T) {
		m, err := pkgfmt.ParseManifest(readFixture(t, "manifest_first_upload.json"))
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		if m.ParentVersion != nil {
			t.Errorf("parent_version = %v, want null", *m.ParentVersion)
		}
		// A non ASCII label has to survive: it is what the other console shows in
		// the conflict dialog.
		if m.DeviceLabel != "리빙룸 스위치" {
			t.Errorf("device_label = %q", m.DeviceLabel)
		}
	})

	t.Run("rejects a newer format", func(t *testing.T) {
		// Guessing at a layout this build does not know is how a save gets written
		// back wrong.
		_, err := pkgfmt.ParseManifest(readFixture(t, "manifest_future_format.json"))
		if err == nil {
			t.Fatal("a format_version of 2 was accepted")
		}
	})

	t.Run("rejects a parent that is not older", func(t *testing.T) {
		_, err := pkgfmt.ParseManifest(readFixture(t, "manifest_bad_parent.json"))
		if !errors.Is(err, pkgfmt.ErrManifest) {
			t.Fatalf("err = %v, want ErrManifest", err)
		}
	})
}

// BuildPackage assembles a save package the way the client does.
func buildPackage(t *testing.T, m pkgfmt.Manifest, files map[string]string) []byte {
	t.Helper()

	entries := make([]pkgfmt.Entry, 0, len(files))
	for path, body := range files {
		entries = append(entries, pkgfmt.Entry{Path: path, Size: uint64(len(body))})
	}
	digest, total, err := pkgfmt.Digest(entries, func(e pkgfmt.Entry) (io.ReadCloser, error) {
		return io.NopCloser(strings.NewReader(files[e.Path])), nil
	})
	if err != nil {
		t.Fatalf("digest: %v", err)
	}
	m.SHA256 = digest
	m.Size = total

	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for path, body := range files {
		w, err := zw.Create(pkgfmt.PayloadDir + path)
		if err != nil {
			t.Fatalf("create entry: %v", err)
		}
		if _, err := io.WriteString(w, body); err != nil {
			t.Fatalf("write entry: %v", err)
		}
	}
	mw, err := zw.Create(pkgfmt.ManifestPath)
	if err != nil {
		t.Fatalf("create manifest: %v", err)
	}
	if err := json.NewEncoder(mw).Encode(m); err != nil {
		t.Fatalf("encode manifest: %v", err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("close zip: %v", err)
	}
	return buf.Bytes()
}

func sampleManifest() pkgfmt.Manifest {
	return pkgfmt.Manifest{
		FormatVersion: pkgfmt.FormatVersion,
		Platform:      pkgfmt.Platform3DS,
		TitleID:       "0004000000055D00",
		SaveType:      pkgfmt.SaveData,
		Version:       0,
		DeviceLabel:   "test console",
		CreatedAt:     "2026-01-01T00:00:00Z",
	}
}

func TestInspectAcceptsAWellFormedPackage(t *testing.T) {
	blob := buildPackage(t, sampleManifest(), map[string]string{
		"main.sav":      "hello world",
		"sub/extra.bin": "more data",
	})

	m, err := pkgfmt.Inspect(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatalf("inspect: %v", err)
	}
	if m.Size != 20 {
		t.Errorf("size = %d, want 20", m.Size)
	}
}

func TestInspectRejectsATamperedPayload(t *testing.T) {
	// The manifest says one thing and the payload is another. The server refuses
	// rather than storing it, because a package it keeps is one it will hand back
	// to a console later.
	m := sampleManifest()
	blob := buildPackage(t, m, map[string]string{"main.sav": "hello world"})

	// Rebuild with the same manifest but different contents.
	var replaced bytes.Buffer
	zr, err := zip.NewReader(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatalf("read package: %v", err)
	}
	zw := zip.NewWriter(&replaced)
	for _, f := range zr.File {
		w, err := zw.Create(f.Name)
		if err != nil {
			t.Fatalf("create entry: %v", err)
		}
		rc, err := f.Open()
		if err != nil {
			t.Fatalf("open entry: %v", err)
		}
		if f.Name == pkgfmt.PayloadDir+"main.sav" {
			_, err = io.WriteString(w, "hello w0rld")
		} else {
			_, err = io.Copy(w, rc)
		}
		_ = rc.Close()
		if err != nil {
			t.Fatalf("copy entry: %v", err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("close zip: %v", err)
	}

	_, err = pkgfmt.Inspect(bytes.NewReader(replaced.Bytes()), int64(replaced.Len()))
	if !errors.Is(err, pkgfmt.ErrChecksum) {
		t.Fatalf("err = %v, want ErrChecksum", err)
	}
}

func TestInspectRejectsUnsafeEntryPaths(t *testing.T) {
	// A package can arrive from a share code, which means from anyone.
	for _, bad := range []string{"../escape.sav", "/absolute.sav", `back\slash.sav`} {
		var buf bytes.Buffer
		zw := zip.NewWriter(&buf)
		w, err := zw.Create(pkgfmt.PayloadDir + bad)
		if err != nil {
			t.Fatalf("create entry: %v", err)
		}
		if _, err := io.WriteString(w, "x"); err != nil {
			t.Fatalf("write entry: %v", err)
		}
		mw, err := zw.Create(pkgfmt.ManifestPath)
		if err != nil {
			t.Fatalf("create manifest: %v", err)
		}
		if err := json.NewEncoder(mw).Encode(sampleManifest()); err != nil {
			t.Fatalf("encode manifest: %v", err)
		}
		if err := zw.Close(); err != nil {
			t.Fatalf("close zip: %v", err)
		}

		if _, err := pkgfmt.Inspect(bytes.NewReader(buf.Bytes()), int64(buf.Len())); err == nil {
			t.Errorf("entry path %q was accepted", bad)
		}
	}
}

func TestInspectRejectsAPackageWithNoManifest(t *testing.T) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	w, err := zw.Create(pkgfmt.PayloadDir + "main.sav")
	if err != nil {
		t.Fatalf("create entry: %v", err)
	}
	if _, err := io.WriteString(w, "data"); err != nil {
		t.Fatalf("write entry: %v", err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("close zip: %v", err)
	}

	if _, err := pkgfmt.Inspect(bytes.NewReader(buf.Bytes()), int64(buf.Len())); err == nil {
		t.Fatal("a package with no manifest was accepted")
	}
}
