package qr_test

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/qr"
)

// The payloads written out as fixtures. A pairing URL is what this exists for;
// the rest bracket it - the shortest thing worth encoding, something with the
// characters a URL actually contains, and enough length to force a larger
// version with more than one error correction block.
var cases = []struct {
	name string
	text string
}{
	{"short", "DaeMoon"},
	{"pair-url", "https://daemoon.example/pair#K7QW-2M9X"},
	{"punctuation", "http://192.168.1.13:8080/pair?code=493747&d=3ds"},
	{"long", strings.Repeat("DaeMoon-pairing-", 8)},
	// The real thing, in the exact shape the panel serves. The C tests decode this
	// one and then hand it to core's parser, so the whole pairing payload - encoder,
	// decoder and parser, three implementations - is checked end to end without a
	// console.
	{"pair-payload", "DAEMOON|1|https://192.168.1.13:8443|493747"},
	{"pair-payload-plain", "DAEMOON|1|http://192.168.1.13:8080|000042"},
}

// Encoded once and read back by the C side. See tools/test/test_qr.c: quirc is a
// separate implementation in a different language, so it disagreeing with this
// one is worth far more than this one agreeing with itself.
func TestWriteFixturesForQuircToDecode(t *testing.T) {
	root := os.Getenv("DAEMOON_ROOT")
	if root == "" {
		root = filepath.Join("..", "..", "..")
	}
	dir := filepath.Join(root, "shared", "fixtures", "qr")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("create %s: %v", dir, err)
	}

	for _, tc := range cases {
		code, err := qr.Encode([]byte(tc.text))
		if err != nil {
			t.Fatalf("%s: encode: %v", tc.name, err)
		}

		// Portable bitmap, ASCII. One byte per module is wasteful and completely
		// legible in a diff, which is what a committed fixture should be.
		var buf bytes.Buffer
		fmt.Fprintf(&buf, "P1\n# %s\n%d %d\n", tc.text, code.Size, code.Size)
		for y := 0; y < code.Size; y++ {
			for x := 0; x < code.Size; x++ {
				if code.At(x, y) {
					buf.WriteString("1")
				} else {
					buf.WriteString("0")
				}
			}
			buf.WriteString("\n")
		}

		path := filepath.Join(dir, tc.name+".pbm")
		old, err := os.ReadFile(path)
		if err == nil && bytes.Equal(old, buf.Bytes()) {
			continue
		}
		if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
			t.Fatalf("%s: write: %v", tc.name, err)
		}
		t.Logf("%s: fixture updated", tc.name)
	}
}

func TestEncodeRefusesWhatItCannotHold(t *testing.T) {
	if _, err := qr.Encode(bytes.Repeat([]byte("x"), 4096)); err == nil {
		t.Fatal("expected a refusal for a payload past version 10")
	}
}

// Every version this package claims to support has to actually produce a grid,
// because a missing entry in the specification tables is silent otherwise.
func TestEveryVersionEncodes(t *testing.T) {
	seen := map[int]bool{}
	for n := 1; n <= 200; n++ {
		code, err := qr.Encode(bytes.Repeat([]byte("A"), n))
		if err != nil {
			break
		}
		version := (code.Size - 17) / 4
		if code.Size != 17+version*4 {
			t.Fatalf("%d bytes: size %d is not a valid version", n, code.Size)
		}
		seen[version] = true
	}
	for v := 1; v <= 10; v++ {
		if !seen[v] {
			t.Errorf("no payload length produced version %d", v)
		}
	}
}
