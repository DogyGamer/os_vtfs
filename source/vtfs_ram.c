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

    //записи в директории
    struct list_head children;

    //связь обьектов
    struct list_head all_link;

    //владелец группа позиция
    kuid_t uid;
    kgid_t gid;
    u64 next_cookie;
};

// вхождение в каталог
struct vtfs_ram_entry {
    char *name;
    size_t name_len;

    struct vtfs_ram_node *node;

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

// найдитти по айди
static struct vtfs_ram_node *ram_find_node(
    struct vtfs_ram *ram,
    u64 ino)
{
    struct vtfs_ram_node *node;

    list_for_each_entry(node, &ram->nodes, all_link) {
        if (node->ino == ino)
            return node;
    }

    return NULL;
}

// найти в кт
static struct vtfs_ram_entry *ram_find_entry(
    struct vtfs_ram_node *dir,
    const char *name,
    size_t name_len)
{
    struct vtfs_ram_entry *entry;

    list_for_each_entry(entry, &dir->children, link) {
        if (entry->name_len == name_len &&
            memcmp(entry->name, name, name_len) == 0)
            return entry;
    }

    return NULL;
}

// Скопировать метаданные во внешний формат
static void ram_get_info(
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

static int ram_lookup(
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

static int ram_list(
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

static int ram_create(
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

static void ram_destroy(struct vtfs_storage *storage)
{
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


static int ram_get(
    struct vtfs_storage *storage,
    u64 ino,
    struct vtfs_node_info *result)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    int ret = 0;

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

static int ram_remove(
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

static int ram_link(
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

static ssize_t ram_read(
    struct vtfs_storage *storage,
    u64 ino, void *buffer, size_t count, loff_t offset)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    ssize_t ret;

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
        ret = 0;  // EOF
        goto out;
    }

    ret = min_t(size_t, count, node->size - offset);
    memcpy(buffer, node->data + offset, ret);

out:
    mutex_unlock(&ram->lock);
    return ret;
}

static ssize_t ram_write(
    struct vtfs_storage *storage,
    u64 ino, const void *buffer, size_t count,
    loff_t offset, struct vtfs_node_info *updated)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    char *new_data;
    size_t end;
    ssize_t ret;

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

static int ram_truncate(
    struct vtfs_storage *storage,
    u64 ino, loff_t size)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;
    char *new_data;
    int ret = 0;

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

static void ram_forget(
    struct vtfs_storage *storage,
    u64 ino)
{
    struct vtfs_ram *ram = storage->private;
    struct vtfs_ram_node *node;

    mutex_lock(&ram->lock);

    node = ram_find_node(ram, ino);

    if (!node || node->nlink != 0)
        goto out;

    // Для удалённой директории список должен быть пуст.
    if (WARN_ON_ONCE(!list_empty(&node->children)))
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

static const struct vtfs_storage_ops ram_ops = {
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