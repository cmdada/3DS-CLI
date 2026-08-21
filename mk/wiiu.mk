#---------------------------------------------------------------------------------
# Wii U (devkitPPC + wut). Run from the repo root: make wiiu
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

TARGET		:=	3ds-cli
BUILD		:=	build-wiiu
SOURCES		:=	source/core source/platform/cafe vendor/ctr-osk-rt/source
INCLUDES	:=	source/core source/platform source/platform/cafe \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

APP_NAME	:=	3DS-CLI
APP_SHORTNAME	:=	3DS-CLI
APP_AUTHOR	:=	cmdada
APP_ICON	:=	$(TOPDIR)/icon.png

#---------------------------------------------------------------------------------
# The interpreter dispatch flags are the 3DS Makefile's, and were measured
# there rather than assumed. They are generic GCC options that apply just as
# well to PowerPC, but the +3% they were worth on ARM11 has NOT been
# re-measured on Espresso, so treat them as the 3DS's numbers rather than
# this console's.
#---------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -O3 -ffunction-sections \
			-fno-gcse -fno-crossjumping \
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(MACHDEP)
LDFLAGS	=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-lz -lwut -lm

LIBDIRS	:=	$(PORTLIBS) $(WUT_ROOT)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export DISTDIR	:=	$(CURDIR)/dist/wiiu
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
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean

all: $(BUILD) $(DISTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/mk/wiiu.mk

$(BUILD) $(DISTDIR):
	@mkdir -p $@

clean:
	@echo clean ... wiiu
	@rm -fr $(BUILD) $(DISTDIR)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).wuhb

$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
