#---------------------------------------------------------------------------------
# Not devkitpro but I wrote this similarly, if you have trouble building it just 
# message me on discord or over email
#---------------------------------------------------------------------------------
PSPSDK := $(shell psp-config --pspsdk-path 2>/dev/null)

ifeq ($(strip $(PSPSDK)),)
$(error "psp-config not found. Install pspdev (https://github.com/pspdev/pspdev) and put its bin/ on PATH.")
endif

TARGET_NAME	:=	3ds-cli
BUILD		:=	build-psp
DISTDIR		:=	dist/psp
TARGET		:=	$(BUILD)/$(TARGET_NAME)

SOURCES		:=	source/core source/platform/psp vendor/ctr-osk-rt/source
INCDIR		:=	source/core source/platform source/platform/psp \
			vendor/mini-rv32ima-mmu \
			vendor/ctr-osk-rt/include vendor/ctr-osk-rt/source

OBJS		:=	$(BUILD)/machine.o $(BUILD)/plat.o \
			$(BUILD)/ctrosk.o $(BUILD)/ctrosk_gfx.o

# build.mak has no directory targets and its EBOOT rule writes straight into
# $(DISTDIR), so both have to exist before any rule runs rather than as a
# prerequisite of one.
$(shell mkdir -p $(BUILD) $(DISTDIR))

#---------------------------------------------------------------------------------
# The interpreter dispatch flags come from the 3DS Makefile
#---------------------------------------------------------------------------------
CFLAGS		:=	-g -Wall -O3 -G0 -ffunction-sections \
			-fno-gcse -fno-crossjumping \
			-MMD -MP
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS		:=	$(CFLAGS)

# curl is only for fetching a missing Image - see source/core/download.h.
# pspdev's build of it is against mbedTLS, which has to be named explicitly
# and in dependency order.
#
# Ordering it differently might break things so like watch out
LIBS		:=	-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lm \
			-lpspnet_resolver -lpspnet_inet \
			-lpspwlan -lpsppower -lpsprtc -lpsputility

#---------------------------------------------------------------------------------
# EBOOT.PBP metadata. MEMSIZE in PARAM.SFO is what asks for the expanded user
# partition on a PSP-2000 or later; build.mak sets it to 2 by default for
# firmware above 3.90, which is every firmware this can run on (i only have a 6.61
# PSP-2000)
#---------------------------------------------------------------------------------
PSP_EBOOT_TITLE	:=	3DS-CLI
PSP_EBOOT_SFO	:=	$(BUILD)/PARAM.SFO
PSP_EBOOT	:=	$(DISTDIR)/EBOOT.PBP
PSP_EBOOT_ICON	:=	assets/psp-icon0.png
PSP_FW_VERSION	:=	660

EXTRA_TARGETS	:=	$(PSP_EBOOT)
EXTRA_CLEAN	:=	$(OBJS:.o=.d) $(TARGET).elf

#---------------------------------------------------------------------------------
# One rule per source directory, because build.mak's flat OBJS list has no
# VPATH behind it. CFLAGS is expanded when the recipe runs, so these still
# pick up the -I flags build.mak prepends below.
#---------------------------------------------------------------------------------
$(BUILD)/%.o: source/core/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: source/platform/psp/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: vendor/ctr-osk-rt/source/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# build.mak supplies all, clean and the EBOOT.PBP rule.
include $(PSPSDK)/lib/build.mak

-include $(OBJS:.o=.d)
