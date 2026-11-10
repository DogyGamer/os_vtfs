#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/limits.h>
#include <linux/stat.h>

#include "vtfs_storage.h"

#include <linux/cred.h>
#include <linux/uidgid.h>

#include "ram_internal.h"

ssize_t ram_read(
    struct vtfs_storage *storage,
    u64 ino, void *buffer, size_t count, loff_t offset)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    ssize_t ret;

    LOG("ram_read %llu", ino);

    if (offset < 0)
        return -EINVAL;

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);
    if (!node) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISREG(node->mode) && !S_ISLNK(node->mode)) {
        ret = -EISDIR;
        goto out;
    }

    if (offset >= node->size) {
        ret = 0;
        goto out;
    }

    ret = min_t(size_t, count, node->size - offset);
    memcpy(buffer, node->data + offset, ret);

out:
    mutex_unlock(&ram->lock);
    return ret;
}

ssize_t ram_write(
    struct vtfs_storage *storage,
    u64 ino, const void *buffer, size_t count,
    loff_t offset, struct vtfs_node_info *updated)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    char *new_data;
    size_t end;
    ssize_t ret;

    LOG("ram_write %llu", ino);

    if (offset < 0)
        return -EINVAL;

    if (!count)
        return 0;

    end = offset + count;

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);
    if (!node) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISREG(node->mode) && !S_ISLNK(node->mode)) {
        ret = -EISDIR;
        goto out;
    }

    if (end > node->size) {
        new_data = kvzalloc(end, GFP_KERNEL);
        if (!new_data) {
            ret = -ENOMEM;
            goto out;
        }

        if (node->data)
            memcpy(new_data, node->data, node->size);

        kvfree(node->data);
        node->data = new_data;
        node->size = end;
    }

    memcpy(node->data + offset, buffer, count);
    ram_get_info(node, updated);
    ret = count;

out:
    mutex_unlock(&ram->lock);
    return ret;
}

int ram_truncate(
    struct vtfs_storage *storage,
    u64 ino, loff_t size)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    char *new_data;
    int ret = 0;

    LOG("ram_truncate %llu", ino);

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);
    if (!node) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISREG(node->mode)) {
        ret = -EISDIR;
        goto out;
    }

    if (size > node->size) {
        new_data = kvzalloc(size, GFP_KERNEL);
        if (!new_data) {
            ret = -ENOMEM;
            goto out;
        }

        if (node->data)
            memcpy(new_data, node->data, node->size);

        kvfree(node->data);
        node->data = new_data;
    } else if (size < node->size) {
        if (node->data)
            memset(node->data + size, 0, node->size - size);
    }

    node->size = size;

out:
    mutex_unlock(&ram->lock);
    return ret;
}