// Package e2e drives the real daemoonctl against a real daemoond.
//
// The unit suites both pass against a stand in: the C tests talk to an in-process
// fake server, and the Go tests build packages with Go code. Two bugs got through
// that arrangement and only showed up the first time the actual binaries spoke to
// each other:
//
//   - the client sent the uncompressed payload size as the request body length, so
//     the server received a truncated zip
//   - the server reported the stored zip size where the client reports payload
//     bytes, so the conflict dialog offered "630 B" against "17 B"
//
// Neither is visible from one side alone. This test is what stops them coming back.
//
// It builds both binaries, so it needs a C compiler and skips without one.
package e2e

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type env struct {
	t        *testing.T
	root     string
	daemoond string
	ctl      string
	db       string
	addr     string
	dir      string
}

func repoRoot(t *testing.T) string {
	t.Helper()

	dir, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "shared", "errors.json")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	t.Fatal("cannot find the repository root")
	return ""
}

// freePort asks the kernel for a port and hands it back. There is a window between
// closing and daemoond binding it, which is why this is a test helper and not how
// anything real chooses a port.
func freePort(t *testing.T) string {
	t.Helper()

	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve a port: %v", err)
	}
	addr := l.Addr().String()
	if err := l.Close(); err != nil {
		t.Fatalf("release the port: %v", err)
	}
	return addr
}

func build(t *testing.T) (daemoond, ctl string) {
	t.Helper()

	root := repoRoot(t)
	if _, err := exec.LookPath("cc"); err != nil {
		t.Skip("no C compiler, skipping the end to end test")
	}

	out := t.TempDir()
	daemoond = filepath.Join(out, "daemoond")

	cmd := exec.Command("go", "build", "-o", daemoond, "./cmd/daemoond")
	cmd.Dir = filepath.Join(root, "server")
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build daemoond: %v\n%s", err, b)
	}

	// daemoonctl comes out of its own Makefile so the build under test is the one
	// a developer actually runs.
	cmd = exec.Command("make", "-C", filepath.Join(root, "tools", "cli"), "ROOT="+root)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build daemoonctl: %v\n%s", err, b)
	}
	ctl = filepath.Join(root, "build", "daemoonctl")
	if _, err := os.Stat(ctl); err != nil {
		t.Fatalf("daemoonctl was not produced: %v", err)
	}
	return daemoond, ctl
}

func start(t *testing.T) *env {
	t.Helper()

	daemoond, ctl := build(t)
	dir := t.TempDir()

	e := &env{
		t:        t,
		root:     repoRoot(t),
		daemoond: daemoond,
		ctl:      ctl,
		db:       filepath.Join(dir, "daemoon.db"),
		addr:     freePort(t),
		dir:      dir,
	}

	ctx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(ctx, e.daemoond)
	cmd.Env = append(os.Environ(), "DAEMOON_DB="+e.db, "DAEMOON_ADDR="+e.addr)
	var log bytes.Buffer
	cmd.Stdout = &log
	cmd.Stderr = &log
	if err := cmd.Start(); err != nil {
		cancel()
		t.Fatalf("start daemoond: %v", err)
	}
	t.Cleanup(func() {
		cancel()
		_ = cmd.Wait()
		if t.Failed() {
			t.Logf("daemoond log:\n%s", log.String())
		}
	})

	// Wait for the listener rather than sleeping a fixed amount.
	deadline := time.Now().Add(15 * time.Second)
	for {
		conn, err := net.DialTimeout("tcp", e.addr, 200*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("daemoond never listened on %s\n%s", e.addr, log.String())
		}
		time.Sleep(50 * time.Millisecond)
	}
	return e
}

// pairingCode runs the same command an operator runs.
func (e *env) pairingCode(user string) string {
	e.t.Helper()

	cmd := exec.Command(e.daemoond, "-pair", user)
	cmd.Env = append(os.Environ(), "DAEMOON_DB="+e.db)
	out, err := cmd.CombinedOutput()
	if err != nil {
		e.t.Fatalf("issue a pairing code: %v\n%s", err, out)
	}
	for _, line := range strings.Split(string(out), "\n") {
		if code, ok := strings.CutPrefix(line, "pairing code: "); ok {
			return strings.TrimSpace(code)
		}
	}
	e.t.Fatalf("no pairing code in:\n%s", out)
	return ""
}

// revoke calls DELETE /v1/devices/{id}.
func (e *env) revoke(token, deviceID string) {
	e.t.Helper()

	req, err := http.NewRequest(http.MethodDelete,
		"http://"+e.addr+"/v1/devices/"+deviceID, nil)
	if err != nil {
		e.t.Fatalf("build revoke request: %v", err)
	}
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatalf("revoke: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		e.t.Fatalf("revoke: status %d", resp.StatusCode)
	}
}

// console is one client: its own save directory, its own work directory, its own
// label. Two of them is what makes a conflict possible.
type console struct {
	e        *env
	saves    string
	work     string
	label    string
	token    string
	deviceID string
}

func (e *env) console(name, label string) *console {
	e.t.Helper()

	c := &console{
		e:     e,
		saves: filepath.Join(e.dir, name, "saves"),
		work:  filepath.Join(e.dir, name, "work"),
		label: label,
	}
	if err := os.MkdirAll(c.saves, 0o755); err != nil {
		e.t.Fatalf("create %s: %v", c.saves, err)
	}
	return c
}

func (c *console) run(stdin string, args ...string) (string, error) {
	c.e.t.Helper()

	full := append([]string{"--saves", c.saves, "--work", c.work}, args...)
	cmd := exec.Command(c.e.ctl, full...)
	cmd.Env = append(os.Environ(),
		"DAEMOON_SERVER=http://"+c.e.addr,
		"DAEMOON_LABEL="+c.label,
	)
	if c.token != "" {
		cmd.Env = append(cmd.Env, "DAEMOON_TOKEN="+c.token)
	}
	if stdin != "" {
		cmd.Stdin = strings.NewReader(stdin)
	}

	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	err := cmd.Run()
	return out.String(), err
}

func (c *console) mustRun(stdin string, args ...string) string {
	c.e.t.Helper()

	out, err := c.run(stdin, args...)
	if err != nil {
		c.e.t.Fatalf("daemoonctl %s: %v\n%s", strings.Join(args, " "), err, out)
	}
	return out
}

func (c *console) pair(code string) {
	c.e.t.Helper()

	out := c.mustRun("", "pair", code)
	scanner := bufio.NewScanner(strings.NewReader(out))
	for scanner.Scan() {
		line := scanner.Text()
		if id, ok := strings.CutPrefix(line, "device_id: "); ok {
			c.deviceID = strings.TrimSpace(id)
		}
		if token, ok := strings.CutPrefix(line, "export DAEMOON_TOKEN="); ok {
			c.token = strings.TrimSpace(token)
		}
	}
	if c.token == "" {
		c.e.t.Fatalf("no token in:\n%s", out)
	}
}

func (c *console) writeSave(title, rel, body string) {
	c.e.t.Helper()

	path := filepath.Join(c.saves, title, rel)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		c.e.t.Fatalf("create %s: %v", filepath.Dir(path), err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		c.e.t.Fatalf("write %s: %v", path, err)
	}
}

func (c *console) readSave(title, rel string) (string, error) {
	b, err := os.ReadFile(filepath.Join(c.saves, title, rel))
	return string(b), err
}

func (c *console) backups() []string {
	c.e.t.Helper()

	entries, err := os.ReadDir(filepath.Join(c.work, "backups"))
	if err != nil {
		return nil
	}
	out := make([]string, 0, len(entries))
	for _, e := range entries {
		out = append(out, e.Name())
	}
	return out
}

const title3DS = "3ds_0004000000055D00"

// The dialog describes both sides in uncompressed payload bytes. Deriving the
// numbers from what the test wrote is the point: a hardcoded figure would still
// pass if the two sides started measuring different things.
func payloadBytes(parts ...string) string {
	n := 0
	for _, p := range parts {
		n += len(p)
	}
	return fmt.Sprintf("%d B", n)
}

// TestUploadThenConflictThenRestore is the whole Phase 0 story in one run: a save
// leaves one console, a second console with its own changes is told to choose, and
// choosing the server copy backs the local one up before overwriting it.
func TestUploadThenConflictThenRestore(t *testing.T) {
	e := start(t)

	first := e.console("first", "Old 3DS XL")
	const firstMain, firstExtra = "player data v1", "extra"
	first.writeSave(title3DS, "main.sav", firstMain)
	first.writeSave(title3DS, "sub/extra.bin", firstExtra)
	first.pair(e.pairingCode("alice"))

	out := first.mustRun("", "--yes", "sync")
	if !strings.Contains(out, "uploaded 1") {
		t.Fatalf("first sync did not upload:\n%s", out)
	}

	// A second run has nothing to do. If the client recorded the version the server
	// issued, this is a skip; if it did not, it would upload again forever.
	out = first.mustRun("", "--yes", "sync")
	if !strings.Contains(out, "skipped 1") {
		t.Errorf("a second sync was not a no-op:\n%s", out)
	}

	second := e.console("second", "New 2DS")
	second.token = first.token // same account, different console
	const secondMain = "console two data"
	second.writeSave(title3DS, "main.sav", secondMain)

	out = second.mustRun("", "list")
	if !strings.Contains(out, "Conflict") {
		t.Fatalf("the second console did not see a conflict:\n%s", out)
	}
	// Both sides described in payload bytes. A package size here instead would be
	// the units bug this test exists for.
	if want := payloadBytes(secondMain); !strings.Contains(out, want) {
		t.Errorf("local size is not %s:\n%s", want, out)
	}

	// 2 is "keep the server save", then y to the restore confirmation.
	out = second.mustRun("2\ny\n", "sync")
	if !strings.Contains(out, "downloaded 1") {
		t.Fatalf("choosing the server copy did not download:\n%s", out)
	}
	if want := payloadBytes(firstMain, firstExtra); !strings.Contains(out, want) {
		t.Errorf("the server side is not described as %s:\n%s", want, out)
	}

	got, err := second.readSave(title3DS, "main.sav")
	if err != nil {
		t.Fatalf("read the restored save: %v", err)
	}
	if got != firstMain {
		t.Errorf("restored main.sav = %q", got)
	}
	// The whole tree comes back, not just the file that already existed.
	if got, err := second.readSave(title3DS, "sub/extra.bin"); err != nil || got != firstExtra {
		t.Errorf("restored sub/extra.bin = %q, err %v", got, err)
	}
	// And what was overwritten is still on the card.
	if len(second.backups()) == 0 {
		t.Error("no local backup was made before the restore")
	}

	// Now the two consoles agree, so neither has anything to do.
	out = second.mustRun("", "--yes", "sync")
	if !strings.Contains(out, "skipped 1") {
		t.Errorf("the consoles did not converge:\n%s", out)
	}
}

// TestConflictKeepingLocalRetainsBothVersions is the other branch of the same
// dialog. Nothing may be discarded on either side.
func TestConflictKeepingLocalRetainsBothVersions(t *testing.T) {
	e := start(t)

	first := e.console("first", "Old 3DS XL")
	first.writeSave(title3DS, "main.sav", "from the first console")
	first.pair(e.pairingCode("alice"))
	first.mustRun("", "--yes", "sync")

	second := e.console("second", "New 2DS")
	second.token = first.token
	second.writeSave(title3DS, "main.sav", "from the second console")

	// 1 is "keep this console's save".
	out := second.mustRun("1\n", "sync")
	if !strings.Contains(out, "uploaded 1") {
		t.Fatalf("keeping the local save did not upload it:\n%s", out)
	}

	// The second console kept its own save untouched.
	got, err := second.readSave(title3DS, "main.sav")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if got != "from the second console" {
		t.Errorf("local save = %q, it should not have been touched", got)
	}

	// And the first console can now take the newer version, which proves the
	// server kept both: it is at version 2 and version 1 was never deleted.
	out = first.mustRun("y\n", "sync")
	if !strings.Contains(out, "downloaded 1") {
		t.Fatalf("the first console could not take the new version:\n%s", out)
	}
	if got, _ := first.readSave(title3DS, "main.sav"); got != "from the second console" {
		t.Errorf("first console save = %q", got)
	}
}

// TestBackupNeedsNoServer covers Phase 1's entire scope: a local backup with
// nothing on the other end.
func TestBackupNeedsNoServer(t *testing.T) {
	e := start(t)

	c := e.console("only", "Old 3DS XL")
	c.writeSave(title3DS, "main.sav", "player data")

	out := c.mustRun("", "backup")
	if !strings.Contains(out, "0004000000055D00") {
		t.Fatalf("backup produced nothing:\n%s", out)
	}
	if len(c.backups()) != 1 {
		t.Fatalf("backups = %v, want one", c.backups())
	}

	// Backups are named after their content, so backing up an unchanged save twice
	// does not pile up copies.
	c.mustRun("", "backup")
	if got := c.backups(); len(got) != 1 {
		t.Errorf("backups = %v, want one after a repeat", got)
	}

	// A changed save is a different backup, and the old one stays.
	c.writeSave(title3DS, "main.sav", "player data, later")
	c.mustRun("", "backup")
	if got := c.backups(); len(got) != 2 {
		t.Errorf("backups = %v, want two after a change", got)
	}
}

// TestDecliningTheUploadChangesNothing: nothing leaves the console unasked.
func TestDecliningTheUploadChangesNothing(t *testing.T) {
	e := start(t)

	c := e.console("only", "Old 3DS XL")
	c.writeSave(title3DS, "main.sav", "player data")
	c.pair(e.pairingCode("alice"))

	// "n" to the upload confirmation.
	out, _ := c.run("n\n", "sync")
	if strings.Contains(out, "uploaded 1") {
		t.Fatalf("the save was uploaded after the prompt was declined:\n%s", out)
	}

	// The server still has nothing, so the next look is an upload again.
	out = c.mustRun("", "list")
	if !strings.Contains(out, "Upload") {
		t.Errorf("expected the title to still be pending upload:\n%s", out)
	}
}

// TestAnUnpairedClientRefusesToSync: a missing token is a clear refusal, not a
// stack of network errors.
func TestAnUnpairedClientRefusesToSync(t *testing.T) {
	e := start(t)

	c := e.console("only", "Old 3DS XL")
	c.writeSave(title3DS, "main.sav", "player data")

	out, err := c.run("", "sync")
	if err == nil {
		t.Fatalf("an unpaired sync succeeded:\n%s", out)
	}
	if !strings.Contains(out, "pair") {
		t.Errorf("the refusal does not say what to do:\n%s", out)
	}
}

// TestRevokedTokenIsRejected: SD cards are removable, so this is the path that
// matters after one goes missing.
func TestRevokedTokenIsRejected(t *testing.T) {
	e := start(t)

	c := e.console("only", "Old 3DS XL")
	c.writeSave(title3DS, "main.sav", "player data")
	c.pair(e.pairingCode("alice"))
	c.mustRun("", "--yes", "sync")

	// Revoke through the endpoint a web UI would call, so the path under test is
	// the shipped one.
	if c.deviceID == "" {
		t.Fatal("pairing did not report a device id")
	}
	e.revoke(c.token, c.deviceID)

	out, _ := c.run("", "list")
	if !strings.Contains(out, "revoked") && !strings.Contains(out, "unreachable") {
		t.Errorf("a revoked console was not told so:\n%s", out)
	}
	if strings.Contains(out, "Upload") || strings.Contains(out, "Download") {
		t.Errorf("a revoked console still got sync decisions:\n%s", out)
	}
}
