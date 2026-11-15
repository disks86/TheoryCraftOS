#!/usr/bin/env bash
# Makes or updates the primary disk image of the OS: a FAT32 volume with the files that belong on the root volume of a system (see docs/FileSystemLayout.md).
# An image that exists keeps its files (it is the disk the OS writes to); the listed files are put on it again so that it follows the build.
# Usage: make-primary-image.sh <image> <size> <primary_image tool> [dir:/folder | file:/path/on/image=host/file]...
set -euo pipefail
image=$1 size=$2 tool=$3
shift 3
if [ ! -f "$image" ]; then
  truncate -s "$size" "$image"      # (sparse: the file takes the space of what is written to it)
  mkfs.fat -F 32 -n TCOS "$image" >/dev/null
else
  # An image is never made anew by itself (it holds what the OS wrote); one of another size than the build asks for is said so.
  wanted=$(numfmt --from=iec "$size" 2>/dev/null || echo 0)
  have=$(stat -c %s "$image")
  if [ "$wanted" -gt 0 ] && [ "$have" -ne "$wanted" ]; then
    echo "make-primary-image.sh: $image is $((have / 1048576)) MiB, the build asks for $size: delete it to have it made anew (its files are lost)" >&2
  fi
fi
exec "$tool" "$image" "$@"
