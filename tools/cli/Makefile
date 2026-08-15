# daemoonctl: core plus the posix backend, built with the host compiler.
#
# No sanitizers here. This one is meant to be run by hand against a real daemoond,
# and the sanitized build lives in tools/test.

ROOT ?= $(abspath ../..)
BUILD := $(ROOT)/build/cli

CC ?= cc
CSTD := -std=c11
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wno-unused-parameter

INCLUDES := -I$(ROOT)/core/include -I$(ROOT)/vendor -I$(ROOT)/platform/posix
CFLAGS := $(CSTD) $(WARN) -O2 -g $(INCLUDES)

CORE_SRC := $(wildcard $(ROOT)/core/src/*.c) $(wildcard $(ROOT)/core/src/util/*.c)
POSIX_SRC := $(wildcard $(ROOT)/platform/posix/*.c)
SRC := $(CORE_SRC) $(POSIX_SRC) $(ROOT)/vendor/miniz/miniz.c $(ROOT)/tools/cli/main.c
OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(SRC))

BIN := $(ROOT)/build/daemoonctl

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@$(CC) -o $@ $^
	@echo "build/daemoonctl"

# Vendored sources keep their own warning settings: they are copied in verbatim and
# patching them to satisfy ours would make the next update a merge.
$(BUILD)/vendor/%.o: $(ROOT)/vendor/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CSTD) -O2 -w $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) $(BIN)
