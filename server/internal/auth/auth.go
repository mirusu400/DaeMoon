// Package auth turns a bearer token into a device.
//
// Console software keyboards are painful to type on, so tokens are never entered
// by hand: they arrive by QR scan on the 3DS or through a device code approved from
// a phone or PC on the Switch. Once issued, a token lives on the SD card, and SD
// cards are removable, which is why revocation is per device.
//
// A hardware id such as PS_GetDeviceId is never used as an authentication factor.
// It is not secret and it identifies a person's console across services.
package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"strings"

	"github.com/mirusu400/DaeMoon/server/internal/apierr"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

// TokenBytes is the entropy in a device token. 32 bytes is far beyond what a
// brute force over the network could reach, and the console never displays it.
const TokenBytes = 32

// NewToken returns a fresh token. It is shown to the client once and never stored.
func NewToken() (string, error) {
	b := make([]byte, TokenBytes)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate token: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// HashToken is what the database holds. A stolen database does not yield working
// tokens.
func HashToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

// NewID returns an opaque identifier for a user, device or share.
func NewID() (string, error) {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate id: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// NewPairingCode returns a six digit code for the device code flow. Digits only:
// it is read off one screen and typed on another, and letters invite confusion
// between O and 0.
func NewPairingCode() (string, error) {
	b := make([]byte, 4)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate pairing code: %w", err)
	}
	n := (uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])) % 1000000
	return fmt.Sprintf("%06d", n), nil
}

// NewShareCode returns a share code. Longer than a pairing code because it is not
// rate limited behind a login and it grants a download to anyone holding it.
func NewShareCode() (string, error) {
	b := make([]byte, 12)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate share code: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// Equal compares two tokens without leaking how much of them matched.
func Equal(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

type ctxKey struct{}

// Device returns the authenticated device for a request.
func Device(ctx context.Context) (store.Device, bool) {
	d, ok := ctx.Value(ctxKey{}).(store.Device)
	return d, ok
}

// MustDevice is for handlers behind Middleware, where the absence of a device is a
// routing mistake rather than a client error.
func MustDevice(ctx context.Context) store.Device {
	d, ok := Device(ctx)
	if !ok {
		panic("auth: handler is not behind auth.Middleware")
	}
	return d
}

// Middleware resolves the bearer token. A revoked device is told so explicitly:
// "unauthorized" would send the user to pair again, and pairing again is exactly
// the wrong response when the point was to cut a lost SD card off.
func Middleware(s *store.Store) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			header := r.Header.Get("Authorization")
			token, ok := strings.CutPrefix(header, "Bearer ")
			if !ok || strings.TrimSpace(token) == "" {
				apierr.Write(w, r, apierr.New(apierr.Unauthorized))
				return
			}

			device, err := s.DeviceByTokenHash(r.Context(), HashToken(strings.TrimSpace(token)))
			if errors.Is(err, store.ErrNotFound) {
				apierr.Write(w, r, apierr.New(apierr.Unauthorized))
				return
			}
			if err != nil {
				apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
				return
			}
			if device.Revoked {
				apierr.Write(w, r, apierr.New(apierr.DeviceRevoked).
					WithDetail(map[string]any{"device_id": device.ID}))
				return
			}

			next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), ctxKey{}, device)))
		})
	}
}
