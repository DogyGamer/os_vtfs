#ifndef VTFS_REMOTE_INTERNAL_H
#define VTFS_REMOTE_INTERNAL_H

#include <linux/types.h>
#include <linux/fs.h>
#include "vtfs_storage.h"

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

struct vtfs_http_arg {
    const char *name;
    const char *value;
};

void u64_arg(char *dest, size_t cap, u64 value);
void u32_arg(char *dest, size_t cap, u32 value);
int decode_node(const u8 *src, size_t len, struct vtfs_node_info *info);
int encode_name(const char *name, size_t len, char *out, size_t cap);
int rpc(const struct vtfs_storage *storage, const char *method,
        const struct vtfs_http_arg *args, size_t nargs,
        u8 *response, size_t capacity, size_t *received);

int remote_get(struct vtfs_storage *storage, u64 ino,
               struct vtfs_node_info *result);
int remote_lookup(struct vtfs_storage *storage, u64 parent_ino,
                  const char *name, size_t name_len, struct vtfs_node_info *result);
int remote_list(struct vtfs_storage *storage, u64 dir_ino, u64 *cursor,
                vtfs_list_callback callback, void *ctx);
int remote_create(struct vtfs_storage *storage, u64 parent_ino,
                  const char *name, size_t name_len, umode_t mode,
                  struct vtfs_node_info *result);
int remote_remove(struct vtfs_storage *storage, u64 parent_ino,
                  const char *name, size_t name_len, bool directory,
                  struct vtfs_node_info *result);
int remote_link(struct vtfs_storage *storage, u64 parent_ino,
                const char *name, size_t name_len, u64 target_ino,
                struct vtfs_node_info *result);
ssize_t remote_read(struct vtfs_storage *storage, u64 ino,
                    void *buffer, size_t count, loff_t offset);
ssize_t remote_write(struct vtfs_storage *storage, u64 ino,
                     const void *buffer, size_t count, loff_t offset,
                     struct vtfs_node_info *updated);
int remote_truncate(struct vtfs_storage *storage, u64 ino, loff_t size);
int remote_update_metadata(struct vtfs_storage *storage, u64 ino,
                           const struct vtfs_node_info *info);
void remote_forget(struct vtfs_storage *storage, u64 ino);

#endif /* VTFS_REMOTE_INTERNAL_H */
