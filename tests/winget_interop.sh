#!/usr/bin/env bash
# Checks the App Manager's Windows side against the real winget source (https://cdn.winget.microsoft.com/cache): fetches source2.msix and reads its
# index (AppManTest with WINGET_SOURCE), then for each package named: fetches its version list and manifest the way an install does (each checked
# against the SHA-256 the step before names) and says which installer would be taken - and, for the ones after --install, downloads and installs them
# into a RAM file system and lists what the Start menu finds. Needs network access and curl.
# Usage: tests/winget_interop.sh [id...] [--install id...]
#   (no arguments: looks at Mozilla.Firefox, Notepad++.Notepad++, 7zip.7zip and Allen&Heath.SQMixPad; installs jqlang.jq and
#    Notepad++.Notepad++)
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/tcos-winget-interop
mkdir -p "$out"
look=() install=()
into=look
for a in "$@"; do
  if [ "$a" = --install ]; then into=install; elif [ $into = look ]; then look+=("$a"); else install+=("$a"); fi
done
if [ $# -eq 0 ]; then
  look=(Mozilla.Firefox Notepad++.Notepad++ 7zip.7zip 'Allen&Heath.SQMixPad')
  install=(jqlang.jq Notepad++.Notepad++)
fi

libs=""
for lib in zlib:ZLIB libzstd:ZSTD liblzma:LZMA liblz4:LZ4 lzo2:LZO; do
  if pkg-config --exists "${lib%%:*}" 2>/dev/null; then libs="$libs -DTCOS_HAVE_${lib##*:} $(pkg-config --cflags --libs "${lib%%:*}")"; fi
done
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -w tests/AppManTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp \
    src/FileSystem/Ram/*.cpp src/FileSystem/Squash/*.cpp src/FileSystem/Dwarfs/*.cpp src/FileSystem/Iso/*.cpp src/FileSystem/Mount/GraftFileSystem.cpp src/Executable/AppImage.cpp src/Program/AppRunScript.cpp \
    src/Program/ShellTools.cpp src/Program/AppImageLaunch.cpp src/Regex/Regex.cpp src/Process/ProgramEnvironment.cpp src/Media/Zlib.cpp src/Media/Zip.cpp src/Media/Zstd.cpp src/Media/Xxh3.cpp src/Media/Xz.cpp \
    src/Media/Lz4.cpp src/Media/Lzo.cpp src/Crypto/Crypto.cpp src/Desktop/AppMan/AppManCatalog.cpp src/Desktop/AppMan/AppManInstaller.cpp src/Desktop/AppMan/InstallFiles.cpp \
    src/Msi/MsiDatabase.cpp src/Msi/MsiExpression.cpp src/Msi/MsiInstaller.cpp src/Media/CompoundFile.cpp src/Media/Cabinet.cpp src/Media/Lzx.cpp \
    src/Desktop/AppMan/SqliteReader.cpp src/Desktop/AppMan/WingetCatalog.cpp src/Desktop/AppMan/WingetInstaller.cpp src/Desktop/AppMan/Yaml.cpp src/Desktop/InstalledApps.cpp \
    src/Executable/ExecutableKind.cpp src/Executable/ElfFile.cpp src/Executable/PeFile.cpp $libs -o "$out/AppManTest"

curl -sfL --max-time 300 -o "$out/source2.msix" https://cdn.winget.microsoft.com/cache/source2.msix
echo "== the index ($(stat -c %s "$out/source2.msix") bytes)"
WINGET_SOURCE="$out/source2.msix" "$out/AppManTest" | grep -E "real source|checks"
if [ ${#look[@]} -gt 0 ]; then
  echo "== manifests"
  "$out/AppManTest" --winget "$out/source2.msix" "${look[@]}"
fi
if [ ${#install[@]} -gt 0 ]; then
  echo "== installs"
  "$out/AppManTest" --winget-install "$out/source2.msix" "${install[@]}"
fi
echo "winget_interop: PASSED"
