#!/usr/bin/env bash
# Makes or updates the primary disk image of the OS: a FAT32 volume with the files that belong on the root volume of a system (see docs/FileSystemLayout.md).
# An image that exists keeps its files (it is the disk the OS writes to); the listed files are put on it again so that it follows the build.
# An image smaller than the build asks for is grown: a new volume of the wanted size is made and the old one's files are copied onto it (with mtools).
# Usage: make-primary-image.sh <image> <size> <primary_image tool> [dir:/folder | file:/path/on/image=host/file]...
set -euo pipefail
image=$1 size=$2 tool=$3
shift 3

make_volume() {      # <file> <size>
  truncate -s "$2" "$1"      # (sparse: the file takes the space of what is written to it)
  mkfs.fat -F 32 -n TCOS "$1" >/dev/null
}

if [ ! -f "$image" ]; then
  make_volume "$image" "$size"
else
  wanted=$(numfmt --from=iec "$size" 2>/dev/null || echo 0)
  have=$(stat -c %s "$image")
  if [ "$wanted" -gt "$have" ] && command -v mcopy >/dev/null; then
    # Grown, not made anew: what the OS wrote to it (installed applications, settings) is kept. Nothing is replaced until the copy succeeded.
    work=$(mktemp -d "${image}.grow.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    mkdir "$work/files"
    if [ -n "$(mdir -i "$image" -b ::/ 2>/dev/null)" ]; then
      MTOOLS_SKIP_CHECK=1 mcopy -s -p -m -n -i "$image" '::/*' "$work/files/"
    fi
    make_volume "$work/new.img" "$size"
    if [ -n "$(ls -A "$work/files")" ]; then
      MTOOLS_SKIP_CHECK=1 mcopy -s -p -m -n -i "$work/new.img" "$work/files/"* ::/
    fi
    mv -f "$work/new.img" "$image"
    rm -rf "$work"
    trap - EXIT
    echo "make-primary-image.sh: grew $image from $((have / 1048576)) MiB to $size, keeping its files" >&2
  elif [ "$wanted" -gt 0 ] && [ "$have" -ne "$wanted" ]; then
    # (smaller than it is, or no mtools to copy with: never made anew by itself, since it holds what the OS wrote)
    echo "make-primary-image.sh: $image is $((have / 1048576)) MiB, the build asks for $size: delete it to have it made anew (its files are lost)" >&2
  fi
fi
exec "$tool" "$image" "$@"
