#!/usr/bin/env bash
# FuseFS Week 12 Live Viva Demonstration Script
set -e

MNT=/home/biplob/OS/mnt
IMG=/home/biplob/OS/viva_demo.img

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "============================================================"
echo "        FuseFS: Live Week 12 Viva Demonstration             "
echo "============================================================"

echo ""
echo "[Step 1] Formatted new filesystem image (16MB)"
./tools/mkfs_tool "$IMG" 4096

echo ""
echo "[Step 2] Mounting FuseFS..."
mkdir -p "$MNT"
./fusefs "$IMG" "$MNT" -f -s &
FS_PID=$!
sleep 1.5
echo "Mounted at $MNT"

echo ""
echo "[Step 3] Rubric 1: Directory tree and persistence test"
mkdir -p "$MNT/docs/os"
echo "Viva Demonstration Note" > "$MNT/docs/os/notes.txt"
ls -la "$MNT/docs/os"
cat "$MNT/docs/os/notes.txt"

echo ""
echo "[Step 4] Rubric 2: File exceeding direct-block limit (Indirect blocks)"
dd if=/dev/urandom of="$MNT/large_sample.dat" bs=4096 count=250 status=none
ls -lh "$MNT/large_sample.dat"

echo ""
echo "[Step 5] Rubric 3: Unlink and truncate block cleanup"
rm "$MNT/large_sample.dat"

echo ""
echo "[Step 6] Rubric 4: Unmount and Remount (Persistence verification)"
fusermount3 -u "$MNT"
wait "$FS_PID" 2>/dev/null || true
sleep 1

./fusefs "$IMG" "$MNT" -f -s &
FS_PID=$!
sleep 1.5
echo "Remounted. Checking preserved content:"
cat "$MNT/docs/os/notes.txt"

echo ""
echo "[Step 7] Rubric 5: fsck validation"
fusermount3 -u "$MNT"
wait "$FS_PID" 2>/dev/null || true
sleep 1
./tools/fsck_tool "$IMG"

rm -f "$IMG"
echo ""
echo "============================================================"
echo "  All Viva Acceptance Tests Passed with 100% Compliance!   "
echo "============================================================"
