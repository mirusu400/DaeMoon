package pkgfmt

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strings"
)

// Build assembles a save package, filling in the manifest's digest and size the
// same way the client does.
//
// The server never builds a package in production: packages come from consoles.
// This exists so the Go tests can produce one that the C side would also accept,
// and so future tooling (an importer, a migration script) has one definition of
// the layout to work from rather than a second one written from the spec.
func Build(m Manifest, files map[string][]byte) ([]byte, error) {
	paths := make([]string, 0, len(files))
	for p := range files {
		if !safePath(p) {
			return nil, fmt.Errorf("%w: unsafe entry path %q", ErrArchive, p)
		}
		paths = append(paths, p)
	}
	sort.Strings(paths)

	entries := make([]Entry, 0, len(paths))
	for _, p := range paths {
		entries = append(entries, Entry{Path: p, Size: uint64(len(files[p]))})
	}

	digest, total, err := Digest(entries, func(e Entry) (io.ReadCloser, error) {
		return io.NopCloser(bytes.NewReader(files[e.Path])), nil
	})
	if err != nil {
		return nil, err
	}
	m.FormatVersion = FormatVersion
	m.SHA256 = digest
	m.Size = total

	if err := m.Validate(); err != nil {
		return nil, err
	}

	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, p := range paths {
		w, err := zw.Create(PayloadDir + p)
		if err != nil {
			return nil, fmt.Errorf("create entry %q: %w", p, err)
		}
		if _, err := w.Write(files[p]); err != nil {
			return nil, fmt.Errorf("write entry %q: %w", p, err)
		}
	}

	mw, err := zw.Create(ManifestPath)
	if err != nil {
		return nil, fmt.Errorf("create %s: %w", ManifestPath, err)
	}
	raw, err := json.Marshal(m)
	if err != nil {
		return nil, fmt.Errorf("encode %s: %w", ManifestPath, err)
	}
	if len(raw) > MaxManifestBytes {
		return nil, fmt.Errorf("%w: %d bytes", ErrManifest, len(raw))
	}
	if _, err := mw.Write(raw); err != nil {
		return nil, fmt.Errorf("write %s: %w", ManifestPath, err)
	}

	if err := zw.Close(); err != nil {
		return nil, fmt.Errorf("finish package: %w", err)
	}
	return buf.Bytes(), nil
}

// StringFiles is a convenience for callers that have text rather than bytes.
func StringFiles(files map[string]string) map[string][]byte {
	out := make(map[string][]byte, len(files))
	for k, v := range files {
		out[k] = []byte(strings.Clone(v))
	}
	return out
}
