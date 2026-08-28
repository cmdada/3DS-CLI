#---------------------------------------------------------------------------------
# Sony PlayStation 3 (ps3toolchain + PSL1GHT). Run from the repo root: make ps3
#
# Not devkitPro. Install ps3toolchain (https://github.com/ps3dev/ps3toolchain),
# which builds the compiler and PSL1GHT from source, then set PSL1GHT - or
# install the AUR's ps3toolchain and ps3-psl1ght, which put it at
# /usr/local/ps3dev.
#---------------------------------------------------------------------------------
.SUFFIXES:

# ps3toolchain installs the rules files beside the compiler; the AUR package
# puts them in a psl1ght/ subdirectory. Try both before giving up.
ifeq ($(strip $(PSL1GHT)),)
  ifneq ($(wildcard /usr/local/ps3dev/psl1ght/ppu_rules),)
    export PSL1GHT := /usr/local/ps3dev/psl1ght
  else ifneq ($(wildcard /usr/local/ps3dev/ppu_rules),)
    export PSL1GHT := /usr/local/ps3dev
  else
    $(error "Please set PSL1GHT in your environment. export PSL1GHT=<path to>psl1ght")
  endif
endif

TOPDIR ?= $(CURDIR)

include $(PSL1GHT)/ppu_rules

TARGET		:=	3ds-cli
BUILD		:=	build-ps3
SOURCES		:=	source/core source/platform/ps3 vendor/ctr-osk-rt/source
INCLUDES	:=	source/core source/platform source/platform/ps3 \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

#---------------------------------------------------------------------------------
# .pkg metadata. APPID is nine characters, four letters then five digits, and
# has to be unique per app - same rule as the Vita's title id.
#---------------------------------------------------------------------------------
TITLE		:=	3DS-CLI
APPID		:=	ADAC00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

# ppu_rules declares ICON0 with ?=, so this only has to name a file when the
# repo actually has one; otherwise the toolchain's placeholder is used. A PS3
# ICON0.PNG is 320x176, which is not a size anything else here ships.
PS3_ICON	:=	$(CURDIR)/assets/ps3-icon0.png
ifneq ($(wildcard $(PS3_ICON)),)
ICON0		:=	$(PS3_ICON)
endif

#---------------------------------------------------------------------------------
# The interpreter dispatch flags come from the 3DS Makefile, where they were
# measured. Generic GCC options that apply to PowerPC too, but the +3% has NOT
# been re-measured here. -mcpu=cell is what the PSL1GHT samples build with;
# MACHDEP comes from ppu_rules.
#---------------------------------------------------------------------------------
CFLAGS		=	-g -Wall -O3 -mcpu=cell $(MACHDEP) \
			-fno-gcse -fno-crossjumping \
			$(INCLUDE)
CXXFLAGS	=	$(CFLAGS) -fno-rtti -fno-exceptions
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# -lz is for the rootfs the Image carries; the rest is PSL1GHT's own split:
# rsx and gcm_sys for the display, io for the pad and the USB keyboard,
# sysutil for the XMB exit callback, net for the socket stack. -lsysmodule
# goes after -lnet because netInitialize is what pulls sysModuleLoad in.
#---------------------------------------------------------------------------------
LIBS		:=	-lrsx -lgcm_sys -lio -lsysutil -lnet -lnetctl -lsysmodule \
			-lz -lrt -llv2 -lm

# zlib comes from ps3libraries, which installs into the ppu portlibs tree.
LIBDIRS		:=	$(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export DISTDIR	:=	$(CURDIR)/dist/ps3
export OUTPUT	:=	$(DISTDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			$(LIBPSL1GHT_INC) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
			$(LIBPSL1GHT_LIB)

.PHONY: all clean pkg

# The .self is what ps3load and a Homebrew-Channel-style loader want; the .pkg
# is what installs from the XMB. Both, because which one a user needs depends
# entirely on how their console is unlocked.
all: $(BUILD) $(DISTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/mk/ps3.mk

pkg: $(BUILD) $(DISTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/mk/ps3.mk pkg

$(BUILD) $(DISTDIR):
	@mkdir -p $@

clean:
	@echo clean ... ps3
	@rm -fr $(BUILD) $(DISTDIR)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).self
pkg	:	$(OUTPUT).pkg

$(OUTPUT).self	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
