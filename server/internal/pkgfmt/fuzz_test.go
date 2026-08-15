package pkgfmt_test

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/pkgfmt"
)

// A save package can arrive from a share code, which means from a stranger, and
// `GET /v1/shares/{code}` has no authentication in front of it by design. The
// parsers are the first thing that touches those bytes.
//
// The property being fuzzed is not "no crash" alone. It is that every outcome is
// one of the declared errors, because an undeclared one becomes internal_error and
// a 500, and a 500 on hostile input is how a denial of service gets found.

func FuzzParseManifest(f *testing.F) {
	f.Add([]byte(`{"format_version":1,"platform":"3ds","title_id":"0004000000055D00",` +
		`"save_type":"savedata","version":1,"parent_version":null,` +
		`"sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",` +
		`"size":0,"device_label":"x","created_at":"1970-01-01T00:00:00Z"}`))
	f.Add([]byte(`{}`))
	f.Add([]byte(`{"format_version":99999999999999999999}`))
	f.Add([]byte(`{"format_version":1,"size":-1}`))
	f.Add([]byte("["))
	f.Add([]byte(""))
	// A label that is not valid UTF-8, a lone surrogate escape, and an embedded
	// NUL. All three are things a hand written package can carry and a console
	// would have to render.
	f.Add([]byte("{\"format_version\":1,\"device_label\":\"\\xff\\xfe\"}"))
	f.Add([]byte("{\"format_version\":1,\"device_label\":\"\\ud800\"}"))
	f.Add([]byte("{\"format_version\":1,\"device_label\":\"a\\u0000b\"}"))

	f.Fuzz(func(t *testing.T, data []byte) {
		m, err := pkgfmt.ParseManifest(data)
		if err != nil {
			if !isDeclared(err) {
				t.Fatalf("undeclared error %v for input %q", err, data)
			}
			return
		}
		// Anything that parses has to satisfy the same invariants the rest of the
		// server assumes, or a later stage gets a struct it was promised could not
		// exist.
		if err := m.Validate(); err != nil {
			t.Fatalf("ParseManifest accepted a manifest that does not validate: %v", err)
		}
		if m.FormatVersion != pkgfmt.FormatVersion {
			t.Fatalf("accepted format_version %d", m.FormatVersion)
		}
		if m.Version != 0 && m.Parent() >= m.Version {
			t.Fatalf("accepted parent %d against version %d", m.Parent(), m.Version)
		}
		if len(m.SHA256) != 64 {
			t.Fatalf("accepted a %d character digest", len(m.SHA256))
		}
	})
}

func FuzzInspect(f *testing.F) {
	good, err := pkgfmt.Build(pkgfmt.Manifest{
		Platform:    pkgfmt.Platform3DS,
		TitleID:     "0004000000055D00",
		SaveType:    pkgfmt.SaveData,
		DeviceLabel: "seed",
		CreatedAt:   "1970-01-01T00:00:00Z",
	}, map[string][]byte{"main.sav": []byte("hello")})
	if err != nil {
		f.Fatalf("build the seed package: %v", err)
	}

	f.Add(good)
	f.Add([]byte("PK\x03\x04"))
	f.Add([]byte("not a zip at all"))
	f.Add([]byte{})
	// Truncations: a download cut off partway is the realistic damaged input.
	if len(good) > 64 {
		f.Add(good[:len(good)/2])
		f.Add(good[:len(good)-1])
	}

	f.Fuzz(func(t *testing.T, data []byte) {
		m, err := pkgfmt.Inspect(bytes.NewReader(data), int64(len(data)))
		if err != nil {
			if !isDeclared(err) {
				t.Fatalf("undeclared error %v", err)
			}
			return
		}
		// A package that Inspect accepts is one the server will store and hand back
		// to a console. Everything the console relies on has to already be true.
		if err := m.Validate(); err != nil {
			t.Fatalf("Inspect accepted a manifest that does not validate: %v", err)
		}
		if len(m.SHA256) != 64 {
			t.Fatalf("accepted a %d character digest", len(m.SHA256))
		}
	})
}

// isDeclared reports whether err is one of the failures the API layer knows how to
// turn into a client error code. Anything else reaches the client as a 500.
func isDeclared(err error) bool {
	return errors.Is(err, pkgfmt.ErrManifest) ||
		errors.Is(err, pkgfmt.ErrFormatVersion) ||
		errors.Is(err, pkgfmt.ErrArchive) ||
		errors.Is(err, pkgfmt.ErrChecksum)
}

// TestBuildRefusesUnsafePaths keeps the builder honest too: it is what the tests
// and any future tooling produce packages with, and a package that cannot be
// written is far better than one that cannot be safely extracted.
func TestBuildRefusesUnsafePaths(t *testing.T) {
	for _, bad := range []string{
		"../escape.sav",
		"/absolute.sav",
		`back\slash.sav`,
		"drive:name.sav",
		"trailing/",
		"double//slash.sav",
		"",
		strings.Repeat("a", 300),
	} {
		_, err := pkgfmt.Build(pkgfmt.Manifest{
			Platform:    pkgfmt.Platform3DS,
			TitleID:     "0004000000055D00",
			SaveType:    pkgfmt.SaveData,
			DeviceLabel: "x",
			CreatedAt:   "1970-01-01T00:00:00Z",
		}, map[string][]byte{bad: []byte("x")})
		if err == nil {
			t.Errorf("Build accepted the entry path %q", bad)
		}
	}
}
