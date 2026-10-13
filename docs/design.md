# FuseFS Architecture & Design Document

## 1. Executive Summary
**FuseFS** is a Unix userspace filesystem implementing a standard POSIX filesystem interface via libfuse3. It is backed by a single raw disk image file and manages block allocation, directory trees, inode tables, direct/indirect/double-indirect block indexing, in-memory caching, crash consistency, and integrity checking.

---

## 2. On-Disk Layout
The backing image is structured into contiguous fixed-size 4096-byte blocks:

```
+----------------+------------------+-----------------+-----------------------+---------------------+
|  Superblock    |   Inode Bitmap   |   Data Bitmap   |      Inode Table      |     Data Blocks     |
|   (Block 0)    |    (Block 1)     |    (Block 2)    |    (Blocks 3 .. 19)   |   (Blocks 20 .. N)  |
+----------------+------------------+-----------------+-----------------------+---------------------+
```

### 2.1 Block Regions
1. **Superblock (Block 0)**: Stores filesystem metadata: magic number (`0xF05EF05E`), total blocks, inode counts, free block counters, version, and the block offset where data begins (`data_start = 20`).
2. **Inode Bitmap (Block 1)**: Tracks inode allocation states. Bit $k$ corresponds to inode $k+1$.
3. **Data Block Bitmap (Block 2)**: Tracks data block allocation states. Bit $k$ corresponds to block $k + \text{data\_start}$.
4. **Inode Table (Blocks 3 to 19)**: Packed array of 64-byte `struct inode` entries (64 inodes per 4096-byte block, supporting 1024 total inodes across 16 blocks).
5. **Data Blocks (Block 20 to end)**: Contain user file contents, directory entry tables, and indirect pointer blocks.

---

## 3. Data Structures

### 3.1 Superblock (`struct superblock`)
```c
struct superblock {
    uint32_t magic;           // 0xF05EF05E
    uint32_t total_blocks;    // Total blocks in disk image
    uint32_t total_inodes;    // Max inodes (1024)
    uint32_t data_start;      // First data block index (Block 20)
    uint32_t free_blocks;     // Free data blocks counter
    uint32_t free_inodes;     // Free inode counter
    uint32_t block_size;      // 4096 bytes
    uint32_t version;         // 1
    uint8_t  _pad[4064];
};
```

### 3.2 Inode (`struct inode`)
Every file, directory, or symlink occupies a 64-byte inode:
```c
struct inode {
    uint16_t type;            // T_DIR = 1, T_FILE = 2, T_LINK = 3
    uint16_t nlink;           // Hard-link reference count
    uint32_t uid, gid;        // Ownership
    uint32_t mode;            // POSIX permission bits
    uint64_t size;            // Exact byte size
    uint32_t blocks;          // Number of allocated blocks
    int64_t  atime, mtime, ctime; // Timestamps (seconds since epoch)
    uint32_t direct[12];      // 12 Direct block pointers
    uint32_t indirect;        // 1 Single-indirect pointer
    uint32_t dindirect;       // 1 Double-indirect pointer
    uint8_t  symlink[60];     // Inline fast symlink buffer
    uint8_t  _pad[4];
};
```

### 3.3 File Capacity & Block Map (`bmap`)
* **12 Direct Blocks**: $12 \times 4096 = 48\text{ KiB}$
* **1 Single Indirect Block**: $1024 \times 4096 = 4\text{ MiB}$
* **1 Double Indirect Block**: $1024 \times 1024 \times 4096 = 4\text{ GiB}$
* **Maximum File Size**: $> 4\text{ GiB}$

---

## 4. Directory Structure
Directories are stored as sequences of 256-byte `struct dirent` records in allocated data blocks:
```c
struct dirent {
    uint32_t inum;               // Inode number (0 = free slot)
    char     name[252];          // Null-terminated name
};
```
Every directory initializes with `.` (pointing to itself) and `..` (pointing to parent). Inode 1 is designated as the root directory (`/`).

---

## 5. Write-Back Block Cache (`cache_entry`)
A 64-entry LRU write-back cache caches frequently accessed disk blocks:
* **Lookup**: O(1) linear associative scan across the 64 entries.
* **Hit**: Serves reads and updates dirty buffers without issuing kernel `pread`/`pwrite`.
* **Miss / Eviction**: Identifies lowest `access_time`; if dirty, flushes data to disk before recycling.
* **Sync & Persistence**: `cache_flush()` writes all dirty blocks and triggers `fsync()`.
* **Direct I/O Bypass**: Supports `--no-cache` / `FUSEFS_NOCACHE=1` for benchmarking and comparison.

---

## 6. Crash-Consistency & Ordering
To prevent persistent corruptions:
1. **Block Allocation**: Data block written and zeroed before inode pointer is updated.
2. **Metadata Ordering**: Inodes written before directory entries link to them.
3. **Directory Updates**: Added entries flushed before updating directory parent size.
4. **Unlink / Truncate**: Directory link removed first; inode reference decremented; blocks freed in bitmap and flushed.
5. **Recovery**: If a crash occurs mid-write, `fsck` detects unreferenced blocks as leaks and repairs them safely.

---

## 7. Syllabus Mapping (Weeks 1 to 12)

| Week | Concept Focus | Implemented Deliverable |
|---|---|---|
| W1 | Syscall Boundary & FUSE | FUSE daemon mount lifecycle, `main()`, `init()`, `destroy()` |
| W2 | C Toolchain & Memory Model | Superblock, inode table, and bitmap on-disk format |
| W3 | Lexing, Parsing & Dirs | Path walker (`path_resolve`), directory entries (`.`, `..`) |
| W4 | Mount Lifecycle & Concurrency | Multi-threaded FUSE handling with mutex synchronization |
| W5 | File Lookup & Status | `open`, `getattr`, POSIX error code returns (`ENOENT`, `EEXIST`, `ENOTDIR`) |
| W6 | Signal Handling | `SIGINT` and `SIGTERM` trap handlers with graceful `cache_flush` |
| W7 | Redirection & Pipes | End-to-end POSIX stream compatibility (`>`, `>>`, `<`, `|`) |
| W8 | Large Files & Indirects | Single and double indirect indexing in `inode_bmap` |
| W9 | On-Disk Persistence | Data survives unmount and remount verified by integration test |
| W10 | Block Cache | 64-entry LRU write-back cache with hit/miss metrics |
| W11 | Concurrency Safety | Multi-process concurrent write verification |
| W12 | fsck & Integration | Integrity validator, repair mode, benchmark suite |
