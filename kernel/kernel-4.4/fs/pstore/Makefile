#
# Makefile for the linux pstorefs routines.
#

subdir-ccflags-y += -Werror
obj-$(CONFIG_PSTORE) += pstore.o

pstore-objs += inode.o platform.o
pstore-$(CONFIG_PSTORE_FTRACE)	+= ftrace.o

pstore-$(CONFIG_PSTORE_PMSG)	+= pmsg.o
obj-$(CONFIG_PSTORE_RTRACE)	+= rtrace.o

ramoops-objs += ram.o ram_core.o
obj-$(CONFIG_PSTORE_RAM)	+= ramoops.o
