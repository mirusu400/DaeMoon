# devkitA64. Run it through docker/ rather than installing a toolchain by hand:
#
#   make docker-nx        from the repository root

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Use `make docker-nx`, or source the devkitPro environment.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET  := daemoon
BUILD   := build
APP_TITLE   := DaeMoon
APP_AUTHOR  := DaeMoon
APP_VERSION := 0.1.0

SOURCES  := source source/ui \
            ../../core/src ../../core/src/util \
            ../../vendor/miniz
INCLUDES := source ../../core/include ../../vendor

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections \
          $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

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

-include $(DEPENDS)

endif
