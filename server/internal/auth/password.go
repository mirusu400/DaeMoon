package auth

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// Passwords, for the web side. Consoles never see one: they authenticate with a
// device token, which is why this file did not exist until there was a browser to
// log in from.
//
// PBKDF2-HMAC-SHA256 from the standard library. Argon2 and bcrypt are both better
// at what they are for, and both are a dependency; the rules here say to refuse
// one without a reason, and "the recommended algorithm is nicer" is not enough
// against a self hosted service whose whole promise is one binary. What is not
// negotiable is that the stored form is salted and slow, and this is.
const (
	// Iterations is deliberately expensive. A login happens by hand, a few times a
	// day at most, so 600,000 costs a person nothing and costs somebody with a
	// stolen database a great deal. It is also OWASP's current figure for this
	// construction, which is a better anchor than a number picked here.
	Iterations = 600000
	saltBytes  = 16
	keyBytes   = 32
)

// ErrBadPassword is returned when a password does not match. Deliberately the same
// error whether the user exists or not: a login form that answers "no such user"
// faster than "wrong password" is a list of who has an account.
var ErrBadPassword = errors.New("auth: password does not match")

// HashPassword returns a self describing string: algorithm, cost, salt, key. The
// cost is stored rather than assumed so raising Iterations later does not lock
// everybody out - an old hash still verifies with the cost it was made at.
func HashPassword(password string) (string, error) {
	if password == "" {
		return "", errors.New("auth: empty password")
	}

	salt := make([]byte, saltBytes)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("generate salt: %w", err)
	}
	key, err := pbkdf2.Key(sha256.New, password, salt, Iterations, keyBytes)
	if err != nil {
		return "", fmt.Errorf("derive key: %w", err)
	}

	return fmt.Sprintf("pbkdf2-sha256$%d$%s$%s", Iterations,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(key)), nil
}

// VerifyPassword reports whether password produced encoded.
func VerifyPassword(encoded, password string) error {
	parts := strings.Split(encoded, "$")
	if len(parts) != 4 || parts[0] != "pbkdf2-sha256" {
		return fmt.Errorf("auth: unrecognised password hash")
	}
	iter, err := strconv.Atoi(parts[1])
	if err != nil || iter <= 0 {
		return fmt.Errorf("auth: bad iteration count in password hash")
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[2])
	if err != nil {
		return fmt.Errorf("auth: bad salt in password hash")
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[3])
	if err != nil {
		return fmt.Errorf("auth: bad key in password hash")
	}

	got, err := pbkdf2.Key(sha256.New, password, salt, iter, len(want))
	if err != nil {
		return fmt.Errorf("derive key: %w", err)
	}
	if subtle.ConstantTimeCompare(got, want) != 1 {
		return ErrBadPassword
	}
	return nil
}
