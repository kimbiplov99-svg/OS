#ifndef FUSEFS_H
#define FUSEFS_H

#define _GNU_SOURCE
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

/* Compile-time constants */
#define BLOCK_SIZE          4096
#define FS_MAGIC            0xF05EF05Eu
#define MAX_INODES          1024
#define MAX_BLOCKS          65536
#define DIRECT_BLOCKS       12
#define MAX_FILENAME        252
#define ROOT_INUM           1
#define INVALID_BLOCK       0
#define INVALID_INUM        0

/* Fixed block layout */
#define SUPERBLOCK_BLK      0
#define INODE_BITMAP_BLK    1
#define DATA_BITMAP_BLK     2
#define INODE_TABLE_BLK     3

/* Derived sizes */
#define PTRS_PER_BLOCK      (BLOCK_SIZE / sizeof(uint32_t))
#define INODES_PER_BLOCK    (BLOCK_SIZE / sizeof(struct inode))
#define DIRENTS_PER_BLOCK   (BLOCK_SIZE / sizeof(struct dirent))

/* Inode types */
#define T_DIR   1
#define T_FILE  2
#define T_LINK  3

/* ---- On-disk structures ---- */

struct superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t block_size;
    uint32_t version;
    uint8_t  _pad[BLOCK_SIZE - 8 * sizeof(uint32_t)];
} __attribute__((packed));

struct inode {
    uint16_t type;
    uint16_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t size;
    uint32_t blocks;
    int64_t  atime;
    int64_t  mtime;
    int64_t  ctime;
    uint32_t direct[DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t dindirect;
    uint8_t  symlink[60];
    uint8_t  _pad[4];
} __attribute__((packed));

struct dirent {
    uint32_t inum;
    char     name[MAX_FILENAME];
} __attribute__((packed));

/* ---- In-memory block cache ---- */
#define CACHE_BLOCKS    64

struct cache_entry {
    uint32_t block_no;
    int      valid;
    int      dirty;
    uint64_t access_time;
    uint8_t  data[BLOCK_SIZE];
};

/* ---- Global filesystem state ---- */
struct fs_state {
    int              fd;
    int              no_cache;          /* 1 = bypass block cache (direct disk I/O) */
    struct superblock sb;
    struct cache_entry cache[CACHE_BLOCKS];
    pthread_mutex_t  lock;
    pthread_mutex_t  cache_lock;
    uint64_t         cache_hits;
    uint64_t         cache_misses;
};

extern struct fs_state gfs;

/* Disk I/O */
int      disk_read (uint32_t block_no, void *buf);
int      disk_write(uint32_t block_no, const void *buf);
void     disk_flush(void);

/* Block Cache */
int      cache_read (uint32_t block_no, void *buf);
int      cache_write(uint32_t block_no, const void *buf);
void     cache_flush(void);
void     cache_init (void);

/* Bitmaps */
int32_t  bitmap_alloc(uint32_t bitmap_block, uint32_t total);
void     bitmap_free (uint32_t bitmap_block, uint32_t bit);
int      bitmap_test (uint32_t bitmap_block, uint32_t bit);
void     bitmap_set  (uint32_t bitmap_block, uint32_t bit);

/* Inodes */
int32_t  inode_alloc  (uint16_t type, uint32_t mode, uint32_t uid, uint32_t gid);
void     inode_free   (uint32_t inum);
int      inode_read   (uint32_t inum, struct inode *ip);
int      inode_write  (uint32_t inum, const struct inode *ip);
uint32_t inode_bmap   (struct inode *ip, uint32_t file_block, int alloc);
int      inode_trunc  (uint32_t inum, struct inode *ip);
ssize_t  inode_read_data (uint32_t inum, struct inode *ip, void *buf, size_t size, off_t off);
ssize_t  inode_write_data(uint32_t inum, struct inode *ip, const void *buf, size_t size, off_t off);

/* Directory operations */
int32_t  dir_lookup(uint32_t dir_inum, const char *name);
int      dir_add   (uint32_t dir_inum, const char *name, uint32_t child_inum);
int      dir_remove(uint32_t dir_inum, const char *name);
int      dir_empty (uint32_t dir_inum);
uint32_t dir_init  (uint32_t inum, uint32_t parent_inum);

/* Path resolution */
int32_t  path_resolve(const char *path);
int32_t  path_resolve_parent(const char *path, char *name_out);

/* Superblock */
void     sb_write(void);
void     sb_read (void);

/* Format helper */
int      mkfs_create(const char *image_path, uint32_t total_blocks);

#endif /* FUSEFS_H */
