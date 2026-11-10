#include <linux/base64.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/unaligned.h>
#include <linux/version.h>

#include "remote.h"
#include "http.h"

#define NODE_INFO_BYTES 32
#define READ_CHUNK      4096
#define WRITE_CHUNK     512
#define LIST_LIMIT      16
#define LIST_REPLY_CAP  8192

struct vtfs_remote {
    char *host;
    unsigned int port;
    char *token;
};

static char server_token[256] = "";


static void u64_arg(char *dest, size_t cap, u64 value)
{
    snprintf(dest, cap, "%llu", (unsigned long long)value);
}

static void u32_arg(char *dest, size_t cap, u32 value)
{
    snprintf(dest, cap, "%u", value);
}

static int decode_node(const u8 *src, size_t len,
                       struct vtfs_node_info *info)
{
    if (len < NODE_INFO_BYTES)
        return -EPROTO;
    info->ino = get_unaligned_le64(src);
    info->mode = (umode_t)get_unaligned_le32(src + 8);
    info->size = get_unaligned_le64(src + 12);
    info->nlink = get_unaligned_le32(src + 20);
    info->uid = KUIDT_INIT(get_unaligned_le32(src + 24));
    info->gid = KGIDT_INIT(get_unaligned_le32(src + 28));
    return 0;
}

struct vtfs_http_arg {
    const char *name;
    const char *value;
};

static int rpc(const struct vtfs_storage *storage, const char *method,
               const struct vtfs_http_arg *args, size_t nargs,
               u8 *response, size_t capacity, size_t *received)
{
    const struct vtfs_remote *remote = storage->private;
    char dummy = 0;
    int64_t ret;

    if (response && capacity)
        memset(response, 0, capacity);
    if (!response) {
        response = (u8 *)&dummy;
        capacity = 1;
    }

    switch (nargs) {
    case 0:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 0);
        break;
    case 1:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 1,
                             args[0].name, args[0].value);
        break;
    case 2:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 2,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value);
        break;
    case 3:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 3,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value,
                             args[2].name, args[2].value);
        break;
    case 4:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 4,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value,
                             args[2].name, args[2].value,
                             args[3].name, args[3].value);
        break;
    case 5:
        ret = vtfs_http_call(remote->token, method, (char *)response, capacity, 5,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value,
                             args[2].name, args[2].value,
                             args[3].name, args[3].value,
                             args[4].name, args[4].value);
        break;
    default:
        return -EINVAL;
    }
    if (ret < 0)
        return ret >= INT_MIN ? (int)ret : -EIO;
    if (ret > 0)
        return ret <= 4095 ? -(int)ret : -EIO;
    if (received)
        *received = capacity;
    return 0;
}


static int encode_name(const char *name, size_t len, char *out, size_t cap)
{
    char temp[256];

    if (!len || len > 255)
        return -ENAMETOOLONG;
    if (cap < 3 * len + 1)
        return -ENOSPC;
    memcpy(temp, name, len);
    temp[len] = 0;
    encode(temp, out);
    return 0;
}

static int remote_get(struct vtfs_storage *storage, u64 ino,
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

static int remote_lookup(struct vtfs_storage *storage, u64 parent_ino,
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

static int remote_list(struct vtfs_storage *storage, u64 dir_ino, u64 *cursor,
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

static int remote_create(struct vtfs_storage *storage, u64 parent_ino,
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

static int remote_remove(struct vtfs_storage *storage, u64 parent_ino,
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

static int remote_link(struct vtfs_storage *storage, u64 parent_ino,
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

static ssize_t remote_read(struct vtfs_storage *storage, u64 ino, void *buffer,
                           size_t count, loff_t offset)
{
    char s_ino[24], off[24], size[24];
    u8 *response;
    size_t done = 0, n = 0;
    int ret;
    struct vtfs_http_arg args[] = {
        { "ino", s_ino }, { "offset", off }, { "size", size },
    };

    if (offset < 0)
        return -EINVAL;
    if (!count)
        return 0;
    response = kmalloc(READ_CHUNK + 4, GFP_KERNEL);
    if (!response)
        return -ENOMEM;

    u64_arg(s_ino, sizeof(s_ino), ino);
    while (done < count) {
        u32 received;
        size_t chunk = min_t(size_t, count - done, READ_CHUNK);

        if ((u64)offset > S64_MAX - done) {
            ret = -EFBIG;
            break;
        }
        u64_arg(off, sizeof(off), (u64)offset + done);
        u64_arg(size, sizeof(size), chunk);
        ret = rpc(storage, "read", args, ARRAY_SIZE(args),
                  response, READ_CHUNK + 4, &n);
        if (ret)
            break;
        if (n < 4) {
            ret = -EPROTO;
            break;
        }
        received = get_unaligned_le32(response);
        if (received > chunk || n < 4 + received) {
            ret = -EPROTO;
            break;
        }
        memcpy((u8 *)buffer + done, response + 4, received);
        done += received;
        if (received < chunk)
            break;
    }
    kfree(response);
    return done ? (ssize_t)done : ret;
}

static int encode_base64url(const u8 *data, size_t len, char *encoded, size_t cap)
{
    int n;
    if (len > WRITE_CHUNK || cap < 4 * ((len + 2) / 3) + 1)
        return -EINVAL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    n = base64_encode(data, len, encoded, false, BASE64_URLSAFE);
#else
    /* Compatibility with older kernels; the user runs Linux 7.0. */
    n = base64_encode(data, len, encoded);
    while (n && encoded[n - 1] == '=')
        n--;
    {
        int i;
        for (i = 0; i < n; i++) {
            if (encoded[i] == '+') encoded[i] = '-';
            if (encoded[i] == '/') encoded[i] = '_';
        }
    }
#endif
    if (n < 0 || n >= cap)
        return -EINVAL;
    encoded[n] = '\0';
    return 0;
}

static ssize_t remote_write(struct vtfs_storage *storage, u64 ino,
                            const void *buffer, size_t count, loff_t offset,
                            struct vtfs_node_info *updated)
{
    char s_ino[24], off[24], encoded[4 * ((WRITE_CHUNK + 2) / 3) + 2];
    u8 response[4 + NODE_INFO_BYTES];
    size_t done = 0, n = 0;
    int ret = 0;
    struct vtfs_http_arg args[] = {
        { "ino", s_ino }, { "offset", off }, { "data", encoded },
    };

    if (offset < 0)
        return -EINVAL;
    if (!count)
        return 0;
    u64_arg(s_ino, sizeof(s_ino), ino);
    while (done < count) {
        u32 written;
        size_t chunk = min_t(size_t, count - done, WRITE_CHUNK);

        if ((u64)offset > S64_MAX - done) {
            ret = -EFBIG;
            break;
        }
        u64_arg(off, sizeof(off), (u64)offset + done);
        ret = encode_base64url((const u8 *)buffer + done, chunk,
                               encoded, sizeof(encoded));
        if (ret)
            break;
        ret = rpc(storage, "write", args, ARRAY_SIZE(args),
                  response, sizeof(response), &n);
        if (ret)
            break;
        if (n != sizeof(response)) {
            ret = -EPROTO;
            break;
        }
        written = get_unaligned_le32(response);
        if (!written || written > chunk) {
            ret = -EPROTO;
            break;
        }
        ret = decode_node(response + 4, NODE_INFO_BYTES, updated);
        if (ret)
            break;
        done += written;
        if (written < chunk)
            break;
    }
    return done ? (ssize_t)done : ret;
}

static int remote_truncate(struct vtfs_storage *storage, u64 ino, loff_t size)
{
    char s_ino[24], s_size[24];
    size_t n = 0;
    struct vtfs_http_arg args[] = {
        { "ino", s_ino }, { "size", s_size },
    };
    if (size < 0)
        return -EINVAL;
    u64_arg(s_ino, sizeof(s_ino), ino);
    u64_arg(s_size, sizeof(s_size), size);
    return rpc(storage, "truncate", args, ARRAY_SIZE(args), NULL, 0, &n);
}

static int remote_update_metadata(struct vtfs_storage *storage, u64 ino,
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

static void remote_forget(struct vtfs_storage *storage, u64 ino)
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
    kfree(storage->private);
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

int vtfs_http_init(struct vtfs_storage *storage)
{
    struct vtfs_remote *r;

    if (!server_token[0])
        return -EINVAL;
    r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    strscpy(r->token, server_token, sizeof(r->token));
    storage->private = r;
    storage->ops = &remote_ops;
    return 0;
}
