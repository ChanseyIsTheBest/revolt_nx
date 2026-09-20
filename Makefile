#---------------------------------------------------------------------------------
# revoltnx -- RVGL loader for Nintendo Switch
#
#   dkp-pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 \
#                switch-sdl2_image switch-zlib switch-libpng
#
# Ships no game code and no game assets. See tools/prepare_game.sh.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Source $$DEVKITPRO/switchvars.sh or install devkitPro.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := revoltnx
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

APP_TITLE   := RVGL
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0

ICON        := icon.jpg

# Path to your five Android .so files. Used only by the pre-build checks;
# nothing from it is compiled or linked in.
LIBDIR      ?=

#---------------------------------------------------------------------------------
# -mtp=soft is not optional. The loaded modules are built for bionic and read
# their stack canary from TPIDR_EL0+0x28. If the compiler is allowed to emit
# hardware TLS accesses for our own code, the two fight over the same register
# and you get a random __stack_chk_fail a long way from the real fault.
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES := -D__SWITCH__

#---------------------------------------------------------------------------------
# SDL2_image is optional.
#
# RVGL imports exactly four of its symbols -- IMG_Init, IMG_Quit, IMG_Load_RW
# and IMG_SavePNG_RW -- and its own data is almost entirely .bmp; SavePNG
# exists for the screenshot key. That is thin enough that requiring the package
# is a worse trade than covering it, so source/rv_image.c stands in when
# switch-sdl2_image is not installed, decoding BMP through SDL and PNG through
# libpng (which this port links anyway).
#
# Install it for the real thing:  dkp-pacman -S switch-sdl2_image
#---------------------------------------------------------------------------------
SDL2_IMAGE_HEADER := $(wildcard $(PORTLIBS)/include/SDL2/SDL_image.h)

ifeq ($(strip $(SDL2_IMAGE_HEADER)),)
  DEFINES    += -DRVNX_NO_SDL2_IMAGE=1
  LIBS_IMAGE :=
else
  LIBS_IMAGE := -lSDL2_image -ljpeg -lwebp
endif

CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 \
           -ffunction-sections -fdata-sections $(ARCH) $(DEFINES)

# On aarch64 an implicit declaration is not a style problem: the compiler
# assumes an int return and guesses the arguments, which corrupts registers
# rather than merely warning.
CFLAGS  += -Werror=implicit-function-declaration -Werror=implicit-int
CFLAGS  += -Werror=incompatible-pointer-types
CFLAGS  += -Wno-unused-but-set-variable

CFLAGS  += $(INCLUDE)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Link with $(CXX) even though this tree is pure C. switch-mesa's libEGL.a
# contains the nouveau shader compiler, which is C++; the stock devkitPro
# template picks $(CC) when there are no .cpp files and the link then fails
# with hundreds of undefined operator new references.
#
# GNU ld resolves left to right, so each library must precede the ones it
# depends on: libGLESv2 needs libglapi, libEGL needs libglapi and
# libdrm_nouveau. -lpng must precede -lz for the same reason.
#---------------------------------------------------------------------------------
# -lpng is needed either way: by SDL2_image when present, by rv_image.c when
# not. It must precede -lz, because GNU ld resolves left to right and would
# otherwise finish with zlib before learning libpng needed it.
LIBS    := $(LIBS_IMAGE) -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
           -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(PORTLIBS)/include \
                   -I$(PORTLIBS)/include/SDL2 \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(wildcard $(TOPDIR)/$(ICON)))
  export APP_ICON := $(TOPDIR)/$(ICON)
endif

# The title, author, version and icon only reach the console through these,
# and they must be exported: the .nacp is generated by the recursive make
# running inside build/, which does not inherit them otherwise.
export APP_TITLE APP_AUTHOR APP_VERSION

ifeq ($(strip $(NO_NACP)),)
  export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif
ifneq ($(strip $(APP_ICON)),)
  export NROFLAGS += --icon=$(APP_ICON)
endif

.PHONY: all clean check

all: check $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Advisory, never fatal.
#
#   check_includes  newlib and glibc disagree about which header owns what.
#                   memalign is in <malloc.h> here and <stdlib.h> on a Linux
#                   host, so a file that compiles on a desktop can fail here.
#   check_links     several files here are reused unmodified from other Switch
#                   ports and expect helpers that lived in that port's headers.
#                   Replacing one of those headers rather than copying it
#                   leaves a call that compiles until the link -- or, worse,
#                   an implicit declaration, which on aarch64 means a guessed
#                   signature and corrupted registers rather than a warning.
#   check_newlib    the table falls through to plain newlib names for anything
#                   the shims do not cover. A few of those are DECLARED by the
#                   toolchain's headers and implemented nowhere, so they
#                   compile and fail at the LINK, one symbol per build. This
#                   resolves every entry against the real archives and reports
#                   them all at once. Runs automatically; DEVKITPRO is set.
#   check_signatures  optional, pass REFDIR=<reference port>/source. Names
#                   matching is not enough: a reused .c calls these with the
#                   types ITS port declared, and on aarch64 a mismatch is an
#                   error, not a warning. Point it at the port the files here
#                   came from; a different port reports its own divergence.
#   verify_imports  an uncovered import is a load-time abort on hardware with
#                   no other symptom, because the modules are BIND_NOW.
check:
ifeq ($(strip $(SDL2_IMAGE_HEADER)),)
	@echo "SDL2_image not found -- using the built-in shim (source/rv_image.c)"
else
	@echo "SDL2_image found -- linking it"
endif
	-@python3 tools/check_includes.py source/
	-@python3 tools/check_links.py source/
	-@python3 tools/check_newlib.py source/imports.c source/
ifneq ($(strip $(REFDIR)),)
	-@python3 tools/check_signatures.py source/ $(REFDIR)
endif
ifneq ($(strip $(LIBDIR)),)
	-@python3 tools/verify_imports.py $(LIBDIR) source/imports.c
	-@python3 tools/check_load_sizes.py $(LIBDIR) source/config.h
else
	@echo "import check skipped (pass LIBDIR=/path/to/lib to enable)"
endif

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
