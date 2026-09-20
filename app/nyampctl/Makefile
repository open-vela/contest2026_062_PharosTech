NYAMPCTL_SOURCE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
NYAMP_PROTOCOL_DIR := $(abspath $(NYAMPCTL_SOURCE_DIR)/../../tools/amp/protocol)

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_NYAMPCTL_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_NYAMPCTL_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_NYAMPCTL_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_NYAMPCTL)

MAINSRC = nyampctl_main.c
CSRCS   = nyampctl_llm.c nyampctl_blob.c nyampctl_shmem.c nyamp_protocol.c

VPATH += $(NYAMP_PROTOCOL_DIR)
CFLAGS += ${INCDIR_PREFIX}$(NYAMPCTL_SOURCE_DIR)
CFLAGS += ${INCDIR_PREFIX}$(NYAMP_PROTOCOL_DIR)

# With the Nyabula Core compute service in the image the endpoint belongs to
# that service, and this diagnostic talks through it (see nyampctl_io.h).
ifeq ($(CONFIG_NYABULA_CORE_COMPUTE),y)
CFLAGS += ${INCDIR_PREFIX}$(abspath $(NYAMPCTL_SOURCE_DIR)/../nyabula_core)
endif

include $(APPDIR)/Application.mk
