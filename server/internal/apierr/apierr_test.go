package apierr_test

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/apierr"
)

func write(t *testing.T, err error) (int, map[string]any) {
	t.Helper()

	rec := httptest.NewRecorder()
	apierr.Write(rec, httptest.NewRequest(http.MethodGet, "/v1/titles", nil), err)

	var body map[string]any
	if decErr := json.Unmarshal(rec.Body.Bytes(), &body); decErr != nil {
		t.Fatalf("decode %q: %v", rec.Body.String(), decErr)
	}
	return rec.Code, body
}

func errorObject(t *testing.T, body map[string]any) map[string]any {
	t.Helper()

	obj, ok := body["error"].(map[string]any)
	if !ok {
		t.Fatalf("body has no error object: %v", body)
	}
	return obj
}

func TestStatusComesFromTheSharedTable(t *testing.T) {
	for code, want := range map[apierr.Code]int{
		apierr.InvalidRequest:  http.StatusBadRequest,
		apierr.Unauthorized:    http.StatusUnauthorized,
		apierr.DeviceRevoked:   http.StatusUnauthorized,
		apierr.Forbidden:       http.StatusForbidden,
		apierr.NotFound:        http.StatusNotFound,
		apierr.VersionConflict: http.StatusConflict,
		apierr.ShareExpired:    http.StatusGone,
		apierr.SaveTooLarge:    http.StatusRequestEntityTooLarge,
		apierr.RateLimited:     http.StatusTooManyRequests,
		apierr.InternalError:   http.StatusInternalServerError,
	} {
		if got := code.Status(); got != want {
			t.Errorf("%s: status %d, want %d", code, got, want)
		}
	}
}

// TestClientOnlyCodesAreNotServerStatuses: codes that only ever happen on a
// console have no HTTP status in shared/errors.json, and if one is ever returned
// by mistake it must not become a plausible looking 4xx.
func TestClientOnlyCodesAreNotServerStatuses(t *testing.T) {
	for _, code := range []apierr.Code{
		apierr.IoError, apierr.OutOfMemory, apierr.NetworkError, apierr.Timeout,
		apierr.UserCancelled, apierr.NoSpace, apierr.ArchiveError, apierr.BufferTooSmall,
	} {
		if got := code.Status(); got != http.StatusInternalServerError {
			t.Errorf("%s: status %d, want 500", code, got)
		}
	}
}

// TestUnknownErrorsBecomeInternal: an unexpected failure must not leak its shape.
func TestUnknownErrorsBecomeInternal(t *testing.T) {
	status, body := write(t, errors.New("a raw error from somewhere"))

	if status != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", status)
	}
	obj := errorObject(t, body)
	if obj["code"] != string(apierr.InternalError) {
		t.Errorf("code = %v", obj["code"])
	}
	// The cause is for the log. It is often a file path or a query, and neither is
	// the client's business.
	if raw := string(mustJSON(t, body)); contains(raw, "a raw error from somewhere") {
		t.Errorf("the cause leaked into the response: %s", raw)
	}
}

func TestCauseIsNeverSent(t *testing.T) {
	_, body := write(t, apierr.Wrap(apierr.NotFound,
		fmt.Errorf("select from titles where user_id = %q: no rows", "user-1")))

	if raw := string(mustJSON(t, body)); contains(raw, "user-1") || contains(raw, "select") {
		t.Errorf("the cause leaked into the response: %s", raw)
	}
}

func TestWrappedErrorsAreStillFound(t *testing.T) {
	// Handlers wrap as they propagate, and errors.As has to keep working or every
	// failure silently becomes a 500.
	inner := apierr.New(apierr.VersionConflict)
	wrapped := fmt.Errorf("upload: %w", fmt.Errorf("store: %w", inner))

	status, body := write(t, wrapped)
	if status != http.StatusConflict {
		t.Fatalf("status = %d, want 409", status)
	}
	if errorObject(t, body)["code"] != "version_conflict" {
		t.Errorf("code = %v", errorObject(t, body)["code"])
	}
}

// TestDetailIsFilteredToWhatTheContractDeclares is the one that matters for
// disclosure: a handler passing a map by accident must not be able to put an
// arbitrary field on the wire, because the client parses only what
// shared/errors.json promises and would ignore the rest anyway.
func TestDetailIsFilteredToWhatTheContractDeclares(t *testing.T) {
	_, body := write(t, apierr.New(apierr.VersionConflict).WithDetail(map[string]any{
		"server_version":      43,
		"parent_version":      41,
		"internal_blob_path":  "/var/lib/daemoon/blobs/9f2a",
		"database_query":      "SELECT * FROM versions",
		"server_device_label": "New 2DS",
	}))

	detail, ok := errorObject(t, body)["detail"].(map[string]any)
	if !ok {
		t.Fatalf("no detail object: %v", body)
	}
	for _, want := range []string{"server_version", "parent_version", "server_device_label"} {
		if _, present := detail[want]; !present {
			t.Errorf("declared field %q was dropped", want)
		}
	}
	for _, unwanted := range []string{"internal_blob_path", "database_query"} {
		if _, present := detail[unwanted]; present {
			t.Errorf("undeclared field %q reached the client", unwanted)
		}
	}
}

func TestDetailOnACodeThatDeclaresNoneIsDropped(t *testing.T) {
	_, body := write(t, apierr.New(apierr.Unauthorized).
		WithDetail(map[string]any{"reason": "token not in the database"}))

	if _, present := errorObject(t, body)["detail"]; present {
		t.Error("a code that declares no detail carried one anyway")
	}
}

// TestEveryCodeIsKnown ties the generated table back to the file it came from, so
// a code removed from shared/errors.json cannot linger in Go.
func TestEveryCodeIsKnown(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join(repoRoot(t), "shared", "errors.json"))
	if err != nil {
		t.Fatalf("read shared/errors.json: %v", err)
	}
	var file struct {
		Errors []struct {
			Code   string   `json:"code"`
			HTTP   *int     `json:"http"`
			Detail []string `json:"detail"`
		} `json:"errors"`
	}
	if err := json.Unmarshal(raw, &file); err != nil {
		t.Fatalf("parse shared/errors.json: %v", err)
	}

	byCode := map[string]bool{}
	for _, c := range apierr.All() {
		byCode[string(c)] = true
		if !c.Known() {
			t.Errorf("%s is in Go but reports itself unknown", c)
		}
	}
	if len(byCode) != len(file.Errors) {
		t.Errorf("Go has %d codes, shared/errors.json has %d", len(byCode), len(file.Errors))
	}

	for _, e := range file.Errors {
		if !byCode[e.Code] {
			t.Errorf("%s is in shared/errors.json but not in Go", e.Code)
		}
		want := http.StatusInternalServerError
		if e.HTTP != nil {
			want = *e.HTTP
		}
		if got := apierr.Code(e.Code).Status(); got != want {
			t.Errorf("%s: status %d, want %d", e.Code, got, want)
		}
		if got, declared := len(apierr.DetailKeys(apierr.Code(e.Code))), len(e.Detail); got != declared {
			t.Errorf("%s: %d detail keys in Go, %d declared", e.Code, got, declared)
		}
	}
}

func TestContentTypeIsJSON(t *testing.T) {
	rec := httptest.NewRecorder()
	apierr.Write(rec, httptest.NewRequest(http.MethodGet, "/v1/titles", nil),
		apierr.New(apierr.NotFound))

	if ct := rec.Header().Get("Content-Type"); ct != "application/json; charset=utf-8" {
		t.Errorf("Content-Type = %q", ct)
	}
}

func mustJSON(t *testing.T, v any) []byte {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return b
}

func contains(haystack, needle string) bool {
	return len(needle) > 0 && len(haystack) >= len(needle) &&
		func() bool {
			for i := 0; i+len(needle) <= len(haystack); i++ {
				if haystack[i:i+len(needle)] == needle {
					return true
				}
			}
			return false
		}()
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
