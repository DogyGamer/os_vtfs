#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/unaligned.h>

#include "remote_internal.h"

int remote_lookup(struct vtfs_storage *storage, u64 parent_ino,
                         const char *name, size_t name_len,
                         struct vtfs_node_info *result)
{
    char parent[24], encoded[3 * 255 + 1];
    u8 response[NODE_INFO_BYTES];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = {
        { "parent", parent }, { "name", encoded },
    };

    if (!name_len || name_len > 255)
        return -ENAMETOOLONG;
    u64_arg(parent, sizeof(parent), parent_ino);
    ret = encode_name(name, name_len, encoded, sizeof(encoded));
    if (ret)
        return ret;
    ret = rpc(storage, "lookup", args, ARRAY_SIZE(args),
              response, sizeof(response), &n);
    if (ret)
        return ret;
    return n == NODE_INFO_BYTES ? decode_node(response, n, result) : -EPROTO;
}

int remote_list(struct vtfs_storage *storage, u64 dir_ino, u64 *cursor,
                       vtfs_list_callback callback, void *ctx)
{
    char dir[24], start[24], limit[12];
    u8 *response;
    size_t n = 0, pos = 13;
    u32 count, i;
    int ret;
    struct vtfs_http_arg args[] = {
        { "dir", dir }, { "cursor", start }, { "limit", limit },
    };

    response = kmalloc(LIST_REPLY_CAP, GFP_KERNEL);
    if (!response)
        return -ENOMEM;
    u64_arg(dir, sizeof(dir), dir_ino);
    u64_arg(start, sizeof(start), *cursor);
    u32_arg(limit, sizeof(limit), LIST_LIMIT);
    ret = rpc(storage, "list", args, ARRAY_SIZE(args),
              response, LIST_REPLY_CAP, &n);
    if (ret)
        goto out;
    if (n < 13) {
        ret = -EPROTO;
        goto out;
    }
    count = get_unaligned_le32(response + 8);
    if (count > LIST_LIMIT || response[12] > 1) {
        ret = -EPROTO;
        goto out;
    }

    for (i = 0; i < count; i++) {
        u64 cookie;
        u16 name_len;
        struct vtfs_node_info info;

        if (n - pos < 10) {
            ret = -EPROTO;
            goto out;
        }
        cookie = get_unaligned_le64(response + pos);
        name_len = get_unaligned_le16(response + pos + 8);
        pos += 10;
        if (!name_len || name_len > 255 || n - pos < (size_t)name_len + NODE_INFO_BYTES ||
            cookie < *cursor || cookie == U64_MAX) {
            ret = -EPROTO;
            goto out;
        }
        ret = decode_node(response + pos + name_len, NODE_INFO_BYTES, &info);
        if (ret)
            goto out;
        if (!callback(ctx, (const char *)(response + pos), name_len, &info)) {
            ret = 0;
            goto out;
        }
        *cursor = cookie + 1;
        pos += name_len + NODE_INFO_BYTES;
    }

out:
    kfree(response);
    return ret;
}

int remote_create(struct vtfs_storage *storage, u64 parent_ino,
                         const char *name, size_t name_len, umode_t mode,
                         struct vtfs_node_info *result)
{
    char parent[24], encoded[3 * 255 + 1], s_mode[16], uid[16], gid[16];
    u8 response[NODE_INFO_BYTES];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = {
        { "parent", parent }, { "name", encoded }, { "mode", s_mode },
        { "uid", uid }, { "gid", gid },
    };

    if (!name_len || name_len > 255)
        return -ENAMETOOLONG;
    u64_arg(parent, sizeof(parent), parent_ino);
    u32_arg(s_mode, sizeof(s_mode), mode);
    u32_arg(uid, sizeof(uid), __kuid_val(current_fsuid()));
    u32_arg(gid, sizeof(gid), __kgid_val(current_fsgid()));
    ret = encode_name(name, name_len, encoded, sizeof(encoded));
    if (ret)
        return ret;
    ret = rpc(storage, "create", args, ARRAY_SIZE(args),
              response, sizeof(response), &n);
    if (ret)
        return ret;
    return n == NODE_INFO_BYTES ? decode_node(response, n, result) : -EPROTO;
}

int remote_remove(struct vtfs_storage *storage, u64 parent_ino,
                         const char *name, size_t name_len, bool directory,
                         struct vtfs_node_info *result)
{
    char parent[24], encoded[3 * 255 + 1];
    u8 response[NODE_INFO_BYTES];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = {
        { "parent", parent }, { "name", encoded },
        { "directory", directory ? "1" : "0" },
    };

    if (!name_len || name_len > 255)
        return -ENAMETOOLONG;
    u64_arg(parent, sizeof(parent), parent_ino);
    ret = encode_name(name, name_len, encoded, sizeof(encoded));
    if (ret)
        return ret;
    ret = rpc(storage, "remove", args, ARRAY_SIZE(args),
              response, sizeof(response), &n);
    if (ret)
        return ret;
    return n == NODE_INFO_BYTES ? decode_node(response, n, result) : -EPROTO;
}

int remote_link(struct vtfs_storage *storage, u64 parent_ino,
                       const char *name, size_t name_len, u64 target_ino,
                       struct vtfs_node_info *result)
{
    char parent[24], encoded[3 * 255 + 1], target[24];
    u8 response[NODE_INFO_BYTES];
    size_t n = 0;
    int ret;
    struct vtfs_http_arg args[] = {
        { "parent", parent }, { "name", encoded }, { "target", target },
    };

    if (!name_len || name_len > 255)
        return -ENAMETOOLONG;
    u64_arg(parent, sizeof(parent), parent_ino);
    u64_arg(target, sizeof(target), target_ino);
    ret = encode_name(name, name_len, encoded, sizeof(encoded));
    if (ret)
        return ret;
    ret = rpc(storage, "link", args, ARRAY_SIZE(args),
              response, sizeof(response), &n);
    if (ret)
        return ret;
    return n == NODE_INFO_BYTES ? decode_node(response, n, result) : -EPROTO;
}
