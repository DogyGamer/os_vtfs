
#ifndef VTFS_HTTP_BACKEND_H
#define VTFS_HTTP_BACKEND_H

#include "../../vtfs_storage.h"

int vtfs_http_init(struct vtfs_storage *storage,
                   const char *host, unsigned int port,
                   const char *token);

#endif
