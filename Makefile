#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

TOPDIR ?= $(CURDIR)

ifeq ($(filter psp vita ps3,$(MAKECMDGOALS)),)
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/3ds_rules
endif

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET		:=	3ds-cli
APP_TITLE		:=	3DS-CLI
APP_DESCRIPTION	:=	Linux Environment for 3DS
APP_AUTHOR		:=	cmdada
BUILD		:=	build
# The touch keyboard is built from source alongside the app rather than
# linked as a prebuilt .a, so there's still exactly one `make` to run and the
# submodule needs no separate build step.
SOURCES		:=	source/core source/platform/ctr vendor/ctr-osk-rt/source
DATA		:=	data
INCLUDES	:=	source/core source/platform source/platform/ctr vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)

ICON		:=	icon.png
#ROMFS		:=	romfs
#GFXBUILD	:=	$(ROMFS)/gfx

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O3 -fomit-frame-pointer -mword-relocations \
			-ffunction-sections \
			-fno-gcse -fno-crossjumping \
			$(ARCH)

# On the two flags above: both are the standard advice for a big interpreter
# dispatch loop, and both were measured on this emulator rather than taken on
# faith. -fno-gcse stops common-subexpression elimination from hoisting values
# across the dispatch switch and spilling them; -fno-crossjumping stops the
# tails of the instruction cases being merged back into one shared block,
# which costs an extra branch on every instruction. Together they were worth
# about +3% guest throughput.
#
# -O3 is likewise measured, not assumed: -O2 came out ~13% *slower*, and even
# produced a larger emulation loop (15008 vs 11796 bytes of ARM).

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl (and the mbedtls it is built on) is only for fetching a missing
# Image - see source/core/download.h. Link order matters: curl pulls
# mbedtls, which pulls mbedx509, which pulls mbedcrypto.
LIBS	:= -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

export BANNERTOOL  ?= bannertool
export MAKEROM     ?= makerom

export BANNER_IMAGE := $(TOPDIR)/banner.png
export BANNER_AUDIO := $(TOPDIR)/assets/silence.wav
export BANNER       := $(TOPDIR)/$(BUILD)/banner.bnr
export APP_RSF      := $(TOPDIR)/assets/app.rsf

.PHONY: all clean cia dtb check wiiu switch wii gamecube psp vita ps3

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@$(MAKE) --no-print-directory check

#---------------------------------------------------------------------------------
# A .3dsx can link cleanly, pass 3dsxtool, and still be rejected outright by
# every loader over one unencodable relocation — with no build-time warning.
# That cost a long bisect once; this makes it a build failure instead.
# Skipped rather than failed when python3 isn't around, so the toolchain
# requirement stays exactly what it was.
check: $(OUTPUT).3dsx
	@if command -v python3 >/dev/null 2>&1; then \
		python3 $(CURDIR)/tools/check3dsx.py $(OUTPUT).3dsx; \
	else \
		echo "check: python3 not found, skipping 3dsx validation"; \
	fi

#---------------------------------------------------------------------------------
# Regenerate source/default64mbdtc.h from source/3ds-cli.dts (requires dtc +
# python3). Every console's build consumes the header, so the generator lives
# in tools/ rather than inline here - CI runs the same script.
dtb:
	@python3 $(CURDIR)/tools/gen_dtb.py

cia: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
# The other consoles. Each has its own makefile under mk/ because devkitPro
# ships a separate rules file per platform and they cannot be included
# together; source/core is shared, source/platform/<console> is not. The PSP
# and the Vita are not devkitPro targets at all - see the note at the top of
# mk/psp.mk and mk/vita.mk.
wiiu switch wii gamecube psp vita ps3:
	@$(MAKE) --no-print-directory -f $(CURDIR)/mk/$@.mk

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD) $(TARGET).cia $(BANNER)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(BANNER): $(BANNER_IMAGE) $(BANNER_AUDIO)
	@echo "building banner ..."
	@$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $@

$(OUTPUT).cia: $(OUTPUT).elf $(_3DSXDEPS) $(BANNER) $(APP_RSF)
	@echo "building cia ..."
	@$(MAKEROM) -f cia -o $@ -target t -elf $(OUTPUT).elf -rsf $(APP_RSF) -icon $(OUTPUT).smdh -banner $(BANNER) -exefslogo

cia: $(OUTPUT).cia

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
