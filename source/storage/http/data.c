#include <linux/base64.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/version.h>

#include "remote_internal.h"

ssize_t remote_read(struct vtfs_storage *storage, u64 ino, void *buffer,
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

    n = base64_encode(data, len, encoded, false, BASE64_URLSAFE);
    if (n < 0 || n >= cap)
        return -EINVAL;
    encoded[n] = '\0';
    return 0;
}

ssize_t remote_write(struct vtfs_storage *storage, u64 ino,
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

int remote_truncate(struct vtfs_storage *storage, u64 ino, loff_t size)
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
