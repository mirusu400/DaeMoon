package store_test

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/mirusu400/DaeMoon/server/internal/store"
)

func TestUsersAreNamedAndUnique(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if n, err := s.CountUsers(ctx); err != nil || n != 0 {
		t.Fatalf("fresh instance: %d users, %v", n, err)
	}

	u, err := s.CreateUser(ctx, "u1", "mirusu", "hash1", true)
	if err != nil {
		t.Fatalf("create: %v", err)
	}
	if !u.IsAdmin || !u.HasPassword {
		t.Fatalf("created user = %+v", u)
	}

	if _, err := s.CreateUser(ctx, "u2", "mirusu", "hash2", false); !errors.Is(err, store.ErrNameTaken) {
		t.Fatalf("duplicate name: %v", err)
	}

	got, err := s.UserByName(ctx, "mirusu")
	if err != nil || got.ID != "u1" {
		t.Fatalf("by name = %+v, %v", got, err)
	}
	if _, err := s.UserByName(ctx, "nobody"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("unknown name: %v", err)
	}
}

// A user made by `daemoond -pair` before accounts existed owns devices and saves
// and has no password. It must survive the migration and must not be loggable
// into, which is the safe direction for a row that arrived without a credential.
func TestAUserFromBeforeAccountsCannotBeLoggedInto(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if err := s.EnsureUser(ctx, "legacy"); err != nil {
		t.Fatalf("ensure: %v", err)
	}
	if _, err := s.PasswordHash(ctx, "legacy"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("a passwordless user offered a hash: %v", err)
	}
	// And it is not counted as an account, so the setup page still offers to make
	// the first one.
	if n, err := s.CountUsers(ctx); err != nil || n != 0 {
		t.Fatalf("count = %d, %v; a passwordless user is not an account", n, err)
	}

	if err := s.SetPassword(ctx, "legacy", "hash"); err != nil {
		t.Fatalf("claim: %v", err)
	}
	if n, _ := s.CountUsers(ctx); n != 1 {
		t.Fatalf("count after claiming = %d", n)
	}
}

func TestSessionsExpireAndAreRevocable(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	now := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)
	s.SetClock(func() time.Time { return now })

	if _, err := s.CreateUser(ctx, "u1", "mirusu", "hash", true); err != nil {
		t.Fatalf("create user: %v", err)
	}
	if err := s.CreateSession(ctx, "sessionhash", "u1", time.Hour); err != nil {
		t.Fatalf("create session: %v", err)
	}

	if u, err := s.SessionUser(ctx, "sessionhash"); err != nil || u.ID != "u1" {
		t.Fatalf("resolve = %+v, %v", u, err)
	}

	// The same query that resolves a session checks its expiry, so there is no
	// window between the two.
	now = now.Add(2 * time.Hour)
	if _, err := s.SessionUser(ctx, "sessionhash"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("an expired session resolved: %v", err)
	}

	now = now.Add(-2 * time.Hour)
	if _, err := s.SessionUser(ctx, "sessionhash"); err != nil {
		t.Fatalf("session should be valid again: %v", err)
	}
	if err := s.DeleteSession(ctx, "sessionhash"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if _, err := s.SessionUser(ctx, "sessionhash"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("a deleted session resolved: %v", err)
	}
}

// Deleting a person takes their devices and saves with them, and their sessions:
// a cookie must not outlive the account it names.
func TestDeletingAUserTakesTheirSessions(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if _, err := s.CreateUser(ctx, "u1", "gone", "hash", false); err != nil {
		t.Fatalf("create: %v", err)
	}
	if err := s.CreateSession(ctx, "sh", "u1", time.Hour); err != nil {
		t.Fatalf("session: %v", err)
	}
	if err := s.DeleteUser(ctx, "u1"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if _, err := s.SessionUser(ctx, "sh"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("session outlived its user: %v", err)
	}
	if err := s.DeleteUser(ctx, "u1"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("deleting twice: %v", err)
	}
}

func TestListDevicesShowsRevocation(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if err := s.EnsureUser(ctx, "u1"); err != nil {
		t.Fatalf("user: %v", err)
	}
	for i, d := range []store.Device{
		{ID: "d1", UserID: "u1", Label: "living room 3DS", Platform: "3ds"},
		{ID: "d2", UserID: "u1", Label: "switch", Platform: "nx"},
	} {
		if err := s.CreateDevice(ctx, d, "tokenhash"+string(rune('a'+i))); err != nil {
			t.Fatalf("device: %v", err)
		}
	}
	if err := s.RevokeDevice(ctx, "u1", "d2"); err != nil {
		t.Fatalf("revoke: %v", err)
	}

	list, err := s.ListDevices(ctx, "u1")
	if err != nil || len(list) != 2 {
		t.Fatalf("list = %d, %v", len(list), err)
	}
	seen := map[string]bool{}
	for _, d := range list {
		seen[d.ID] = d.Revoked
	}
	if seen["d1"] || !seen["d2"] {
		t.Fatalf("revocation not reflected: %+v", seen)
	}
}
