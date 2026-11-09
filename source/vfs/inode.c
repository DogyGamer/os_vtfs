
#include "vtfs.h"

struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct vtfs_node_info *info)
{
    struct inode *inode;

    inode = iget_locked(sb, (unsigned long)info->ino);
    if (!inode)
        return NULL;

    // если есть в кеше
    if (!(inode_state_read_once(inode) & I_NEW))
        return inode;

    // если новая то надо иниц
    inode->i_ino = info->ino;
    inode->i_mode = info->mode;
    inode->i_uid = info->uid;
    inode->i_gid = info->gid;

    set_nlink(inode, info->nlink);
    i_size_write(inode, info->size);
    simple_inode_init_ts(inode);

    if (S_ISDIR(info->mode)) {
        inode->i_op = &vtfs_dir_inode_ops;
        inode->i_fop = &vtfs_dir_ops;
    } else if (S_ISREG(info->mode)) {
        inode->i_op = &vtfs_regular_inode_ops;
        inode->i_fop = &vtfs_regular_ops;
    } else if (S_ISLNK(info->mode)) {
        struct vtfs_storage *storage = vtfs_storage_of(sb);
        char *target;
        ssize_t n;

        if (!info->size || info->size >= PAGE_SIZE)
            goto bad_inode;

        target = kmalloc(info->size + 1, GFP_KERNEL);
        if (!target)
            goto bad_inode;

        n = storage->ops->read(
            storage, info->ino, target, info->size, 0
        );

        if (n != (ssize_t)info->size) {
            kfree(target);
            goto bad_inode;
        }

        target[info->size] = '\0';

        inode->i_op = &simple_symlink_inode_operations;
        inode_set_cached_link(inode, target, info->size);
    }

    unlock_new_inode(inode);
    return inode;

    bad_inode:
        discard_new_inode(inode);
    return NULL;
}

//вызывается когда vfs считает что на иноду не осталось ссылок (открытых фд )
void vtfs_evict_inode(struct inode *inode)
{
    struct vtfs_storage *storage;

    storage = vtfs_storage_of(inode->i_sb);

    truncate_inode_pages_final(&inode->i_data);

    if (S_ISLNK(inode->i_mode))
        kfree(inode->i_link);

    clear_inode(inode);

    storage->ops->forget(storage, inode->i_ino);
}