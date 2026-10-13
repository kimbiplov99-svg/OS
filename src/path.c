
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include "../include/fusefs.h"

/* Walk path from root; return inum or negative errno */
int32_t path_resolve(const char *path) {
    if (!path || path[0] != '/') return -EINVAL;
    uint32_t inum = ROOT_INUM;
    char buf[4096];
    strncpy(buf, path + 1, sizeof buf - 1);
    char *tok = strtok(buf, "/");
    while (tok) {
        int32_t child = dir_lookup(inum, tok);
        if (child < 0) return child;
        inum = (uint32_t)child;
        tok = strtok(NULL, "/");
    }
    return (int32_t)inum;
}

/* Resolve parent directory of path; writes last component into name_out */
int32_t path_resolve_parent(const char *path, char *name_out) {
    char buf[4096];
    strncpy(buf, path, sizeof buf - 1);
    char *slash = strrchr(buf, '/');
    if (!slash) return -EINVAL;
    strncpy(name_out, slash + 1, MAX_FILENAME - 1);
    if (slash == buf) {
        /* parent is root */
        return ROOT_INUM;
    }
    *slash = '\0';
    return path_resolve(buf);
}
