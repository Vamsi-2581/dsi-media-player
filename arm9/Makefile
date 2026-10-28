.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

BUILD    := build
SOURCES  := source source/audio source/ui source/fs
INCLUDES := include ../libs ../libs/helix ../libs/helix/pub ../libs/helix/real
DATA     :=

ARCH := -march=armv5te -mtune=arm946e-s -mthumb-interwork

CFLAGS   := -g -Wall -O3 -ffunction-sections -fdata-sections \
            -fomit-frame-pointer -ffast-math \
            $(ARCH) $(INCLUDE) -DARM9
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map \
            -Wl,--defsym,__sp_usr=0x02F00000

LIBS := -lfat -lmm9 -lnds9

LIBDIRS := $(LIBNDS) $(DEVKITPRO)/portlibs/nds

ifneq ($(BUILD),$(notdir $(CURDIR)))

export ARM9ELF := $(CURDIR)/$(TARGET).elf
export DEPSDIR := $(CURDIR)/$(BUILD)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif

export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o) $(addsuffix .o,$(BINFILES))

export INCLUDE  := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                   $(foreach dir,$(SOURCES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

$(ARM9ELF) : $(OFILES)

-include $(DEPENDS)

endif
