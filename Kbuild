obj-m := vtfs.o

vtfs-y := source/module.o \
                    source/vfs/mount.o \
                    source/vfs/inode.o \
                    source/vfs/directory.o \
                    source/vfs/mutations.o \
                    source/vfs/file.o \
                    source/storage/ram/core.o \
                    source/storage/ram/directory.o \
                    source/storage/ram/data.o \
                    source/storage/http/http.o \
                    source/storage/http/protocol.o \
                    source/storage/http/core.o \
                    source/storage/http/directory.o \
                    source/storage/http/data.o

ccflags-y += -Wall -g
ccflags-y += -I$(src)/source