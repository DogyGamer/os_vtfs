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

//Используется при ls, для того чтобы посмотреть
int vtfs_iterate(struct file *f, struct dir_context *ctx) {
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

struct dentry *vtfs_lookup(
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

const struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

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