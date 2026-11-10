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

// найти в кт
static struct vtfs_ram_entry *ram_find_entry(
    struct vtfs_ram_node *dir,
    const char *name,
    size_t name_len)
{
    struct vtfs_ram_entry *entry;
    LOG("vtfs_ram_entry %s, dir inode %llu", name, dir->ino);
    list_for_each_entry(entry, &dir->children, link) {
        if (entry->name_len == name_len &&
            memcmp(entry->name, name, name_len) == 0)
            return entry;
    }

    return NULL;
}

int ram_lookup(
    struct vtfs_storage *storage,
    u64 parent_ino,
    const char *name,
    size_t name_len,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *parent;
    struct vtfs_ram_entry *entry;
    int ret = 0;

    mutex_lock(&ram->lock);

    parent = ram_find_node(ram, parent_ino);

    if (!parent) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISDIR(parent->mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    entry = ram_find_entry(parent, name, name_len);

    if (!entry) {
        ret = -ENOENT;
        goto out;
    }

    ram_get_info(entry->node, result);

    out:
        mutex_unlock(&ram->lock);
    return ret;
}

int ram_list(
    struct vtfs_storage *storage,
    u64 dir_ino,
    u64 *cursor,
    vtfs_list_callback callback,
    void *arg)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *dir;
    struct vtfs_ram_entry *entry;
    struct vtfs_node_info info;
    int ret = 0;

    LOG("ram_list %llu, curs %llu", dir_ino, *cursor);

    mutex_lock(&ram->lock);

    dir = ram_find_node(ram, dir_ino);

    if (!dir) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISDIR(dir->mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    list_for_each_entry(entry, &dir->children, link) {
        if (entry->cookie < *cursor)
            continue;

        ram_get_info(entry->node, &info);

        if (!callback(arg, entry->name,
                      entry->name_len, &info))
            break;

        *cursor = entry->cookie + 1;
    }

    out:
        mutex_unlock(&ram->lock);
    return ret;
}

int ram_create(
    struct vtfs_storage *storage,
    u64 parent_ino,
    const char *name,
    size_t name_len,
    umode_t mode,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *parent;
    struct vtfs_ram_node *node;
    struct vtfs_ram_entry *entry;
    int ret = 0;

    LOG("ram_create parent %llu, name %s ", parent_ino, name);

    if (!name || !name_len || name_len > NAME_MAX ||
        memchr(name, '/', name_len) ||
        memchr(name, '\0', name_len) ||
        (name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.'))
        return -EINVAL;

    if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
        return -EINVAL;

    mutex_lock(&ram->lock);

    parent = ram_find_node(ram, parent_ino);

    if (!parent) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISDIR(parent->mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    if (ram_find_entry(parent, name, name_len)) {
        ret = -EEXIST;
        goto out;
    }

    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node) {
        ret = -ENOMEM;
        goto out;
    }

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        kfree(node);
        ret = -ENOMEM;
        goto out;
    }

    entry->name = kmemdup_nul(name, name_len, GFP_KERNEL);
    if (!entry->name) {
        kfree(entry);
        kfree(node);
        ret = -ENOMEM;
        goto out;
    }

    INIT_LIST_HEAD(&node->children);

    node->ino = ram->next_ino++;
    node->mode = mode;
    node->nlink = S_ISDIR(mode) ? 2 : 1;

    node->uid = current_fsuid();
    node->gid = current_fsgid();

    //пушто . и ..
    node->next_cookie = 2;
    entry->cookie = parent->next_cookie++;

    entry->name_len = name_len;
    entry->node = node;



    list_add_tail(&node->all_link, &ram->nodes);
    list_add_tail(&entry->link, &parent->children);

    if (S_ISDIR(mode))
        parent->nlink++;

    ram_get_info(node, result);

    out:
        mutex_unlock(&ram->lock);
    return ret;
}

int ram_remove(
    struct vtfs_storage *storage,
    u64 parent_ino,
    const char *name,
    size_t name_len,
    bool directory,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *parent, *node;
    struct vtfs_ram_entry *entry;
    int ret = 0;

    LOG("ram_remove parent %llu, name %s ", parent_ino, name);

    mutex_lock(&ram->lock);

    parent = ram_find_node(ram, parent_ino);

    if (!parent) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISDIR(parent->mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    entry = ram_find_entry(parent, name, name_len);

    if (!entry) {
        ret = -ENOENT;
        goto out;
    }

    node = entry->node;

    if (directory) {
        if (!S_ISDIR(node->mode)) {
            ret = -ENOTDIR;
            goto out;
        }

        if (!list_empty(&node->children)) {
            ret = -ENOTEMPTY;
            goto out;
        }

        node->nlink = 0;
        parent->nlink--;
    } else {
        if (S_ISDIR(node->mode)) {
            ret = -EISDIR;
            goto out;
        }

        node->nlink--;
    }

    ram_get_info(node, result);

    list_del(&entry->link);
    kfree(entry->name);
    kfree(entry);

    out:
        mutex_unlock(&ram->lock);
    return ret;
}

int ram_link(
    struct vtfs_storage *storage,
    u64 parent_ino,
    const char *name,
    size_t name_len,
    u64 target_ino,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *parent, *target;
    struct vtfs_ram_entry *entry;
    int ret = 0;

    LOG("ram_link parent %llu, name %s tino %llu ", parent_ino, name, target_ino);

    if (!name || !name_len || name_len > NAME_MAX ||
        memchr(name, '/', name_len) ||
        memchr(name, '\0', name_len) ||
        (name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.'))
        return -EINVAL;

    mutex_lock(&ram->lock);

    parent = ram_find_node(ram, parent_ino);
    target = ram_find_node(ram, target_ino);

    if (!parent || !target || !target->nlink) {
        ret = -ENOENT;
        goto out;
    }

    if (!S_ISDIR(parent->mode) || !S_ISREG(target->mode)) {
        ret = -EPERM;
        goto out;
    }

    if (ram_find_entry(parent, name, name_len)) {
        ret = -EEXIST;
        goto out;
    }

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        ret = -ENOMEM;
        goto out;
    }

    entry->name = kmemdup_nul(name, name_len, GFP_KERNEL);
    if (!entry->name) {
        kfree(entry);
        ret = -ENOMEM;
        goto out;
    }

    entry->name_len = name_len;
    entry->node = target;
    entry->cookie = parent->next_cookie++;

    list_add_tail(&entry->link, &parent->children);
    target->nlink++;

    ram_get_info(target, result);

    out:
        mutex_unlock(&ram->lock);
    return ret;
}