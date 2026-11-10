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
static ssize_t vtfs_read(
    struct file *file,
    char __user *user_buffer,
    size_t count,
    loff_t *offset)
{
    struct vtfs_storage *storage;
    char *buffer;
    ssize_t ret;

    if (!count)
        return 0;

    buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buffer)
        return -ENOMEM;

    storage = vtfs_storage_of(file_inode(file)->i_sb);

    ret = storage->ops->read(
        storage,
        file_inode(file)->i_ino,
        buffer,
        min_t(size_t, count, PAGE_SIZE),
        *offset
    );

    if (ret > 0) {
        if (copy_to_user(user_buffer, buffer, ret)) {
            ret = -EFAULT;
        } else {
            *offset += ret;
        }
    }

    kfree(buffer);
    return ret;
}

static ssize_t vtfs_write(
    struct file *file,
    const char __user *user_buffer,
    size_t count,
    loff_t *offset)
{
    struct inode *inode = file_inode(file);
    struct vtfs_storage *storage = vtfs_storage_of(inode->i_sb);
    struct vtfs_node_info updated;
    char *buffer;
    loff_t pos;
    ssize_t done = 0;
    ssize_t ret;
    size_t chunk;

    if (!count)
        return 0;

    buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buffer)
        return -ENOMEM;

    inode_lock(inode);

    pos = (file->f_flags & O_APPEND)
        ? i_size_read(inode) : *offset;

    while (done < count) {
        chunk = min_t(size_t, PAGE_SIZE, count - done);

        if (copy_from_user(buffer, user_buffer + done, chunk)) {
            if (!done)
                done = -EFAULT;
            break;
        }

        ret = storage->ops->write(
            storage, inode->i_ino,
            buffer, chunk, pos, &updated
        );

        if (ret <= 0) {
            if (!done)
                done = ret;
            break;
        }

        pos += ret;
        done += ret;
        i_size_write(inode, updated.size);

        if (ret < chunk)
            break;
    }

    if (done > 0) {
        struct timespec64 now;

        *offset = pos;
        now = inode_set_ctime_current(inode);
        inode_set_mtime_to_ts(inode, now);
    }

    inode_unlock(inode);
    kfree(buffer);
    return done;
}

static int vtfs_setattr(
    struct mnt_idmap *idmap,
    struct dentry *dentry,
    struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    struct vtfs_storage *storage;
    int ret;

    LOG("vtfs_setattr");

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;

    if (attr->ia_valid & ATTR_SIZE) {
        storage = vtfs_storage_of(inode->i_sb);

        ret = storage->ops->truncate(
            storage, inode->i_ino, attr->ia_size
        );

        if (ret)
            return ret;

        truncate_setsize(inode, attr->ia_size);
    }

    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

const struct file_operations vtfs_regular_ops = {
    .open = simple_open,
    .llseek = generic_file_llseek,
    .read = vtfs_read,
    .write = vtfs_write,
};

const struct inode_operations vtfs_regular_inode_ops = {
    .setattr = vtfs_setattr,
};