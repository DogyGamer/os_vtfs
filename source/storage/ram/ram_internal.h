#ifndef VTFS_RAM_INTERNAL_H
#define VTFS_RAM_INTERNAL_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/uidgid.h>

#include "vtfs_storage.h"

// для рамфс, реализация файла
struct vtfs_ram_node {
    //инода
    u64 ino;
    //права и тип
    umode_t mode;
    //ссылки
    unsigned int nlink;

    //размер данных
    size_t size;
    //данные
    char *data;

    //имена внутри директории (только у директорий)
    struct list_head children;

    //обратная ссылка, для ram->nodes
    struct list_head all_link;

    //владелец группа позиция
    kuid_t uid;
    kgid_t gid;
    //как pos для чтения итерациями
    u64 next_cookie;
};

// вхождение в каталог
struct vtfs_ram_entry {
    char *name;
    size_t name_len;

    struct vtfs_ram_node *node;
    //обратная ссылка для node->children
    struct list_head link;

    //позиция записи при чтении каталога
    u64 cookie;
};

// состояние инстанса
struct vtfs_ram {
    struct mutex lock;
    struct list_head nodes;

    u64 next_ino;
};

//хелпер для поиска файл
struct vtfs_ram_node *ram_find_node(
    struct vtfs_ram *ram, u64 ino);

void ram_get_info(
    const struct vtfs_ram_node *node,
    struct vtfs_node_info *info);

/* Операции каталогов */
int ram_lookup(struct vtfs_storage *, u64,
               const char *, size_t, struct vtfs_node_info *);

int ram_list(struct vtfs_storage *, u64, u64 *,
             vtfs_list_callback, void *);

int ram_create(struct vtfs_storage *, u64,
               const char *, size_t, umode_t,
               struct vtfs_node_info *);

int ram_remove(struct vtfs_storage *, u64,
               const char *, size_t, bool,
               struct vtfs_node_info *);

int ram_link(struct vtfs_storage *, u64,
             const char *, size_t, u64,
             struct vtfs_node_info *);



ssize_t ram_read(struct vtfs_storage *, u64,
                 void *, size_t, loff_t);

ssize_t ram_write(struct vtfs_storage *, u64,
                  const void *, size_t, loff_t,
                  struct vtfs_node_info *);

int ram_truncate(struct vtfs_storage *, u64, loff_t);

#endif