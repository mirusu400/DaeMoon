// Package api is the HTTP surface described by shared/openapi.yaml.
//
// That file is authoritative: changing a handler here means changing it there in
// the same commit, and `make spec-check` fails the build otherwise.
//
// Handlers stay thin. They validate, call internal packages, and render. There is
// no SQL in this file and no user facing sentence either: the server returns codes
// and the client renders the text.
package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"strconv"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/mirusu400/DaeMoon/server/internal/apierr"
	"github.com/mirusu400/DaeMoon/server/internal/auth"
	"github.com/mirusu400/DaeMoon/server/internal/config"
	"github.com/mirusu400/DaeMoon/server/internal/pkgfmt"
	"github.com/mirusu400/DaeMoon/server/internal/store"
)

// Version is stamped at build time. It appears in /healthz only.
var Version = "dev"

type Server struct {
	store *store.Store
	cfg   config.Config
}

func New(s *store.Store, cfg config.Config) *Server {
	return &Server{store: s, cfg: cfg}
}

func (s *Server) Routes() http.Handler {
	r := chi.NewRouter()

	r.Get("/healthz", s.health)

	r.Route("/v1", func(r chi.Router) {
		r.Post("/devices/pair", s.pairDevice)
		r.Get("/shares/{code}", s.getShare) // no auth: the code is the credential

		r.Group(func(r chi.Router) {
			r.Use(auth.Middleware(s.store))

			r.Delete("/devices/{id}", s.revokeDevice)
			r.Get("/titles", s.listTitles)
			r.Get("/titles/{tid}/latest", s.latest)
			r.Get("/titles/{tid}/blob/{v}", s.download)
			r.Post("/titles/{tid}/blob", s.upload)
			r.Post("/shares", s.createShare)
		})
	})

	return r
}

// ------------------------------------------------------------------ helpers

func writeJSON(w http.ResponseWriter, r *http.Request, status int, body any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(body); err != nil {
		// The status line is already sent, so this can only be logged. apierr
		// does that with the request context attached.
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
	}
}

// platformParam reads the platform query parameter. A title id is only unique
// together with its platform, so this is required rather than defaulted.
func platformParam(r *http.Request) (string, error) {
	p := r.URL.Query().Get("platform")
	if p == "" {
		return "", apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "platform"})
	}
	if !pkgfmt.Platform(p).Valid() {
		return "", apierr.New(apierr.UnsupportedPlatform).
			WithDetail(map[string]any{"platform": p})
	}
	return p, nil
}

func mapStoreError(err error) error {
	switch {
	case errors.Is(err, store.ErrNotFound):
		return apierr.New(apierr.NotFound)
	case errors.Is(err, store.ErrExpired):
		return apierr.New(apierr.ShareExpired)
	default:
		return apierr.Wrap(apierr.InternalError, err)
	}
}

// ------------------------------------------------------------------ handlers

func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, map[string]string{"status": "ok", "version": Version})
}

type pairRequest struct {
	Grant    string `json:"grant"`
	Code     string `json:"code"`
	Label    string `json:"label"`
	Platform string `json:"platform"`
}

func (s *Server) pairDevice(w http.ResponseWriter, r *http.Request) {
	var req pairRequest
	dec := json.NewDecoder(io.LimitReader(r.Body, 4<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InvalidRequest, err))
		return
	}

	switch req.Grant {
	case "qr", "device_code":
	default:
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "grant"}))
		return
	}
	if req.Label == "" || len(req.Label) > 64 {
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "label"}))
		return
	}
	if !pkgfmt.Platform(req.Platform).Valid() {
		apierr.Write(w, r, apierr.New(apierr.UnsupportedPlatform).
			WithDetail(map[string]any{"platform": req.Platform}))
		return
	}

	pairing, err := s.store.ClaimPairing(r.Context(), req.Code)
	switch {
	case errors.Is(err, store.ErrNotFound):
		apierr.Write(w, r, apierr.New(apierr.NotFound))
		return
	case errors.Is(err, store.ErrExpired):
		apierr.Write(w, r, apierr.New(apierr.PairingExpired))
		return
	case err != nil:
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}
	if !pairing.Approved {
		// The user has not approved on the other device yet. Retryable, and the
		// client polls with a ceiling rather than waiting without a bound.
		apierr.Write(w, r, apierr.New(apierr.PairingPending).
			WithDetail(map[string]any{"interval": 3}))
		return
	}

	token, err := auth.NewToken()
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}
	deviceID, err := auth.NewID()
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	if err := s.store.CreateDevice(r.Context(), store.Device{
		ID:       deviceID,
		UserID:   pairing.UserID,
		Label:    req.Label,
		Platform: req.Platform,
	}, auth.HashToken(token)); err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	// The only time the token exists outside the console.
	writeJSON(w, r, http.StatusOK, map[string]string{
		"device_id": deviceID,
		"token":     token,
	})
}

func (s *Server) revokeDevice(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	err := s.store.RevokeDevice(r.Context(), device.UserID, chi.URLParam(r, "id"))
	if err != nil {
		apierr.Write(w, r, mapStoreError(err))
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listTitles(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	platform := r.URL.Query().Get("platform")
	if platform != "" && !pkgfmt.Platform(platform).Valid() {
		apierr.Write(w, r, apierr.New(apierr.UnsupportedPlatform).
			WithDetail(map[string]any{"platform": platform}))
		return
	}

	titles, err := s.store.ListTitles(r.Context(), device.UserID, platform)
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"titles": titles})
}

func (s *Server) latest(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	platform, err := platformParam(r)
	if err != nil {
		apierr.Write(w, r, err)
		return
	}

	meta, err := s.store.Latest(r.Context(), device.UserID, platform, chi.URLParam(r, "tid"))
	if err != nil {
		apierr.Write(w, r, mapStoreError(err))
		return
	}
	writeJSON(w, r, http.StatusOK, meta)
}

func (s *Server) download(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	platform, err := platformParam(r)
	if err != nil {
		apierr.Write(w, r, err)
		return
	}
	version, convErr := strconv.ParseUint(chi.URLParam(r, "v"), 10, 32)
	if convErr != nil || version == 0 {
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "v"}))
		return
	}

	meta, err := s.store.Version(r.Context(), device.UserID, platform,
		chi.URLParam(r, "tid"), uint32(version))
	if err != nil {
		apierr.Write(w, r, mapStoreError(err))
		return
	}
	s.streamPackage(w, r, meta)
}

// streamPackage writes a package out chunk by chunk. Nothing here holds a whole
// save in memory, on either side of the wire.
func (s *Server) streamPackage(w http.ResponseWriter, r *http.Request, meta store.VersionMeta) {
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("X-DaeMoon-SHA256", meta.SHA256)
	w.Header().Set("X-DaeMoon-Version", strconv.FormatUint(uint64(meta.Version), 10))
	w.WriteHeader(http.StatusOK)

	if err := s.store.WriteBlobTo(r.Context(), meta, w); err != nil {
		// The 200 is already on the wire, so the client sees a truncated body and
		// its own verify step catches it. Nothing here can improve on that except
		// leaving a log line behind.
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
	}
}

func (s *Server) upload(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	titleID := chi.URLParam(r, "tid")
	parentRaw := r.URL.Query().Get("parent_version")
	parent, convErr := strconv.ParseUint(parentRaw, 10, 32)
	if parentRaw == "" || convErr != nil {
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "parent_version"}))
		return
	}

	// Refuse an oversized upload before reading the body.
	if r.ContentLength > s.cfg.MaxSaveSize {
		apierr.Write(w, r, apierr.New(apierr.SaveTooLarge).
			WithDetail(map[string]any{"size": r.ContentLength, "max_size": s.cfg.MaxSaveSize}))
		return
	}

	// A package has to be read back to front to find its central directory, so it
	// is staged on disk rather than buffered in memory. The limit is enforced again
	// here because Content-Length is a claim, not a fact.
	tmp, err := os.CreateTemp("", "daemoon-upload-*.zip")
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}
	defer func() {
		_ = tmp.Close()
		_ = os.Remove(tmp.Name())
	}()

	limited := io.LimitReader(r.Body, s.cfg.MaxSaveSize+1)
	written, err := io.Copy(tmp, limited)
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InvalidRequest, err))
		return
	}
	if written > s.cfg.MaxSaveSize {
		apierr.Write(w, r, apierr.New(apierr.SaveTooLarge).
			WithDetail(map[string]any{"size": written, "max_size": s.cfg.MaxSaveSize}))
		return
	}

	// Verify the package against its own manifest before storing it. A package the
	// server keeps is one it will hand back to a console later, and accepting a
	// broken one turns into a failed restore on some other day with nothing to
	// explain it.
	manifest, err := pkgfmt.Inspect(tmp, written)
	switch {
	case errors.Is(err, pkgfmt.ErrChecksum):
		apierr.Write(w, r, apierr.Wrap(apierr.ChecksumMismatch, err))
		return
	case errors.Is(err, pkgfmt.ErrFormatVersion), errors.Is(err, pkgfmt.ErrManifest),
		errors.Is(err, pkgfmt.ErrArchive):
		apierr.Write(w, r, apierr.Wrap(apierr.InvalidManifest, err))
		return
	case err != nil:
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	// The body limit above is about the transfer. This one is about what the
	// console will have to write back: a highly compressible save can be tiny on
	// the wire and still be far larger than any archive it has to fit into.
	if manifest.Size > uint64(s.cfg.MaxSaveSize) {
		apierr.Write(w, r, apierr.New(apierr.SaveTooLarge).
			WithDetail(map[string]any{"size": manifest.Size, "max_size": s.cfg.MaxSaveSize}))
		return
	}

	if manifest.TitleID != titleID {
		apierr.Write(w, r, apierr.New(apierr.InvalidManifest).
			WithDetail(map[string]any{"field": "title_id"}))
		return
	}
	if device.Platform != "" && !pkgfmt.Platform(device.Platform).CanCarry(manifest.Platform) {
		// A 3DS uploading a Switch package is never intentional. A 3DS uploading
		// an nds package is Phase 2, which is why this asks what the device can
		// carry rather than whether the two strings match.
		apierr.Write(w, r, apierr.New(apierr.UnsupportedPlatform).
			WithDetail(map[string]any{
				"platform":        string(manifest.Platform),
				"device_platform": device.Platform,
			}))
		return
	}
	if uint64(manifest.Parent()) != parent {
		// The query parameter exists so the conflict check can run before the body
		// is read. If it disagrees with the manifest, the client is confused about
		// its own state and nothing should be stored.
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "parent_version"}))
		return
	}

	if _, err := tmp.Seek(0, io.SeekStart); err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	meta, err := s.store.Put(r.Context(), store.PutRequest{
		UserID:        device.UserID,
		DeviceID:      device.ID,
		DeviceLabel:   manifest.DeviceLabel,
		Platform:      string(manifest.Platform),
		TitleID:       manifest.TitleID,
		SaveType:      string(manifest.SaveType),
		ParentVersion: uint32(parent),
		SHA256:        manifest.SHA256,
		Size:          manifest.Size,
		Body:          tmp,
	})
	if errors.Is(err, store.ErrConflict) {
		s.writeConflict(w, r, device.UserID, string(manifest.Platform), manifest.TitleID,
			uint32(parent))
		return
	}
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	writeJSON(w, r, http.StatusCreated, meta)
}

// writeConflict describes the server side so the user can choose. Nothing was
// discarded: the version the client tried to build on is still here, and so is
// whatever it uploaded before.
func (s *Server) writeConflict(w http.ResponseWriter, r *http.Request, userID, platform,
	titleID string, parent uint32) {
	detail := map[string]any{"parent_version": parent}

	latest, err := s.store.Latest(r.Context(), userID, platform, titleID)
	if err == nil {
		detail["server_version"] = latest.Version
		detail["server_size"] = latest.Size
		detail["server_device_label"] = latest.DeviceLabel
		detail["server_received_at"] = latest.ReceivedAt
	} else if !errors.Is(err, store.ErrNotFound) {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	apierr.Write(w, r, apierr.New(apierr.VersionConflict).WithDetail(detail))
}

type shareRequest struct {
	TitleID    string `json:"title_id"`
	Platform   string `json:"platform"`
	Version    uint32 `json:"version"`
	TTLSeconds int    `json:"ttl_seconds"`
}

func (s *Server) createShare(w http.ResponseWriter, r *http.Request) {
	device := auth.MustDevice(r.Context())

	var req shareRequest
	dec := json.NewDecoder(io.LimitReader(r.Body, 4<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InvalidRequest, err))
		return
	}
	if !pkgfmt.Platform(req.Platform).Valid() {
		apierr.Write(w, r, apierr.New(apierr.UnsupportedPlatform).
			WithDetail(map[string]any{"platform": req.Platform}))
		return
	}
	if req.Version == 0 {
		apierr.Write(w, r, apierr.New(apierr.InvalidRequest).
			WithDetail(map[string]any{"field": "version"}))
		return
	}

	ttl := s.cfg.ShareTTL
	if req.TTLSeconds > 0 {
		ttl = time.Duration(req.TTLSeconds) * time.Second
	}

	code, err := auth.NewShareCode()
	if err != nil {
		apierr.Write(w, r, apierr.Wrap(apierr.InternalError, err))
		return
	}

	expires, err := s.store.CreateShare(r.Context(), code, device.UserID, req.Platform,
		req.TitleID, req.Version, ttl)
	if err != nil {
		apierr.Write(w, r, mapStoreError(err))
		return
	}

	writeJSON(w, r, http.StatusCreated, map[string]string{
		"code":       code,
		"expires_at": expires,
	})
}

func (s *Server) getShare(w http.ResponseWriter, r *http.Request) {
	meta, err := s.store.ResolveShare(r.Context(), chi.URLParam(r, "code"))
	if err != nil {
		apierr.Write(w, r, mapStoreError(err))
		return
	}
	s.streamPackage(w, r, meta)
}

// Describe returns the routes this server serves, so tools/speccheck can hold them
// against shared/openapi.yaml without starting anything.
func Describe() []string {
	return []string{
		"GET /healthz",
		"POST /v1/devices/pair",
		"DELETE /v1/devices/{id}",
		"GET /v1/titles",
		"GET /v1/titles/{tid}/latest",
		"GET /v1/titles/{tid}/blob/{v}",
		"POST /v1/titles/{tid}/blob",
		"POST /v1/shares",
		"GET /v1/shares/{code}",
	}
}
