#!/usr/bin/env bash
# FuseFS Simulated Crash Consistency Test (Distinction Bar)
set -u

MNT=/tmp/fusefs_crash_mnt
IMG=/tmp/fusefs_crash.img
BIN=../fusefs
MKFS=../tools/mkfs_tool
FSCK=../tools/fsck_tool

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    rm -f "$IMG"
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=========================================================="
echo "  Distinction Bar: Crash-Consistency & Mid-Write Failure  "
echo "=========================================================="

mkdir -p "$MNT"
"$MKFS" "$IMG" 8192 >/dev/null 2>&1

"$BIN" "$IMG" "$MNT" -f -s &
FS_PID=$!
sleep 1.5

echo "1. Writing verified base files..."
echo "important transaction data" > "$MNT/base.txt"
sync

echo "2. Simulating sudden power loss / kill -9 during heavy write stream..."
( dd if=/dev/urandom of="$MNT/crash_target.bin" bs=4096 count=500 status=none ) &
WRITE_PID=$!
sleep 0.05
kill -9 "$FS_PID" 2>/dev/null || true
wait "$WRITE_PID" 2>/dev/null || true
wait "$FS_PID" 2>/dev/null || true
fusermount3 -u "$MNT" 2>/dev/null || true

echo "3. Running fsck on abruptly halted filesystem..."
"$FSCK" "$IMG" || true

echo "4. Running fsck auto-repair (-r) if uncommitted blocks were leaked..."
"$FSCK" -r "$IMG"

echo "5. Remounting after repair to verify base data survived unscathed..."
"$BIN" "$IMG" "$MNT" -f -s &
FS2_PID=$!
sleep 1.5

BASE_DATA=$(cat "$MNT/base.txt" 2>/dev/null || echo "")
if [ "$BASE_DATA" = "important transaction data" ]; then
    echo "  [PASS] Existing committed data remained 100% intact after simulated crash!"
else
    echo "  [FAIL] Base data was damaged"
    exit 1
fi

fusermount3 -u "$MNT" 2>/dev/null || true
wait "$FS2_PID" 2>/dev/null || true
echo "Crash consistency verification completed successfully."
