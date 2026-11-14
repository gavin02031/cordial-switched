# Standalone JNI smoke probe. Intentionally does not link to or execute Roblox.
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET       := morrow-jni-probe
BUILD        := build
SOURCES      := source
APP_TITLE    := Morrow JNI Probe
APP_AUTHOR   := Morrow contributors
APP_VERSION  := 0.42.0

ARCH         := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS     := -std=gnu++14 -O2 -g -Wall -Wextra -Werror -ffunction-sections $(ARCH) \
                -Wno-error=unused-parameter -Wno-error=ignored-qualifiers \
                -Wno-error=reorder -Wno-error=missing-field-initializers \
                -Wno-error=unused-function -D__SWITCH__ -DEnableJNIVMGC $(INCLUDE)
LDFLAGS      := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBJNIVM     ?= /tmp/morrow-jnivm-a64/libjnivm.a
LIBS         := $(LIBJNIVM) -lnx -lm
LIBDIRS      := $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CPPFILES        := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export OFILES   := $(CPPFILES:.cpp=.o)
export LD       := $(CXX)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -isystem $(CURDIR)/../../third_party/libjnivm/include -I$(CURDIR)/$(BUILD)
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
