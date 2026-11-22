#---------------------------------------------------------------------------------
# Nova Browser by CRUYFFORD — PSL1GHT build (shares gfx/arabic/osk/vfs with File Manager)
#---------------------------------------------------------------------------------
.SUFFIXES:
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT    := $(abspath $(PROJECT_ROOT)/..)
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

TITLE		:=	Nova Browser
APPID		:=	CRBR00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
ICON0		:=	pkgfiles/ICON0.PNG
SFOXML		:=	sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET		:=	NovaBrowser
BUILD		:=	build
SOURCES		:=	source
SHARED		:=	gfx.c arabic.c osk.c vfs.c
INCLUDES	:=	include
PKGFILES	:=	$(CURDIR)/pkgfiles

LIBS		:=	$(REPO_ROOT)/third_party/fs/lib/libntfs_ext.a $(REPO_ROOT)/third_party/fs/lib/libfatfs.a \
				-lxml2 -lcurl -lpolarssl -lz \
				-lya2d -lfont3d -ltiny3d -lfreetype -lsimdmath -lgcm_sys -lio -lsysutil -lrt -llv2 \
				-lpngdec -ljpgdec -lsysmodule -lsysfs -lnetctl -lnet -lstdc++ -lm

CFLAGS		=	-O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(REPO_ROOT)/source
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) $(SHARED)

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					-I$(REPO_ROOT)/include \
					-I$(REPO_ROOT)/third_party/fs/include \
					$(LIBPSL1GHT_INC) \
					-I$(PORTLIBS)/include/freetype2 \
					-I$(PORTLIBS)/include/libxml2 \
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

-include $(DEPENDS)

endif
