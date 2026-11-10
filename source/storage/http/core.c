
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/inet.h>

#include "remote.h"
#include "remote_internal.h"

int remote_get(struct vtfs_storage *storage, u64 ino,
                      struct vtfs_node_info *result)
{
    char s_ino[24];
    u8 response[NODE_INFO_BYTES];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = { { "ino", s_ino } };

    u64_arg(s_ino, sizeof(s_ino), ino);
    ret = rpc(storage, "get", args, ARRAY_SIZE(args),
              response, sizeof(response), &n);
    if (ret)
        return ret;
    return n == NODE_INFO_BYTES ? decode_node(response, n, result) : -EPROTO;
}

int remote_update_metadata(struct vtfs_storage *storage, u64 ino,
                                  const struct vtfs_node_info *info)
{
    char s_ino[24], mode[16], uid[16], gid[16];
    size_t n = 0;
    struct vtfs_http_arg args[] = {
        { "ino", s_ino }, { "mode", mode }, { "uid", uid }, { "gid", gid },
    };
    u64_arg(s_ino, sizeof(s_ino), ino);
    u32_arg(mode, sizeof(mode), info->mode);
    u32_arg(uid, sizeof(uid), __kuid_val(info->uid));
    u32_arg(gid, sizeof(gid), __kgid_val(info->gid));
    return rpc(storage, "metadata", args, ARRAY_SIZE(args), NULL, 0, &n);
}

void remote_forget(struct vtfs_storage *storage, u64 ino)
{
    char s_ino[24];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = { { "ino", s_ino } };
    u64_arg(s_ino, sizeof(s_ino), ino);
    ret = rpc(storage, "forget", args, ARRAY_SIZE(args), NULL, 0, &n);
    if (ret)
        pr_warn_ratelimited("vtfs: HTTP forget inode %llu failed: %d\n",
                            (unsigned long long)ino, ret);
}

static void remote_destroy(struct vtfs_storage *storage)
{
    struct vtfs_remote *r = storage->private;

    if (r) {
        kfree(r->host);
        kfree_sensitive(r->token);
        kfree(r);
    }
    storage->private = NULL;
    storage->ops = NULL;
}

static const struct vtfs_storage_ops remote_ops = {
    .get = remote_get,
    .lookup = remote_lookup,
    .list = remote_list,
    .create = remote_create,
    .remove = remote_remove,
    .link = remote_link,
    .destroy = remote_destroy,
    .read = remote_read,
    .write = remote_write,
    .truncate = remote_truncate,
    .forget = remote_forget,
    .update_metadata = remote_update_metadata,
};

int vtfs_http_init(struct vtfs_storage *storage,
                   const char *host, unsigned int port,
                   const char *token)
{
    struct vtfs_remote *r;
    u8 ipv4[4];
    size_t i, token_len;

    if (!storage || !host || !*host || !token || !*token ||
        port == 0 || port > 65535)
        return -EINVAL;

    if (!in4_pton(host, -1, ipv4, -1, NULL))
        return -EINVAL;

    token_len = strnlen(token, 256);
    if (!token_len || token_len >= 256)
        return -EINVAL;
    for (i = 0; i < token_len; i++) {
        char c = token[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return -EINVAL;
    }

    r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;

    r->host = kstrdup(host, GFP_KERNEL);
    r->token = kstrdup(token, GFP_KERNEL);
    r->port = port;
    if (!r->host || !r->token) {
        kfree(r->host);
        kfree_sensitive(r->token);
        kfree(r);
        return -ENOMEM;
    }

    storage->private = r;
    storage->ops = &remote_ops;
    return 0;
}
