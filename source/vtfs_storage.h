#ifndef VTFS_STORAGE_H
#define VTFS_STORAGE_H

#include <linux/types.h>

//инода корня
#define VTFS_ROOT_INO 1

//метаданные обьнфайла
struct vtfs_node_info {
    //айдт
    u64 ino;
    //права и тип
    umode_t mode;
    //размер
    u64 size;
    //клво ссылок
    unsigned int nlink;

    //овнер игруппа
    kuid_t uid;
    kgid_t gid;
};

struct vtfs_storage;

// Функция, вызываемая для каждого элемента каталога
typedef bool (*vtfs_list_callback)(
    void *arg,
    const char *name,
    size_t name_len,
    const struct vtfs_node_info *info
);

//операции хранилища
struct vtfs_storage_ops {
    int (*lookup)(
        struct vtfs_storage *storage,
        u64 parent_ino,
        const char *name,
        size_t name_len,
        struct vtfs_node_info *result
    );

    int (*list)(
        struct vtfs_storage *storage,
        u64 dir_ino,
        u64 *cursor,
        vtfs_list_callback callback,
        void *arg
    );

    int (*create)(
        struct vtfs_storage *storage,
        u64 parent_ino,
        const char *name,
        size_t name_len,
        umode_t mode,
        struct vtfs_node_info *result
    );

    void (*destroy)(struct vtfs_storage *storage);
        int (*get)(
        struct vtfs_storage *storage,
        u64 ino,
        struct vtfs_node_info *result
    );

    int (*remove)(
        struct vtfs_storage *storage,
        u64 parent_ino,
        const char *name,
        size_t name_len,
        bool directory,
        struct vtfs_node_info *result
    );

    int (*link)(
        struct vtfs_storage *storage,
        u64 parent_ino,
        const char *name,
        size_t name_len,
        u64 target_ino,
        struct vtfs_node_info *result
    );

    ssize_t (*read)(
        struct vtfs_storage *storage,
        u64 ino,
        void *buffer,
        size_t count,
        loff_t offset
    );

    ssize_t (*write)(
        struct vtfs_storage *storage,
        u64 ino,
        const void *buffer,
        size_t count,
        loff_t offset,
        struct vtfs_node_info *updated
    );

    int (*truncate)(
        struct vtfs_storage *storage,
        u64 ino,
        loff_t size
    );

    //лсовбождение удаленны
    void (*forget)(
        struct vtfs_storage *storage,
        u64 ino
    );
    //обнволение прав и владельца
    int (*update_metadata)(
        struct vtfs_storage *storage,
        u64 ino,
        const struct vtfs_node_info *info
    );
};

struct vtfs_storage {
    const struct vtfs_storage_ops *ops;
    void *private;
};

int vtfs_ram_init(struct vtfs_storage *storage);

#endif