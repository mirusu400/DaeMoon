// Package config holds the server's settings.
//
// Self hosting is a primary goal, so everything has a working default and the
// whole service is one binary plus one database file. Configuration is environment
// variables only: no config file format to document, parse, or get wrong.
package config

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type Config struct {
	Addr     string
	Database string

	// MaxSaveSize is enforced before the body is read. Default 64 MiB.
	MaxSaveSize int64

	// ReadTimeout covers uploads, which are the slow case: a console on hotel
	// wifi pushing a few megabytes.
	ReadTimeout  time.Duration
	WriteTimeout time.Duration
	IdleTimeout  time.Duration

	// ShareTTL is how long a share code lives by default.
	ShareTTL time.Duration
	// PairingTTL is how long a pairing code lives. Short: the user is standing in
	// front of both devices.
	PairingTTL time.Duration

	// BlobChunkSize is the row size for blob_chunks. Do not change it without
	// measuring; it is what keeps reads streaming and memory bounded.
	BlobChunkSize int
}

func Default() Config {
	return Config{
		Addr:          ":8080",
		Database:      "daemoon.db",
		MaxSaveSize:   64 << 20,
		ReadTimeout:   5 * time.Minute,
		WriteTimeout:  5 * time.Minute,
		IdleTimeout:   2 * time.Minute,
		ShareTTL:      24 * time.Hour,
		PairingTTL:    10 * time.Minute,
		BlobChunkSize: 1 << 20,
	}
}

// FromEnv layers DAEMOON_* variables over the defaults.
func FromEnv() (Config, error) {
	c := Default()

	if v := os.Getenv("DAEMOON_ADDR"); v != "" {
		c.Addr = v
	}
	if v := os.Getenv("DAEMOON_DB"); v != "" {
		c.Database = v
	}
	if v := os.Getenv("DAEMOON_MAX_SAVE_SIZE"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil {
			return c, fmt.Errorf("DAEMOON_MAX_SAVE_SIZE: %w", err)
		}
		if n <= 0 {
			return c, fmt.Errorf("DAEMOON_MAX_SAVE_SIZE must be positive, got %d", n)
		}
		c.MaxSaveSize = n
	}
	for _, d := range []struct {
		env string
		dst *time.Duration
	}{
		{"DAEMOON_READ_TIMEOUT", &c.ReadTimeout},
		{"DAEMOON_WRITE_TIMEOUT", &c.WriteTimeout},
		{"DAEMOON_IDLE_TIMEOUT", &c.IdleTimeout},
		{"DAEMOON_SHARE_TTL", &c.ShareTTL},
		{"DAEMOON_PAIRING_TTL", &c.PairingTTL},
	} {
		v := os.Getenv(d.env)
		if v == "" {
			continue
		}
		parsed, err := time.ParseDuration(v)
		if err != nil {
			return c, fmt.Errorf("%s: %w", d.env, err)
		}
		if parsed <= 0 {
			// Every request gets a timeout. There are no unbounded waits, so
			// "0 means forever" is not an option worth offering.
			return c, fmt.Errorf("%s must be positive, got %s", d.env, parsed)
		}
		*d.dst = parsed
	}
	return c, nil
}
