#!/usr/bin/env bash
# Makes or updates the primary disk image of the OS: a FAT32 volume with the files that belong on the root volume of a system (see docs/FileSystemLayout.md).
# An image that exists keeps its files (it is the disk the OS writes to); the listed files are put on it again so that it follows the build.
# Usage: make-primary-image.sh <image> <size> <primary_image tool> [dir:/folder | file:/path/on/image=host/file]...
set -euo pipefail
image=$1 size=$2 tool=$3
shift 3
if [ ! -f "$image" ]; then
  truncate -s "$size" "$image"
  mkfs.fat -F 32 -n TCOS "$image" >/dev/null
fi
exec "$tool" "$image" "$@"
