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

# The libctru stub comes first: platform/3ds/source includes <3ds.h>, and on a
# desktop the only one that exists is the one under ctru_stub.
INCLUDES := -I$(ROOT)/tools/test/ctru_stub \
            -I$(ROOT)/core/include -I$(ROOT)/vendor -I$(ROOT)/vendor/quirc -I$(ROOT)/platform/posix \
            -I$(ROOT)/tools/test -I$(ROOT)/platform/3ds/source

CFLAGS := $(CSTD) $(WARN) $(SAN) -g -O1 -MMD -MP $(INCLUDES)
LDFLAGS := $(SAN)

CORE_SRC := $(wildcard $(ROOT)/core/src/*.c) $(wildcard $(ROOT)/core/src/util/*.c)
POSIX_SRC := $(wildcard $(ROOT)/platform/posix/*.c)
TEST_SRC := $(wildcard $(ROOT)/tools/test/*.c) $(ROOT)/tools/test/ctru_stub/ctru_stub.c

# The 3DS backend, run here against the stub. Only the files that are free of
# libctru's UI and service init: the rest of platform/3ds needs a console.
TDS_SRC := $(ROOT)/platform/3ds/source/save_backend.c \
           $(ROOT)/platform/3ds/source/fs_backend.c \
           $(ROOT)/platform/3ds/source/nds_backend.c \
           $(ROOT)/platform/3ds/source/nds_icon.c \
           $(ROOT)/platform/3ds/source/cam_tile.c \
           $(ROOT)/platform/3ds/source/config.c \
           $(ROOT)/platform/3ds/source/title_cache.c \
           $(ROOT)/platform/3ds/source/backup_list.c \
           $(ROOT)/platform/3ds/source/trace.c \
           $(ROOT)/platform/3ds/source/title_id.c
VENDOR_SRC := $(ROOT)/vendor/miniz/miniz.c $(wildcard $(ROOT)/vendor/quirc/*.c)

# quirc 1.2 calls memcpy with a null source and a length of zero the first time a
# buffer is sized. Harmless, and undefined by the letter of the standard, so the
# sanitizer says so on every run. Vendored sources are not patched in place - see
# vendor/README.md - so the report is suppressed rather than the code changed.
UBSAN_OPTIONS ?= suppressions=$(ROOT)/tools/test/ubsan.supp
export UBSAN_OPTIONS

SRC := $(CORE_SRC) $(POSIX_SRC) $(TEST_SRC) $(TDS_SRC) $(VENDOR_SRC)
OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(SRC))

DEPS := $(OBJ:.o=.d)

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

# Header dependencies, so a change to a generated header rebuilds what uses it.
-include $(DEPS)
