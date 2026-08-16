package config_test

import (
	"testing"
	"time"

	"github.com/mirusu400/DaeMoon/server/internal/config"
)

// Misconfiguration has to stop the server at startup. A self hosted instance is
// usually left alone for months, so a setting that was quietly ignored is found
// the day it was needed.

func TestDefaultsAreUsable(t *testing.T) {
	c := config.Default()

	if c.Addr == "" || c.Database == "" {
		t.Fatalf("defaults are incomplete: %+v", c)
	}
	if c.MaxSaveSize != 64<<20 {
		t.Errorf("MaxSaveSize = %d, want 64 MiB", c.MaxSaveSize)
	}
	if c.BlobChunkSize != 1<<20 {
		// Documented in the schema and in CLAUDE.md. Changing it without measuring
		// is what the comment there warns against, so it is pinned here.
		t.Errorf("BlobChunkSize = %d, want 1 MiB", c.BlobChunkSize)
	}
	// Every timeout is finite. There are no unbounded waits anywhere in this
	// project, and a zero here would be one.
	for name, d := range map[string]time.Duration{
		"ReadTimeout":  c.ReadTimeout,
		"WriteTimeout": c.WriteTimeout,
		"IdleTimeout":  c.IdleTimeout,
		"ShareTTL":     c.ShareTTL,
		"PairingTTL":   c.PairingTTL,
	} {
		if d <= 0 {
			t.Errorf("%s = %s, want a positive duration", name, d)
		}
	}
	// An upload from a console on hotel wifi is the slow case, so the read timeout
	// has to be the generous one.
	if c.ReadTimeout < time.Minute {
		t.Errorf("ReadTimeout = %s, too short for a multi megabyte upload", c.ReadTimeout)
	}
}

func TestEnvOverrides(t *testing.T) {
	t.Setenv("DAEMOON_ADDR", "127.0.0.1:9999")
	t.Setenv("DAEMOON_DB", "/tmp/somewhere.db")
	t.Setenv("DAEMOON_MAX_SAVE_SIZE", "1048576")
	t.Setenv("DAEMOON_READ_TIMEOUT", "90s")
	t.Setenv("DAEMOON_SHARE_TTL", "48h")

	c, err := config.FromEnv()
	if err != nil {
		t.Fatalf("FromEnv: %v", err)
	}
	if c.Addr != "127.0.0.1:9999" {
		t.Errorf("Addr = %q", c.Addr)
	}
	if c.Database != "/tmp/somewhere.db" {
		t.Errorf("Database = %q", c.Database)
	}
	if c.MaxSaveSize != 1<<20 {
		t.Errorf("MaxSaveSize = %d", c.MaxSaveSize)
	}
	if c.ReadTimeout != 90*time.Second {
		t.Errorf("ReadTimeout = %s", c.ReadTimeout)
	}
	if c.ShareTTL != 48*time.Hour {
		t.Errorf("ShareTTL = %s", c.ShareTTL)
	}
	// Anything not set keeps its default.
	if c.WriteTimeout != config.Default().WriteTimeout {
		t.Errorf("WriteTimeout was changed to %s", c.WriteTimeout)
	}
}

func TestEmptyValuesKeepTheDefault(t *testing.T) {
	// An unset variable and one set to the empty string are the same thing in a
	// systemd unit or a docker compose file.
	t.Setenv("DAEMOON_ADDR", "")
	t.Setenv("DAEMOON_READ_TIMEOUT", "")

	c, err := config.FromEnv()
	if err != nil {
		t.Fatalf("FromEnv: %v", err)
	}
	if c.Addr != config.Default().Addr {
		t.Errorf("Addr = %q, want the default", c.Addr)
	}
	if c.ReadTimeout != config.Default().ReadTimeout {
		t.Errorf("ReadTimeout = %s, want the default", c.ReadTimeout)
	}
}

func TestBadValuesAreRefused(t *testing.T) {
	for _, tc := range []struct{ name, key, value string }{
		{"max size is not a number", "DAEMOON_MAX_SAVE_SIZE", "sixty four megabytes"},
		{"max size is zero", "DAEMOON_MAX_SAVE_SIZE", "0"},
		{"max size is negative", "DAEMOON_MAX_SAVE_SIZE", "-1"},
		{"timeout has no unit", "DAEMOON_READ_TIMEOUT", "30"},
		{"timeout is nonsense", "DAEMOON_WRITE_TIMEOUT", "soon"},
		{"timeout is zero", "DAEMOON_IDLE_TIMEOUT", "0s"},
		{"timeout is negative", "DAEMOON_SHARE_TTL", "-1h"},
		{"pairing ttl is zero", "DAEMOON_PAIRING_TTL", "0"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv(tc.key, tc.value)
			if _, err := config.FromEnv(); err == nil {
				t.Fatalf("%s=%q was accepted", tc.key, tc.value)
			}
		})
	}
}

// TestErrorsNameTheVariable: whoever is reading this is looking at a service that
// will not start, usually over ssh.
func TestErrorsNameTheVariable(t *testing.T) {
	t.Setenv("DAEMOON_WRITE_TIMEOUT", "nope")

	_, err := config.FromEnv()
	if err == nil {
		t.Fatal("a bad duration was accepted")
	}
	if got := err.Error(); !contains(got, "DAEMOON_WRITE_TIMEOUT") {
		t.Errorf("error does not name the variable: %s", got)
	}
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}

// Half configured TLS is the case where somebody believes they have https and
// does not, which for this project means saves crossing a network in the clear
// while the operator thinks otherwise.
func TestTLSMustBeBothOrNeither(t *testing.T) {
	for _, tc := range []struct {
		name    string
		cert    string
		key     string
		wantErr bool
		wantTLS bool
	}{
		{"neither", "", "", false, false},
		{"both", "cert.pem", "key.pem", false, true},
		{"cert only", "cert.pem", "", true, false},
		{"key only", "", "key.pem", true, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("DAEMOON_TLS_CERT", tc.cert)
			t.Setenv("DAEMOON_TLS_KEY", tc.key)

			c, err := config.FromEnv()
			if (err != nil) != tc.wantErr {
				t.Fatalf("err = %v, wantErr %v", err, tc.wantErr)
			}
			if err == nil && c.TLS() != tc.wantTLS {
				t.Fatalf("TLS() = %v, want %v", c.TLS(), tc.wantTLS)
			}
		})
	}
}
