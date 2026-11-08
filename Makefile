KDIR := /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(CURDIR)/build

.PHONY: all clean load unload

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) modules

clean:
	rm -rf -- "$(BUILD_DIR)"

load: all
	sudo insmod $(BUILD_DIR)/vtfs.ko

unload:
	-sudo rmmod vtfs

compdb: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD_DIR) compile_commands.json

umount:
	-sudo umount /mnt/vtfs

mount: umount unload load
	sudo mount -t vtfs none /mnt/vtfs