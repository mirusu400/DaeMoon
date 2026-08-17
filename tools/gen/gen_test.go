package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

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

// TestGeneratedFilesAreCurrent is the check CI runs. A committed table that no
// longer matches shared/ would mean a console build ships text the repository does
// not contain.
func TestGeneratedFilesAreCurrent(t *testing.T) {
	if err := run(repoRoot(t), true); err != nil {
		t.Fatalf("%v", err)
	}
}

// TestLanguageFilesAgree is the parity guarantee the root CLAUDE.md asks CI for.
// It runs as part of loading, so this test states the intent explicitly rather than
// leaving it as a side effect of generation.
func TestLanguageFilesAgree(t *testing.T) {
	root := repoRoot(t)

	langs, keys, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	if len(keys) == 0 {
		t.Fatal("en.json has no keys")
	}
	for _, code := range langOrder {
		if len(langs[code]) != len(keys) {
			t.Errorf("%s.json has %d keys, en.json has %d", code, len(langs[code]), len(keys))
		}
	}
}

// TestEveryErrorCodeHasText makes adding a code without its text impossible.
func TestEveryErrorCodeHasText(t *testing.T) {
	root := repoRoot(t)

	errs, err := loadErrors(filepath.Join(root, "shared", "errors.json"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	langs, _, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		t.Fatalf("%v", err)
	}

	for _, e := range errs {
		for _, code := range langOrder {
			if _, ok := langs[code]["err."+e.Code]; !ok {
				t.Errorf("%s.json has no text for err.%s", code, e.Code)
			}
		}
	}
}

// TestErrorValuesAreFrozen guards the numbering. The values end up compiled into
// consoles that will not all be updated at once, so reusing one would make an old
// client report a different failure than the server meant.
func TestErrorValuesAreFrozen(t *testing.T) {
	root := repoRoot(t)

	errs, err := loadErrors(filepath.Join(root, "shared", "errors.json"))
	if err != nil {
		t.Fatalf("%v", err)
	}

	seen := map[int]string{}
	for _, e := range errs {
		if prev, dup := seen[e.Value]; dup {
			t.Errorf("value %d is used by both %s and %s", e.Value, prev, e.Code)
		}
		seen[e.Value] = e.Code
	}
}

// TestPlaceholderSetsMatch is what stops a translation from introducing a
// placeholder the caller never passes, which would render as a hole in a sentence.
func TestPlaceholderSetsMatch(t *testing.T) {
	root := repoRoot(t)

	langs, keys, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	for _, k := range keys {
		want := placeholders(langs["en"][k])
		for _, code := range langOrder {
			if got := placeholders(langs[code][k]); got != want {
				t.Errorf("%s.json key %q uses %s, en.json uses %s", code, k, got, want)
			}
		}
	}
}

// TestNoPrintfSpecifiersInTranslations: templates use {0} and not %s on purpose.
// A translator turning "%s uses %d" into "%d uses %s" would be undefined behaviour
// in C, and the language files are the least reviewed part of the tree.
func TestNoPrintfSpecifiersInTranslations(t *testing.T) {
	root := repoRoot(t)

	langs, keys, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	for _, code := range langOrder {
		for _, k := range keys {
			v := langs[code][k]
			for _, spec := range []string{"%s", "%d", "%u", "%x", "%p", "%n", "%lu", "%llu"} {
				if strings.Contains(v, spec) {
					t.Errorf("%s.json key %q contains %q; use {0} style placeholders",
						code, k, spec)
				}
			}
		}
	}
}

// TestNoEmDashesAnywhere: a colon, a comma or a full stop says the same thing and
// is on every keyboard. loadLangs refuses one, so this is the assertion that the
// refusal is real rather than a comment about intent.
func TestNoEmDashesAnywhere(t *testing.T) {
	root := repoRoot(t)

	langs, keys, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	for _, code := range langOrder {
		for _, k := range keys {
			if strings.ContainsAny(langs[code][k], "—–") {
				t.Errorf("%s.json key %q contains a dash loadLangs should have refused", code, k)
			}
		}
	}

	// And it does refuse, so a file that grows one cannot be generated from.
	dir := t.TempDir()
	for _, code := range langOrder {
		v := "text"
		if code == "de" {
			v = "text — more"
		}
		body := fmt.Sprintf("{\n  %q: %q,\n  %q: %q\n}\n", "lang.name", code, "a.key", v)
		if err := os.WriteFile(filepath.Join(dir, code+".json"), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	_, _, err = loadLangs(dir)
	if err == nil || !strings.Contains(err.Error(), "em or en dash") {
		t.Fatalf("loadLangs accepted a dash: %v", err)
	}
}
