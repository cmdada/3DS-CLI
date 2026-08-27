#---------------------------------------------------------------------------------
# Not devkitpro either, and unlike pspdev there's no build.mak to include
#---------------------------------------------------------------------------------
VITASDK		?=	/usr/local/vitasdk

ifeq ($(wildcard $(VITASDK)/bin/arm-vita-eabi-gcc),)
$(error "arm-vita-eabi-gcc not found under $(VITASDK). Install VitaSDK (https://vitasdk.org) and set VITASDK.")
endif

PREFIX		:=	$(VITASDK)/bin/arm-vita-eabi
CC		:=	$(PREFIX)-gcc
STRIP		:=	$(PREFIX)-strip

TARGET_NAME	:=	3ds-cli
BUILD		:=	build-vita
DISTDIR		:=	dist/vita
TARGET		:=	$(BUILD)/$(TARGET_NAME)

INCDIR		:=	source/core source/platform source/platform/vita \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

OBJS		:=	$(BUILD)/machine.o $(BUILD)/plat.o \
			$(BUILD)/ctrosk.o $(BUILD)/ctrosk_gfx.o

#---------------------------------------------------------------------------------
# VPK metadata. title id is nine characters, four letters then five digits, and
# has to be unique per app
#---------------------------------------------------------------------------------
TITLE_ID	:=	ADAC00001
APP_NAME	:=	3DS-CLI
VITA_ICON	:=	assets/vita-icon0.png
VITA_LIVEAREA	:=	assets/vita-livearea

#---------------------------------------------------------------------------------
# Interpreter dispatch flags come from the 3DS Makefile, see the note there for
# why -fno-gcse and -fno-crossjumping are on and why this is -O3 not -O2.
#
# -Wl,-q IS NOT OPTIONAL. vita-elf-create needs the relocations it keeps, and
# without it the link works fine and then the .velf step falls over
#---------------------------------------------------------------------------------
ARCH		:=	-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard

CFLAGS		:=	-g -Wall -O3 -ffunction-sections \
			-fno-gcse -fno-crossjumping \
			-MMD -MP $(ARCH) \
			$(foreach dir,$(INCDIR),-I$(dir))

LDFLAGS		:=	-Wl,-q $(ARCH)

# curl is only ever for fetching a missing Image - see source/core/download.h.
# vitasdk builds it against mbedTLS
#
# Same deal as psp, reorder it and watch out
LIBS		:=	-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lzstd -lpthread \
			-lSceAppMgr_stub -lSceCtrl_stub -lSceDisplay_stub \
			-lSceKernelModulemgr_stub -lSceLibKernel_stub \
			-lSceMotion_stub -lSceNet_stub -lSceNetCtl_stub \
			-lScePower_stub -lSceSysmem_stub -lSceSysmodule_stub \
			-lSceTouch_stub -lm

VPK		:=	$(DISTDIR)/$(TARGET_NAME).vpk

.PHONY: all clean

all: $(VPK)

#---------------------------------------------------------------------------------
# One rule per source directory, the OBJS list up there is flat with no VPATH
# behind it
#---------------------------------------------------------------------------------
$(BUILD)/%.o: source/core/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: source/platform/vita/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: vendor/ctr-osk-rt/source/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD) $(DISTDIR):
	@mkdir -p $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).velf: $(TARGET).elf
	$(VITASDK)/bin/vita-elf-create $< $@

# No -s here on purpose. I live an unsafe life (also filesystem access whatever)
$(BUILD)/eboot.bin: $(TARGET).velf
	$(VITASDK)/bin/vita-make-fself $< $@

$(BUILD)/param.sfo:| $(BUILD)
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLE_ID) "$(APP_NAME)" $@

$(VPK): $(BUILD)/eboot.bin $(BUILD)/param.sfo $(VITA_ICON) | $(DISTDIR)
	$(VITASDK)/bin/vita-pack-vpk \
		-s $(BUILD)/param.sfo \
		-b $(BUILD)/eboot.bin \
		-a $(VITA_ICON)=sce_sys/icon0.png \
		-a $(VITA_LIVEAREA)/bg.png=sce_sys/livearea/contents/bg.png \
		-a $(VITA_LIVEAREA)/startup.png=sce_sys/livearea/contents/startup.png \
		-a $(VITA_LIVEAREA)/template.xml=sce_sys/livearea/contents/template.xml \
		$@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(VPK)

-include $(OBJS:.o=.d)
