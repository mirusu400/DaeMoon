// Package pkgfmt reads save packages and manifests.
//
// It is the Go half of core/src/manifest.c and core/src/archive.c. The two are kept
// honest by shared/fixtures, which both test suites read: if the client and the
// server ever disagree about what a manifest says or how a payload hashes, one of
// the suites fails instead of a save being written back wrong on a console.
package pkgfmt

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"sort"
	"strings"
)

const (
	// FormatVersion is the only manifest layout this build understands. A package
	// from a newer build is refused rather than guessed at.
	FormatVersion = 1

	ManifestPath = "manifest.json"
	PayloadDir   = "payload/"

	// MaxManifestBytes mirrors DAEMOON_MANIFEST_MAX_BYTES.
	MaxManifestBytes = 2048
	// MaxEntries mirrors DAEMOON_ARCHIVE_MAX_ENTRIES.
	MaxEntries = 192
)

// Platform and SaveType are spelled exactly as they go over the wire.
type Platform string

const (
	Platform3DS Platform = "3ds"
	PlatformNX  Platform = "nx"
	PlatformNDS Platform = "nds"
)

func (p Platform) Valid() bool {
	switch p {
	case Platform3DS, PlatformNX, PlatformNDS:
		return true
	}
	return false
}

type SaveType string

const (
	SaveData SaveType = "savedata"
	ExtData  SaveType = "extdata"
	SaveNDS  SaveType = "nds"
)

func (s SaveType) Valid() bool {
	switch s {
	case SaveData, ExtData, SaveNDS:
		return true
	}
	return false
}

// Manifest is manifest.json. See shared/manifest.schema.json.
type Manifest struct {
	FormatVersion int      `json:"format_version"`
	Platform      Platform `json:"platform"`
	TitleID       string   `json:"title_id"`
	SaveType      SaveType `json:"save_type"`
	Version       uint32   `json:"version"`
	// ParentVersion is null for a first upload. 0 means the same thing and is what
	// the C client writes when it has never synced.
	ParentVersion *uint32 `json:"parent_version"`
	SHA256        string  `json:"sha256"`
	Size          uint64  `json:"size"`
	DeviceLabel   string  `json:"device_label"`
	// CreatedAt is informational and never used for ordering: the console RTC is
	// user settable.
	CreatedAt string `json:"created_at"`
}

// Parent returns ParentVersion with null flattened to 0.
func (m *Manifest) Parent() uint32 {
	if m.ParentVersion == nil {
		return 0
	}
	return *m.ParentVersion
}

var (
	titleIDRe = regexp.MustCompile(`^[0-9A-Z_-]{4,32}$`)
	sha256Re  = regexp.MustCompile(`^[0-9a-f]{64}$`)

	ErrFormatVersion = errors.New("unsupported manifest format_version")
	ErrManifest      = errors.New("invalid manifest")
	ErrArchive       = errors.New("invalid save package")
	ErrChecksum      = errors.New("payload digest does not match the manifest")
)

// Validate applies the same checks as daemoon_manifest_validate.
func (m *Manifest) Validate() error {
	if m.FormatVersion != FormatVersion {
		return fmt.Errorf("%w: %d", ErrFormatVersion, m.FormatVersion)
	}
	if !m.Platform.Valid() {
		return fmt.Errorf("%w: platform %q", ErrManifest, m.Platform)
	}
	if !m.SaveType.Valid() {
		return fmt.Errorf("%w: save_type %q", ErrManifest, m.SaveType)
	}
	if !titleIDRe.MatchString(m.TitleID) {
		return fmt.Errorf("%w: title_id %q", ErrManifest, m.TitleID)
	}
	if !sha256Re.MatchString(m.SHA256) {
		// Digests are compared as text, so there is one canonical spelling.
		return fmt.Errorf("%w: sha256 %q", ErrManifest, m.SHA256)
	}
	if m.DeviceLabel == "" {
		return fmt.Errorf("%w: empty device_label", ErrManifest)
	}
	if len(m.DeviceLabel) > 64 {
		return fmt.Errorf("%w: device_label too long", ErrManifest)
	}
	// Versions are server issued and strictly increase.
	if m.Version != 0 && m.Parent() >= m.Version {
		return fmt.Errorf("%w: parent_version %d is not older than version %d",
			ErrManifest, m.Parent(), m.Version)
	}
	return nil
}

// ParseManifest reads manifest.json.
func ParseManifest(b []byte) (*Manifest, error) {
	if len(b) > MaxManifestBytes {
		return nil, fmt.Errorf("%w: %d bytes", ErrManifest, len(b))
	}
	var m Manifest
	dec := json.NewDecoder(strings.NewReader(string(b)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&m); err != nil {
		// An unknown field is how a newer format_version usually shows up first.
		return nil, fmt.Errorf("%w: %v", ErrManifest, err)
	}
	if err := m.Validate(); err != nil {
		return nil, err
	}
	return &m, nil
}

// Entry is one payload file.
type Entry struct {
	Path string
	Size uint64
}

// safePath rejects anything that could escape a save root or confuse a console
// filesystem. A package can arrive from a share code, so its entry paths are
// treated as hostile input.
func safePath(p string) bool {
	if p == "" || len(p) >= 256 {
		return false
	}
	if strings.HasPrefix(p, "/") || strings.Contains(p, "..") {
		return false
	}
	if strings.ContainsAny(p, `\:`) || strings.Contains(p, "//") || strings.HasSuffix(p, "/") {
		return false
	}
	for _, r := range p {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return true
}

// Digest computes the payload digest defined in core/include/daemoon/archive.h:
// entries sorted by raw path bytes, and for each one the path, a NUL, the size as
// eight big endian bytes, then the contents.
//
// open returns a reader for one entry. It is called once per entry, in order.
func Digest(entries []Entry, open func(Entry) (io.ReadCloser, error)) (string, uint64, error) {
	sorted := make([]Entry, len(entries))
	copy(sorted, entries)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Path < sorted[j].Path })

	h := sha256.New()
	var total uint64
	var sizeBuf [8]byte

	for _, e := range sorted {
		h.Write([]byte(e.Path))
		h.Write([]byte{0})
		binary.BigEndian.PutUint64(sizeBuf[:], e.Size)
		h.Write(sizeBuf[:])

		rc, err := open(e)
		if err != nil {
			return "", 0, fmt.Errorf("open %q: %w", e.Path, err)
		}
		n, err := io.Copy(h, rc)
		closeErr := rc.Close()
		if err != nil {
			return "", 0, fmt.Errorf("read %q: %w", e.Path, err)
		}
		if closeErr != nil {
			return "", 0, fmt.Errorf("close %q: %w", e.Path, closeErr)
		}
		if uint64(n) != e.Size {
			return "", 0, fmt.Errorf("%w: %q is %d bytes, listed as %d",
				ErrArchive, e.Path, n, e.Size)
		}
		total += uint64(n)
	}
	return hex.EncodeToString(h.Sum(nil)), total, nil
}

// Inspect reads a package and returns its manifest, having checked that the
// payload actually hashes to what the manifest claims.
//
// The server verifies uploads because a package it stores is one it will hand back
// to a console later. Accepting a package whose digest is wrong would mean the
// client's pre-restore check fails on some other day, on some other console, with
// no way to tell what went wrong.
func Inspect(r io.ReaderAt, size int64) (*Manifest, error) {
	zr, err := zip.NewReader(r, size)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrArchive, err)
	}

	var manifestFile *zip.File
	byPath := make(map[string]*zip.File)
	entries := make([]Entry, 0, 8)

	for _, f := range zr.File {
		name := f.Name
		if name == ManifestPath {
			manifestFile = f
			continue
		}
		if strings.HasSuffix(name, "/") {
			continue // directory entry
		}
		if !strings.HasPrefix(name, PayloadDir) {
			return nil, fmt.Errorf("%w: unexpected entry %q", ErrArchive, name)
		}
		rel := strings.TrimPrefix(name, PayloadDir)
		if !safePath(rel) {
			return nil, fmt.Errorf("%w: unsafe entry path %q", ErrArchive, rel)
		}
		if _, dup := byPath[rel]; dup {
			return nil, fmt.Errorf("%w: duplicate entry %q", ErrArchive, rel)
		}
		if len(entries) >= MaxEntries {
			return nil, fmt.Errorf("%w: more than %d entries", ErrArchive, MaxEntries)
		}
		byPath[rel] = f
		entries = append(entries, Entry{Path: rel, Size: f.UncompressedSize64})
	}

	if manifestFile == nil {
		return nil, fmt.Errorf("%w: no %s", ErrManifest, ManifestPath)
	}
	if manifestFile.UncompressedSize64 > MaxManifestBytes {
		return nil, fmt.Errorf("%w: %s is %d bytes", ErrManifest, ManifestPath,
			manifestFile.UncompressedSize64)
	}

	mr, err := manifestFile.Open()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrArchive, err)
	}
	raw, err := io.ReadAll(io.LimitReader(mr, MaxManifestBytes+1))
	closeErr := mr.Close()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrArchive, err)
	}
	if closeErr != nil {
		return nil, fmt.Errorf("%w: %v", ErrArchive, closeErr)
	}

	m, err := ParseManifest(raw)
	if err != nil {
		return nil, err
	}

	digest, total, err := Digest(entries, func(e Entry) (io.ReadCloser, error) {
		return byPath[e.Path].Open()
	})
	if err != nil {
		return nil, err
	}
	if digest != m.SHA256 || total != m.Size {
		return nil, fmt.Errorf("%w: payload is %s (%d bytes), manifest says %s (%d bytes)",
			ErrChecksum, digest, total, m.SHA256, m.Size)
	}
	return m, nil
}
