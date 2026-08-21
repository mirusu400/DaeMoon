# The Switch build is CMake. See CMakeLists.txt for why.
#
# This file exists so that the one command the rest of the project uses still works:
#
#   make docker-nx        from the repository root
#
# and so that CI, which runs that target and publishes platform/nx/daemoon.nro, did
# not have to learn a second way to build a console.

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Use `make docker-nx`, or source the devkitPro environment.)
endif

BUILD ?= build
JOBS  ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all clean

all:
	@cmake -S $(CURDIR) -B $(CURDIR)/$(BUILD) -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(CURDIR)/$(BUILD) -j$(JOBS)

clean:
	@echo clean ...
	@rm -rf $(CURDIR)/$(BUILD) $(CURDIR)/daemoon.nro
