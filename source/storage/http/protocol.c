#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/unaligned.h>

#include "remote_internal.h"
#include "http.h"

void u64_arg(char *dest, size_t cap, u64 value)
{
    snprintf(dest, cap, "%llu", (unsigned long long)value);
}

void u32_arg(char *dest, size_t cap, u32 value)
{
    snprintf(dest, cap, "%u", value);
}

int decode_node(const u8 *src, size_t len,
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

int rpc(const struct vtfs_storage *storage, const char *method,
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
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 0);
        break;
    case 1:
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 1,
                             args[0].name, args[0].value);
        break;
    case 2:
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 2,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value);
        break;
    case 3:
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 3,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value,
                             args[2].name, args[2].value);
        break;
    case 4:
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 4,
                             args[0].name, args[0].value,
                             args[1].name, args[1].value,
                             args[2].name, args[2].value,
                             args[3].name, args[3].value);
        break;
    case 5:
        ret = vtfs_http_call(remote->host, remote->port,
                             remote->token, method, (char *)response, capacity, 5,
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

int encode_name(const char *name, size_t len, char *out, size_t cap)
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
