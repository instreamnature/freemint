#
# Makefile for sv3tty.xdd, the FreeMiNT driver for the sv3tty virtual serial
# ports to PetaLinux on SuperVidel 3 (see ../PROTOCOL.md)
#
SHELL = /bin/sh
SUBDIRS =

srcdir = .
top_srcdir = ../../..
subdir = sv3tty

default: all

# Disable built-in rules
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

# 060: the driver; deb: the same with debug output to the kernel log
sv3ttytargets = 060 deb

XDDDEFS_deb = -DDEV_DEBUG

include $(top_srcdir)/CONFIGVARS
include $(top_srcdir)/RULES
include $(top_srcdir)/PHONY

all-here: all-targets

# default overwrites
INCLUDES = -I$(top_srcdir)
DEFINITIONS = -D__KERNEL_XDD__

# default definitions
compile_all_dirs = $(addprefix .compile_,$(sv3ttytargets))
GENFILES = $(compile_all_dirs) *.xdd

help:
	@echo '#'
	@echo '# targets:'
	@echo '# --------'
	@echo '# - all'
	@echo '# - $(sv3ttytargets)'
	@echo '#'
	@echo '# - clean'
	@echo '# - distclean'
	@echo '# - bakclean'
	@echo '# - strip'
	@echo '# - help'
	@echo '#'


ALL_TARGETS = $(addsuffix /sv3tty.xdd, $(addprefix .compile_,$(sv3ttytargets)))

STRIPFLAGS =
strip:
	$(STRIP) $(ALL_TARGETS)

all-targets: $(ALL_TARGETS)

#
# multi target stuff
#

LDEXTRA = -nostdlib -Wl,--entry,init_xdd -Wl,--fatal-warnings

define TARGET_TEMPLATE

LIBKERN_NAME_$(1) = kern$$(CPU_$(1))
LIBKERN_DEP_$(1) = $(top_srcdir)/libkern/lib$$(LIBKERN_NAME_$(1)).a
LIBKERN_$(1) = -L$(top_srcdir)/libkern -l$$(LIBKERN_NAME_$(1))
DEFINITIONS_$(1) = $(DEFINITIONS) $$(XDDDEFS_$(1))
LIBS_$(1) = $$(LIBKERN_$(1)) -lgcc
OBJS_$(1) = $(addsuffix .o, $(addprefix .compile_$(1)/,$(notdir $(basename $(COBJS)))))

$(1): $(addprefix .compile_,$(1))/sv3tty.xdd

$(addprefix .compile_,$(1))/sv3tty.xdd: $$(OBJS_$(1)) $$(LIBKERN_DEP_$(1))
	$(LD) -o $$@ $$(CFLAGS_$$(CPU_$(1))) $$(OBJS_$(1)) $$(LIBS_$(1))

endef

define LIBKERN_TEMPLATE
$(top_srcdir)/libkern/libkern$(1).a:
	$(MAKE) -C $(top_srcdir)/libkern libkern$(1).a
endef

$(foreach TARGET,$(sv3ttytargets),$(eval $(call TARGET_TEMPLATE,$(TARGET))))

$(foreach TARGET,$(sv3ttytargets),$(foreach OBJ,$(COBJS),$(eval $(call CC_TEMPLATE,$(TARGET),$(OBJ)))))

$(foreach TARGET,$(cputargets),$(eval $(call LIBKERN_TEMPLATE,$(TARGET))))

ifneq (clean,$(findstring clean,$(MAKECMDGOALS)))
DEPS_MAGIC := $(shell mkdir -p $(addsuffix /.deps,$(addprefix .compile_,$(sv3ttytargets))) > /dev/null 2>&1 || :)
endif
