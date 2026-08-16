// Package store is every SQL statement in the server. Handlers stay thin and none
// of them contain SQL.
//
// Blobs live in SQLite rather than on the filesystem on purpose: self hosting means
// the entire service state is one file to back up, an upload becomes transactional
// with its metadata, and an orphaned file on disk cannot happen.
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"sort"
	"time"

	_ "modernc.org/sqlite" // pure Go driver: cross compiles without cgo

	"github.com/mirusu400/DaeMoon/server/migrations"
)

var (
	ErrNotFound = errors.New("not found")
	// ErrConflict means parent_version did not match the server's latest. Nothing
	// was discarded; the caller turns this into a 409 with both sides described.
	ErrConflict = errors.New("version conflict")
	ErrExpired  = errors.New("expired")
)

type Store struct {
	db        *sql.DB
	chunkSize int
	now       func() time.Time
}

// Open connects, applies migrations, and returns a ready store.
func Open(ctx context.Context, path string, chunkSize int) (*Store, error) {
	if chunkSize <= 0 {
		return nil, fmt.Errorf("chunk size must be positive, got %d", chunkSize)
	}

	// WAL so a reader streaming a blob out does not block an upload, and a
	// generous busy timeout because a self hosted instance is usually on modest
	// storage where a checkpoint can take a moment.
	dsn := path + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(10000)" +
		"&_pragma=foreign_keys(ON)&_pragma=synchronous(NORMAL)"

	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	// SQLite takes one writer at a time. Letting the pool grow only turns a queue
	// into a pile of busy errors.
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)

	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("ping %s: %w", path, err)
	}

	s := &Store{db: db, chunkSize: chunkSize, now: time.Now}
	if err := s.migrate(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error { return s.db.Close() }

// DB exposes the handle for tests that want to look at rows directly.
func (s *Store) DB() *sql.DB { return s.db }

func (s *Store) migrate(ctx context.Context) error {
	if _, err := s.db.ExecContext(ctx,
		`CREATE TABLE IF NOT EXISTS schema_migrations (
			name       TEXT PRIMARY KEY,
			applied_at TEXT NOT NULL
		) STRICT`); err != nil {
		return fmt.Errorf("create schema_migrations: %w", err)
	}

	entries, err := fs.Glob(migrations.FS, "*.sql")
	if err != nil {
		return fmt.Errorf("list migrations: %w", err)
	}
	sort.Strings(entries) // numbered prefixes, so lexical order is apply order

	for _, name := range entries {
		var seen int
		err := s.db.QueryRowContext(ctx,
			`SELECT count(*) FROM schema_migrations WHERE name = ?`, name).Scan(&seen)
		if err != nil {
			return fmt.Errorf("check migration %s: %w", name, err)
		}
		if seen > 0 {
			continue
		}

		body, err := migrations.FS.ReadFile(name)
		if err != nil {
			return fmt.Errorf("read migration %s: %w", name, err)
		}

		tx, err := s.db.BeginTx(ctx, nil)
		if err != nil {
			return fmt.Errorf("begin migration %s: %w", name, err)
		}
		if _, err := tx.ExecContext(ctx, string(body)); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("apply migration %s: %w", name, err)
		}
		if _, err := tx.ExecContext(ctx,
			`INSERT INTO schema_migrations (name, applied_at) VALUES (?, ?)`,
			name, s.timestamp()); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("record migration %s: %w", name, err)
		}
		if err := tx.Commit(); err != nil {
			return fmt.Errorf("commit migration %s: %w", name, err)
		}
	}
	return nil
}

func (s *Store) timestamp() string {
	return s.now().UTC().Format(time.RFC3339)
}

// ------------------------------------------------------------------ users

func (s *Store) EnsureUser(ctx context.Context, id string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO users (id, created_at) VALUES (?, ?) ON CONFLICT (id) DO NOTHING`,
		id, s.timestamp())
	if err != nil {
		return fmt.Errorf("ensure user %s: %w", id, err)
	}
	return nil
}

// ---------------------------------------------------------------- devices

type Device struct {
	ID       string
	UserID   string
	Label    string
	Platform string
	Revoked  bool
}

func (s *Store) CreateDevice(ctx context.Context, d Device, tokenHash string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO devices (id, user_id, label, platform, token_hash, created_at)
		 VALUES (?, ?, ?, ?, ?, ?)`,
		d.ID, d.UserID, d.Label, d.Platform, tokenHash, s.timestamp())
	if err != nil {
		return fmt.Errorf("create device %s: %w", d.ID, err)
	}
	return nil
}

// DeviceByTokenHash looks a device up by the hash of its token. The token itself
// is never stored.
func (s *Store) DeviceByTokenHash(ctx context.Context, tokenHash string) (Device, error) {
	var d Device
	var revokedAt sql.NullString

	err := s.db.QueryRowContext(ctx,
		`SELECT id, user_id, label, platform, revoked_at FROM devices WHERE token_hash = ?`,
		tokenHash).Scan(&d.ID, &d.UserID, &d.Label, &d.Platform, &revokedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return d, ErrNotFound
	}
	if err != nil {
		return d, fmt.Errorf("lookup device by token: %w", err)
	}
	d.Revoked = revokedAt.Valid
	return d, nil
}

// RevokeDevice is idempotent: revoking twice is not an error, because a console
// that lost its SD card may be revoked from two places at once.
// RotateDeviceToken replaces a device's token in place.
//
// A console pairing again is the same console, and this keeps it as one row rather
// than a new one beside the old. The previous token stops working the moment this
// commits - which is the point: two live credentials for one console is a thing
// nobody asked for and cannot see.
//
// The label comes along because it is the console's own name for itself and may
// have changed since the last pairing.
func (s *Store) RotateDeviceToken(ctx context.Context, deviceID, tokenHash,
	label string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE devices SET token_hash = ?, label = ?, revoked_at = NULL
		  WHERE id = ?`,
		tokenHash, label, deviceID)
	if err != nil {
		return fmt.Errorf("rotate device token: %w", err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) RevokeDevice(ctx context.Context, userID, deviceID string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE devices SET revoked_at = ? WHERE id = ? AND user_id = ? AND revoked_at IS NULL`,
		s.timestamp(), deviceID, userID)
	if err != nil {
		return fmt.Errorf("revoke device %s: %w", deviceID, err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("revoke device %s: %w", deviceID, err)
	}
	if n == 0 {
		// Either it does not exist, belongs to someone else, or was already
		// revoked. Distinguishing the first two would leak whether an id exists.
		var exists int
		if err := s.db.QueryRowContext(ctx,
			`SELECT count(*) FROM devices WHERE id = ? AND user_id = ?`,
			deviceID, userID).Scan(&exists); err != nil {
			return fmt.Errorf("revoke device %s: %w", deviceID, err)
		}
		if exists == 0 {
			return ErrNotFound
		}
	}
	return nil
}

// ----------------------------------------------------------------- titles

type TitleSummary struct {
	TitleID  string `json:"title_id"`
	Platform string `json:"platform"`
	SaveType string `json:"save_type"`
	// TitleName is what the console calls the game, when it said. Omitted from the
	// wire when unknown: informational, never keyed by, and absent from every title
	// synced before consoles started sending it.
	TitleName     string `json:"title_name,omitempty"`
	LatestVersion uint32 `json:"latest_version"`
	UpdatedAt     string `json:"updated_at"`
}

func (s *Store) ListTitles(ctx context.Context, userID, platform string) ([]TitleSummary, error) {
	query := `SELECT title_id, platform, save_type, title_name, latest_version,
	                 updated_at
	          FROM titles WHERE user_id = ?`
	args := []any{userID}
	if platform != "" {
		query += ` AND platform = ?`
		args = append(args, platform)
	}
	query += ` ORDER BY platform, title_id`

	rows, err := s.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list titles: %w", err)
	}
	defer rows.Close()

	out := []TitleSummary{}
	for rows.Next() {
		var t TitleSummary
		var name sql.NullString

		if err := rows.Scan(&t.TitleID, &t.Platform, &t.SaveType, &name,
			&t.LatestVersion, &t.UpdatedAt); err != nil {
			return nil, fmt.Errorf("scan title: %w", err)
		}
		t.TitleName = name.String
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("list titles: %w", err)
	}
	return out, nil
}

// --------------------------------------------------------------- versions

type VersionMeta struct {
	TitleID       string  `json:"title_id"`
	Platform      string  `json:"platform"`
	Version       uint32  `json:"version"`
	ParentVersion *uint32 `json:"parent_version"`
	SHA256        string  `json:"sha256"`
	Size          uint64  `json:"size"`
	DeviceLabel   string  `json:"device_label"`
	ReceivedAt    string  `json:"received_at"`

	blobID int64
}

const versionSelect = `
	SELECT t.title_id, t.platform, v.version, v.parent_version, b.sha256, b.size,
	       v.device_label, v.received_at, v.blob_id
	  FROM versions v
	  JOIN titles t ON t.id = v.title_row_id
	  JOIN blobs  b ON b.id = v.blob_id`

func scanVersion(row interface{ Scan(...any) error }) (VersionMeta, error) {
	var m VersionMeta
	var parent sql.NullInt64

	if err := row.Scan(&m.TitleID, &m.Platform, &m.Version, &parent, &m.SHA256, &m.Size,
		&m.DeviceLabel, &m.ReceivedAt, &m.blobID); err != nil {
		return m, err
	}
	if parent.Valid {
		p := uint32(parent.Int64)
		m.ParentVersion = &p
	}
	return m, nil
}

// Latest returns the newest version of a title, or ErrNotFound when the server has
// never seen it. Never uploaded is a normal state, not a failure.
func (s *Store) Latest(ctx context.Context, userID, platform, titleID string) (VersionMeta, error) {
	row := s.db.QueryRowContext(ctx, versionSelect+`
		 WHERE t.user_id = ? AND t.platform = ? AND t.title_id = ?
		   AND v.version = t.latest_version`, userID, platform, titleID)

	m, err := scanVersion(row)
	if errors.Is(err, sql.ErrNoRows) {
		return m, ErrNotFound
	}
	if err != nil {
		return m, fmt.Errorf("latest version of %s/%s: %w", platform, titleID, err)
	}
	return m, nil
}

// Version returns one specific version.
func (s *Store) Version(ctx context.Context, userID, platform, titleID string,
	version uint32) (VersionMeta, error) {
	row := s.db.QueryRowContext(ctx, versionSelect+`
		 WHERE t.user_id = ? AND t.platform = ? AND t.title_id = ? AND v.version = ?`,
		userID, platform, titleID, version)

	m, err := scanVersion(row)
	if errors.Is(err, sql.ErrNoRows) {
		return m, ErrNotFound
	}
	if err != nil {
		return m, fmt.Errorf("version %d of %s/%s: %w", version, platform, titleID, err)
	}
	return m, nil
}

// PutRequest is one upload. The package itself is read from Body, which must be
// positioned at the start.
type PutRequest struct {
	UserID        string
	DeviceID      string
	DeviceLabel   string
	Platform      string
	TitleID       string
	SaveType      string
	ParentVersion uint32
	SHA256        string
	Size          uint64
	// TitleName is what the console calls the game. Optional: a package written
	// before consoles sent one has none, and a backend with no names never will.
	TitleName string

	Body io.Reader
}

// Put stores a new version.
//
// The parent check and the insert happen in one transaction, so two consoles
// uploading at the same moment cannot both win: one gets a version, the other gets
// ErrConflict and its save is still on its own SD card, untouched.
func (s *Store) Put(ctx context.Context, req PutRequest) (VersionMeta, error) {
	var out VersionMeta

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return out, fmt.Errorf("begin upload: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	var titleRowID int64
	var latest uint32
	err = tx.QueryRowContext(ctx,
		`SELECT id, latest_version FROM titles
		  WHERE user_id = ? AND platform = ? AND title_id = ?`,
		req.UserID, req.Platform, req.TitleID).Scan(&titleRowID, &latest)

	switch {
	case errors.Is(err, sql.ErrNoRows):
		if req.ParentVersion != 0 {
			// The client thinks it is continuing from a version this server does
			// not have. That is a conflict, not a first upload.
			return out, ErrConflict
		}
		res, err := tx.ExecContext(ctx,
			`INSERT INTO titles (user_id, title_id, platform, save_type, title_name,
			                     latest_version, updated_at)
			 VALUES (?, ?, ?, ?, ?, 0, ?)`,
			req.UserID, req.TitleID, req.Platform, req.SaveType,
			nullIfEmpty(req.TitleName), s.timestamp())
		if err != nil {
			return out, fmt.Errorf("create title %s/%s: %w", req.Platform, req.TitleID, err)
		}
		titleRowID, err = res.LastInsertId()
		if err != nil {
			return out, fmt.Errorf("create title %s/%s: %w", req.Platform, req.TitleID, err)
		}
		latest = 0
	case err != nil:
		return out, fmt.Errorf("lookup title %s/%s: %w", req.Platform, req.TitleID, err)
	}

	if req.ParentVersion != latest {
		return out, ErrConflict
	}

	// A name that arrives later fills one in, and a package without one leaves
	// whatever is there. A console that has learned a game's name should not have
	// to be the first to sync it for the server to know.
	if req.TitleName != "" {
		if _, err := tx.ExecContext(ctx,
			`UPDATE titles SET title_name = ? WHERE id = ?`,
			req.TitleName, titleRowID); err != nil {
			return out, fmt.Errorf("record title name: %w", err)
		}
	}

	blobID, err := s.insertBlob(ctx, tx, req)
	if err != nil {
		return out, err
	}

	version := latest + 1
	var parent any
	if latest > 0 {
		parent = latest
	}
	receivedAt := s.timestamp()

	if _, err := tx.ExecContext(ctx,
		`INSERT INTO versions (title_row_id, version, parent_version, blob_id, device_id,
		                       device_label, received_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		titleRowID, version, parent, blobID, nullIfEmpty(req.DeviceID), req.DeviceLabel,
		receivedAt); err != nil {
		return out, fmt.Errorf("insert version %d: %w", version, err)
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE blobs SET refcount = refcount + 1 WHERE id = ?`, blobID); err != nil {
		return out, fmt.Errorf("bump refcount: %w", err)
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE titles SET latest_version = ?, updated_at = ? WHERE id = ?`,
		version, receivedAt, titleRowID); err != nil {
		return out, fmt.Errorf("advance latest_version: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return out, fmt.Errorf("commit upload: %w", err)
	}

	out = VersionMeta{
		TitleID:     req.TitleID,
		Platform:    req.Platform,
		Version:     version,
		SHA256:      req.SHA256,
		Size:        req.Size,
		DeviceLabel: req.DeviceLabel,
		ReceivedAt:  receivedAt,
		blobID:      blobID,
	}
	if latest > 0 {
		p := latest
		out.ParentVersion = &p
	}
	return out, nil
}

func nullIfEmpty(s string) any {
	if s == "" {
		return nil
	}
	return s
}

// insertBlob writes the package into blob_chunks, or reuses an existing row when
// the same content is already stored. Content addressing means two consoles with
// identical saves cost one copy, and the two sides of a conflict dedupe naturally.
func (s *Store) insertBlob(ctx context.Context, tx *sql.Tx, req PutRequest) (int64, error) {
	var id int64
	err := tx.QueryRowContext(ctx, `SELECT id FROM blobs WHERE sha256 = ?`, req.SHA256).Scan(&id)
	if err == nil {
		return id, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return 0, fmt.Errorf("lookup blob %s: %w", req.SHA256, err)
	}

	// size is the uncompressed payload, matching sha256, which is a digest of the
	// payload and not of the zip container. Two consoles packing the same save with
	// different compression produce the same row, which is what makes content
	// addressing work at all.
	res, err := tx.ExecContext(ctx,
		`INSERT INTO blobs (sha256, size, refcount, created_at) VALUES (?, ?, 0, ?)`,
		req.SHA256, req.Size, s.timestamp())
	if err != nil {
		return 0, fmt.Errorf("insert blob %s: %w", req.SHA256, err)
	}
	id, err = res.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("insert blob %s: %w", req.SHA256, err)
	}

	buf := make([]byte, s.chunkSize)
	for seq := 0; ; seq++ {
		n, readErr := io.ReadFull(req.Body, buf)
		if n > 0 {
			if _, err := tx.ExecContext(ctx,
				`INSERT INTO blob_chunks (blob_id, seq, data) VALUES (?, ?, ?)`,
				id, seq, buf[:n]); err != nil {
				return 0, fmt.Errorf("insert chunk %d: %w", seq, err)
			}
		}
		if errors.Is(readErr, io.EOF) || errors.Is(readErr, io.ErrUnexpectedEOF) {
			break
		}
		if readErr != nil {
			return 0, fmt.Errorf("read upload body: %w", readErr)
		}
	}
	return id, nil
}

// WriteBlobTo streams a version's package out, one chunk at a time. A blob is never
// assembled whole in memory on either side of the wire.
func (s *Store) WriteBlobTo(ctx context.Context, m VersionMeta, w io.Writer) error {
	rows, err := s.db.QueryContext(ctx,
		`SELECT data FROM blob_chunks WHERE blob_id = ? ORDER BY seq`, m.blobID)
	if err != nil {
		return fmt.Errorf("read blob %d: %w", m.blobID, err)
	}
	defer rows.Close()

	for rows.Next() {
		var chunk []byte
		if err := rows.Scan(&chunk); err != nil {
			return fmt.Errorf("scan chunk of blob %d: %w", m.blobID, err)
		}
		if _, err := w.Write(chunk); err != nil {
			// A console dropping the connection mid download is ordinary.
			return fmt.Errorf("write blob %d: %w", m.blobID, err)
		}
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("read blob %d: %w", m.blobID, err)
	}
	return nil
}

// ----------------------------------------------------------------- shares

func (s *Store) CreateShare(ctx context.Context, code, userID, platform, titleID string,
	version uint32, ttl time.Duration) (string, error) {
	var titleRowID int64
	err := s.db.QueryRowContext(ctx,
		`SELECT t.id FROM titles t
		  JOIN versions v ON v.title_row_id = t.id AND v.version = ?
		 WHERE t.user_id = ? AND t.platform = ? AND t.title_id = ?`,
		version, userID, platform, titleID).Scan(&titleRowID)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("lookup version for share: %w", err)
	}

	expires := s.now().UTC().Add(ttl).Format(time.RFC3339)
	if _, err := s.db.ExecContext(ctx,
		`INSERT INTO shares (code, title_row_id, version, expires_at, created_at)
		 VALUES (?, ?, ?, ?, ?)`,
		code, titleRowID, version, expires, s.timestamp()); err != nil {
		return "", fmt.Errorf("create share: %w", err)
	}
	return expires, nil
}

// ResolveShare returns the version a share code points at. An expired code is
// ErrExpired and not ErrNotFound, because the two mean different things to the
// person holding the code.
func (s *Store) ResolveShare(ctx context.Context, code string) (VersionMeta, error) {
	var out VersionMeta
	var expiresAt string
	var titleRowID int64
	var version uint32

	err := s.db.QueryRowContext(ctx,
		`SELECT title_row_id, version, expires_at FROM shares WHERE code = ?`,
		code).Scan(&titleRowID, &version, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		return out, ErrNotFound
	}
	if err != nil {
		return out, fmt.Errorf("resolve share: %w", err)
	}

	exp, err := time.Parse(time.RFC3339, expiresAt)
	if err != nil {
		return out, fmt.Errorf("share %s has an unparseable expiry %q: %w", code, expiresAt, err)
	}
	if s.now().UTC().After(exp) {
		return out, ErrExpired
	}

	row := s.db.QueryRowContext(ctx, versionSelect+`
		 WHERE v.title_row_id = ? AND v.version = ?`, titleRowID, version)
	out, err = scanVersion(row)
	if errors.Is(err, sql.ErrNoRows) {
		return out, ErrNotFound
	}
	if err != nil {
		return out, fmt.Errorf("resolve share %s: %w", code, err)
	}
	return out, nil
}

// --------------------------------------------------------------- pairings

func (s *Store) CreatePairing(ctx context.Context, code, userID string, approved bool,
	ttl time.Duration) error {
	flag := 0
	if approved {
		flag = 1
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO pairings (code, user_id, approved, expires_at, created_at)
		 VALUES (?, ?, ?, ?, ?)`,
		code, userID, flag, s.now().UTC().Add(ttl).Format(time.RFC3339), s.timestamp())
	if err != nil {
		return fmt.Errorf("create pairing: %w", err)
	}
	return nil
}

type Pairing struct {
	Code     string
	UserID   string
	Approved bool
}

// ClaimPairing consumes a pairing code. An approved code can only be used once.
func (s *Store) ClaimPairing(ctx context.Context, code string) (Pairing, error) {
	var p Pairing
	var approved int
	var expiresAt string
	var userID sql.NullString

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return p, fmt.Errorf("begin claim pairing: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	err = tx.QueryRowContext(ctx,
		`SELECT user_id, approved, expires_at FROM pairings WHERE code = ?`,
		code).Scan(&userID, &approved, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		return p, ErrNotFound
	}
	if err != nil {
		return p, fmt.Errorf("claim pairing: %w", err)
	}

	exp, err := time.Parse(time.RFC3339, expiresAt)
	if err != nil {
		return p, fmt.Errorf("pairing %s has an unparseable expiry %q: %w", code, expiresAt, err)
	}
	if s.now().UTC().After(exp) {
		return p, ErrExpired
	}

	p.Code = code
	p.UserID = userID.String
	p.Approved = approved != 0
	if !p.Approved {
		// Still waiting for the user to approve from a phone or PC. Leave the row
		// alone so the console can poll again.
		return p, nil
	}

	if _, err := tx.ExecContext(ctx, `DELETE FROM pairings WHERE code = ?`, code); err != nil {
		return p, fmt.Errorf("consume pairing: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return p, fmt.Errorf("commit claim pairing: %w", err)
	}
	return p, nil
}

// ApprovePairing marks a code approved, which is what the web side calls.
func (s *Store) ApprovePairing(ctx context.Context, code, userID string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE pairings SET approved = 1, user_id = ? WHERE code = ?`, userID, code)
	if err != nil {
		return fmt.Errorf("approve pairing: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("approve pairing: %w", err)
	}
	if n == 0 {
		return ErrNotFound
	}
	return nil
}

// SetClock replaces the clock. Tests use it so an expiry can be reached without
// sleeping; nothing in the sync logic depends on a timestamp.
func (s *Store) SetClock(now func() time.Time) { s.now = now }

// Vacuum reclaims space after a GC pass. Not wired to anything yet: deletion is
// reference counted and rows are kept for a retention window first.
func (s *Store) Vacuum(ctx context.Context) error {
	if _, err := s.db.ExecContext(ctx, "VACUUM"); err != nil {
		return fmt.Errorf("vacuum: %w", err)
	}
	return nil
}
