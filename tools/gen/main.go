// Command gen turns the contracts in shared/ into generated source for both sides.
//
//	shared/errors.json  -> core/include/daemoon/error_codes.h
//	                       core/src/error_table.c
//	                       server/internal/apierr/codes_gen.go
//	shared/lang/*.json  -> core/include/daemoon/str_ids.h
//	                       core/src/lang_table.c
//	                       server/internal/i18n/strings_gen.go
//
// Runtime JSON parsing for UI strings would waste 3DS heap, so the language tables
// are compiled in as C arrays.
//
// One set of files feeds two clients, and each gets only its own half: keys under
// `web.` belong to the panel and keys outside it to the consoles, so a 3DS binary
// does not carry eight translations of "Sign out" and the panel does not carry the
// text of a restore confirmation. The parity checks run across the whole file
// regardless, which is the point of keeping them together.
//
// With -check it writes nothing and exits non-zero when the generated files are
// stale or a language file is inconsistent. That is what CI runs.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"go/format"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// langOrder is the display order of the language picker, and freezes the numeric
// value of daemoon_lang_t. Append only; never reorder.
var langOrder = []string{"en", "ko", "ja", "zh-Hans", "zh-Hant", "es", "fr", "de"}

type errDef struct {
	Code      string   `json:"code"`
	Value     int      `json:"value"`
	HTTP      *int     `json:"http"`
	Retryable bool     `json:"retryable"`
	Detail    []string `json:"detail"`
}

type errFile struct {
	FormatVersion int      `json:"format_version"`
	Errors        []errDef `json:"errors"`
}

var placeholderRe = regexp.MustCompile(`\{(\d+)\}`)

func main() {
	root := flag.String("root", ".", "repository root")
	checkMode := flag.Bool("check", false, "verify only: write nothing, fail if anything is stale")
	flag.Parse()

	if err := run(*root, *checkMode); err != nil {
		fmt.Fprintln(os.Stderr, "gen:", err)
		os.Exit(1)
	}
}

func run(root string, checkMode bool) error {
	errs, err := loadErrors(filepath.Join(root, "shared", "errors.json"))
	if err != nil {
		return err
	}
	langs, keys, err := loadLangs(filepath.Join(root, "shared", "lang"))
	if err != nil {
		return err
	}
	if err := checkErrorKeys(errs, langs["en"]); err != nil {
		return err
	}

	consoleKeys, webKeys := splitKeys(keys)
	if len(webKeys) == 0 {
		return fmt.Errorf("no %s* keys in shared/lang/en.json: the panel would have no text", webPrefix)
	}

	out := map[string][]byte{
		filepath.Join(root, "core", "include", "daemoon", "error_codes.h"):  genErrorCodesH(errs),
		filepath.Join(root, "core", "src", "error_table.c"):                 genErrorTableC(errs),
		filepath.Join(root, "core", "include", "daemoon", "str_ids.h"):      genStrIdsH(consoleKeys),
		filepath.Join(root, "core", "src", "lang_table.c"):                  genLangTableC(consoleKeys, langs),
		filepath.Join(root, "server", "internal", "apierr", "codes_gen.go"): genCodesGo(errs),
		filepath.Join(root, "server", "internal", "i18n", "strings_gen.go"): genPanelGo(webKeys, langs),
	}

	var stale []string
	for path, want := range out {
		got, err := os.ReadFile(path)
		if err == nil && bytes.Equal(got, want) {
			continue
		}
		stale = append(stale, path)
		if checkMode {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(path, want, 0o644); err != nil {
			return err
		}
	}
	sort.Strings(stale)
	if checkMode && len(stale) > 0 {
		return fmt.Errorf("generated files are stale, run `make gen`:\n  %s",
			strings.Join(stale, "\n  "))
	}
	for _, p := range stale {
		fmt.Println("gen:", mustRel(root, p))
	}
	return nil
}

func mustRel(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil {
		return r
	}
	return p
}

// ---------------------------------------------------------------- loading

func loadErrors(path string) ([]errDef, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var f errFile
	if err := json.Unmarshal(raw, &f); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if f.FormatVersion != 1 {
		return nil, fmt.Errorf("%s: unsupported format_version %d", path, f.FormatVersion)
	}
	seenCode := map[string]bool{}
	seenValue := map[int]string{}
	for _, e := range f.Errors {
		if e.Value <= 0 {
			return nil, fmt.Errorf("%s: %s has non positive value %d", path, e.Code, e.Value)
		}
		if seenCode[e.Code] {
			return nil, fmt.Errorf("%s: duplicate code %q", path, e.Code)
		}
		if prev, ok := seenValue[e.Value]; ok {
			return nil, fmt.Errorf("%s: value %d used by both %q and %q (values are frozen, append only)",
				path, e.Value, prev, e.Code)
		}
		seenCode[e.Code] = true
		seenValue[e.Value] = e.Code
	}
	sort.Slice(f.Errors, func(i, j int) bool { return f.Errors[i].Value < f.Errors[j].Value })
	return f.Errors, nil
}

// loadLangs reads every shared/lang/*.json. en.json defines the key set; a file
// that is missing a key or carries an unknown one is an error, which is the parity
// guarantee the root CLAUDE.md asks CI for.
func loadLangs(dir string) (map[string]map[string]string, []string, error) {
	langs := map[string]map[string]string{}
	for _, code := range langOrder {
		path := filepath.Join(dir, code+".json")
		raw, err := os.ReadFile(path)
		if err != nil {
			return nil, nil, err
		}
		var m map[string]string
		dec := json.NewDecoder(bytes.NewReader(raw))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&m); err != nil {
			return nil, nil, fmt.Errorf("%s: %w", path, err)
		}
		langs[code] = m
	}

	en := langs["en"]
	if len(en) == 0 {
		return nil, nil, fmt.Errorf("%s: en.json is empty", dir)
	}
	keys := make([]string, 0, len(en))
	for k := range en {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	var problems []string

	// No em or en dashes, in any language.
	//
	// A colon, a comma or a full stop says the same thing, and every one of them is
	// on a keyboard. A dash somewhere between two clauses is also the punctuation a
	// language model reaches for, which makes text carrying it read as generated -
	// so it goes, and it goes here rather than in a review, because it comes back
	// otherwise.
	for _, code := range langOrder {
		for _, k := range keysOf(langs[code]) {
			if strings.ContainsAny(langs[code][k], "—–") {
				problems = append(problems, fmt.Sprintf(
					"%s.json: key %q uses an em or en dash; a colon, a comma or a full stop says it",
					code, k))
			}
		}
	}

	for _, code := range langOrder {
		if code == "en" {
			continue
		}
		m := langs[code]
		for _, k := range keys {
			v, ok := m[k]
			if !ok {
				problems = append(problems, fmt.Sprintf("%s.json: missing key %q", code, k))
				continue
			}
			if strings.TrimSpace(v) == "" {
				problems = append(problems, fmt.Sprintf("%s.json: key %q is empty", code, k))
			}
			// Word order varies by language, so a translation may reorder
			// placeholders, but it must use exactly the same set.
			if a, b := placeholders(en[k]), placeholders(v); a != b {
				problems = append(problems, fmt.Sprintf(
					"%s.json: key %q uses placeholders %s, en.json uses %s", code, k, b, a))
			}
		}
		for k := range m {
			if _, ok := en[k]; !ok {
				problems = append(problems, fmt.Sprintf("%s.json: key %q is not in en.json", code, k))
			}
		}
	}
	if len(problems) > 0 {
		sort.Strings(problems)
		return nil, nil, fmt.Errorf("language files are inconsistent:\n  %s",
			strings.Join(problems, "\n  "))
	}
	return langs, keys, nil
}

func keysOf(m map[string]string) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// webPrefix marks a key as the panel's rather than a console's.
const webPrefix = "web."

// splitKeys divides the language files between the two clients that read them.
//
// It is a prefix and not a second directory because the parity check is the
// valuable part: one file per language, one place to add a string, and CI fails if
// any language is short a key no matter which client will render it.
func splitKeys(keys []string) (console, web []string) {
	for _, k := range keys {
		if strings.HasPrefix(k, webPrefix) {
			web = append(web, k)
			continue
		}
		console = append(console, k)
	}
	return console, web
}

func placeholders(s string) string {
	found := map[string]bool{}
	for _, m := range placeholderRe.FindAllStringSubmatch(s, -1) {
		found[m[1]] = true
	}
	out := make([]string, 0, len(found))
	for k := range found {
		out = append(out, k)
	}
	sort.Strings(out)
	return "{" + strings.Join(out, ",") + "}"
}

// checkErrorKeys makes adding an error code impossible without adding its text.
func checkErrorKeys(errs []errDef, en map[string]string) error {
	var missing []string
	for _, e := range errs {
		if _, ok := en["err."+e.Code]; !ok {
			missing = append(missing, "err."+e.Code)
		}
	}
	if len(missing) > 0 {
		return fmt.Errorf("shared/lang/en.json is missing text for: %s", strings.Join(missing, ", "))
	}
	return nil
}

// ---------------------------------------------------------------- naming

func enumSuffix(s string) string {
	r := strings.NewReplacer(".", "_", "-", "_", " ", "_")
	return strings.ToUpper(r.Replace(s))
}

func goName(code string) string {
	parts := strings.Split(code, "_")
	for i, p := range parts {
		if p == "" {
			continue
		}
		switch p {
		case "http":
			parts[i] = "HTTP"
		case "id":
			parts[i] = "ID"
		default:
			parts[i] = strings.ToUpper(p[:1]) + p[1:]
		}
	}
	return strings.Join(parts, "")
}

func cString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '\n':
			b.WriteString("\\n")
		case c == '\t':
			b.WriteString("\\t")
		case c == '\r':
			b.WriteString("\\r")
		case c < 0x20 || c == 0x7f:
			fmt.Fprintf(&b, "\\%03o", c)
		default:
			b.WriteByte(c) // UTF-8 passes through
		}
	}
	b.WriteByte('"')
	return b.String()
}

const banner = `/* GENERATED by tools/gen. DO NOT EDIT.
 * Source: %s
 * Regenerate with: make gen
 */
`

// ---------------------------------------------------------------- emit: C

func genErrorCodesH(errs []errDef) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, banner, "shared/errors.json")
	b.WriteString(`
#ifndef DAEMOON_ERROR_CODES_H
#define DAEMOON_ERROR_CODES_H

/* Values are frozen in shared/errors.json. Never renumber, never reuse. */
enum {
    DAEMOON_OK = 0,
`)
	width := 0
	for _, e := range errs {
		if n := len("DAEMOON_ERR_" + enumSuffix(e.Code)); n > width {
			width = n
		}
	}
	for _, e := range errs {
		name := "DAEMOON_ERR_" + enumSuffix(e.Code)
		fmt.Fprintf(&b, "    %-*s = %d,\n", width, name, e.Value)
	}
	fmt.Fprintf(&b, "\n    DAEMOON_ERR_COUNT_ = %d\n};\n\n#endif /* DAEMOON_ERROR_CODES_H */\n",
		len(errs)+1)
	return b.Bytes()
}

func genErrorTableC(errs []errDef) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, banner, "shared/errors.json")
	b.WriteString(`
#include <daemoon/result.h>
#include <daemoon/str_ids.h>

#include <string.h>

typedef struct {
    daemoon_result_t  result;
    const char       *code;
    daemoon_str_id_t  str_id;
    short             http;      /* 0 when the server never emits this code */
    unsigned char     retryable;
} daemoon_error_row_t;

static const daemoon_error_row_t k_errors[] = {
`)
	for _, e := range errs {
		http := 0
		if e.HTTP != nil {
			http = *e.HTTP
		}
		retry := 0
		if e.Retryable {
			retry = 1
		}
		fmt.Fprintf(&b, "    { DAEMOON_ERR_%s, %s, DAEMOON_STR_ERR_%s, %d, %d },\n",
			enumSuffix(e.Code), cString(e.Code), enumSuffix(e.Code), http, retry)
	}
	b.WriteString(`};

static const size_t k_error_count = sizeof(k_errors) / sizeof(k_errors[0]);

static const daemoon_error_row_t *find_row(daemoon_result_t r)
{
    size_t i;
    for (i = 0; i < k_error_count; ++i) {
        if (k_errors[i].result == r) {
            return &k_errors[i];
        }
    }
    return NULL;
}

const char *daemoon_result_code(daemoon_result_t r)
{
    const daemoon_error_row_t *row;
    if (r == DAEMOON_OK) {
        return "ok";
    }
    row = find_row(r);
    return row != NULL ? row->code : "internal_error";
}

daemoon_result_t daemoon_result_from_code(const char *code, size_t len)
{
    size_t i;
    if (code == NULL) {
        return DAEMOON_ERR_INTERNAL_ERROR;
    }
    if (len == 0) {
        len = strlen(code);
    }
    for (i = 0; i < k_error_count; ++i) {
        if (strlen(k_errors[i].code) == len &&
            memcmp(k_errors[i].code, code, len) == 0) {
            return k_errors[i].result;
        }
    }
    /* An unknown code from a newer server must not be mistaken for success. */
    return DAEMOON_ERR_INTERNAL_ERROR;
}

daemoon_str_id_t daemoon_result_str_id(daemoon_result_t r)
{
    const daemoon_error_row_t *row = find_row(r);
    return row != NULL ? row->str_id : DAEMOON_STR_ERR_INTERNAL_ERROR;
}

daemoon_result_t daemoon_result_from_http(int status, const char *code, size_t len)
{
    size_t i;
    if (status >= 200 && status < 300) {
        return DAEMOON_OK;
    }
    if (code != NULL && *code != '\0') {
        return daemoon_result_from_code(code, len);
    }
    /* No body, or a body the proxy replaced: fall back to the status. */
    for (i = 0; i < k_error_count; ++i) {
        if (k_errors[i].http == status) {
            return k_errors[i].result;
        }
    }
    return DAEMOON_ERR_INTERNAL_ERROR;
}

int daemoon_result_retryable(daemoon_result_t r)
{
    const daemoon_error_row_t *row = find_row(r);
    return row != NULL ? (int)row->retryable : 0;
}
`)
	return b.Bytes()
}

func genStrIdsH(keys []string) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, banner, "shared/lang/en.json")
	b.WriteString(`
#ifndef DAEMOON_STR_IDS_H
#define DAEMOON_STR_IDS_H

/* Most substitutions take one or two arguments; eight is far more headroom than
 * any sentence in shared/lang/ needs and keeps daemoon_str_ref_t small. */
#define DAEMOON_STR_MAX_ARGS 8

/* Every user visible string is one of these. The UI backend takes daemoon_str_id_t
 * and not const char *, so a literal cannot reach the screen by accident. */
typedef enum {
`)
	for _, k := range keys {
		fmt.Fprintf(&b, "    DAEMOON_STR_%s,\n", enumSuffix(k))
	}
	b.WriteString(`
    DAEMOON_STR_COUNT
} daemoon_str_id_t;

typedef enum {
`)
	for _, code := range langOrder {
		fmt.Fprintf(&b, "    DAEMOON_LANG_%s,\n", enumSuffix(code))
	}
	b.WriteString(`
    DAEMOON_LANG_COUNT
} daemoon_lang_t;

#endif /* DAEMOON_STR_IDS_H */
`)
	return b.Bytes()
}

func genLangTableC(keys []string, langs map[string]map[string]string) []byte {
	var b bytes.Buffer
	fmt.Fprintf(&b, banner, "shared/lang/")
	b.WriteString(`
#include <daemoon/i18n.h>

/* Indexed [language][string id]. English is index 0 and is the fallback for any
 * entry a translation has not filled in. */
const char *const daemoon_lang_table[DAEMOON_LANG_COUNT][DAEMOON_STR_COUNT] = {
`)
	for _, code := range langOrder {
		m := langs[code]
		fmt.Fprintf(&b, "    /* %s */ {\n", code)
		for _, k := range keys {
			v, ok := m[k]
			if !ok {
				fmt.Fprintf(&b, "        NULL, /* %s */\n", k)
				continue
			}
			fmt.Fprintf(&b, "        %s, /* %s */\n", cString(v), k)
		}
		b.WriteString("    },\n")
	}
	b.WriteString("};\n\nconst char *const daemoon_lang_codes[DAEMOON_LANG_COUNT] = {\n")
	for _, code := range langOrder {
		fmt.Fprintf(&b, "    %s,\n", cString(code))
	}
	b.WriteString("};\n")
	return b.Bytes()
}

// ---------------------------------------------------------------- emit: Go

func genPanelGo(keys []string, langs map[string]map[string]string) []byte {
	var b bytes.Buffer
	b.WriteString("// Code generated by tools/gen from shared/lang/. DO NOT EDIT.\n\n")
	b.WriteString("package i18n\n\n")

	b.WriteString("// Order is the language picker's order, and it is the order in\n")
	b.WriteString("// shared/lang/. English is first because it is the fallback.\nvar Order = []Lang{\n")
	for _, code := range langOrder {
		fmt.Fprintf(&b, "\t%q,\n", code)
	}
	b.WriteString("}\n\n")

	b.WriteString("// Names holds each language's name in itself. A picker that says \"Korean\"\n")
	b.WriteString("// to somebody who cannot read English has not offered them anything.\nvar Names = map[Lang]string{\n")
	for _, code := range langOrder {
		fmt.Fprintf(&b, "\t%q: %q,\n", code, langs[code]["lang.name"])
	}
	b.WriteString("}\n\n")

	b.WriteString("// table holds the panel's half of shared/lang/, keyed by language and then\n")
	b.WriteString("// by the key from those files. The consoles' half is not here.\nvar table = map[Lang]map[string]string{\n")
	for _, code := range langOrder {
		m := langs[code]
		fmt.Fprintf(&b, "\t%q: {\n", code)
		for _, k := range keys {
			fmt.Fprintf(&b, "\t\t%q: %q,\n", k, m[k])
		}
		b.WriteString("\t},\n")
	}
	b.WriteString("}\n")

	src, err := format.Source(b.Bytes())
	if err != nil {
		panic(fmt.Sprintf("generated Go does not parse: %v", err))
	}
	return src
}

func genCodesGo(errs []errDef) []byte {
	var b bytes.Buffer
	b.WriteString("// Code generated by tools/gen from shared/errors.json. DO NOT EDIT.\n\n")
	b.WriteString("package apierr\n\nimport \"net/http\"\n\n")
	b.WriteString("// Code is a wire error code. The server never localizes: it returns one of\n")
	b.WriteString("// these and the client renders the text from shared/lang/.\ntype Code string\n\nconst (\n")
	for _, e := range errs {
		fmt.Fprintf(&b, "\t%s Code = %q\n", goName(e.Code), e.Code)
	}
	b.WriteString(")\n\n")

	b.WriteString("// statusOf maps a code to its HTTP status. Codes that only ever happen on the\n")
	b.WriteString("// client are absent and fall back to 500.\nvar statusOf = map[Code]int{\n")
	for _, e := range errs {
		if e.HTTP == nil {
			continue
		}
		fmt.Fprintf(&b, "\t%s: %d,\n", goName(e.Code), *e.HTTP)
	}
	b.WriteString("}\n\n")

	b.WriteString("// Status returns the HTTP status for a code.\nfunc (c Code) Status() int {\n")
	b.WriteString("\tif s, ok := statusOf[c]; ok {\n\t\treturn s\n\t}\n\treturn http.StatusInternalServerError\n}\n\n")

	b.WriteString("// Known reports whether c came from shared/errors.json.\nfunc (c Code) Known() bool {\n")
	b.WriteString("\t_, ok := detailKeys[c]\n\treturn ok\n}\n\n")

	b.WriteString("// detailKeys lists the detail fields each code is allowed to carry. Tests use\n")
	b.WriteString("// it to keep handlers and shared/errors.json from drifting apart.\nvar detailKeys = map[Code][]string{\n")
	for _, e := range errs {
		if len(e.Detail) == 0 {
			fmt.Fprintf(&b, "\t%s: nil,\n", goName(e.Code))
			continue
		}
		quoted := make([]string, len(e.Detail))
		for i, d := range e.Detail {
			quoted[i] = fmt.Sprintf("%q", d)
		}
		fmt.Fprintf(&b, "\t%s: {%s},\n", goName(e.Code), strings.Join(quoted, ", "))
	}
	b.WriteString("}\n\n")

	b.WriteString("// DetailKeys returns the declared detail fields for c.\nfunc DetailKeys(c Code) []string { return detailKeys[c] }\n\n")
	b.WriteString("// All returns every code, in the order declared in shared/errors.json.\nfunc All() []Code {\n\treturn []Code{\n")
	for _, e := range errs {
		fmt.Fprintf(&b, "\t\t%s,\n", goName(e.Code))
	}
	b.WriteString("\t}\n}\n")

	src, err := format.Source(b.Bytes())
	if err != nil {
		// The template above is fixed, so this only fires while editing the
		// generator. Emit the unformatted source so the error is readable.
		panic(fmt.Sprintf("generated Go does not parse: %v", err))
	}
	return src
}
