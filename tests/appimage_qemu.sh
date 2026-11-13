#!/usr/bin/env bash
# Runs an AppImage in the real kernel under QEMU: tests/linux/appimagetest.c (static) packed with a shell AppRun into /Desktop/Downloads/Hello.AppImage
# on the primary disk, started by the kernel's autorun (TCOS_AUTORUN) with two arguments. Passes if the program's checks all pass. Uses its own build folder
# (build-appimage-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-appimage-qemu}
work=${TMPDIR:-/tmp}/tcos-appimage-qemu
rm -rf "$work"; mkdir -p "$work/app/usr/bin" "$work/app/usr/share/hello" "$work/app/usr/lib"
gcc -static -O1 -o "$work/app/usr/bin/appimagetest" tests/linux/appimagetest.c
printf 'data inside the image\n' > "$work/app/usr/share/hello/data.txt"
ln -s data.txt "$work/app/usr/share/hello/link.txt"
python3 -I -c "import sys; sys.stdout.buffer.write(bytes((i*7 + i//1000) & 255 for i in range(1000000)))" > "$work/app/usr/share/hello/big.bin"
cat > "$work/app/AppRun" <<'SH'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export GREETING="hello from AppRun"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${HERE}/usr/bin/appimagetest" "$@"
SH
chmod +x "$work/app/AppRun"
printf '[Desktop Entry]\nName=Hello\nExec=AppRun\nType=Application\n' > "$work/app/hello.desktop"
libs=""
for lib in zlib:ZLIB libzstd:ZSTD liblzma:LZMA liblz4:LZ4 lzo2:LZO; do
  if pkg-config --exists "${lib%%:*}" 2>/dev/null; then libs="$libs -DTCOS_HAVE_${lib##*:} $(pkg-config --cflags --libs "${lib%%:*}")"; fi
done
g++ -std=c++20 -O2 -I src -w tests/AppImageTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
    src/FileSystem/Squash/*.cpp src/FileSystem/Mount/GraftFileSystem.cpp src/Executable/AppImage.cpp src/Program/AppRunScript.cpp src/Program/AppImageLaunch.cpp \
    src/Process/ProgramEnvironment.cpp src/Media/Zlib.cpp src/Media/Zstd.cpp src/Media/Xz.cpp src/Media/Lz4.cpp src/Media/Lzo.cpp src/Crypto/Crypto.cpp $libs -o "$work/AppImageTest"
"$work/AppImageTest" --make "$work/Hello.AppImage" "$work/app"

cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Desktop/Downloads/Hello.AppImage "-DTCOS_AUTORUN_ARGUMENTS=one;two words" >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Desktop/Downloads/Hello.AppImage=$work/Hello.AppImage" >/dev/null
# The run-std target's QEMU command, without a window and with no sound, the debug console going to debugcon.log, for a minute and a half.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout 90 bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "appimagetest:|autorun:" "$build/debugcon.log" || { echo "no output from the program"; exit 1; }
grep -a -q "appimagetest: [0-9]* passed, 0 failed" "$build/debugcon.log"
