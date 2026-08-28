// Package turnstile verifies Cloudflare Turnstile tokens. The browser widget is
// only the prompt; registration trusts a token only after this package has sent it
// to Siteverify and received a successful response for the register action.
package turnstile

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const (
	verifyURL        = "https://challenges.cloudflare.com/turnstile/v0/siteverify"
	registerAction   = "register"
	maxTokenLength   = 2048
	maxResponseBytes = 64 << 10
	requestTimeout   = 10 * time.Second
)

// ErrRejected means Siteverify did not accept the token for this form. Callers
// deliberately show the same translated message for every rejection, while still
// distinguishing service failures in the server log.
var ErrRejected = errors.New("turnstile token rejected")

type Client struct {
	secret   string
	endpoint string
	http     *http.Client
}

func New(secret string) *Client {
	return &Client{
		secret:   secret,
		endpoint: verifyURL,
		http:     &http.Client{Timeout: requestTimeout},
	}
}

type verifyResponse struct {
	Success    bool     `json:"success"`
	Action     string   `json:"action"`
	ErrorCodes []string `json:"error-codes"`
}

// Verify checks one token. Turnstile tokens are single-use, so the caller must do
// this once, immediately before the account-changing work it protects.
func (c *Client) Verify(ctx context.Context, token string) error {
	token = strings.TrimSpace(token)
	if token == "" || len(token) > maxTokenLength {
		return ErrRejected
	}

	form := url.Values{
		"secret":   {c.secret},
		"response": {token},
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.endpoint,
		strings.NewReader(form.Encode()))
	if err != nil {
		return fmt.Errorf("create Siteverify request: %w", err)
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

	resp, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("call Siteverify: %w", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes+1))
	if err != nil {
		return fmt.Errorf("read Siteverify response: %w", err)
	}
	if len(body) > maxResponseBytes {
		return fmt.Errorf("Siteverify response exceeds %d bytes", maxResponseBytes)
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("Siteverify returned HTTP %d", resp.StatusCode)
	}

	var result verifyResponse
	if err := json.Unmarshal(body, &result); err != nil {
		return fmt.Errorf("decode Siteverify response: %w", err)
	}
	if !result.Success {
		if len(result.ErrorCodes) == 0 {
			return ErrRejected
		}
		return fmt.Errorf("%w: %s", ErrRejected, strings.Join(result.ErrorCodes, ","))
	}
	if result.Action != registerAction {
		return fmt.Errorf("%w: action %q", ErrRejected, result.Action)
	}
	return nil
}
