#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <linux/fs.h>
#include <linux/fs_context.h>


#include <linux/slab.h>
#include <linux/err.h>
#include <linux/dcache.h>

#include <linux/uaccess.h>

#include "vtfs.h"

static int vtfs_symlink(
    struct mnt_idmap *idmap,
    struct inode *parent,
    struct dentry *child,
    const char *target)
{
    struct vtfs_storage *storage = vtfs_storage_of(parent->i_sb);
    struct vtfs_node_info info, updated, removed;
    struct inode *inode;
    size_t len = strlen(target);
    ssize_t written;
    int ret;

    LOG("vtfs_symlink");
    if (!len)
        return -EINVAL;

    if (len >= PAGE_SIZE)
        return -ENAMETOOLONG;

    ret = storage->ops->create(
        storage, parent->i_ino,
        child->d_name.name, child->d_name.len,
        S_IFLNK | 0777, &info
    );
    if (ret)
        return ret;

    written = storage->ops->write(
        storage, info.ino, target, len, 0, &updated
    );

    if (written != (ssize_t)len) {
        ret = written < 0 ? (int)written : -EIO;
        goto rollback;
    }

    inode = vtfs_get_inode(parent->i_sb, &updated);
    if (!inode) {
        ret = -ENOMEM;
        goto rollback;
    }

    d_instantiate(child, inode);
    return 0;

    rollback:
        storage->ops->remove(
            storage, parent->i_ino,
            child->d_name.name, child->d_name.len,
            false, &removed
        );

    storage->ops->forget(storage, info.ino);
    return ret;
}

static int vtfs_create(
    struct mnt_idmap *idmap,
    struct inode *parent,
    struct dentry *child,
    umode_t mode,
    bool excl)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info, discarded;
    struct inode *inode;
    int ret;

    LOG("vtfs_create");
    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->create(
        storage,
        parent->i_ino,
        child->d_name.name,
        child->d_name.len,
        S_IFREG | mode,
        &info
    );
    if (ret)
        return ret;

    inode = vtfs_get_inode(parent->i_sb, &info);
    if (!inode) {
        storage->ops->remove(
            storage, parent->i_ino,
            child->d_name.name, child->d_name.len,
            false, &discarded
        );
        return -ENOMEM;
    }

    d_instantiate(child, inode);
    return 0;
}

static struct dentry *vtfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *parent,
    struct dentry *child,
    umode_t mode)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info, discarded;
    struct inode *inode;
    int ret;

    LOG("vtfs_mkdir");
    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->create(
        storage,
        parent->i_ino,
        child->d_name.name,
        child->d_name.len,
        S_IFDIR | mode,
        &info
    );
    if (ret)
        return ERR_PTR(ret);

    inode = vtfs_get_inode(parent->i_sb, &info);
    if (!inode) {
        storage->ops->remove(
            storage, parent->i_ino,
            child->d_name.name, child->d_name.len,
            true, &discarded
        );
        return ERR_PTR(-ENOMEM);
    }

    inc_nlink(parent);
    d_instantiate(child, inode);

    return NULL;
}

static int vtfs_unlink(
    struct inode *parent,
    struct dentry *child)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info;
    int ret;

    LOG("vtfs_unlink");

    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->remove(
        storage,
        parent->i_ino,
        child->d_name.name,
        child->d_name.len,
        false,
        &info
    );

    if (!ret)
        set_nlink(d_inode(child), info.nlink);

    return ret;
}

static int vtfs_rmdir(
    struct inode *parent,
    struct dentry *child)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info;
    int ret;

    LOG("vtfs_rmdir");
    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->remove(
        storage,
        parent->i_ino,
        child->d_name.name,
        child->d_name.len,
        true,
        &info
    );

    if (!ret) {
        clear_nlink(d_inode(child));
        drop_nlink(parent);
    }

    return ret;
}

static int vtfs_link(
    struct dentry *old_dentry,
    struct inode *parent,
    struct dentry *new_dentry)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info;
    struct inode *inode = d_inode(old_dentry);
    int ret;

    LOG("vtfs_link");

    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->link(
        storage,
        parent->i_ino,
        new_dentry->d_name.name,
        new_dentry->d_name.len,
        inode->i_ino,
        &info
    );

    if (ret)
        return ret;

    set_nlink(inode, info.nlink);

    ihold(inode);
    d_instantiate(new_dentry, inode);

    return 0;
}

const struct inode_operations vtfs_dir_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .mkdir = vtfs_mkdir,
    .unlink = vtfs_unlink,
    .rmdir = vtfs_rmdir,
    .link = vtfs_link,
    .symlink = vtfs_symlink,
};