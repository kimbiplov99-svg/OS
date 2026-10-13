#!/usr/bin/env bash
# FuseFS Complete 12-Week Test Battery
set -u

PASS=0
FAIL=0
MNT=/tmp/fusefs_test_mnt
IMG=/tmp/fusefs_test.img
BIN=../fusefs
MKFS=../tools/mkfs_tool
FSCK=../tools/fsck_tool

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
    rm -f "$IMG"
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ok() {
    echo "  [PASS] $1"
    PASS=$((PASS + 1))
}

fail() {
    echo "  [FAIL] $1"
    FAIL=$((FAIL + 1))
}

check_eq() {
    if [ "$1" = "$2" ]; then
        ok "$3"
    else
        fail "$3 (expected '$2', got '$1')"
    fi
}

echo "=========================================================="
echo "         FuseFS Complete Acceptance Test Suite           "
echo "=========================================================="

mkdir -p "$MNT"

# W1 & W2: Image Formatting & Superblock
echo "[Week 1-2] mkfs & mount initialization"
if "$MKFS" "$IMG" 8192 >/dev/null 2>&1; then
    ok "mkfs formatted 32MB image"
else
    fail "mkfs failed"
fi

"$BIN" "$IMG" "$MNT" -f -s &
FS_PID=$!
sleep 1.5

if mountpoint -q "$MNT"; then
    ok "filesystem mounted successfully"
else
    fail "failed to mount filesystem"
fi

# W3 & W5: File Creation, Writes, and Path Resolution
echo "[Week 3 & 5] File creation, write, read & path lookup"
echo "Hello Operating Systems!" > "$MNT/test.txt"
CONTENT=$(cat "$MNT/test.txt" 2>/dev/null || echo "")
check_eq "$CONTENT" "Hello Operating Systems!" "write and read back small file"

# W3: Directories & Hierarchy
echo "[Week 3] Directories (mkdir, nested subdirs, readdir)"
mkdir "$MNT/kernel"
mkdir -p "$MNT/kernel/fs/vfs"
echo "vfs layer" > "$MNT/kernel/fs/vfs/vfs.c"
CONTENT_NESTED=$(cat "$MNT/kernel/fs/vfs/vfs.c" 2>/dev/null || echo "")
check_eq "$CONTENT_NESTED" "vfs layer" "nested directory path navigation"

# W6 & W7: Redirection & File Descriptors
echo "[Week 7] Shell redirection (>>, >, <, pipes)"
echo "line 1" > "$MNT/pipe.txt"
echo "line 2" >> "$MNT/pipe.txt"
LINE_COUNT=$(wc -l < "$MNT/pipe.txt" | tr -d ' ')
check_eq "$LINE_COUNT" "2" "append and pipe read redirection"

# W8: Large File & Single Indirect Blocks
echo "[Week 8] Large file support (single-indirect blocks)"
dd if=/dev/zero of="$MNT/single_indirect.bin" bs=4096 count=200 status=none
SIZE_SI=$(stat -c%s "$MNT/single_indirect.bin" 2>/dev/null || echo 0)
check_eq "$SIZE_SI" "819200" "single-indirect block file size (800 KiB)"

# Distinction Bar: Double-Indirect Blocks (>1036 blocks = 4.2+ MiB)
echo "[Distinction Bar] Double-indirect block support (> 1036 blocks)"
dd if=/dev/zero of="$MNT/double_indirect.bin" bs=4096 count=1100 status=none
SIZE_DI=$(stat -c%s "$MNT/double_indirect.bin" 2>/dev/null || echo 0)
check_eq "$SIZE_DI" "4505600" "double-indirect block file size (4.3 MiB)"

# W8: Hard Links & Reference Counts
echo "[Week 8] Hard links & nlink reference counting"
echo "shared content" > "$MNT/hl_orig.txt"
ln "$MNT/hl_orig.txt" "$MNT/hl_link.txt"
NLINK=$(stat -c%h "$MNT/hl_orig.txt" 2>/dev/null || echo 0)
check_eq "$NLINK" "2" "inode nlink count increments on hard link"
check_eq "$(cat "$MNT/hl_link.txt")" "shared content" "hard link reads shared data"

# Symbolic Links
echo "[Stretch] Symbolic link resolution"
ln -s hl_orig.txt "$MNT/symlink.txt"
check_eq "$(cat "$MNT/symlink.txt")" "shared content" "symlink traversal"

# Truncate
echo "[Core] File truncation to zero"
truncate -s 0 "$MNT/single_indirect.bin"
SIZE_TRUNC=$(stat -c%s "$MNT/single_indirect.bin" 2>/dev/null || echo -1)
check_eq "$SIZE_TRUNC" "0" "truncate to 0 frees file blocks"

# Unlink & Rmdir
echo "[Core] Unlink & directory removal"
rm "$MNT/double_indirect.bin"
rm "$MNT/hl_orig.txt"
check_eq "$(cat "$MNT/hl_link.txt")" "shared content" "unlinking one hard link preserves data"
rm "$MNT/hl_link.txt"
rm -rf "$MNT/kernel"
if [ ! -e "$MNT/kernel" ]; then
    ok "rmdir recursive cleanup successful"
else
    fail "rmdir recursive cleanup failed"
fi

# W9: Full Persistence Across Unmount and Remount
echo "[Week 9] Full persistence (unmount -> remount)"
echo "persistence_key_12345" > "$MNT/persist.txt"
sync
fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
kill "$FS_PID" 2>/dev/null || true
wait "$FS_PID" 2>/dev/null || true
sleep 1

"$BIN" "$IMG" "$MNT" -f -s &
FS_PID=$!
sleep 1.5

PERSIST_DATA=$(cat "$MNT/persist.txt" 2>/dev/null || echo "")
check_eq "$PERSIST_DATA" "persistence_key_12345" "data persists across remount"

# W10 & W11: Concurrency (Multiple Processes Writing Concurrently)
echo "[Week 11] Multi-process concurrent stress test"
WORKER_PIDS=()
for i in $(seq 1 15); do
    ( echo "worker $i" > "$MNT/worker_${i}.dat" ) &
    WORKER_PIDS+=($!)
done
for wpid in "${WORKER_PIDS[@]}"; do
    wait "$wpid" 2>/dev/null || true
done
WORKER_FILES=$(ls "$MNT"/worker_*.dat 2>/dev/null | wc -l | tr -d ' ')
check_eq "$WORKER_FILES" "15" "concurrent writes from 15 processes without metadata race"

# Unmount cleanly
fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
kill "$FS_PID" 2>/dev/null || true
wait "$FS_PID" 2>/dev/null || true
sleep 1

# W12: FSCK Validation
echo "[Week 12] fsck on-disk validation"
if "$FSCK" "$IMG" >/dev/null 2>&1; then
    ok "fsck reports CLEAN on-disk filesystem"
else
    fail "fsck reported errors on-disk"
fi

echo "=========================================================="
echo "Final Score: $PASS PASSED, $FAIL FAILED"
echo "=========================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi
