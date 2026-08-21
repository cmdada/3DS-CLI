#---------------------------------------------------------------------------------
# Shared by mk/wii.mk and mk/gamecube.mk. OGC_CONSOLE picks which.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

TOPDIR ?= $(CURDIR)

ifeq ($(OGC_CONSOLE),wii)
include $(DEVKITPPC)/wii_rules
BUILD		:=	build-wii
MACHDEP_EXTRA	:=
LIBS		:=	-lz -lfat -lwiiuse -lbte -logc -lm
else
include $(DEVKITPPC)/gamecube_rules
BUILD		:=	build-gamecube
MACHDEP_EXTRA	:=
LIBS		:=	-lz -lfat -logc -lm
endif

TARGET		:=	3ds-cli
SOURCES		:=	source/core source/platform/ogc vendor/ctr-osk-rt/source
INCLUDES	:=	source/core source/platform source/platform/ogc \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

#---------------------------------------------------------------------------------
# The interpreter dispatch flags come from the 3DS Makefile, where they were
# measured. Generic GCC options that apply to PowerPC too, but the +3% has NOT
# been re-measured here.
#---------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -O3 -ffunction-sections \
			-fno-gcse -fno-crossjumping \
			$(MACHDEP)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(MACHDEP)
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $*.map)

LIBDIRS	:=	$(PORTLIBS) $(LIBOGC)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

# Both consoles produce a .dol, so they cannot share the repo root - the name
# is the same on purpose (see the branding note in the README) and only the
# directory distinguishes them. This also matches the layout the Homebrew
# Channel wants: apps/<name>/boot.dol.
export DISTDIR	:=	$(CURDIR)/dist/$(OGC_CONSOLE)
export OUTPUT	:=	$(DISTDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(LIBOGC_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)

.PHONY: all clean

all: $(BUILD) $(DISTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/mk/ogc-common.mk OGC_CONSOLE=$(OGC_CONSOLE)

$(BUILD) $(DISTDIR):
	@mkdir -p $@

clean:
	@echo clean ... $(OGC_CONSOLE)
	@rm -fr $(BUILD) $(DISTDIR)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).dol

$(OUTPUT).dol	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
