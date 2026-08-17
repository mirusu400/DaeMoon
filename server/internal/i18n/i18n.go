// Package i18n renders the panel's text in the language a browser asks for.
//
// The rule in server/CLAUDE.md still stands: the *API* does not localize, it
// returns a code from shared/errors.json and the client renders the text. This
// package is not the API. The panel is a client - the only one written in Go - and
// a client is exactly the thing that turns codes into sentences.
//
// The sentences come from shared/lang/, the same files the consoles read, so a
// string exists in one place for both. tools/gen splits them: keys under `web.`
// are compiled into strings_gen.go here and left out of the C table, so a 3DS does
// not carry text no console will ever draw.
package i18n

import (
	"net/http"
	"strconv"
	"strings"
)

// Lang is a language tag, one of the file names in shared/lang/.
type Lang string

// Default is the fallback, and the language every key is guaranteed to have:
// tools/gen refuses to emit a table when en.json is missing one.
const Default Lang = "en"

// Known reports whether l is a language this panel has text for.
func Known(l Lang) bool {
	_, ok := table[l]
	return ok
}

// T returns the text for key in l.
//
// A missing key falls back to English rather than to the key, because a key drawn
// on a page is a bug that reads as a broken page. A key that is missing from
// English too cannot happen - gen fails the build first - so it returns the key,
// which is the only useful thing left to say.
func T(l Lang, key string) string {
	if m, ok := table[l]; ok {
		if s, ok := m[key]; ok && s != "" {
			return s
		}
	}
	if s, ok := table[Default][key]; ok {
		return s
	}
	return key
}

// Tf substitutes {0}, {1} ... in a template.
//
// Positional and not printf, because word order varies by language and a
// translation has to be able to reorder the arguments. The parity check in
// tools/gen already refuses a translation that uses a different set of them.
func Tf(l Lang, key string, args ...any) string {
	s := T(l, key)
	if len(args) == 0 || !strings.ContainsRune(s, '{') {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] != '{' {
			b.WriteByte(s[i])
			continue
		}
		end := strings.IndexByte(s[i:], '}')
		if end < 0 {
			b.WriteByte(s[i])
			continue
		}
		n, err := strconv.Atoi(s[i+1 : i+end])
		if err != nil || n < 0 || n >= len(args) {
			b.WriteString(s[i : i+end+1])
			i += end
			continue
		}
		b.WriteString(str(args[n]))
		i += end
	}
	return b.String()
}

func str(v any) string {
	switch t := v.(type) {
	case string:
		return t
	case int:
		return strconv.Itoa(t)
	case int64:
		return strconv.FormatInt(t, 10)
	default:
		return ""
	}
}

// Cookie is where a browser's choice is remembered. Per browser and not per
// account, the same as the theme: it is a property of the thing displaying the
// page, and the panel has to be readable on the login screen where there is no
// account yet.
const Cookie = "daemoon_lang"

// Of picks the language for a request: an explicit choice first, then what the
// browser asked for, then English.
func Of(r *http.Request) Lang {
	if c, err := r.Cookie(Cookie); err == nil {
		if l := Lang(c.Value); Known(l) {
			return l
		}
	}
	if l := fromAccept(r.Header.Get("Accept-Language")); l != "" {
		return l
	}
	return Default
}

// fromAccept reads Accept-Language well enough for this.
//
// It walks the header in order and takes the first tag there is text for, which
// ignores q-values. A browser writes them in preference order anyway, and honouring
// the weights would only change the outcome for a header that lists a language it
// prefers *less* first - which is not a header browsers send.
//
// A region is dropped when the exact tag is unknown, so `ko-KR` finds `ko`. The
// script is not, because zh-Hans and zh-Hant are different text.
func fromAccept(header string) Lang {
	for _, part := range strings.Split(header, ",") {
		tag := part
		if i := strings.IndexByte(tag, ';'); i >= 0 {
			tag = tag[:i]
		}
		tag = strings.TrimSpace(tag)
		if tag == "" || tag == "*" {
			continue
		}
		if l := match(tag); l != "" {
			return l
		}
	}
	return ""
}

func match(tag string) Lang {
	for _, l := range Order {
		if strings.EqualFold(tag, string(l)) {
			return l
		}
	}
	// zh-CN and zh-SG are Simplified, zh-TW and zh-HK Traditional. A bare `zh`
	// falls to Simplified, which is what the majority of browsers sending it mean.
	switch strings.ToLower(tag) {
	case "zh", "zh-cn", "zh-sg", "zh-my":
		return "zh-Hans"
	case "zh-tw", "zh-hk", "zh-mo":
		return "zh-Hant"
	}
	if i := strings.IndexByte(tag, '-'); i > 0 {
		base := tag[:i]
		for _, l := range Order {
			if strings.EqualFold(base, string(l)) {
				return l
			}
		}
	}
	return ""
}
