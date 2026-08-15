// Package apierr is the server side of shared/errors.json.
//
// The server never localizes. It returns a machine readable code and the client
// renders the text from its compiled in language tables, which is why there is not
// a single user facing sentence anywhere in this package.
package apierr

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
)

// Error is a failure on its way to the client.
type Error struct {
	Code   Code
	Detail map[string]any
	// Cause is logged and never sent. What went wrong internally is not the
	// client's business and is often not safe to disclose.
	Cause error
}

func (e *Error) Error() string {
	if e.Cause != nil {
		return fmt.Sprintf("%s: %v", e.Code, e.Cause)
	}
	return string(e.Code)
}

func (e *Error) Unwrap() error { return e.Cause }

// New builds an error with no detail.
func New(code Code) *Error { return &Error{Code: code} }

// Wrap keeps the underlying cause for the log while the client sees only the code.
func Wrap(code Code, cause error) *Error { return &Error{Code: code, Cause: cause} }

// WithDetail attaches the fields shared/errors.json declares for this code. Fields
// that are not declared are dropped rather than sent: the client parses only what
// the contract promises, and an undeclared field would be silently ignored anyway.
func (e *Error) WithDetail(detail map[string]any) *Error {
	allowed := detailKeys[e.Code]
	if len(allowed) == 0 {
		return e
	}
	filtered := make(map[string]any, len(detail))
	for _, k := range allowed {
		if v, ok := detail[k]; ok {
			filtered[k] = v
		}
	}
	e.Detail = filtered
	return e
}

type wireError struct {
	Code   Code           `json:"code"`
	Detail map[string]any `json:"detail,omitempty"`
}

type wireBody struct {
	Error wireError `json:"error"`
}

// Write renders err as the response body described by the Error schema in
// shared/openapi.yaml. Anything that is not an *Error becomes internal_error: an
// unexpected failure must not leak its shape to the client.
func Write(w http.ResponseWriter, r *http.Request, err error) {
	var e *Error
	if !errors.As(err, &e) {
		e = Wrap(InternalError, err)
	}

	status := e.Code.Status()
	if status >= 500 {
		slog.ErrorContext(r.Context(), "request failed",
			"code", e.Code, "method", r.Method, "path", r.URL.Path, "err", e.Error())
	} else {
		slog.DebugContext(r.Context(), "request rejected",
			"code", e.Code, "method", r.Method, "path", r.URL.Path, "err", e.Error())
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	// The header is already out, so a failure here can only be logged.
	if encErr := json.NewEncoder(w).Encode(wireBody{Error: wireError{
		Code:   e.Code,
		Detail: e.Detail,
	}}); encErr != nil {
		slog.ErrorContext(r.Context(), "writing error body failed", "err", encErr)
	}
}
