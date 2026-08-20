# devkitA64. Run it through docker/ rather than installing a toolchain by hand:
#
#   make docker-nx        from the repository root

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Use `make docker-nx`, or source the devkitPro environment.)
endif

# The built in suffix rules go first, before anything defines a rule of its own.
# Left on, make chains them: it decides cacert.bin could be built from cacert.bin.o
# and reports a circular dependency it then drops. Harmless and alarming, and the
# devkitPro templates clear them for exactly this reason.
.SUFFIXES:

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET  := daemoon
BUILD   := build
APP_TITLE   := DaeMoon
APP_AUTHOR  := DaeMoon
APP_VERSION := 0.1.0

SOURCES  := source source/ui ../common \
            ../../core/src ../../core/src/util \
            ../../vendor/miniz
INCLUDES := source ../common ../../core/include ../../vendor ../../tools/test

# The public root certificates, linked in rather than read off the SD card. Same
# reason as the 3DS build: a console that needs a .pem placed beside it before
# https works is one that works for whoever built it and nobody else.
DATA     := ../../vendor/cacert/data

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections \
          $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

# curl and mbedtls, statically, for the same reason the 3DS build links them: a self
# hosted server is exactly the case where the operator cannot be asked to downgrade
# their TLS to suit a console. The order matters - curl needs mbedtls, which needs z.
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(CURDIR)/../../tools/test \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
# The backend contract, checked on hardware rather than only on a desktop. The same
# file the 3DS build links and the same one `make core-test` runs against a stub: it
# is written against daemoon_save_backend_t and knows nothing about how a save is
# reached, which is the whole reason it can be pointed at this backend unchanged.
#
# One file, not the directory: the rest of tools/test has a main() and wants fixtures.
CFILES   += backend_conformance.c test_failure.c
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export OFILES := $(OFILES_BIN) $(OFILES_SRC)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard $(CURDIR)/assets/icon.jpg)
    ifneq (,$(icons))
        export APP_ICON := $(CURDIR)/assets/icon.jpg
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_ICON),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

# bin2s. devkitA64 supplies the recipe and leaves the rule to the project, so the
# CA bundle needs this to become an object at all.
$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

endif
