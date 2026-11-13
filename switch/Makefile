# Native Horizon OS diagnostic target; independent of Cordial's Linux build.
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET       := cordial-switched
BUILD        := build
SOURCES      := source
APP_TITLE    := Cordial Switched
APP_AUTHOR   := Cordial Switched contributors
APP_VERSION  := 0.19.0

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -O2 -g -Wall -Wextra -Werror -ffunction-sections $(ARCH) -D__SWITCH__ $(INCLUDE) \
               `sdl2-config --cflags` `$(PREFIX)pkg-config --cflags SDL2_ttf`
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS        := `sdl2-config --libs` `$(PREFIX)pkg-config --libs SDL2_ttf` -lnx -lm
LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES   := $(CFILES:.c=.o) $(SFILES:.s=.o)
export LD       := $(CC)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS := --icon=$(DEVKITPRO)/libnx/default_icon.jpg --nacp=$(OUTPUT).nacp

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@$(RM) -r $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all
all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(OFILES:.o=.d)

endif
