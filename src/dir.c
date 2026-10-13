
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <time.h>
#include "../include/fusefs.h"

int32_t dir_lookup(uint32_t dir_inum, const char *name) {
    struct inode ip;
    if (inode_read(dir_inum, &ip) < 0) return -EIO;
    if (ip.type != T_DIR) return -ENOTDIR;

    struct dirent de;
    uint64_t off = 0;
    while (off < ip.size) {
        ssize_t n = inode_read_data(dir_inum, &ip, &de, sizeof de, (off_t)off);
        if (n < (ssize_t)sizeof de) break;
        if (de.inum && strncmp(de.name, name, MAX_FILENAME) == 0)
            return (int32_t)de.inum;
        off += sizeof de;
    }
    return -ENOENT;
}

int dir_add(uint32_t dir_inum, const char *name, uint32_t child_inum) {
    struct inode ip;
    if (inode_read(dir_inum, &ip) < 0) return -EIO;

    /* Find empty slot */
    struct dirent de;
    uint64_t off = 0;
    int64_t  free_off = -1;
    while (off < ip.size) {
        ssize_t n = inode_read_data(dir_inum, &ip, &de, sizeof de, (off_t)off);
        if (n < (ssize_t)sizeof de) break;
        if (!de.inum) { free_off = (int64_t)off; break; }
        off += sizeof de;
    }
    if (free_off < 0) free_off = (int64_t)ip.size;

    memset(&de, 0, sizeof de);
    de.inum = child_inum;
    strncpy(de.name, name, MAX_FILENAME - 1);
    inode_write_data(dir_inum, &ip, &de, sizeof de, free_off);
    return 0;
}

int dir_remove(uint32_t dir_inum, const char *name) {
    struct inode ip;
    if (inode_read(dir_inum, &ip) < 0) return -EIO;

    struct dirent de;
    uint64_t off = 0;
    while (off < ip.size) {
        ssize_t n = inode_read_data(dir_inum, &ip, &de, sizeof de, (off_t)off);
        if (n < (ssize_t)sizeof de) break;
        if (de.inum && strncmp(de.name, name, MAX_FILENAME) == 0) {
            memset(&de, 0, sizeof de);
            inode_write_data(dir_inum, &ip, &de, sizeof de, (off_t)off);
            return 0;
        }
        off += sizeof de;
    }
    return -ENOENT;
}

int dir_empty(uint32_t dir_inum) {
    struct inode ip;
    inode_read(dir_inum, &ip);
    struct dirent de;
    uint64_t off = 0;
    while (off < ip.size) {
        ssize_t n = inode_read_data(dir_inum, &ip, &de, sizeof de, (off_t)off);
        if (n < (ssize_t)sizeof de) break;
        if (de.inum && strcmp(de.name, ".") && strcmp(de.name, ".."))
            return 0;
        off += sizeof de;
    }
    return 1;
}

uint32_t dir_init(uint32_t inum, uint32_t parent_inum) {
    struct inode ip;
    inode_read(inum, &ip);
    dir_add(inum, ".",  inum);
    dir_add(inum, "..", parent_inum);
    return inum;
}
