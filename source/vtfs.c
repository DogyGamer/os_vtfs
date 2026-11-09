#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <linux/fs.h>
#include <linux/fs_context.h>


#include <linux/slab.h>
#include <linux/err.h>
#include <linux/dcache.h>

#include <linux/uaccess.h>

#include "vtfs_storage.h"


#define MODULE_NAME "vtfs"



MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandr Plushev");
MODULE_DESCRIPTION("A amazing 67 vtfs vfs fs");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

//fs ops
static int vtfs_init(struct fs_context *fs_ctx);
static int vtfs_get_tree(struct fs_context *fs_ctx);
static int vtfs_fill_super(struct super_block *sb, struct fs_context *fc);

//file ops
//static int vtfs_iterate(struct file *f, struct dir_context *dir_ctx);
static int vtfs_iterate(
    struct file *, struct dir_context *);

//inode ops
//static struct dentry *vtfs_lookup(struct inode *parent, struct dentry *child, unsigned int flags);
static struct dentry *vtfs_lookup(
    struct inode *, struct dentry *, unsigned int);

static int vtfs_create(
    struct mnt_idmap *, struct inode *,
    struct dentry *, umode_t, bool);

static struct dentry *vtfs_mkdir(
    struct mnt_idmap *, struct inode *,
    struct dentry *, umode_t);

static int vtfs_unlink(struct inode *, struct dentry *);
static int vtfs_rmdir(struct inode *, struct dentry *);

static int vtfs_link(
    struct dentry *, struct inode *, struct dentry *);

static int vtfs_setattr(
    struct mnt_idmap *idmap,
    struct dentry *dentry,
    struct iattr *attr);

static ssize_t vtfs_read(
    struct file *file,
    char __user *user_buffer,
    size_t count,
    loff_t *offset);

static ssize_t vtfs_write(
    struct file *file,
    const char __user *user_buffer,
    size_t count,
    loff_t *offset);

static void vtfs_evict_inode(struct inode *inode);

static int vtfs_symlink(
    struct mnt_idmap *idmap,
    struct inode *parent,
    struct dentry *child,
    const char *target);

//описание этой фс, которое передастся ядру
static struct file_system_type vtfs_type = {
    .name = "vtfs",
    .owner = THIS_MODULE,
    .init_fs_context = &vtfs_init,
    //хелпер помогающий освободить ресурсы инстанса фс
    .kill_sb = kill_anon_super
};



struct vtfs_sb_info {
    struct vtfs_storage storage;
};

static struct vtfs_storage *vtfs_storage_of(struct super_block *sb)
{
    struct vtfs_sb_info *info = sb->s_fs_info;
    return &info->storage;
}

//освобождение ресурсов на замену kill_anon_super
static void vtfs_put_super(struct super_block *sb)
{
    struct vtfs_sb_info *info = sb->s_fs_info;

    if (!info)
        return;

    info->storage.ops->destroy(&info->storage);
    kfree(info);

    sb->s_fs_info = NULL;
}

static const struct super_operations vtfs_super_ops = {
    .put_super = vtfs_put_super,
    .evict_inode = vtfs_evict_inode,
};

// static struct file_operations vtfs_file_ops = {
//   .iterate_shared = vtfs_iterate,
// };
//
// static struct inode_operations vtfs_inode_ops = {
//   .lookup = vtfs_lookup,
// };

static const struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

static const struct file_operations vtfs_regular_ops = {
    .open = simple_open,
    .llseek = generic_file_llseek,
    .read = vtfs_read,
    .write = vtfs_write,
};

static const struct inode_operations vtfs_regular_inode_ops = {
    .setattr = vtfs_setattr,
};

static const struct inode_operations vtfs_dir_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .mkdir = vtfs_mkdir,
    .unlink = vtfs_unlink,
    .rmdir = vtfs_rmdir,
    .link = vtfs_link,
    .symlink = vtfs_symlink,
};

static struct inode *vtfs_get_inode(
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

//структура с операциями над нашей фс, их дергает ядро когда ему хочца
static struct fs_context_operations vtfs_ops = {
    //при монтировании для получения дерева фс
    .get_tree = vtfs_get_tree,
};

//Вызывает vfs чтобы получить дерево фс при монтировании
static int vtfs_get_tree (struct fs_context *fs_ctx) {
    //эдакий хелпер для фс без блочных устройств
    //помогает получить superblock и заполнить его через метод вторым аргументом
    return get_tree_nodev(fs_ctx, &vtfs_fill_super);
}

//получение информации непосредственно о файле например stat
// static struct dentry *vtfs_lookup(struct inode *parent, struct dentry *child, unsigned int) {
//
//     if (strcmp((const char *)child->d_name.name, "67_gazan.txt") == 0) {
//         //Почему не через child->d_sb?
//         struct inode *n_inode = new_inode(parent->i_sb);
//         // не понимаю тут полноценно нужно повторить то что было в vtfs_fill_super
//         //Типа все опсы добавить и прочее?
//
//
//         //если не получилось, вернем ошибку неудалось выделить память
//         if (!n_inode) {
//             return ERR_PTR(-ENOMEM);
//         }
//
//         //номер иноды, раз корень будет первый миллион я первый чемпион
//         n_inode->i_ino = 2;
//
//         //Так правильнее и заодно устанавливает владельца и группу нулл потому что нет родителя мы сделали корень
//         inode_init_owner(&nop_mnt_idmap, n_inode, parent,
//                      S_IFREG | 0755);
//
//
//         set_nlink(n_inode, 1);
//
//         //хелпер для временных меток
//         simple_inode_init_ts(n_inode);
//
//
//
//         d_add(child, n_inode);
//         return NULL;
//     }
//
//     //если файл не найден то свяжем dentry с null
//     d_add(child, NULL);
//     return NULL;
// }

static struct dentry *vtfs_lookup(
    struct inode *parent,
    struct dentry *child,
    unsigned int flags)
{
    struct vtfs_storage *storage;
    struct vtfs_node_info info;
    struct inode *inode;
    int ret;

    storage = vtfs_storage_of(parent->i_sb);

    ret = storage->ops->lookup(
        storage,
        parent->i_ino,
        child->d_name.name,
        child->d_name.len,
        &info
    );

    if (ret == -ENOENT) {
        d_add(child, NULL);
        return NULL;
    }

    if (ret)
        return ERR_PTR(ret);

    inode = vtfs_get_inode(parent->i_sb, &info);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    d_add(child, inode);
    return NULL;
}


static bool vtfs_emit_entry(
    void *arg,
    const char *name,
    size_t name_len,
    const struct vtfs_node_info *info)
{
    struct dir_context *ctx = arg;

    unsigned int type =
        S_ISDIR(info->mode) ? DT_DIR :
        S_ISLNK(info->mode) ? DT_LNK :
        DT_REG;

    return dir_emit(
        ctx,
        name,
        name_len,
        info->ino,
        type
    );
}

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

//Используется при ls, для того чтобы посмотреть
static int vtfs_iterate(struct file *f, struct dir_context *ctx) {
    struct vtfs_storage *storage;
    u64 cursor;
    int ret;
    LOG("vtfs_iterate %lld \n", ctx->pos);

    if (!dir_emit_dots(f, ctx))
        return 0;

    storage = vtfs_storage_of(file_inode(f)->i_sb);
    cursor = ctx->pos;

    //фиктивный файл
    // if (dir_ctx->pos == 2) {
    //     if (!dir_emit(dir_ctx, "67_gazan.txt", 12, 2, DT_REG))
    //         return 0;
    //     dir_ctx->pos = 3;
    // }

    ret = storage->ops->list(
        storage,
        file_inode(f)->i_ino,
        &cursor,
        vtfs_emit_entry,
        ctx
    );

    ctx->pos = cursor;
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

static void vtfs_evict_inode(struct inode *inode)
{
    struct vtfs_storage *storage;

    storage = vtfs_storage_of(inode->i_sb);

    truncate_inode_pages_final(&inode->i_data);

    if (S_ISLNK(inode->i_mode))
        kfree(inode->i_link);

    clear_inode(inode);

    storage->ops->forget(storage, inode->i_ino);
}

//заполняет супреблок
// static int vtfs_fill_super(struct super_block *sb, struct fs_context *fc) {
//     sb->s_magic = 0x67676767;
//
//     //сама должна выделить память
//     struct inode *root_inode = new_inode(sb);
//
//     //если не получилось, вернем ошибку неудалось выделить память
//     if (!root_inode) {
//         return -ENOMEM;
//     }
//
//     //номер иноды, раз корень будет первый миллион я первый чемпион
//     root_inode->i_ino = 1;
//     //директория с правами 755
//     // root_inode-> i_mode = S_IFDIR | 0755;
//
//     //Так правильнее и заодно устанавливает владельца и группу нулл потому что нет родителя мы сделали корень
//     inode_init_owner(&nop_mnt_idmap, root_inode, NULL,
//                  S_IFDIR | 0755);
//
//     //хелперы линукс реализующие простые базовые операции
//     // root_inode->i_op = &simple_dir_inode_operations;
//     // root_inode->i_fop = &simple_dir_operations;
//
//     //уже свои операции
//     root_inode->i_op = &vtfs_inode_ops;
//     root_inode->i_fop = &vtfs_file_ops;
//
//     // root_inode->i_nlink = 2;
//     //. и .. поэтому 2 лол
//     set_nlink(root_inode, 2);
//
//     //хелпер для временных меток
//     simple_inode_init_ts(root_inode);
//
//     //Метод который из иноды сам сделает нам корневую dentry
//     sb->s_root = d_make_root(root_inode);
//     //если не получилось выделить память под dentry структуру
//     if (!sb->s_root)
//         return -ENOMEM;
//
//     return 0;
// }

static int vtfs_fill_super(
    struct super_block *sb,
    struct fs_context *fc)
{
    struct vtfs_sb_info *info;
    struct vtfs_node_info root_info;
    struct inode *root_inode;
    int ret;

    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info)
        return -ENOMEM;

    ret = vtfs_ram_init(&info->storage);
    if (ret) {
        kfree(info);
        return ret;
    }

    sb->s_magic = 0x67676767;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_op = &vtfs_super_ops;
    sb->s_fs_info = info;

    ret = info->storage.ops->get(
        &info->storage,
        VTFS_ROOT_INO,
        &root_info
    );
    if (ret)
        goto fail;

    root_inode = vtfs_get_inode(sb, &root_info);
    if (!root_inode) {
        ret = -ENOMEM;
        goto fail;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }

    return 0;

    fail:
        vtfs_put_super(sb);
    return ret;
}

//хранит информацию при подготовке монтирования
//контекст может хранить параметры монтирования, методы, инфу какую-то
static int vtfs_init(struct fs_context *fs_ctx) {
    fs_ctx->ops = &vtfs_ops;
    return 0;
}

//insmod hook
static int __init vtfs_module_init(void) {
    int ret;
    LOG("VTFS joined the kernel\n");

    //сообщить ядру о новой фс
    ret = register_filesystem(&vtfs_type);

    if (ret != 0) {
        ERR("VTFS registration failed: %d\n", ret);
        return ret;
    }

    LOG("VTFS registered\n");
    return 0;
}

//rmmod hook
static void __exit vtfs_exit(void) {
    LOG("VTFS left the kernel\n");
    unregister_filesystem(&vtfs_type);
}

module_init(vtfs_module_init);
module_exit(vtfs_exit);
