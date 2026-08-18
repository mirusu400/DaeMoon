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

# The shipped app keeps everything on the SD card and needs no save archive. A
# test build asks for one so the self test has an archive it owns, which is the
# only archive it may destroy without asking a person first.
SAVEDATA_SIZE ?= 128K

# Stamped into the app so the console can say which build it is running. Without
# it, "did that install take?" is answered by squinting at behaviour.
#
# It costs reproducibility, which is a real thing to give up: two builds of the
# same tree no longer produce the same file. Only main.c includes the generated
# header, so a new stamp does not rebuild the world.
BUILD_STAMP ?= unknown

# Core is compiled straight into the app rather than linked as a library: one
# compiler, one set of flags, and no stale archive to explain.
SOURCES  := source source/ui ../common \
            ../../core/src ../../core/src/util \
            ../../vendor/miniz ../../vendor/quirc
INCLUDES := source ../common ../../core/include ../../vendor ../../vendor/quirc

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -fdata-sections \
          $(ARCH) $(INCLUDE) -D__3DS__ -std=gnu11
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

# 3ds-curl and 3ds-mbedtls rather than httpc:C, which ships old cipher suites and a
# stale root CA store. See CLAUDE.md in this directory.
LIBS    := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS := $(PORTLIBS) $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export SAVEDATA_SIZE
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(CURDIR)/../../tools/test \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
# The backend contract, checked on hardware rather than only on a desktop. One
# file, not the directory: the rest of tools/test has a main() and wants fixtures.
CFILES   += backend_conformance.c test_failure.c
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

.PHONY: all cia clean stamp FORCE

all: $(BUILD) stamp
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# What actually gets tested. A .3dsx has no permission to reach another title's
# save archive, so the CIA is not an afterthought for release day.
cia: $(BUILD) stamp
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

# Rewritten only when it changes, so main.c is not rebuilt for a stamp that is
# the same as last time.
stamp: $(BUILD)
	@printf '#define DAEMOON_BUILD_STAMP "%s"\n' '$(BUILD_STAMP)' > $(BUILD)/build_stamp.h.new
	@cmp -s $(BUILD)/build_stamp.h.new $(BUILD)/build_stamp.h \
		|| mv $(BUILD)/build_stamp.h.new $(BUILD)/build_stamp.h
	@rm -f $(BUILD)/build_stamp.h.new

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
# The stamp carries the current SAVEDATA_SIZE. Without it, switching between a
# shipped build and a test build left the previous CIA in place: make had no file
# to notice had changed, and the console got a build that looked right and was
# not. Same failure as the RSF not being a dependency, one variable further out.
$(BUILD)/savedata.stamp: FORCE
	@mkdir -p $(dir $@)
	@echo "$(SAVEDATA_SIZE)" | cmp -s - $@ || echo "$(SAVEDATA_SIZE)" > $@

FORCE:

$(OUTPUT).cia: $(OUTPUT).elf $(OUTPUT).smdh $(RSF) $(BUILD)/savedata.stamp \
              $(TOPDIR)/assets/banner.png $(TOPDIR)/assets/audio.wav
	@bannertool makebanner -i $(TOPDIR)/assets/banner.png -a $(TOPDIR)/assets/audio.wav \
		-o $(OUTPUT).bnr >/dev/null
	@makerom -f cia -o $@ -elf $(OUTPUT).elf -rsf $(RSF) \
		-DSAVEDATA_SIZE=$(SAVEDATA_SIZE) \
		-icon $(OUTPUT).smdh -banner $(OUTPUT).bnr -exefslogo -target t \
		-major 1 -minor 0 -micro 0
	@echo built ... $(notdir $@)

-include $(DEPENDS)

endif
