# Delegation only. There is deliberately no unified build: the devkitPro templates
# lean hard on DEVKITARM and DEVKITA64 and break when forced into one invocation.
#
# The Go modules live under server/ and tools/ rather than at the root, because the
# root vendor/ directory holds copied in C sources and Go claims that name.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: help all test check core-test server-test tools-test e2e gen gen-check lang-check \
        core-isolation spec-check server 3ds nx run-server clean \
        docker-images docker-3ds docker-cia docker-nx docker-test docker-shell \
        cia-verify emu-selftest

all: help

help:
	@echo "make test          core tests plus go tests, no console required"
	@echo "make core-test     core only, the main development loop"
	@echo "make e2e           the real client against the real server"
	@echo "make check         everything CI runs except the console builds"
	@echo "make server        build the server binary into build/"
	@echo "make run-server    build and run it on :8080 with a local database"
	@echo "make docker-cia    3DS CIA in a container, no toolchain to install"
	@echo "make cia-verify    read the built CIA back and check its permissions"
	@echo "make emu-selftest  run the backend suite inside an emulator"
	@echo "make docker-nx     Switch NRO in a container"
	@echo "make docker-test   the desktop suites in a container"
	@echo "make 3ds           devkitARM, needs a local toolchain"
	@echo "make nx            devkitA64, needs a local toolchain"
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

# ------------------------------------------------------------------- docker
#
# devkitPro is a large install with a package manager of its own, and the version
# it happens to be on decides whether a build works. Pinning it to an image means
# a contributor with docker can build a CIA without any of that, and means CI and
# a laptop are running the same toolchain.
#
# Every run maps the invoking user, or the tree fills up with root owned build
# artifacts that then need root to delete.
DOCKER_RUN := docker run --rm -u $(shell id -u):$(shell id -g) -e HOME=/tmp \
              -v $(ROOT):/work -w /work

docker-images:
	@docker build -q -f $(ROOT)/docker/dev.Dockerfile     -t daemoon-dev:local     $(ROOT) >/dev/null
	@docker build -q -f $(ROOT)/docker/3ds.Dockerfile     -t daemoon-3ds:local     $(ROOT) >/dev/null
	@docker build -q -f $(ROOT)/docker/nx.Dockerfile      -t daemoon-nx:local      $(ROOT) >/dev/null
	@docker build -q -f $(ROOT)/docker/ctrtool.Dockerfile -t daemoon-ctrtool:local $(ROOT) >/dev/null
	@echo "daemoon-dev:local daemoon-3ds:local daemoon-nx:local daemoon-ctrtool:local"

# A .3dsx is for iterating on the UI. It cannot reach another title's save
# archive, so it is never what a save path is tested with.
docker-3ds:
	@$(DOCKER_RUN) -w /work/platform/3ds daemoon-3ds:local make

# The build that matters.
docker-cia:
	@$(DOCKER_RUN) -w /work/platform/3ds daemoon-3ds:local make cia

# Reads the CIA back rather than trusting that makerom did what app.rsf said. A
# .3dsx cannot reach another title's save archive; the exheader is the difference,
# and a missing right there looks exactly like a code bug on hardware.
cia-verify: docker-cia
	@$(DOCKER_RUN) daemoon-ctrtool:local sh tools/verify-cia.sh platform/3ds/daemoon.cia

# Runs the conformance suite as a real ARM binary through real libctru. Not a
# substitute for hardware - emulators do not reproduce save archive behaviour -
# but it is the difference between code that has been executed on the target
# instruction set and code that has only been compiled for it.
emu-selftest:
	@sh $(ROOT)/tools/emu-selftest.sh

docker-nx:
	@$(DOCKER_RUN) -w /work/platform/nx daemoon-nx:local make

docker-test:
	@$(DOCKER_RUN) daemoon-dev:local make check

# For poking at a toolchain by hand: make docker-shell IMAGE=daemoon-3ds:local
IMAGE ?= daemoon-dev:local
docker-shell:
	@docker run --rm -it -u $(shell id -u):$(shell id -g) -e HOME=/tmp \
		-v $(ROOT):/work -w /work $(IMAGE) bash

clean:
	@$(MAKE) -C $(ROOT)/tools/test clean
	@rm -rf $(ROOT)/build
