# devkitARM. Requires DEVKITARM and DEVKITPRO in the environment.
#
# Phase 1. The target here is a CIA, not a 3dsx: a .3dsx has no permission to reach
# another title's save archive, and finding that out late means redoing the phase.

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set. Source the devkitPro environment first.)
endif

include $(DEVKITARM)/3ds_rules

TARGET      := daemoon
BUILD       := build
APP_TITLE   := DaeMoon
APP_DESCRIPTION := Save data sync
APP_AUTHOR  := DaeMoon
ICON        := assets/icon.png
RSF         := app.rsf

# Core is compiled straight into the app rather than linked as a library: one
# compiler, one set of flags, and no stale archive to explain.
SOURCES  := source source/ui ../../core/src ../../core/src/util \
            ../../vendor/miniz ../../vendor/jsmn
INCLUDES := source ../../core/include ../../vendor

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections \
            $(ARCH) $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -D__3DS__ -DARM11 -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# 3ds-curl and 3ds-mbedtls, not httpc:C. See CLAUDE.md in this directory.
LIBS     := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

.PHONY: all cia clean

all: $(TARGET).3dsx

# The build every test run uses. A 3dsx build exists only for quick UI iteration.
cia: $(TARGET).cia

$(TARGET).cia: $(TARGET).elf
	@makerom -f cia -o $@ -elf $< -rsf $(RSF) -icon icon.icn -banner banner.bnr \
		-exefslogo -target t

clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).cia
