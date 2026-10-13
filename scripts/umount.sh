#!/usr/bin/env bash
MNT="${1:-mnt}"
fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null
echo "Unmounted $MNT"
