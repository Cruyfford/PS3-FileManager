#---------------------------------------------------------------------------------
# File Manager by CRUYFFORD — PSL1GHT build
#---------------------------------------------------------------------------------
.SUFFIXES:
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

TITLE		:=	File Manager
APPID		:=	CRPK00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
ICON0		:=	pkgfiles/ICON0.PNG
SFOXML		:=	sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET		:=	FileManager
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
PKGFILES	:=	$(CURDIR)/pkgfiles

LIBS		:=	$(PROJECT_ROOT)/third_party/fs/lib/libntfs_ext.a $(PROJECT_ROOT)/third_party/fs/lib/libfatfs.a \
				-lzip -lunrar -lun7zip -lmpg123 -lvorbisfile -lvorbis -logg \
				-lya2d -lfont3d -ltiny3d -lfreetype -lz -lpolarssl -lsimdmath -lgcm_sys -lio -lsysutil -lrt -llv2 \
				-lpngdec -ljpgdec -lsysmodule -lsysfs -laudio -lnetctl -lnet -lstdc++ -lm

CFLAGS		=	-O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
PNGFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))

export LD	:=	$(CC)
export OFILES	:=	$(addsuffix .o,$(PNGFILES)) $(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					-I$(CURDIR)/third_party/fs/include \
					$(LIBPSL1GHT_INC) \
					-I$(PORTLIBS)/include/freetype2 \
					-I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include

export LIBPATHS	:=	$(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self $(OUTPUT)*.pkg

pkg: $(BUILD) $(OUTPUT).pkg

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

%.png.o	:	%.png
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
