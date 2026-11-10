#include "ram_internal.h"
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

//существование обьектов их время жизни айдишники

// найдитти по айди
struct vtfs_ram_node *ram_find_node(
    struct vtfs_ram *ram,
    u64 ino)
{
    struct vtfs_ram_node *node;
    LOG("ram_find_node %lu", ino);

    list_for_each_entry(node, &ram->nodes, all_link) {
        if (node->ino == ino)
            return node;
    }

    return NULL;
}

// Скопировать метаданные во внешний формат
void ram_get_info(
    const struct vtfs_ram_node *node,
    struct vtfs_node_info *info)
{
    info->ino = node->ino;
    info->mode = node->mode;
    info->size = node->size;
    info->nlink = node->nlink;

    info->uid = node->uid;
    info->gid = node->gid;
}

static int ram_get(
    struct vtfs_storage *storage,
    u64 ino,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    int ret = 0;

    LOG("ram_get %llu", ino);

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);

    if (!node) {
        ret = -ENOENT;
    } else {
        ram_get_info(node, result);
    }

    mutex_unlock(&ram->lock);
    return ret;
}

static void ram_forget(
    struct vtfs_storage *storage,
    u64 ino)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;

    LOG("ram_forget %llu", ino);
    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);

    if (!node || node->nlink != 0)
        goto out;

    // для удалённой директории список должен быть пуст.
    if (!list_empty(&node->children))
        goto out;

    list_del(&node->all_link);

    pr_info("vtfs: freed orphan inode %llu\n",
            (unsigned long long)ino);

    kvfree(node->data);
    kfree(node);

    out:
        mutex_unlock(&ram->lock);
}

static int ram_update_metadata(
    struct vtfs_storage *storage,
    u64 ino,
    const struct vtfs_node_info *info)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    int ret = 0;

    LOG("ram_update_metadata %llu", ino);

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);

    if (!node) {
        ret = -ENOENT;
        goto out;
    }

    node->mode = info->mode;
    node->uid = info->uid;
    node->gid = info->gid;

    //не здесь
    // node->nlink = info->nlink;

    out:
        mutex_unlock(&ram->lock);
    return ret;
}

static void ram_destroy(struct vtfs_storage *storage)
{
    LOG("ram_destroy");
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node, *next_node;
    struct vtfs_ram_entry *entry, *next_entry;

    list_for_each_entry_safe(node, next_node,
                             &ram->nodes, all_link) {
        list_for_each_entry_safe(entry, next_entry,
                                 &node->children, link) {
            list_del(&entry->link);
            kfree(entry->name);
            kfree(entry);
        }

        list_del(&node->all_link);
        kvfree(node->data);
        kfree(node);
    }

    kfree(ram);
    storage->private = NULL;
    storage->ops = NULL;
}



const struct vtfs_storage_ops ram_ops = {
    .get = ram_get,
    .lookup = ram_lookup,
    .list = ram_list,
    .create = ram_create,
    .remove = ram_remove,
    .link = ram_link,
    .destroy = ram_destroy,
    .read = ram_read,
    .write = ram_write,
    .truncate = ram_truncate,
    .forget = ram_forget,
    .update_metadata = ram_update_metadata,
};

int vtfs_ram_init(struct vtfs_storage *storage)
{

    LOG("vtfs_ram_init");
    struct vtfs_ram *ram;
    struct vtfs_ram_node *root;

    ram = kzalloc(sizeof(*ram), GFP_KERNEL);
    if (!ram)
        return -ENOMEM;

    root = kzalloc(sizeof(*root), GFP_KERNEL);
    if (!root) {
        kfree(ram);
        return -ENOMEM;
    }

    mutex_init(&ram->lock);
    INIT_LIST_HEAD(&ram->nodes);
    INIT_LIST_HEAD(&root->children);

    root->ino = VTFS_ROOT_INO;
    root->nlink = 2;

    root->mode = S_IFDIR | 0777;
    root->uid = GLOBAL_ROOT_UID;
    root->gid = GLOBAL_ROOT_GID;
    root->next_cookie = 2;

    list_add_tail(&root->all_link, &ram->nodes);

    ram->next_ino = VTFS_ROOT_INO + 1;

    storage->private = ram;
    storage->ops = &ram_ops;

    return 0;
}