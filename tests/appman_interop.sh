#!/usr/bin/env bash
# Checks the App Manager's reading of real AppMan install scripts: for each application (names given, or N picked at random from AppMan's catalog), its
# script (programs/x86_64/<name> of https://github.com/ivan-hc/AM) has its download lines worked out by bash and by the OS's built-in shell
# (AppManTest --resolve), and the two links must be the same. curl is answered from a cache in both, filled by one real request per address (GitHub's API
# allows 60 an hour without a token). Also parses the real catalog. Needs network access.
# Usage: tests/appman_interop.sh [-n N] [name...]
set -euo pipefail
cd "$(dirname "$0")/.."
count=20
if [ "${1:-}" = -n ]; then count=$2; shift 2; fi
out=${TMPDIR:-/tmp}/tcos-appman-interop
mkdir -p "$out/cache" "$out/bin" "$out/scripts"
repo=https://raw.githubusercontent.com/ivan-hc/AM/main/programs

libs=""
for lib in zlib:ZLIB libzstd:ZSTD liblzma:LZMA liblz4:LZ4 lzo2:LZO; do
  if pkg-config --exists "${lib%%:*}" 2>/dev/null; then libs="$libs -DTCOS_HAVE_${lib##*:} $(pkg-config --cflags --libs "${lib%%:*}")"; fi
done
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -w tests/AppManTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp \
    src/FileSystem/Ram/*.cpp src/FileSystem/Squash/*.cpp src/FileSystem/Iso/*.cpp src/FileSystem/Mount/GraftFileSystem.cpp src/Executable/AppImage.cpp src/Program/AppRunScript.cpp \
    src/Program/ShellTools.cpp src/Program/AppImageLaunch.cpp src/Regex/Regex.cpp src/Process/ProgramEnvironment.cpp src/Media/Zlib.cpp src/Media/Zstd.cpp src/Media/Xz.cpp \
    src/Media/Lz4.cpp src/Media/Lzo.cpp src/Crypto/Crypto.cpp src/Desktop/AppMan/AppManCatalog.cpp src/Desktop/AppMan/AppManInstaller.cpp src/Desktop/InstalledApps.cpp \
    src/Executable/ExecutableKind.cpp src/Executable/ElfFile.cpp src/Executable/PeFile.cpp $libs -o "$out/AppManTest"

# curl for bash: the same cache, filled on a miss.
cat > "$out/bin/curl" <<SH
#!/usr/bin/env bash
for a in "\$@"; do case "\$a" in http*) url="\$a";; esac; done
key=\$(printf %s "\$url" | tr -c 'A-Za-z0-9' '_' | cut -c1-200)
[ -f "$out/cache/\$key" ] || /usr/bin/curl -Ls "\$url" > "$out/cache/\$key"
cat "$out/cache/\$key"
SH
chmod +x "$out/bin/curl"

curl -Ls "$repo/stats-appimages" > "$out/stats-appimages"
APPMAN_CATALOG="$out/stats-appimages" "$out/AppManTest" | grep -E "real catalog|AppManTest:"
if [ $# -gt 0 ]; then names=("$@"); else mapfile -t names < <(sed -n 's/^◆ \([^ ]*\) : .*/\1/p' "$out/stats-appimages" | shuf -n "$count"); fi

same=0 differ=0 empty=0
for name in "${names[@]}"; do
  curl -Ls "$repo/x86_64/$name" > "$out/scripts/$name"
  "$out/AppManTest" --lines "$out/scripts/$name" > "$out/scripts/$name.lines" || { echo "  $name: no version line"; differ=$((differ + 1)); continue; }
  want=$(PATH="$out/bin:$PATH" bash -c "$(cat "$out/scripts/$name.lines")"$'\n''printf %s "$version"' 2>/dev/null || true)
  got=$("$out/AppManTest" --resolve "$out/scripts/$name" "$out/cache" || true)
  [ "$got" = "<unset>" ] && got=""
  if [ "$want" = "$got" ]; then
    same=$((same + 1))
    [ -z "$want" ] && empty=$((empty + 1))
    echo "  $name: ${want:-<nothing (the site gave no link)>}"
  else
    differ=$((differ + 1))
    echo "  $name: DIFFERS"; echo "    bash: $want"; echo "    ours: $got"
  fi
done
echo "appman interop: $same the same ($empty of them empty in both), $differ differ"
[ $differ -eq 0 ]
