# devkitARM. Run it through docker/ rather than installing a toolchain by hand:
#
#   make docker-3ds        from the repository root
#
# The CIA is the build that matters. A .3dsx cannot reach another title's save
# archive, so it is only useful for iterating on the UI, and the permissions that
# make this project work at all come from app.rsf via makerom.

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set. Use `make docker-3ds`, or source the devkitPro environment.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := daemoon
BUILD       := build
APP_TITLE   := DaeMoon
APP_DESCRIPTION := Save data sync
APP_AUTHOR  := DaeMoon
RSF         := $(TOPDIR)/app.rsf

# Core is compiled straight into the app rather than linked as a library: one
# compiler, one set of flags, and no stale archive to explain.
SOURCES  := source source/ui \
            ../../core/src ../../core/src/util \
            ../../vendor/miniz
INCLUDES := source ../../core/include ../../vendor

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -fdata-sections \
          $(ARCH) $(INCLUDE) -D__3DS__ -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

# 3ds-curl and 3ds-mbedtls rather than httpc:C, which ships old cipher suites and a
# stale root CA store. See CLAUDE.md in this directory.
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS := $(PORTLIBS) $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard $(CURDIR)/assets/icon.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(icons))
            export APP_ICON := $(CURDIR)/assets/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

.PHONY: all cia clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# What actually gets tested. A .3dsx has no permission to reach another title's
# save archive, so the CIA is not an afterthought for release day.
cia: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia \
	        $(TARGET).bnr $(TARGET).icn

else

DEPENDS := $(OFILES:.o=.d)

ifeq ($(strip $(NO_SMDH)),)
    all: $(OUTPUT).3dsx
else
    all: $(OUTPUT).elf
endif

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

cia: $(OUTPUT).cia

# makerom and bannertool are third party and are not in the devkitPro repositories.
# docker/3ds.Dockerfile installs them at pinned versions with checksums; a local
# toolchain needs them on PATH.
$(OUTPUT).cia: $(OUTPUT).elf $(OUTPUT).smdh $(TOPDIR)/assets/banner.png $(TOPDIR)/assets/audio.wav
	@bannertool makebanner -i $(TOPDIR)/assets/banner.png -a $(TOPDIR)/assets/audio.wav \
		-o $(OUTPUT).bnr >/dev/null
	@makerom -f cia -o $@ -elf $(OUTPUT).elf -rsf $(RSF) \
		-icon $(OUTPUT).smdh -banner $(OUTPUT).bnr -exefslogo -target t \
		-major 1 -minor 0 -micro 0
	@echo built ... $(notdir $@)

-include $(DEPENDS)

endif
