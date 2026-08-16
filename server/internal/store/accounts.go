package store

// People and browser sessions.
//
// Kept apart from store.go, which is about what a console does. Nothing in this
// file is reachable with a device token and nothing in that one is reachable with
// a session cookie: the two ways in stay separate all the way down, so revoking a
// console cannot log anybody out and losing a password cannot cost somebody their
// saves.

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// ErrNameTaken is returned when a username already exists. Distinguished from a
// generic failure because it is the one the sign up form has to explain.
var ErrNameTaken = errors.New("store: username already taken")

type User struct {
	ID        string
	Username  string
	IsAdmin   bool
	CreatedAt string
	// HasPassword is false for a user made by `daemoond -pair` before accounts
	// existed. Such a user owns devices and saves and cannot be logged into until
	// somebody claims it, which is the safe direction for a row that arrived
	// without a credential.
	HasPassword bool
}

func scanUser(row interface{ Scan(...any) error }) (User, error) {
	var u User
	var name sql.NullString
	var hash sql.NullString

	if err := row.Scan(&u.ID, &name, &hash, &u.IsAdmin, &u.CreatedAt); err != nil {
		return User{}, err
	}
	u.Username = name.String
	u.HasPassword = hash.Valid && hash.String != ""
	return u, nil
}

const userColumns = `id, username, password_hash, is_admin, created_at`

// CountUsers is how the setup page knows whether this is a fresh instance.
func (s *Store) CountUsers(ctx context.Context) (int, error) {
	var n int
	err := s.db.QueryRowContext(ctx,
		`SELECT count(*) FROM users WHERE password_hash IS NOT NULL`).Scan(&n)
	if err != nil {
		return 0, fmt.Errorf("count users: %w", err)
	}
	return n, nil
}

func (s *Store) CreateUser(ctx context.Context, id, username, passwordHash string,
	isAdmin bool) (User, error) {
	admin := 0
	if isAdmin {
		admin = 1
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO users (id, username, password_hash, is_admin, created_at)
		 VALUES (?, ?, ?, ?, ?)`,
		id, username, passwordHash, admin, s.timestamp())
	if err != nil {
		// modernc's driver reports this as a message rather than a typed error,
		// and a self hosted instance is not worth a driver-specific dependency to
		// classify one collision. The name is the only unique column on this
		// table, so a constraint failure here is that name.
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			return User{}, ErrNameTaken
		}
		return User{}, fmt.Errorf("create user %s: %w", username, err)
	}
	return User{ID: id, Username: username, IsAdmin: isAdmin, HasPassword: true}, nil
}

func (s *Store) UserByName(ctx context.Context, username string) (User, error) {
	u, err := scanUser(s.db.QueryRowContext(ctx,
		`SELECT `+userColumns+` FROM users WHERE username = ?`, username))
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, fmt.Errorf("look up user %s: %w", username, err)
	}
	return u, nil
}

func (s *Store) UserByID(ctx context.Context, id string) (User, error) {
	u, err := scanUser(s.db.QueryRowContext(ctx,
		`SELECT `+userColumns+` FROM users WHERE id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, fmt.Errorf("look up user %s: %w", id, err)
	}
	return u, nil
}

// PasswordHash is separate from UserByName so a hash is only in memory where it is
// about to be checked, rather than riding along in every struct that names a user.
func (s *Store) PasswordHash(ctx context.Context, userID string) (string, error) {
	var hash sql.NullString
	err := s.db.QueryRowContext(ctx,
		`SELECT password_hash FROM users WHERE id = ?`, userID).Scan(&hash)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("read password hash: %w", err)
	}
	if !hash.Valid {
		return "", ErrNotFound
	}
	return hash.String, nil
}

func (s *Store) SetPassword(ctx context.Context, userID, passwordHash string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE users SET password_hash = ? WHERE id = ?`, passwordHash, userID)
	if err != nil {
		return fmt.Errorf("set password: %w", err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) ListUsers(ctx context.Context) ([]User, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT `+userColumns+` FROM users ORDER BY username IS NULL, username`)
	if err != nil {
		return nil, fmt.Errorf("list users: %w", err)
	}
	defer rows.Close()

	var out []User
	for rows.Next() {
		u, err := scanUser(rows)
		if err != nil {
			return nil, fmt.Errorf("scan user: %w", err)
		}
		out = append(out, u)
	}
	return out, rows.Err()
}

// DeleteUser removes a person and, by foreign key, their devices, titles, versions
// and sessions. The blobs stay: they are reference counted and collected later,
// and content addressing means another user may be holding the same one.
func (s *Store) DeleteUser(ctx context.Context, id string) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM users WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("delete user %s: %w", id, err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// CountAdmins exists so the last administrator cannot delete themselves and leave
// an instance nobody can administer.
func (s *Store) CountAdmins(ctx context.Context) (int, error) {
	var n int
	err := s.db.QueryRowContext(ctx,
		`SELECT count(*) FROM users WHERE is_admin = 1`).Scan(&n)
	if err != nil {
		return 0, fmt.Errorf("count admins: %w", err)
	}
	return n, nil
}

// ---------------------------------------------------------------- sessions

func (s *Store) CreateSession(ctx context.Context, tokenHash, userID string,
	ttl time.Duration) error {
	expires := s.now().UTC().Add(ttl).Format(time.RFC3339)
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO sessions (token_hash, user_id, expires_at, created_at)
		 VALUES (?, ?, ?, ?)`,
		tokenHash, userID, expires, s.timestamp())
	if err != nil {
		return fmt.Errorf("create session: %w", err)
	}
	return nil
}

// SessionUser resolves a cookie to the person who owns it, refusing an expired one
// in the same query so there is no window between the check and the use.
func (s *Store) SessionUser(ctx context.Context, tokenHash string) (User, error) {
	u, err := scanUser(s.db.QueryRowContext(ctx,
		`SELECT u.id, u.username, u.password_hash, u.is_admin, u.created_at
		   FROM sessions s JOIN users u ON u.id = s.user_id
		  WHERE s.token_hash = ? AND s.expires_at > ?`,
		tokenHash, s.timestamp()))
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, fmt.Errorf("resolve session: %w", err)
	}
	return u, nil
}

func (s *Store) DeleteSession(ctx context.Context, tokenHash string) error {
	_, err := s.db.ExecContext(ctx,
		`DELETE FROM sessions WHERE token_hash = ?`, tokenHash)
	if err != nil {
		return fmt.Errorf("delete session: %w", err)
	}
	return nil
}

// PurgeExpiredSessions is called on the way through login rather than on a timer.
// A self hosted instance may go weeks without a request, and a background sweeper
// would be a goroutine that exists to delete rows nobody can use anyway.
func (s *Store) PurgeExpiredSessions(ctx context.Context) error {
	_, err := s.db.ExecContext(ctx,
		`DELETE FROM sessions WHERE expires_at <= ?`, s.timestamp())
	if err != nil {
		return fmt.Errorf("purge sessions: %w", err)
	}
	return nil
}

// ---------------------------------------------------------------- devices

type DeviceInfo struct {
	Device
	CreatedAt string
	RevokedAt string
}

// ListDevices is what the web panel shows. The console side never needs it: a
// console knows its own token and nothing about its siblings.
func (s *Store) ListDevices(ctx context.Context, userID string) ([]DeviceInfo, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, label, platform, created_at, revoked_at
		   FROM devices WHERE user_id = ? ORDER BY created_at DESC`, userID)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	defer rows.Close()

	var out []DeviceInfo
	for rows.Next() {
		var d DeviceInfo
		var revoked sql.NullString

		if err := rows.Scan(&d.ID, &d.Label, &d.Platform, &d.CreatedAt, &revoked); err != nil {
			return nil, fmt.Errorf("scan device: %w", err)
		}
		d.UserID = userID
		d.Revoked = revoked.Valid
		d.RevokedAt = revoked.String
		out = append(out, d)
	}
	return out, rows.Err()
}

// ---------------------------------------------------------------- pairing

// PairingExists reports whether a code is still outstanding. ClaimPairing deletes
// a code as it hands over a token, so "gone" is how the web page learns a console
// took it - and an expired code is gone for the same purpose, which is the answer
// the page wants either way.
func (s *Store) PairingExists(ctx context.Context, code string) bool {
	var n int
	err := s.db.QueryRowContext(ctx,
		`SELECT count(*) FROM pairings WHERE code = ? AND expires_at > ?`,
		code, s.timestamp()).Scan(&n)
	return err == nil && n > 0
}

// ---------------------------------------------------------------- versions

// ListVersions is every version of one title, newest first. The console side never
// asks for this - it wants the latest and nothing else - but a person looking at a
// conflict wants to see both sides and everything before them.
func (s *Store) ListVersions(ctx context.Context, userID, platform, titleID string) (
	[]VersionMeta, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT v.version, v.parent_version, b.sha256, b.size, v.device_label,
		        v.received_at, b.id
		   FROM versions v
		   JOIN titles t ON t.id = v.title_row_id
		   JOIN blobs  b ON b.id = v.blob_id
		  WHERE t.user_id = ? AND t.platform = ? AND t.title_id = ?
		  ORDER BY v.version DESC`,
		userID, platform, titleID)
	if err != nil {
		return nil, fmt.Errorf("list versions: %w", err)
	}
	defer rows.Close()

	var out []VersionMeta
	for rows.Next() {
		m, err := scanVersion(rows)
		if err != nil {
			return nil, fmt.Errorf("scan version: %w", err)
		}
		out = append(out, m)
	}
	return out, rows.Err()
}
