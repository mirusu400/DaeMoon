// Command speccheck holds the server's routes against shared/openapi.yaml.
//
// The spec is authoritative, so a handler that exists without an entry there is a
// build failure rather than something to notice during review six weeks later.
//
// It reads both files as text on purpose. Importing the api package would mean this
// module depends on the server module and on chi, and a check that only runs when
// the thing it checks already compiles is worth less than one that always runs.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Routes are declared in api.Describe(). Reading them from there keeps one list.
var describeRe = regexp.MustCompile(`(?m)^\s*"([A-Z]+) (/[^"]*)",\s*$`)

// A path key in an OpenAPI document: two spaces of indent, then /something:
var pathRe = regexp.MustCompile(`(?m)^  (/[^:\s]*):\s*$`)

// A method under a path: four spaces, then get/post/...
var methodRe = regexp.MustCompile(`(?m)^    (get|post|put|patch|delete|head|options):\s*$`)

func main() {
	root := flag.String("root", ".", "repository root")
	flag.Parse()

	if err := run(*root); err != nil {
		fmt.Fprintln(os.Stderr, "speccheck:", err)
		os.Exit(1)
	}
	fmt.Println("spec check: ok")
}

func run(root string) error {
	code, err := os.ReadFile(filepath.Join(root, "server", "internal", "api", "api.go"))
	if err != nil {
		return err
	}
	spec, err := os.ReadFile(filepath.Join(root, "shared", "openapi.yaml"))
	if err != nil {
		return err
	}

	served, err := servedRoutes(string(code))
	if err != nil {
		return err
	}
	documented := documentedRoutes(string(spec))

	var problems []string
	for r := range served {
		if !documented[r] {
			problems = append(problems, fmt.Sprintf(
				"%s is served but not in shared/openapi.yaml", r))
		}
	}
	for r := range documented {
		if !served[r] {
			problems = append(problems, fmt.Sprintf(
				"%s is in shared/openapi.yaml but not served", r))
		}
	}
	if len(problems) > 0 {
		sort.Strings(problems)
		return fmt.Errorf("the spec and the handlers disagree:\n  %s",
			strings.Join(problems, "\n  "))
	}
	return nil
}

// servedRoutes reads the list api.Describe() returns.
func servedRoutes(code string) (map[string]bool, error) {
	start := strings.Index(code, "func Describe() []string {")
	if start < 0 {
		return nil, fmt.Errorf("api.go has no Describe function to read routes from")
	}
	end := strings.Index(code[start:], "\n}")
	if end < 0 {
		return nil, fmt.Errorf("api.go: Describe is not terminated")
	}
	body := code[start : start+end]

	out := map[string]bool{}
	for _, m := range describeRe.FindAllStringSubmatch(body, -1) {
		out[normalize(m[1], m[2])] = true
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("api.go: Describe lists no routes")
	}
	return out, nil
}

func documentedRoutes(spec string) map[string]bool {
	out := map[string]bool{}

	pathLocs := pathRe.FindAllStringSubmatchIndex(spec, -1)
	for i, loc := range pathLocs {
		path := spec[loc[2]:loc[3]]
		end := len(spec)
		if i+1 < len(pathLocs) {
			end = pathLocs[i+1][0]
		}
		for _, m := range methodRe.FindAllStringSubmatch(spec[loc[1]:end], -1) {
			out[normalize(strings.ToUpper(m[1]), path)] = true
		}
	}
	return out
}

// normalize makes the two sources comparable: chi writes {tid}, OpenAPI writes
// {tid} as well, but the parameter names are free to differ, so they are erased.
var paramRe = regexp.MustCompile(`\{[^}]*\}`)

func normalize(method, path string) string {
	return method + " " + paramRe.ReplaceAllString(path, "{}")
}
