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

//Вызывает vfs чтобы получить дерево фс при монтировании
static int vtfs_get_tree (struct fs_context *fs_ctx) {
    //эдакий хелпер для фс без блочных устройств
    //помогает получить superblock и заполнить его через метод вторым аргументом
    return get_tree_nodev(fs_ctx, &vtfs_fill_super);
}

//структура с операциями над нашей фс, их дергает ядро когда ему хочца
static struct fs_context_operations vtfs_context_ops  = {
    //при монтировании для получения дерева фс
    .get_tree = vtfs_get_tree,
};

//хранит информацию при подготовке монтирования
//контекст может хранить параметры монтирования, методы, инфу какую-то
int vtfs_init_fs_context(struct fs_context *fs_ctx) {
    fs_ctx->ops = &vtfs_context_ops;
    return 0;
}
