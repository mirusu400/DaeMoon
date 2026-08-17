package web

import (
	"regexp"
	"strings"
	"testing"

	"github.com/mirusu400/DaeMoon/server/internal/i18n"
)

// A key resolves to itself when nothing has it, which is what lets a page be titled
// after a title id - and it is also what would put `web.nav.saves` on a screen if a
// key were misspelled. Nothing else would notice, so this does.
var keyRe = regexp.MustCompile(`\.Tf?\s+"([^"]+)"`)

func TestEveryKeyTheTemplatesAskForExists(t *testing.T) {
	files, err := assets.ReadDir("templates")
	if err != nil {
		t.Fatal(err)
	}
	seen := 0
	for _, f := range files {
		raw, err := assets.ReadFile("templates/" + f.Name())
		if err != nil {
			t.Fatal(err)
		}
		for _, m := range keyRe.FindAllStringSubmatch(string(raw), -1) {
			key := m[1]
			// `.T .Title` and `.T .Error` pass a value rather than a literal, and
			// those are checked below where they are set.
			if !strings.HasPrefix(key, "web.") {
				t.Errorf("%s: %q is not a web. key", f.Name(), key)
				continue
			}
			seen++
			if i18n.T(i18n.Default, key) == key {
				t.Errorf("%s: no text for %q in shared/lang/en.json", f.Name(), key)
			}
		}
	}
	if seen < 40 {
		t.Errorf("only %d keys found in the templates; the regexp has probably stopped matching", seen)
	}
}

// The language files are checked by tools/gen. The templates are not, and they are
// the other place a character can reach a screen from.
func TestNoEmDashesInTheTemplates(t *testing.T) {
	files, err := assets.ReadDir("templates")
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		raw, err := assets.ReadFile("templates/" + f.Name())
		if err != nil {
			t.Fatal(err)
		}
		if strings.ContainsAny(string(raw), "—–") {
			t.Errorf("%s contains an em or en dash; a colon, a comma or a full stop says it",
				f.Name())
		}
	}
}

// The keys a handler puts in page.Title and page.Error never pass through a
// template literal, so the test above cannot see them.
func TestTheKeysHandlersSetExist(t *testing.T) {
	for _, key := range []string{
		"web.nav.saves", "web.nav.consoles", "web.nav.people", "web.nav.add_console",
		"web.login.title", "web.login.failed",
		"web.setup.title",
		"web.err.name_taken", "web.err.name_required", "web.err.name_too_long",
		"web.err.name_chars", "web.err.password_short", "web.err.password_store",
		"web.people.admin_only", "web.people.last_admin",
	} {
		if i18n.T(i18n.Default, key) == key {
			t.Errorf("no text for %q", key)
		}
	}
	// checkCredentials returns keys, not sentences.
	for _, c := range []struct{ user, pass string }{
		{"", "hunter2hunter2"},
		{strings.Repeat("a", 65), "hunter2hunter2"},
		{"a b", "hunter2hunter2"},
		{"ok", "short"},
	} {
		msg := checkCredentials(c.user, c.pass)
		if msg == "" {
			t.Errorf("checkCredentials(%q, %q) accepted it", c.user, c.pass)
			continue
		}
		if i18n.T(i18n.Default, msg) == msg {
			t.Errorf("checkCredentials returned %q, which is not a key with text", msg)
		}
	}
	if msg := checkCredentials("mirusu", "hunter2hunter2"); msg != "" {
		t.Errorf("a good pair was refused with %q", msg)
	}
}
