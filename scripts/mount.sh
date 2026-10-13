#!/usr/bin/env bash
# Mount a FuseFS image
IMG="${1:-fs.img}"
MNT="${2:-mnt}"
mkdir -p "$MNT"
./fusefs "$IMG" "$MNT" -f -o allow_other "$@"
