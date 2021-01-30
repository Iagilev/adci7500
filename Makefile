# 
# Makefile for the ADDI-DATA serial device drivers
# 

KDIR := /lib/modules/$(shell uname -r)/build

obj-m	+= addi_serial.o

build:
	make -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD) modules

clean:
	make -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD) clean
