# Delegation only. There is deliberately no unified build: the devkitPro templates
# lean hard on DEVKITARM and DEVKITA64 and break when forced into one invocation.
#
# The Go modules live under server/ and tools/ rather than at the root, because the
# root vendor/ directory holds copied in C sources and Go claims that name.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: help all test check core-test server-test tools-test e2e gen gen-check lang-check \
        core-isolation spec-check server 3ds nx run-server clean

all: help

help:
	@echo "make test          core tests plus go tests, no console required"
	@echo "make core-test     core only, the main development loop"
	@echo "make e2e           the real client against the real server"
	@echo "make check         everything CI runs except the console builds"
	@echo "make server        build the server binary into build/"
	@echo "make run-server    build and run it on :8080 with a local database"
	@echo "make 3ds           devkitARM CIA build"
	@echo "make nx            devkitA64 NRO build"
	@echo "make gen           regenerate what shared/ feeds into core/ and server/"

# ------------------------------------------------------------------ generated

# shared/errors.json and shared/lang/*.json are the single source of truth for both
# sides. Everything they produce is committed, so a console build never needs Go.
gen:
	@cd $(ROOT)/tools && go run ./gen -root $(ROOT)

gen-check:
	@cd $(ROOT)/tools && go run ./gen -root $(ROOT) -check

# The language parity check the root CLAUDE.md asks CI for lives inside the
# generator: it refuses to emit a table when a key is missing from any language, is
# empty, or uses a different placeholder set than English.
lang-check: gen-check

# ------------------------------------------------------------------ the rules

# core/ must never include a platform header. Breaking that collapses the whole
# design, so it fails the build rather than waiting for a review.
core-isolation:
	@if grep -rnE '#[[:space:]]*include[[:space:]]*[<"](3ds|switch)\.h[>"]' $(ROOT)/core/ ; then \
		echo "core/ includes a platform header - see the coding rules in CLAUDE.md"; \
		exit 1; \
	fi
	@echo "core isolation: ok"

# shared/openapi.yaml is authoritative. A handler without an entry there fails here.
spec-check:
	@cd $(ROOT)/tools && go run ./speccheck -root $(ROOT)

# ------------------------------------------------------------------- targets

core-test:
	@$(MAKE) -C $(ROOT)/tools/test ROOT=$(ROOT) run

server-test:
	@cd $(ROOT)/server && go test ./...

tools-test:
	@cd $(ROOT)/tools && go vet ./... && go test $$(cd $(ROOT)/tools && go list ./... | grep -v /e2e)

# Builds both binaries and drives daemoonctl against daemoond. Kept out of `test`
# because it is slower and needs a C compiler; it is the only thing that sees the
# two sides talking to each other, so `check` does run it.
e2e:
	@cd $(ROOT)/tools && go test ./e2e/

test: core-test server-test tools-test

check: gen-check core-isolation spec-check test e2e
	@echo "all checks passed"

server:
	@mkdir -p $(ROOT)/build
	@cd $(ROOT)/server && go build -trimpath -o $(ROOT)/build/daemoond ./cmd/daemoond
	@echo "build/daemoond"

run-server: server
	@DAEMOON_DB=$(ROOT)/build/daemoon.db $(ROOT)/build/daemoond

3ds:
	@$(MAKE) -C $(ROOT)/platform/3ds

nx:
	@$(MAKE) -C $(ROOT)/platform/nx

clean:
	@$(MAKE) -C $(ROOT)/tools/test clean
	@rm -rf $(ROOT)/build
