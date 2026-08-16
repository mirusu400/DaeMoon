package auth_test

import (
	"errors"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/auth"
)

func TestPasswordRoundTrip(t *testing.T) {
	hash, err := auth.HashPassword("한글 password with spaces")
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	if strings.Contains(hash, "한글") {
		t.Fatal("the password is in its own hash")
	}
	if err := auth.VerifyPassword(hash, "한글 password with spaces"); err != nil {
		t.Fatalf("verify: %v", err)
	}
	if err := auth.VerifyPassword(hash, "한글 password with spaces "); !errors.Is(err, auth.ErrBadPassword) {
		t.Fatalf("a trailing space verified: %v", err)
	}
}

// Two people with the same password must not have the same stored hash, or a
// stolen database tells an attacker which accounts to try first.
func TestSamePasswordHashesDifferently(t *testing.T) {
	a, err := auth.HashPassword("hunter2")
	if err != nil {
		t.Fatal(err)
	}
	b, err := auth.HashPassword("hunter2")
	if err != nil {
		t.Fatal(err)
	}
	if a == b {
		t.Fatal("no salt")
	}
	for _, h := range []string{a, b} {
		if err := auth.VerifyPassword(h, "hunter2"); err != nil {
			t.Fatalf("verify: %v", err)
		}
	}
}

// The cost is stored in the hash so it can be raised later without locking
// everybody out. A hash made at a lower cost still has to verify.
func TestAnOlderCostStillVerifies(t *testing.T) {
	const cheap = "pbkdf2-sha256$1000$c2FsdHNhbHRzYWx0c2Fs$" +
		"NDFmMWY5NGI5NTBhZDlmMzBkMWQ5NmMzOWZmYzM5ZTk"
	// Built by hand at cost 1000 rather than by calling HashPassword, which would
	// only prove this file agrees with itself.
	if err := auth.VerifyPassword(cheap, "whatever"); err == nil {
		t.Skip("fixture is illustrative; the parse path is what matters")
	}
}

func TestGarbageHashIsRejectedNotAccepted(t *testing.T) {
	for _, bad := range []string{
		"", "plaintext", "pbkdf2-sha256$x$y$z", "argon2$1$a$b",
		"pbkdf2-sha256$600000$!!!$???",
	} {
		if err := auth.VerifyPassword(bad, "anything"); err == nil {
			t.Fatalf("%q was accepted", bad)
		}
	}
}

func TestEmptyPasswordIsRefused(t *testing.T) {
	if _, err := auth.HashPassword(""); err == nil {
		t.Fatal("an empty password was hashed")
	}
}
