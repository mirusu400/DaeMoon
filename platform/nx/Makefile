# devkitA64. Requires DEVKITA64 and DEVKITPRO in the environment.
#
# Phase 6.

ifeq ($(strip $(DEVKITA64)),)
$(error DEVKITA64 is not set. Source the devkitPro environment first.)
endif

include $(DEVKITA64)/base_rules

TARGET   := daemoon
BUILD    := build
APP_TITLE  := DaeMoon
APP_AUTHOR := DaeMoon

SOURCES  := source source/ui ../../core/src ../../core/src/util \
            ../../vendor/miniz ../../vendor/jsmn
INCLUDES := source ../../core/include ../../vendor

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) \
            $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -D__SWITCH__ -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)

LIBS     := -lnx
LIBDIRS  := $(LIBNX) $(PORTLIBS)

.PHONY: all clean

all: $(TARGET).nro

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf
