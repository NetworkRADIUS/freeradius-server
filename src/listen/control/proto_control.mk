TARGETNAME	:= proto_control

ifneq "$(TARGETNAME)" ""
TARGET		:= $(TARGETNAME)$(L)
endif

SOURCES		:= proto_control.c

TGT_PREREQS	:= $(LIBFREERADIUS_SERVER) libfreeradius-radius$(L) libfreeradius-io$(L)
