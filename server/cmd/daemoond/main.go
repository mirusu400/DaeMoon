// Command daemoond is the DaeMoon server.
//
// One static binary plus one database file. That is the whole deployment, because
// self hosting is a primary goal and anything more becomes a reason not to bother.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/mirusu400/DaeMoon/server/internal/api"
	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

// version is set with -ldflags "-X main.version=..." by the release build.
var version = "dev"

func main() {
	if err := run(); err != nil {
		slog.Error("daemoond failed", "err", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		showVersion = flag.Bool("version", false, "print the version and exit")
		pairUser    = flag.String("pair", "", "issue a pairing code for this user id and exit")
		debug       = flag.Bool("debug", false, "log at debug level")
	)
	flag.Parse()

	if *showVersion {
		fmt.Println(version)
		return nil
	}

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	cfg, err := config.FromEnv()
	if err != nil {
		return fmt.Errorf("configuration: %w", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	st, err := store.Open(ctx, cfg.Database, cfg.BlobChunkSize)
	if err != nil {
		return fmt.Errorf("open store: %w", err)
	}
	defer func() {
		if err := st.Close(); err != nil {
			slog.Error("closing the store failed", "err", err)
		}
	}()

	if *pairUser != "" {
		return issuePairing(ctx, st, cfg, *pairUser)
	}

	api.Version = version
	srv := &http.Server{
		Addr:    cfg.Addr,
		Handler: api.New(st, cfg).Routes(),
		// Every request gets a timeout. No unbounded waits.
		ReadTimeout:       cfg.ReadTimeout,
		WriteTimeout:      cfg.WriteTimeout,
		IdleTimeout:       cfg.IdleTimeout,
		ReadHeaderTimeout: 15 * time.Second,
	}

	errc := make(chan error, 1)
	go func() {
		slog.Info("listening", "addr", cfg.Addr, "db", cfg.Database, "version", version)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errc <- fmt.Errorf("listen: %w", err)
			return
		}
		errc <- nil
	}()

	select {
	case err := <-errc:
		return err
	case <-ctx.Done():
		slog.Info("shutting down")
		// A console may be halfway through an upload. Give it a moment rather than
		// cutting the connection and leaving the user unsure what happened.
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			return fmt.Errorf("shutdown: %w", err)
		}
		return nil
	}
}

// issuePairing prints a code the console can use. Until there is a web interface
// this is how a device gets paired, and it keeps the flow the same one the API
// implements rather than inventing a second path.
func issuePairing(ctx context.Context, st *store.Store, cfg config.Config, userID string) error {
	if err := st.EnsureUser(ctx, userID); err != nil {
		return fmt.Errorf("ensure user: %w", err)
	}

	code, err := auth.NewPairingCode()
	if err != nil {
		return fmt.Errorf("generate pairing code: %w", err)
	}
	// Approved on creation: whoever can run this command on the server is already
	// the account holder, so there is nobody left to ask.
	if err := st.CreatePairing(ctx, code, userID, true, cfg.PairingTTL); err != nil {
		return fmt.Errorf("create pairing: %w", err)
	}

	fmt.Printf("pairing code: %s\nvalid for:    %s\n", code, cfg.PairingTTL)
	return nil
}
