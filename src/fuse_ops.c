#define _GNU_SOURCE
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include "../include/fusefs.h"

/* ---- getattr ---- */
static int fs_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    int r = inode_read((uint32_t)inum, &ip);
    pthread_mutex_unlock(&gfs.lock);
    if (r < 0) return r;

    memset(st, 0, sizeof *st);
    st->st_ino   = (ino_t)inum;
    st->st_uid   = ip.uid;
    st->st_gid   = ip.gid;
    st->st_nlink = ip.nlink;
    st->st_size  = (off_t)ip.size;
    st->st_atime = (time_t)ip.atime;
    st->st_mtime = (time_t)ip.mtime;
    st->st_ctime = (time_t)ip.ctime;
    st->st_blksize = BLOCK_SIZE;
    st->st_blocks  = (ip.size + 511) / 512;

    if (ip.type == T_DIR)
        st->st_mode = S_IFDIR | (ip.mode ? ip.mode : 0755);
    else if (ip.type == T_LINK)
        st->st_mode = S_IFLNK | 0777;
    else
        st->st_mode = S_IFREG | (ip.mode ? ip.mode : 0644);
    return 0;
}

/* ---- readdir ---- */
static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (ip.type != T_DIR) { pthread_mutex_unlock(&gfs.lock); return -ENOTDIR; }

    struct dirent de;
    uint64_t off = 0;
    while (off < ip.size) {
        ssize_t n = inode_read_data((uint32_t)inum, &ip, &de, sizeof de, (off_t)off);
        if (n < (ssize_t)sizeof de) break;
        if (de.inum) filler(buf, de.name, NULL, 0, 0);
        off += sizeof de;
    }
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- open ---- */
static int fs_open(const char *path, struct fuse_file_info *fi) {
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    pthread_mutex_unlock(&gfs.lock);
    if (inum < 0) return inum;
    fi->fh = (uint64_t)inum;
    return 0;
}

/* ---- read ---- */
static int fs_read(const char *path, char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    pthread_mutex_lock(&gfs.lock);
    uint32_t inum = fi->fh ? (uint32_t)fi->fh : (uint32_t)path_resolve(path);
    struct inode ip;
    inode_read(inum, &ip);
    ssize_t n = inode_read_data(inum, &ip, buf, size, off);
    pthread_mutex_unlock(&gfs.lock);
    return (int)n;
}

/* ---- write ---- */
static int fs_write(const char *path, const char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi) {
    pthread_mutex_lock(&gfs.lock);
    uint32_t inum = fi->fh ? (uint32_t)fi->fh : (uint32_t)path_resolve(path);
    struct inode ip;
    inode_read(inum, &ip);
    ssize_t n = inode_write_data(inum, &ip, buf, size, off);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return (int)n;
}

/* ---- create ---- */
static int fs_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
    pthread_mutex_lock(&gfs.lock);
    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(path, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }

    int32_t existing = dir_lookup((uint32_t)parent, name);
    if (existing >= 0) { pthread_mutex_unlock(&gfs.lock); return -EEXIST; }

    struct fuse_context *ctx = fuse_get_context();
    int32_t inum = inode_alloc(T_FILE, mode, ctx->uid, ctx->gid);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    dir_add((uint32_t)parent, name, (uint32_t)inum);
    cache_flush();
    if (fi) fi->fh = (uint64_t)inum;
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- mkdir ---- */
static int fs_mkdir(const char *path, mode_t mode) {
    pthread_mutex_lock(&gfs.lock);
    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(path, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }

    struct fuse_context *ctx = fuse_get_context();
    int32_t inum = inode_alloc(T_DIR, mode | S_IFDIR, ctx->uid, ctx->gid);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    dir_init((uint32_t)inum, (uint32_t)parent);
    dir_add((uint32_t)parent, name, (uint32_t)inum);

    struct inode pip;
    inode_read((uint32_t)parent, &pip);
    pip.nlink++;
    pip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)parent, &pip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- unlink ---- */
static int fs_unlink(const char *path) {
    pthread_mutex_lock(&gfs.lock);
    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(path, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }
    int32_t inum = dir_lookup((uint32_t)parent, name);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }

    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (ip.type == T_DIR) { pthread_mutex_unlock(&gfs.lock); return -EISDIR; }

    dir_remove((uint32_t)parent, name);
    ip.nlink--;
    if (ip.nlink == 0) {
        inode_trunc((uint32_t)inum, &ip);
        inode_free((uint32_t)inum);
    } else {
        ip.ctime = (int64_t)time(NULL);
        inode_write((uint32_t)inum, &ip);
    }
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- rmdir ---- */
static int fs_rmdir(const char *path) {
    pthread_mutex_lock(&gfs.lock);
    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(path, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }
    int32_t inum = dir_lookup((uint32_t)parent, name);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }

    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (ip.type != T_DIR) { pthread_mutex_unlock(&gfs.lock); return -ENOTDIR; }
    if (!dir_empty((uint32_t)inum)) { pthread_mutex_unlock(&gfs.lock); return -ENOTEMPTY; }

    dir_remove((uint32_t)parent, name);
    inode_trunc((uint32_t)inum, &ip);
    inode_free((uint32_t)inum);

    struct inode pip;
    inode_read((uint32_t)parent, &pip);
    pip.nlink--;
    pip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)parent, &pip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- truncate ---- */
static int fs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (size == 0) {
        inode_trunc((uint32_t)inum, &ip);
    } else if ((uint64_t)size < ip.size) {
        uint32_t new_blocks = (uint32_t)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
        uint32_t old_blocks = (uint32_t)((ip.size + BLOCK_SIZE - 1) / BLOCK_SIZE);
        for (uint32_t b = new_blocks; b < old_blocks; b++) {
            uint32_t bno = inode_bmap(&ip, b, 0);
            if (bno != INVALID_BLOCK) {
                bitmap_free(DATA_BITMAP_BLK, bno - gfs.sb.data_start);
                gfs.sb.free_blocks++;
            }
        }
        ip.size = (uint64_t)size;
        ip.ctime = ip.mtime = (int64_t)time(NULL);
        inode_write((uint32_t)inum, &ip);
        sb_write();
    } else {
        ip.size = (uint64_t)size;
        ip.ctime = ip.mtime = (int64_t)time(NULL);
        inode_write((uint32_t)inum, &ip);
    }
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- rename ---- */
static int fs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    pthread_mutex_lock(&gfs.lock);
    char from_name[MAX_FILENAME], to_name[MAX_FILENAME];
    int32_t from_parent = path_resolve_parent(from, from_name);
    int32_t to_parent   = path_resolve_parent(to,   to_name);
    if (from_parent < 0 || to_parent < 0) {
        pthread_mutex_unlock(&gfs.lock);
        return from_parent < 0 ? from_parent : to_parent;
    }
    int32_t inum = dir_lookup((uint32_t)from_parent, from_name);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }

    int32_t old_dest = dir_lookup((uint32_t)to_parent, to_name);
    if (old_dest >= 0) {
        struct inode odip;
        inode_read((uint32_t)old_dest, &odip);
        dir_remove((uint32_t)to_parent, to_name);
        odip.nlink--;
        if (odip.nlink == 0) {
            inode_trunc((uint32_t)old_dest, &odip);
            inode_free((uint32_t)old_dest);
        } else {
            inode_write((uint32_t)old_dest, &odip);
        }
    }

    dir_add((uint32_t)to_parent, to_name, (uint32_t)inum);
    dir_remove((uint32_t)from_parent, from_name);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- link ---- */
static int fs_link(const char *from, const char *to) {
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(from);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (ip.type == T_DIR) { pthread_mutex_unlock(&gfs.lock); return -EPERM; }

    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(to, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }
    dir_add((uint32_t)parent, name, (uint32_t)inum);
    ip.nlink++;
    ip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)inum, &ip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- symlink / readlink ---- */
static int fs_symlink(const char *target, const char *linkpath) {
    pthread_mutex_lock(&gfs.lock);
    char name[MAX_FILENAME];
    int32_t parent = path_resolve_parent(linkpath, name);
    if (parent < 0) { pthread_mutex_unlock(&gfs.lock); return parent; }
    struct fuse_context *ctx = fuse_get_context();
    int32_t inum = inode_alloc(T_LINK, 0777, ctx->uid, ctx->gid);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    strncpy((char*)ip.symlink, target, sizeof ip.symlink - 1);
    ip.size = strlen(target);
    inode_write((uint32_t)inum, &ip);
    dir_add((uint32_t)parent, name, (uint32_t)inum);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

static int fs_readlink(const char *path, char *buf, size_t size) {
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    pthread_mutex_unlock(&gfs.lock);
    if (ip.type != T_LINK) return -EINVAL;
    strncpy(buf, (char*)ip.symlink, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

/* ---- chmod / chown ---- */
static int fs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    ip.mode  = mode;
    ip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)inum, &ip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

static int fs_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    if (uid != (uid_t)-1) ip.uid = uid;
    if (gid != (gid_t)-1) ip.gid = gid;
    ip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)inum, &ip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- utimens ---- */
static int fs_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&gfs.lock);
    int32_t inum = path_resolve(path);
    if (inum < 0) { pthread_mutex_unlock(&gfs.lock); return inum; }
    struct inode ip;
    inode_read((uint32_t)inum, &ip);
    ip.atime = (int64_t)tv[0].tv_sec;
    ip.mtime = (int64_t)tv[1].tv_sec;
    ip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)inum, &ip);
    cache_flush();
    pthread_mutex_unlock(&gfs.lock);
    return 0;
}

/* ---- statfs ---- */
static int fs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    stbuf->f_bsize   = BLOCK_SIZE;
    stbuf->f_frsize  = BLOCK_SIZE;
    stbuf->f_blocks  = gfs.sb.total_blocks - gfs.sb.data_start;
    stbuf->f_bfree   = gfs.sb.free_blocks;
    stbuf->f_bavail  = gfs.sb.free_blocks;
    stbuf->f_files   = gfs.sb.total_inodes;
    stbuf->f_ffree   = gfs.sb.free_inodes;
    stbuf->f_namemax = MAX_FILENAME - 1;
    return 0;
}

/* ---- flush / release / fsync ---- */
static int fs_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    cache_flush();
    return 0;
}

static int fs_release(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    cache_flush();
    return 0;
}

static int fs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi;
    cache_flush();
    return 0;
}

/* ---- init / destroy ---- */
static void *fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino          = 1;
    cfg->kernel_cache     = 0;
    cfg->entry_timeout    = 0.0;
    cfg->attr_timeout     = 0.0;
    cfg->negative_timeout = 0.0;
    return NULL;
}

static void fs_destroy(void *private_data) {
    (void)private_data;
    cache_flush();
    fprintf(stderr, "[fusefs] Cache stats: hits=%lu misses=%lu (no_cache=%d)\n",
            gfs.cache_hits, gfs.cache_misses, gfs.no_cache);
    close(gfs.fd);
}

/* Signal handler for clean unmount on SIGINT/SIGTERM */
static void handle_signal(int sig) {
    (void)sig;
    cache_flush();
    fuse_exit(fuse_get_context()->fuse);
}

static const struct fuse_operations fs_ops = {
    .getattr   = fs_getattr,
    .readdir   = fs_readdir,
    .open      = fs_open,
    .read      = fs_read,
    .write     = fs_write,
    .create    = fs_create,
    .mkdir     = fs_mkdir,
    .unlink    = fs_unlink,
    .rmdir     = fs_rmdir,
    .truncate  = fs_truncate,
    .rename    = fs_rename,
    .link      = fs_link,
    .symlink   = fs_symlink,
    .readlink  = fs_readlink,
    .chmod     = fs_chmod,
    .chown     = fs_chown,
    .utimens   = fs_utimens,
    .statfs    = fs_statfs,
    .flush     = fs_flush,
    .release   = fs_release,
    .fsync     = fs_fsync,
    .init      = fs_init,
    .destroy   = fs_destroy,
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image> <mountpoint> [--no-cache] [fuse options]\n", argv[0]);
        return 1;
    }

    const char *image = argv[1];

    /* Environment variable or command line check for no_cache */
    if (getenv("FUSEFS_NOCACHE") != NULL) {
        gfs.no_cache = 1;
    }

    /* Filter arguments for fuse_main */
    char *new_argv[64];
    int new_argc = 0;
    new_argv[new_argc++] = argv[0];

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            gfs.no_cache = 1;
        } else {
            new_argv[new_argc++] = argv[i];
        }
    }
    new_argv[new_argc] = NULL;

    gfs.fd = open(image, O_RDWR);
    if (gfs.fd < 0) {
        perror("open image");
        return 1;
    }

    sb_read();
    if (gfs.sb.magic != FS_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08X (expected 0x%08X)\n",
                gfs.sb.magic, FS_MAGIC);
        return 1;
    }

    pthread_mutex_init(&gfs.lock, NULL);
    pthread_mutex_init(&gfs.cache_lock, NULL);
    cache_init();

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stderr, "[fusefs] Mounting %s on %s (cache: %s)\n",
            image, new_argv[1], gfs.no_cache ? "DISABLED" : "ENABLED");
    return fuse_main(new_argc, new_argv, &fs_ops, NULL);
}
