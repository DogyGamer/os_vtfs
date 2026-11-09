#ifndef VTFS_H
#define VTFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/printk.h>

#include "../vtfs_storage.h"

#define VTFS_MAGIC 0x67676767

#define LOG(fmt, ...) pr_info("[vtfs]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[vtfs]: " fmt, ##__VA_ARGS__)

// Состояние одного экземпляра файловой системы
struct vtfs_sb_info {
    struct vtfs_storage storage;
};

// получить хранилище, связанное с superblock
static inline struct vtfs_storage *
vtfs_storage_of(struct super_block *sb)
{
    struct vtfs_sb_info *info = sb->s_fs_info;
    return &info->storage;
}

// Монтирование
int vtfs_init_fs_context(struct fs_context *fc);

// хелпер для иноды
struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct vtfs_node_info *info
);

void vtfs_evict_inode(struct inode *inode);

// поиск в директории
struct dentry *vtfs_lookup(
    struct inode *parent,
    struct dentry *child,
    unsigned int flags
);

// таблицы операций
extern const struct inode_operations vtfs_dir_inode_ops;
extern const struct inode_operations vtfs_regular_inode_ops;

extern const struct file_operations vtfs_dir_ops;
extern const struct file_operations vtfs_regular_ops;

#endif