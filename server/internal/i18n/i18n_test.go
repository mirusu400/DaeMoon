package i18n

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestEnglishHasEveryKeyEveryLanguageHas(t *testing.T) {
	// gen already refuses to emit a table otherwise, so this is the assertion that
	// the table in the binary is the one gen checked.
	for _, l := range Order {
		if len(table[l]) != len(table[Default]) {
			t.Errorf("%s has %d keys, en has %d", l, len(table[l]), len(table[Default]))
		}
		for k, v := range table[l] {
			if v == "" {
				t.Errorf("%s: %q is empty", l, k)
			}
			if _, ok := table[Default][k]; !ok {
				t.Errorf("%s: %q is not in en", l, k)
			}
		}
	}
}

func TestAMissingKeyFallsBackToEnglishAndThenToItself(t *testing.T) {
	// Korean has every key, so a fallback has to be provoked.
	const key = "web.nav.saves"
	saved := table["ko"][key]
	delete(table["ko"], key)
	defer func() { table["ko"][key] = saved }()

	if got, want := T("ko", key), table["en"][key]; got != want {
		t.Errorf("fallback = %q, want the English %q", got, want)
	}
	if got := T("ko", "web.nothing.here"); got != "web.nothing.here" {
		t.Errorf("unknown key = %q, want the key", got)
	}
	// Which is what lets a page be titled after a title id.
	if got := T("ko", "0004000000055D00"); got != "0004000000055D00" {
		t.Errorf("literal = %q", got)
	}
}

func TestPlaceholdersMaySwapAround(t *testing.T) {
	table["xx"] = map[string]string{"t": "{2} then {0} then {1}"}
	defer delete(table, "xx")

	if got, want := Tf("xx", "t", "a", "b", 3), "3 then a then b"; got != want {
		t.Errorf("Tf = %q, want %q", got, want)
	}
	// A template with more slots than arguments leaves the extra ones alone rather
	// than rendering an index nobody can act on.
	if got, want := Tf("xx", "t", "a"), "{2} then a then {1}"; got != want {
		t.Errorf("short args = %q, want %q", got, want)
	}
}

func TestTheRealSubtitleSubstitutes(t *testing.T) {
	got := Tf("ko", "web.saves.subtitle", 3, 1, 2)
	for _, want := range []string{"3", "1", "2"} {
		if !contains(got, want) {
			t.Errorf("Tf = %q, missing %q", got, want)
		}
	}
	if contains(got, "{") {
		t.Errorf("Tf = %q, left a placeholder", got)
	}
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

func TestTheBrowserIsAskedBeforeEnglishIsAssumed(t *testing.T) {
	cases := []struct {
		header string
		cookie string
		want   Lang
	}{
		{"", "", Default},
		{"ko", "", "ko"},
		{"ko-KR,ko;q=0.9,en;q=0.8", "", "ko"},
		{"en-GB,en;q=0.9", "", "en"},
		// A region this project has no file for still finds its language.
		{"de-AT", "", "de"},
		// Script matters for Chinese and region only decides which script.
		{"zh-TW", "", "zh-Hant"},
		{"zh-CN", "", "zh-Hans"},
		{"zh", "", "zh-Hans"},
		{"zh-Hant", "", "zh-Hant"},
		// A language with no file is skipped rather than ending the search.
		{"is-IS,ko;q=0.5", "", "ko"},
		{"*", "", Default},
		// An explicit choice beats the header, and a nonsense one does not.
		{"ja", "ko", "ko"},
		{"ja", "klingon", "ja"},
	}
	for _, c := range cases {
		r := httptest.NewRequest(http.MethodGet, "/", nil)
		if c.header != "" {
			r.Header.Set("Accept-Language", c.header)
		}
		if c.cookie != "" {
			r.AddCookie(&http.Cookie{Name: Cookie, Value: c.cookie})
		}
		if got := Of(r); got != c.want {
			t.Errorf("Of(%q, cookie %q) = %q, want %q", c.header, c.cookie, got, c.want)
		}
	}
}
