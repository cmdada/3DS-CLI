#---------------------------------------------------------------------------------
# Nintendo Switch (devkitA64 + libnx). Run from the repo root: make switch
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	3ds-cli
BUILD		:=	build-switch
SOURCES		:=	source/core source/platform/nx vendor/ctr-osk-rt/source
INCLUDES	:=	source/core source/platform source/platform/nx \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

APP_TITLE	:=	3DS-CLI
APP_AUTHOR	:=	cmdada
APP_VERSION	:=	1.0.0
ICON		:=	$(TOPDIR)/icon.png

ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

#---------------------------------------------------------------------------------
# The interpreter dispatch flags come from the 3DS Makefile, where they were
# measured. They are generic GCC options, but the +3% has NOT been re-measured
# on AArch64, so treat them as the 3DS's numbers rather than this console's.
#---------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -O3 -ffunction-sections \
			-fno-gcse -fno-crossjumping \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl is only for fetching a missing Image - see source/core/download.h.
# No mbedtls here, unlike the other two: switch-curl is built against
# libnx's own SSL service, so libnx is the TLS backend.
LIBS	:=	-lcurl -lz -lnx -lm
LIBDIRS	:=	$(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export DISTDIR	:=	$(CURDIR)/dist/switch
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

export APP_ICON := $(ICON)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(DISTDIR)/$(TARGET).nacp

.PHONY: all clean

all: $(BUILD) $(DISTDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/mk/switch.mk

$(BUILD) $(DISTDIR):
	@mkdir -p $@

clean:
	@echo clean ... switch
	@rm -fr $(BUILD) $(DISTDIR)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
