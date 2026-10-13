# FuseFS — A Userspace Filesystem

A mountable filesystem via FUSE backed by a single image file.

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install libfuse3-dev fuse3 pkg-config gcc

# Build
make

# Create a 16 MiB image
./tools/mkfs_tool fs.img 4096

# Mount
mkdir -p mnt
./fusefs fs.img mnt -f &

# Use it
echo "hello" > mnt/hello.txt
mkdir mnt/docs
cat mnt/hello.txt

# Unmount
fusermount3 -u mnt

# Run fsck
./tools/fsck_tool fs.img

# Run tests
make test

# Benchmark
make bench
```

## Features
- **Core**: create, read, write (incl. grow), truncate, mkdir, unlink, rmdir, rename
- **Persistence**: unmount and remount preserves all data
- **Large files**: single + double indirect blocks (up to ~4 GiB)
- **Hard links**: correct nlink tracking
- **Symbolic links**: inline symlink target in inode
- **Permissions**: chmod, chown, utimens
- **Block cache**: 64-entry write-back LRU cache
- **fsck**: validates bitmaps, inodes, directories, and block references
- **Signal-safe**: SIGINT/SIGTERM flush cache before exit

## Layout
```
include/fusefs.h     — on-disk structures & prototypes
src/fs_core.c        — disk I/O, block cache, superblock
src/bitmap.c         — bitmap allocator
src/inode.c          — inode CRUD + block map + data R/W
src/dir.c            — directory lookup/add/remove
src/path.c           — path resolution
src/fuse_ops.c       — FUSE callbacks + main
tools/mkfs.c         — image formatter
tools/fsck.c         — filesystem checker
tests/test_suite.sh  — full acceptance test
scripts/benchmark.sh — throughput benchmark
docs/design.md       — design document
```
