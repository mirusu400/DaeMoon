package api_test

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/mirusu400/DaeMoon/server/internal/api"
	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/pkgfmt"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

type harness struct {
	t     *testing.T
	srv   *httptest.Server
	store *store.Store
	cfg   config.Config
	token string
}

func newHarness(t *testing.T) *harness {
	t.Helper()

	cfg := config.Default()
	cfg.Database = filepath.Join(t.TempDir(), "test.db")
	// A small chunk size so the chunking path is actually exercised by a payload a
	// test is willing to build.
	cfg.BlobChunkSize = 512

	st, err := store.Open(context.Background(), cfg.Database, cfg.BlobChunkSize)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() {
		if err := st.Close(); err != nil {
			t.Errorf("close store: %v", err)
		}
	})

	srv := httptest.NewServer(api.New(st, cfg).Routes())
	t.Cleanup(srv.Close)

	h := &harness{t: t, srv: srv, store: st, cfg: cfg}
	h.token = h.pair("user-1", "test console")
	return h
}

// pair walks the real pairing flow rather than inserting a token directly, so the
// flow the consoles use is the flow under test.
func (h *harness) pair(userID, label string) string {
	h.t.Helper()

	ctx := context.Background()
	if err := h.store.EnsureUser(ctx, userID); err != nil {
		h.t.Fatalf("ensure user: %v", err)
	}
	code, err := auth.NewPairingCode()
	if err != nil {
		h.t.Fatalf("pairing code: %v", err)
	}
	if err := h.store.CreatePairing(ctx, code, userID, true, h.cfg.PairingTTL); err != nil {
		h.t.Fatalf("create pairing: %v", err)
	}

	body, _ := json.Marshal(map[string]string{
		"grant": "device_code", "code": code, "label": label, "platform": "3ds",
	})
	resp := h.do("POST", "/v1/devices/pair", "application/json", bytes.NewReader(body), "")
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		h.t.Fatalf("pair: status %d: %s", resp.StatusCode, readAll(h.t, resp.Body))
	}
	var out struct {
		DeviceID string `json:"device_id"`
		Token    string `json:"token"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		h.t.Fatalf("decode pair response: %v", err)
	}
	if out.Token == "" {
		h.t.Fatal("pairing returned an empty token")
	}
	return out.Token
}

func (h *harness) do(method, path, contentType string, body io.Reader, token string) *http.Response {
	h.t.Helper()

	req, err := http.NewRequest(method, h.srv.URL+path, body)
	if err != nil {
		h.t.Fatalf("build request: %v", err)
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := h.srv.Client().Do(req)
	if err != nil {
		h.t.Fatalf("%s %s: %v", method, path, err)
	}
	return resp
}

func (h *harness) auth(method, path, contentType string, body io.Reader) *http.Response {
	return h.do(method, path, contentType, body, h.token)
}

func readAll(t *testing.T, r io.Reader) string {
	t.Helper()
	b, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	return string(b)
}

func errorCode(t *testing.T, resp *http.Response) string {
	t.Helper()

	var body struct {
		Error struct {
			Code   string         `json:"code"`
			Detail map[string]any `json:"detail"`
		} `json:"error"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode error body: %v", err)
	}
	return body.Error.Code
}

func samplePackage(t *testing.T, parent uint32, files map[string]string) []byte {
	t.Helper()

	m := pkgfmt.Manifest{
		Platform:    pkgfmt.Platform3DS,
		TitleID:     "0004000000055D00",
		SaveType:    pkgfmt.SaveData,
		DeviceLabel: "test console",
		CreatedAt:   "2026-01-01T00:00:00Z",
	}
	if parent > 0 {
		p := parent
		m.ParentVersion = &p
	}
	blob, err := pkgfmt.Build(m, pkgfmt.StringFiles(files))
	if err != nil {
		t.Fatalf("build package: %v", err)
	}
	return blob
}

func (h *harness) upload(parent uint32, files map[string]string) *http.Response {
	h.t.Helper()

	blob := samplePackage(h.t, parent, files)
	return h.auth("POST",
		fmt.Sprintf("/v1/titles/0004000000055D00/blob?parent_version=%d&platform=3ds", parent),
		"application/zip", bytes.NewReader(blob))
}

// ------------------------------------------------------------------- tests

func TestHealth(t *testing.T) {
	h := newHarness(t)

	resp := h.do("GET", "/healthz", "", nil, "")
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d", resp.StatusCode)
	}
}

func TestUploadDownloadRoundTrip(t *testing.T) {
	h := newHarness(t)

	files := map[string]string{"main.sav": "player data", "sub/extra.bin": "more"}
	resp := h.upload(0, files)
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated {
		t.Fatalf("upload: status %d: %s", resp.StatusCode, readAll(t, resp.Body))
	}

	var meta store.VersionMeta
	if err := json.NewDecoder(resp.Body).Decode(&meta); err != nil {
		t.Fatalf("decode upload response: %v", err)
	}
	if meta.Version != 1 {
		t.Fatalf("version = %d, want 1", meta.Version)
	}

	latest := h.auth("GET", "/v1/titles/0004000000055D00/latest?platform=3ds", "", nil)
	defer latest.Body.Close()
	if latest.StatusCode != http.StatusOK {
		t.Fatalf("latest: status %d", latest.StatusCode)
	}
	var got store.VersionMeta
	if err := json.NewDecoder(latest.Body).Decode(&got); err != nil {
		t.Fatalf("decode latest: %v", err)
	}
	if got.SHA256 != meta.SHA256 {
		t.Errorf("latest digest = %s, want %s", got.SHA256, meta.SHA256)
	}

	dl := h.auth("GET", "/v1/titles/0004000000055D00/blob/1?platform=3ds", "", nil)
	defer dl.Body.Close()
	if dl.StatusCode != http.StatusOK {
		t.Fatalf("download: status %d", dl.StatusCode)
	}
	if dl.Header.Get("X-DaeMoon-SHA256") != meta.SHA256 {
		t.Errorf("digest header = %q", dl.Header.Get("X-DaeMoon-SHA256"))
	}

	blob, err := io.ReadAll(dl.Body)
	if err != nil {
		t.Fatalf("read blob: %v", err)
	}
	// What comes back has to still be a package that verifies against its own
	// manifest, which is what the console checks before restoring.
	round, err := pkgfmt.Inspect(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatalf("downloaded package does not verify: %v", err)
	}
	if round.SHA256 != meta.SHA256 {
		t.Errorf("round trip digest = %s, want %s", round.SHA256, meta.SHA256)
	}
}

// TestChunkedBlobRoundTrip pushes a payload well past the chunk size, because a
// blob that fits in one row proves nothing about the chunking.
func TestChunkedBlobRoundTrip(t *testing.T) {
	h := newHarness(t)

	big := bytes.Repeat([]byte("save data. "), 4000) // ~44 KiB, chunk size is 512
	resp := h.upload(0, map[string]string{"big.sav": string(big)})
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated {
		t.Fatalf("upload: status %d: %s", resp.StatusCode, readAll(t, resp.Body))
	}

	dl := h.auth("GET", "/v1/titles/0004000000055D00/blob/1?platform=3ds", "", nil)
	defer dl.Body.Close()

	blob, err := io.ReadAll(dl.Body)
	if err != nil {
		t.Fatalf("read blob: %v", err)
	}
	if _, err := pkgfmt.Inspect(bytes.NewReader(blob), int64(len(blob))); err != nil {
		t.Fatalf("chunked package does not verify: %v", err)
	}
}

// TestVersionConflictRetainsBothSides is the rule the whole sync model rests on.
func TestVersionConflictRetainsBothSides(t *testing.T) {
	h := newHarness(t)

	first := h.upload(0, map[string]string{"main.sav": "from console A"})
	first.Body.Close()
	if first.StatusCode != http.StatusCreated {
		t.Fatalf("first upload: status %d", first.StatusCode)
	}

	// A second console still thinks the server is empty.
	stale := h.upload(0, map[string]string{"main.sav": "from console B"})
	defer stale.Body.Close()
	if stale.StatusCode != http.StatusConflict {
		t.Fatalf("stale upload: status %d, want 409", stale.StatusCode)
	}

	var body struct {
		Error struct {
			Code   string `json:"code"`
			Detail struct {
				ServerVersion     uint32 `json:"server_version"`
				ParentVersion     uint32 `json:"parent_version"`
				ServerSize        uint64 `json:"server_size"`
				ServerDeviceLabel string `json:"server_device_label"`
				ServerReceivedAt  string `json:"server_received_at"`
			} `json:"detail"`
		} `json:"error"`
	}
	if err := json.NewDecoder(stale.Body).Decode(&body); err != nil {
		t.Fatalf("decode conflict: %v", err)
	}
	if body.Error.Code != "version_conflict" {
		t.Fatalf("code = %q", body.Error.Code)
	}
	// The client shows all of this through ui->choose, so an empty field here is a
	// dialog the user cannot answer.
	if body.Error.Detail.ServerVersion != 1 {
		t.Errorf("server_version = %d", body.Error.Detail.ServerVersion)
	}
	if body.Error.Detail.ServerDeviceLabel == "" {
		t.Error("server_device_label is empty")
	}
	if body.Error.Detail.ServerReceivedAt == "" {
		t.Error("server_received_at is empty")
	}

	// Nothing was discarded: version 1 is still downloadable and still correct.
	dl := h.auth("GET", "/v1/titles/0004000000055D00/blob/1?platform=3ds", "", nil)
	defer dl.Body.Close()
	if dl.StatusCode != http.StatusOK {
		t.Fatalf("version 1 is gone: status %d", dl.StatusCode)
	}

	// And the console that lost the race can upload on top once it has chosen.
	retry := h.upload(1, map[string]string{"main.sav": "from console B"})
	defer retry.Body.Close()
	if retry.StatusCode != http.StatusCreated {
		t.Fatalf("retry: status %d: %s", retry.StatusCode, readAll(t, retry.Body))
	}
	// Both versions are on the server.
	for _, v := range []int{1, 2} {
		r := h.auth("GET", fmt.Sprintf("/v1/titles/0004000000055D00/blob/%d?platform=3ds", v), "", nil)
		if r.StatusCode != http.StatusOK {
			t.Errorf("version %d: status %d", v, r.StatusCode)
		}
		r.Body.Close()
	}
}

func TestLatestOfAnUnknownTitleIsNotFound(t *testing.T) {
	h := newHarness(t)

	// Never uploaded is a normal state. The client turns this into "nothing on the
	// server yet" rather than an error the user sees.
	resp := h.auth("GET", "/v1/titles/0004000000000000/latest?platform=3ds", "", nil)
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", resp.StatusCode)
	}
	if code := errorCode(t, resp); code != "not_found" {
		t.Errorf("code = %q", code)
	}
}

func TestAuthIsRequired(t *testing.T) {
	h := newHarness(t)

	resp := h.do("GET", "/v1/titles", "", nil, "")
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", resp.StatusCode)
	}

	bad := h.do("GET", "/v1/titles", "", nil, "not-a-real-token")
	defer bad.Body.Close()
	if bad.StatusCode != http.StatusUnauthorized {
		t.Fatalf("bad token: status = %d, want 401", bad.StatusCode)
	}
}

// TestRevokedDeviceIsToldSo matters because "unauthorized" would send the user to
// pair again, which is the opposite of what revoking a lost SD card is for.
func TestRevokedDeviceIsToldSo(t *testing.T) {
	h := newHarness(t)

	list := h.auth("GET", "/v1/titles", "", nil)
	list.Body.Close()
	if list.StatusCode != http.StatusOK {
		t.Fatalf("pre-revoke: status %d", list.StatusCode)
	}

	device, err := h.store.DeviceByTokenHash(context.Background(), auth.HashToken(h.token))
	if err != nil {
		t.Fatalf("lookup device: %v", err)
	}
	del := h.auth("DELETE", "/v1/devices/"+device.ID, "", nil)
	del.Body.Close()
	if del.StatusCode != http.StatusNoContent {
		t.Fatalf("revoke: status %d", del.StatusCode)
	}

	after := h.auth("GET", "/v1/titles", "", nil)
	defer after.Body.Close()
	if after.StatusCode != http.StatusUnauthorized {
		t.Fatalf("post-revoke: status %d", after.StatusCode)
	}
	if code := errorCode(t, after); code != "device_revoked" {
		t.Errorf("code = %q, want device_revoked", code)
	}
}

func TestTenantIsolation(t *testing.T) {
	h := newHarness(t)

	up := h.upload(0, map[string]string{"main.sav": "user one's save"})
	up.Body.Close()
	if up.StatusCode != http.StatusCreated {
		t.Fatalf("upload: status %d", up.StatusCode)
	}

	// A second account must not see it, by title id or by version number.
	other := h.pair("user-2", "someone else's console")

	latest := h.do("GET", "/v1/titles/0004000000055D00/latest?platform=3ds", "", nil, other)
	defer latest.Body.Close()
	if latest.StatusCode != http.StatusNotFound {
		t.Errorf("latest across accounts: status %d, want 404", latest.StatusCode)
	}

	dl := h.do("GET", "/v1/titles/0004000000055D00/blob/1?platform=3ds", "", nil, other)
	defer dl.Body.Close()
	if dl.StatusCode != http.StatusNotFound {
		t.Errorf("download across accounts: status %d, want 404", dl.StatusCode)
	}
}

func TestUploadRejectsAnOversizedSave(t *testing.T) {
	cfg := config.Default()
	cfg.Database = filepath.Join(t.TempDir(), "small.db")
	cfg.BlobChunkSize = 512
	cfg.MaxSaveSize = 1024

	st, err := store.Open(context.Background(), cfg.Database, cfg.BlobChunkSize)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer func() { _ = st.Close() }()

	srv := httptest.NewServer(api.New(st, cfg).Routes())
	defer srv.Close()

	small := &harness{t: t, srv: srv, store: st, cfg: cfg}
	small.token = small.pair("user-1", "test console")

	resp := small.upload(0, map[string]string{"big.sav": string(bytes.Repeat([]byte("x"), 8192))})
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusRequestEntityTooLarge {
		t.Fatalf("status = %d, want 413", resp.StatusCode)
	}
	if code := errorCode(t, resp); code != "save_too_large" {
		t.Errorf("code = %q", code)
	}
}

func TestUploadRejectsAMismatchedParentVersion(t *testing.T) {
	h := newHarness(t)

	// The query parameter and the manifest have to agree. If they do not, the
	// client is confused about its own state and nothing should be stored.
	blob := samplePackage(t, 0, map[string]string{"main.sav": "data"})
	resp := h.auth("POST",
		"/v1/titles/0004000000055D00/blob?parent_version=5&platform=3ds",
		"application/zip", bytes.NewReader(blob))
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusCreated {
		t.Fatal("a package whose manifest disagrees with the request was stored")
	}
}

func TestUploadRejectsAMismatchedTitleID(t *testing.T) {
	h := newHarness(t)

	blob := samplePackage(t, 0, map[string]string{"main.sav": "data"})
	resp := h.auth("POST",
		"/v1/titles/0004000000009999/blob?parent_version=0&platform=3ds",
		"application/zip", bytes.NewReader(blob))
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
	if code := errorCode(t, resp); code != "invalid_manifest" {
		t.Errorf("code = %q", code)
	}
}

func TestUploadRejectsGarbage(t *testing.T) {
	h := newHarness(t)

	resp := h.auth("POST", "/v1/titles/0004000000055D00/blob?parent_version=0&platform=3ds",
		"application/zip", bytes.NewReader([]byte("this is not a zip")))
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
}

func TestSharesNeedNoAuthAndExpire(t *testing.T) {
	h := newHarness(t)

	up := h.upload(0, map[string]string{"main.sav": "shared save"})
	up.Body.Close()

	body, _ := json.Marshal(map[string]any{
		"title_id": "0004000000055D00", "platform": "3ds", "version": 1, "ttl_seconds": 3600,
	})
	created := h.auth("POST", "/v1/shares", "application/json", bytes.NewReader(body))
	defer created.Body.Close()
	if created.StatusCode != http.StatusCreated {
		t.Fatalf("create share: status %d: %s", created.StatusCode, readAll(t, created.Body))
	}
	var share struct {
		Code      string `json:"code"`
		ExpiresAt string `json:"expires_at"`
	}
	if err := json.NewDecoder(created.Body).Decode(&share); err != nil {
		t.Fatalf("decode share: %v", err)
	}

	// The code is the credential: no token, and it still works.
	dl := h.do("GET", "/v1/shares/"+share.Code, "", nil, "")
	defer dl.Body.Close()
	if dl.StatusCode != http.StatusOK {
		t.Fatalf("download share: status %d", dl.StatusCode)
	}
	blob, err := io.ReadAll(dl.Body)
	if err != nil {
		t.Fatalf("read share: %v", err)
	}
	if _, err := pkgfmt.Inspect(bytes.NewReader(blob), int64(len(blob))); err != nil {
		t.Fatalf("shared package does not verify: %v", err)
	}

	// Move the clock past the expiry rather than sleeping.
	h.store.SetClock(func() time.Time { return time.Now().Add(2 * time.Hour) })
	expired := h.do("GET", "/v1/shares/"+share.Code, "", nil, "")
	defer expired.Body.Close()
	if expired.StatusCode != http.StatusGone {
		t.Fatalf("expired share: status %d, want 410", expired.StatusCode)
	}
	if code := errorCode(t, expired); code != "share_expired" {
		t.Errorf("code = %q", code)
	}
}

func TestPairingRequiresApproval(t *testing.T) {
	h := newHarness(t)

	ctx := context.Background()
	code, err := auth.NewPairingCode()
	if err != nil {
		t.Fatalf("pairing code: %v", err)
	}
	if err := h.store.CreatePairing(ctx, code, "user-1", false, h.cfg.PairingTTL); err != nil {
		t.Fatalf("create pairing: %v", err)
	}

	body, _ := json.Marshal(map[string]string{
		"grant": "device_code", "code": code, "label": "waiting", "platform": "nx",
	})
	resp := h.do("POST", "/v1/devices/pair", "application/json", bytes.NewReader(body), "")
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d", resp.StatusCode)
	}
	if code := errorCode(t, resp); code != "pairing_pending" {
		t.Errorf("code = %q, want pairing_pending", code)
	}
}

func TestPlatformIsRequiredWhereATitleIDIsAmbiguous(t *testing.T) {
	h := newHarness(t)

	resp := h.auth("GET", "/v1/titles/0004000000055D00/latest", "", nil)
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
	if code := errorCode(t, resp); code != "invalid_request" {
		t.Errorf("code = %q", code)
	}
}
