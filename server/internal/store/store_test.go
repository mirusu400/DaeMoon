package store_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/mirusu400/DaeMoon/server/internal/store"
	"github.com/mirusu400/DaeMoon/server/migrations"
)

func open(t *testing.T) *store.Store {
	t.Helper()

	// A small chunk size so the chunking path runs on payloads a test is willing
	// to build.
	s, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "test.db"), 512)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Errorf("close: %v", err)
		}
	})
	if err := s.EnsureUser(context.Background(), "user-1"); err != nil {
		t.Fatalf("ensure user: %v", err)
	}
	return s
}

func put(t *testing.T, s *store.Store, parent uint32, sha string, size uint64,
	body []byte) (store.VersionMeta, error) {
	t.Helper()

	return s.Put(context.Background(), store.PutRequest{
		UserID:        "user-1",
		DeviceLabel:   "test console",
		Platform:      "3ds",
		TitleID:       "0004000000055D00",
		SaveType:      "savedata",
		ParentVersion: parent,
		SHA256:        sha,
		Size:          size,
		Body:          bytes.NewReader(body),
	})
}

// digest strings that are the right shape without being real. The store does not
// compute them; pkgfmt does, and the handler passes them through.
func fakeSHA(n byte) string {
	return fmt.Sprintf("%064x", n)
}

func countRows(t *testing.T, s *store.Store, query string, args ...any) int {
	t.Helper()

	var n int
	if err := s.DB().QueryRow(query, args...).Scan(&n); err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	return n
}

// TestMigrationsAreIdempotent: a self hosted instance restarts, and every restart
// runs the migrations again.
func TestMigrationsAreIdempotent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "restart.db")

	for i := 0; i < 3; i++ {
		s, err := store.Open(context.Background(), path, 512)
		if err != nil {
			t.Fatalf("open %d: %v", i, err)
		}
		if err := s.Close(); err != nil {
			t.Fatalf("close %d: %v", i, err)
		}
	}

	s, err := store.Open(context.Background(), path, 512)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer func() { _ = s.Close() }()

	// Counted from the embedded files rather than written down here, so adding a
	// migration does not mean remembering to bump a number in a test - which is a
	// failure that says nothing about migrations and takes a minute to read.
	files, err := fs.Glob(migrations.FS, "*.sql")
	if err != nil {
		t.Fatalf("list migrations: %v", err)
	}
	if n := countRows(t, s, `SELECT count(*) FROM schema_migrations`); n != len(files) {
		t.Errorf("schema_migrations has %d rows after four opens, want %d",
			n, len(files))
	}
}

func TestPutIssuesConsecutiveVersions(t *testing.T) {
	s := open(t)

	first, err := put(t, s, 0, fakeSHA(1), 10, []byte("package one"))
	if err != nil {
		t.Fatalf("first put: %v", err)
	}
	if first.Version != 1 {
		t.Fatalf("first version = %d, want 1", first.Version)
	}
	if first.ParentVersion != nil {
		t.Errorf("first parent = %v, want null", *first.ParentVersion)
	}

	second, err := put(t, s, 1, fakeSHA(2), 11, []byte("package two"))
	if err != nil {
		t.Fatalf("second put: %v", err)
	}
	if second.Version != 2 {
		t.Fatalf("second version = %d, want 2", second.Version)
	}
	if second.ParentVersion == nil || *second.ParentVersion != 1 {
		t.Errorf("second parent = %v, want 1", second.ParentVersion)
	}
}

// TestPutRejectsAStaleParent is the whole conflict model at the storage layer.
func TestPutRejectsAStaleParent(t *testing.T) {
	s := open(t)

	if _, err := put(t, s, 0, fakeSHA(1), 10, []byte("first")); err != nil {
		t.Fatalf("first put: %v", err)
	}

	// Another console still thinks the server is empty.
	if _, err := put(t, s, 0, fakeSHA(2), 10, []byte("stale")); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("stale put: %v, want ErrConflict", err)
	}
	// And one that is ahead of the server is just as wrong.
	if _, err := put(t, s, 9, fakeSHA(3), 10, []byte("ahead")); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("future put: %v, want ErrConflict", err)
	}

	// Neither attempt left anything behind: still one version, still one blob.
	if n := countRows(t, s, `SELECT count(*) FROM versions`); n != 1 {
		t.Errorf("versions = %d, want 1", n)
	}
	if n := countRows(t, s, `SELECT count(*) FROM blobs`); n != 1 {
		t.Errorf("blobs = %d, want 1", n)
	}
}

// TestFirstUploadOfAKnownTitleConflicts: a console that lost its state file must
// not be able to reset a title's history by claiming parent 0.
func TestFirstUploadOfAKnownTitleConflicts(t *testing.T) {
	s := open(t)

	if _, err := put(t, s, 0, fakeSHA(1), 10, []byte("first")); err != nil {
		t.Fatalf("first put: %v", err)
	}
	if _, err := put(t, s, 0, fakeSHA(9), 10, []byte("reset")); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("reset attempt: %v, want ErrConflict", err)
	}

	latest, err := s.Latest(context.Background(), "user-1", "3ds", "0004000000055D00")
	if err != nil {
		t.Fatalf("latest: %v", err)
	}
	if latest.Version != 1 || latest.SHA256 != fakeSHA(1) {
		t.Errorf("latest = v%d %s, want v1 %s", latest.Version, latest.SHA256, fakeSHA(1))
	}
}

// TestIdenticalContentIsStoredOnce is the content addressing the schema comment
// promises. Two consoles with the same save cost one copy, and so do the two sides
// of a conflict once they converge.
func TestIdenticalContentIsStoredOnce(t *testing.T) {
	s := open(t)
	body := []byte("the same package bytes")

	if _, err := put(t, s, 0, fakeSHA(1), 22, body); err != nil {
		t.Fatalf("first put: %v", err)
	}
	if _, err := put(t, s, 1, fakeSHA(2), 22, []byte("something else")); err != nil {
		t.Fatalf("second put: %v", err)
	}
	// Back to the original content, as happens when a console restores and then
	// syncs again.
	if _, err := put(t, s, 2, fakeSHA(1), 22, body); err != nil {
		t.Fatalf("third put: %v", err)
	}

	if n := countRows(t, s, `SELECT count(*) FROM versions`); n != 3 {
		t.Errorf("versions = %d, want 3", n)
	}
	if n := countRows(t, s, `SELECT count(*) FROM blobs`); n != 2 {
		t.Errorf("blobs = %d, want 2: identical content should be stored once", n)
	}

	// The shared blob is referenced twice, which is what a future GC will read.
	var refcount int
	if err := s.DB().QueryRow(`SELECT refcount FROM blobs WHERE sha256 = ?`,
		fakeSHA(1)).Scan(&refcount); err != nil {
		t.Fatalf("read refcount: %v", err)
	}
	if refcount != 2 {
		t.Errorf("refcount = %d, want 2", refcount)
	}
}

// TestBlobsAreChunked: one row per save would mean reading a whole save into
// memory on every download, which is the reason blob_chunks exists.
func TestBlobsAreChunked(t *testing.T) {
	s := open(t)

	body := bytes.Repeat([]byte("x"), 512*4+7) // chunk size is 512
	meta, err := put(t, s, 0, fakeSHA(1), uint64(len(body)), body)
	if err != nil {
		t.Fatalf("put: %v", err)
	}

	if n := countRows(t, s, `SELECT count(*) FROM blob_chunks`); n != 5 {
		t.Errorf("chunks = %d, want 5", n)
	}

	var out bytes.Buffer
	if err := s.WriteBlobTo(context.Background(), meta, &out); err != nil {
		t.Fatalf("write blob: %v", err)
	}
	if !bytes.Equal(out.Bytes(), body) {
		t.Errorf("round trip returned %d bytes, want %d", out.Len(), len(body))
	}
}

func TestEmptyBlobRoundTrips(t *testing.T) {
	// A save with no files at all is unusual but legal, and a zero length blob is
	// the kind of edge that gets discovered by a user rather than a test.
	s := open(t)

	meta, err := put(t, s, 0, fakeSHA(1), 0, nil)
	if err != nil {
		t.Fatalf("put: %v", err)
	}

	var out bytes.Buffer
	if err := s.WriteBlobTo(context.Background(), meta, &out); err != nil {
		t.Fatalf("write blob: %v", err)
	}
	if out.Len() != 0 {
		t.Errorf("empty blob returned %d bytes", out.Len())
	}
}

// TestWriteBlobToPropagatesAWriteFailure: a console dropping the connection mid
// download is ordinary, and it has to come back as an error rather than a silent
// short write.
func TestWriteBlobToPropagatesAWriteFailure(t *testing.T) {
	s := open(t)

	body := bytes.Repeat([]byte("y"), 512*3)
	meta, err := put(t, s, 0, fakeSHA(1), uint64(len(body)), body)
	if err != nil {
		t.Fatalf("put: %v", err)
	}

	err = s.WriteBlobTo(context.Background(), meta, failingWriter{after: 512})
	if err == nil {
		t.Fatal("a failing writer did not produce an error")
	}
}

type failingWriter struct{ after int }

func (w failingWriter) Write(p []byte) (int, error) {
	if len(p) >= w.after {
		return 0, io.ErrClosedPipe
	}
	return len(p), nil
}

// TestConcurrentPutsProduceOneWinner is the race two consoles syncing at once
// actually run into. Exactly one may get a version; the rest have to be told.
func TestConcurrentPutsProduceOneWinner(t *testing.T) {
	s := open(t)

	const attempts = 8
	var wg sync.WaitGroup
	results := make([]error, attempts)

	wg.Add(attempts)
	for i := 0; i < attempts; i++ {
		go func(i int) {
			defer wg.Done()
			_, err := put(t, s, 0, fakeSHA(byte(i+1)), 10, []byte(fmt.Sprintf("body %d", i)))
			results[i] = err
		}(i)
	}
	wg.Wait()

	won := 0
	for i, err := range results {
		switch {
		case err == nil:
			won++
		case errors.Is(err, store.ErrConflict):
		default:
			t.Errorf("attempt %d: %v, want nil or ErrConflict", i, err)
		}
	}
	if won != 1 {
		t.Fatalf("%d attempts succeeded, want exactly 1", won)
	}
	if n := countRows(t, s, `SELECT count(*) FROM versions`); n != 1 {
		t.Errorf("versions = %d, want 1", n)
	}
}

func TestTitlesAreScopedToAnAccountAndPlatform(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if err := s.EnsureUser(ctx, "user-2"); err != nil {
		t.Fatalf("ensure user-2: %v", err)
	}
	if _, err := put(t, s, 0, fakeSHA(1), 10, []byte("user one")); err != nil {
		t.Fatalf("put: %v", err)
	}

	// The same title id under another account is a different title.
	if _, err := s.Latest(ctx, "user-2", "3ds", "0004000000055D00"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("cross account latest: %v, want ErrNotFound", err)
	}
	// And so is the same id on another platform.
	if _, err := s.Latest(ctx, "user-1", "nx", "0004000000055D00"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("cross platform latest: %v, want ErrNotFound", err)
	}

	titles, err := s.ListTitles(ctx, "user-2", "")
	if err != nil {
		t.Fatalf("list titles: %v", err)
	}
	if len(titles) != 0 {
		t.Errorf("user-2 sees %d titles", len(titles))
	}
}

func TestDeviceRevocation(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if err := s.CreateDevice(ctx, store.Device{
		ID: "dev-1", UserID: "user-1", Label: "3DS", Platform: "3ds",
	}, "hash-1"); err != nil {
		t.Fatalf("create device: %v", err)
	}

	d, err := s.DeviceByTokenHash(ctx, "hash-1")
	if err != nil {
		t.Fatalf("lookup: %v", err)
	}
	if d.Revoked {
		t.Error("a new device is already revoked")
	}

	if err := s.RevokeDevice(ctx, "user-1", "dev-1"); err != nil {
		t.Fatalf("revoke: %v", err)
	}
	// Revoking twice is not an error: a lost SD card may be revoked from two
	// places at once, and the second attempt should not look like a failure.
	if err := s.RevokeDevice(ctx, "user-1", "dev-1"); err != nil {
		t.Fatalf("second revoke: %v", err)
	}

	d, err = s.DeviceByTokenHash(ctx, "hash-1")
	if err != nil {
		t.Fatalf("lookup after revoke: %v", err)
	}
	if !d.Revoked {
		t.Error("the device is not marked revoked")
	}

	// Another account cannot revoke this device, and cannot learn it exists.
	if err := s.EnsureUser(ctx, "user-2"); err != nil {
		t.Fatalf("ensure user-2: %v", err)
	}
	if err := s.RevokeDevice(ctx, "user-2", "dev-1"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("cross account revoke: %v, want ErrNotFound", err)
	}
	if err := s.RevokeDevice(ctx, "user-1", "no-such-device"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("unknown device: %v, want ErrNotFound", err)
	}
}

func TestSharesExpire(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	if _, err := put(t, s, 0, fakeSHA(1), 10, []byte("shared")); err != nil {
		t.Fatalf("put: %v", err)
	}
	if _, err := s.CreateShare(ctx, "code-1", "user-1", "3ds", "0004000000055D00", 1,
		time.Hour); err != nil {
		t.Fatalf("create share: %v", err)
	}

	meta, err := s.ResolveShare(ctx, "code-1")
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if meta.Version != 1 {
		t.Errorf("share points at v%d", meta.Version)
	}

	// Expiry is distinct from absence: the two mean different things to whoever is
	// holding the code.
	s.SetClock(func() time.Time { return time.Now().Add(2 * time.Hour) })
	if _, err := s.ResolveShare(ctx, "code-1"); !errors.Is(err, store.ErrExpired) {
		t.Errorf("expired share: %v, want ErrExpired", err)
	}
	if _, err := s.ResolveShare(ctx, "no-such-code"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("unknown share: %v, want ErrNotFound", err)
	}
}

func TestShareOfAVersionThatDoesNotExist(t *testing.T) {
	s := open(t)

	if _, err := s.CreateShare(context.Background(), "code-1", "user-1", "3ds",
		"0004000000055D00", 7, time.Hour); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("share of a missing version: %v, want ErrNotFound", err)
	}
}

func TestPairingLifecycle(t *testing.T) {
	s := open(t)
	ctx := context.Background()

	// Unapproved: the console polls and the row survives.
	if err := s.CreatePairing(ctx, "111111", "user-1", false, time.Minute); err != nil {
		t.Fatalf("create pairing: %v", err)
	}
	p, err := s.ClaimPairing(ctx, "111111")
	if err != nil {
		t.Fatalf("claim: %v", err)
	}
	if p.Approved {
		t.Fatal("an unapproved pairing came back approved")
	}
	if _, err := s.ClaimPairing(ctx, "111111"); err != nil {
		t.Fatalf("second poll: %v", err)
	}

	// Approved: claiming consumes it, so a code cannot mint two tokens.
	if err := s.ApprovePairing(ctx, "111111", "user-1"); err != nil {
		t.Fatalf("approve: %v", err)
	}
	p, err = s.ClaimPairing(ctx, "111111")
	if err != nil {
		t.Fatalf("claim approved: %v", err)
	}
	if !p.Approved || p.UserID != "user-1" {
		t.Fatalf("claim returned %+v", p)
	}
	if _, err := s.ClaimPairing(ctx, "111111"); !errors.Is(err, store.ErrNotFound) {
		t.Errorf("reused code: %v, want ErrNotFound", err)
	}

	// Expired codes are refused rather than silently working.
	if err := s.CreatePairing(ctx, "222222", "user-1", true, time.Minute); err != nil {
		t.Fatalf("create second pairing: %v", err)
	}
	s.SetClock(func() time.Time { return time.Now().Add(time.Hour) })
	if _, err := s.ClaimPairing(ctx, "222222"); !errors.Is(err, store.ErrExpired) {
		t.Errorf("expired pairing: %v, want ErrExpired", err)
	}
}

func TestOpenRejectsAnUnusableChunkSize(t *testing.T) {
	if _, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "x.db"), 0); err == nil {
		t.Error("a zero chunk size was accepted")
	}
}
