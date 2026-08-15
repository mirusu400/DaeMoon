# Core tests, linked against the posix backend. No console, no devkitPro.
#
# The warning set is deliberately strict and the sanitizers are on by default: this
# is the only place the sync path runs where a bad pointer produces a readable
# report instead of a console hang.

ROOT ?= $(abspath ../..)
BUILD := $(ROOT)/build/coretest

CC ?= cc
CSTD := -std=c11

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wpointer-arith -Wwrite-strings -Wcast-qual -Wno-unused-parameter

SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer

INCLUDES := -I$(ROOT)/core/include -I$(ROOT)/vendor -I$(ROOT)/platform/posix -I$(ROOT)/tools/test

CFLAGS := $(CSTD) $(WARN) $(SAN) -g -O1 $(INCLUDES)
LDFLAGS := $(SAN)

CORE_SRC := $(wildcard $(ROOT)/core/src/*.c) $(wildcard $(ROOT)/core/src/util/*.c)
POSIX_SRC := $(wildcard $(ROOT)/platform/posix/*.c)
TEST_SRC := $(wildcard $(ROOT)/tools/test/*.c)
VENDOR_SRC := $(ROOT)/vendor/miniz/miniz.c

SRC := $(CORE_SRC) $(POSIX_SRC) $(TEST_SRC) $(VENDOR_SRC)
OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(SRC))

BIN := $(BUILD)/coretest

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJ)
	@$(CC) $(LDFLAGS) -o $@ $^

# Vendored sources are compiled without the project warning set. They are third
# party code copied in verbatim and patching them to satisfy our warnings would
# make the next update a merge.
$(BUILD)/vendor/%.o: $(ROOT)/vendor/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CSTD) $(SAN) -g -O1 -w $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	@$(BIN) $(ROOT)

clean:
	@rm -rf $(BUILD)
