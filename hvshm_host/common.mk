
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

NAME=hvshm_host



#This has to be included last
include $(MKFILES_ROOT)/qtargets.mk
