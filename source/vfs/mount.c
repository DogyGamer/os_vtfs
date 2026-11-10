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

#include <linux/fs_parser.h>
#include "../storage/http/remote.h"

struct vtfs_mount_options {
    bool use_http;
    char *host;
    unsigned int port;
    char *token;
};

enum {
    Opt_backend,
    Opt_host,
    Opt_port,
    Opt_token,
};

static const struct fs_parameter_spec vtfs_params[] = {
    fsparam_string("backend", Opt_backend),
    fsparam_string("host", Opt_host),
    fsparam_u32("port", Opt_port),
    fsparam_string("token", Opt_token),
    {}
};

static int vtfs_parse_param(
    struct fs_context *fc,
    struct fs_parameter *param)
{
    struct vtfs_mount_options *opts = fc->fs_private;
    struct fs_parse_result result;
    char *value;
    int opt;

    opt = fs_parse(fc, vtfs_params, param, &result);
    if (opt < 0)
        return opt;

    switch (opt) {
        case Opt_backend:
            if (!strcmp(param->string, "ram"))
                opts->use_http = false;
            else if (!strcmp(param->string, "http"))
                opts->use_http = true;
            else
                return -EINVAL;
            break;

        case Opt_host:
            value = kstrdup(param->string, GFP_KERNEL);
            if (!value)
                return -ENOMEM;

            kfree(opts->host);
            opts->host = value;
            break;

        case Opt_port:
            if (result.uint_32 == 0 || result.uint_32 > 65535)
                return -EINVAL;

            opts->port = result.uint_32;
            break;

        case Opt_token:
            value = kstrdup(param->string, GFP_KERNEL);
            if (!value)
                return -ENOMEM;

            kfree_sensitive(opts->token);
            opts->token = value;
            break;
    }

    return 0;
}

static void vtfs_free_fs_context(struct fs_context *fc)
{
    struct vtfs_mount_options *opts = fc->fs_private;

    if (!opts)
        return;

    kfree(opts->host);
    kfree_sensitive(opts->token);
    kfree(opts);
}

//освобождение ресурсов на замену kill_anon_super
static void vtfs_put_super(struct super_block *sb)
{
    LOG("vtfs_put_super");
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

    LOG("vtfs_fill_super");

    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info)
        return -ENOMEM;

    struct vtfs_mount_options *opts = fc->fs_private;

    if (opts->use_http) {
        if (!opts->token || !*opts->token) {
            kfree(info);
            return -EINVAL;
        }

        ret = vtfs_http_init(
            &info->storage,
            opts->host ? opts->host : "127.0.0.1",
            opts->port,
            opts->token
        );
    } else {
        ret = vtfs_ram_init(&info->storage);
    }
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
    .parse_param = vtfs_parse_param,
    .free = vtfs_free_fs_context,
};

//хранит информацию при подготовке монтирования
//контекст может хранить параметры монтирования, методы, инфу какую-то
int vtfs_init_fs_context(struct fs_context *fs_ctx) {
    LOG("vtfs_init_fs_context");

    struct vtfs_mount_options *opts;
    opts = kzalloc(sizeof(*opts), GFP_KERNEL);
    if (!opts)
        return -ENOMEM;
    opts->use_http = false;
    opts->port = 6767;
    fs_ctx->fs_private = opts;
    fs_ctx->ops = &vtfs_context_ops;
    return 0;
}


