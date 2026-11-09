obj-m := vtfs.o

vtfs-y := source/module.o \
                    source/vfs/mount.o \
                    source/vfs/inode.o \
                    source/vfs/directory.o \
                    source/vfs/mutations.o \
                    source/vfs/file.o \
                    source/storage/ram/core.o \
                    source/storage/ram/directory.o \
                    source/storage/ram/data.o

ccflags-y += -Wall -g
ccflags-y += -I$(src)/source