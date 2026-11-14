# AppImages

A Linux AppImage (`Something.AppImage`) runs like any other Linux program: start it from a terminal or open it from the desktop. The OS
does what the AppImage's own runtime would do, without running the runtime. It does not need FUSE, a `/dev/fuse` or a setuid helper.

## What happens

`desktop::run_program` (src/Program/ProgramRunner.cpp) looks at every ELF it is asked to run. If the ELF carries the AppImage mark
(`AI` and the type in bytes 8..10 of its identification), the runner hands it to `AppImageSession` (src/Program/AppImageLaunch.hpp):

1. **Find the image.** For type 2, the SquashFS (or, with uruntime, DwarFS) image starts where the runtime ELF ends. That is the end of its section header table or of
   its last section, whichever is later (`exe::appimage_payload_offset`, src/Executable/AppImage.hpp). A type 1 AppImage *is* an ISO 9660
   image, with the runtime in the 32 KiB system area that ISO 9660 leaves free, so its image starts at byte 0.
2. **Mount it.** `SquashFileSystem` (src/FileSystem/Squash), `DwarfsFileSystem` (src/FileSystem/Dwarfs) or, for type 1, `IsoFileSystem` (src/FileSystem/Iso) reads the image straight
   from the AppImage file. A `GraftFileSystem`
   (src/FileSystem/Mount) places it at `/tmp/.mount_<first six characters of the name><id>` of the Linux world, the way the runtime names its
   mount point. The graft is the program's own view of the OS file system, so nothing else sees the mount and nothing is left behind. It
   lasts as long as the program runs.

   Links that point out of the image work as they do through a Linux mount. An absolute target means that path in the Linux world, so
   `usr/share/zoneinfo -> /usr/share/zoneinfo` reaches `/Linux/usr/share/zoneinfo`. A `..` above the image's root climbs into
   `/tmp`, next to the mount point. The image's reader resolves the path through its own links (src/FileSystem/LinkWalk.hpp) and reports where
   it leaves the image (`IFileSystem::resolve_exit`). The graft carries on from there, and may come back into the image. A path may make at
   most 8 such jumps; one that bounces forever fails with TooManyLinks.
3. **Set the runtime's variables:** `APPDIR` (the mount point), `APPIMAGE` and `ARGV0` (the AppImage's path: inside the Linux world if it
   is there, otherwise the OS path), `OWD` (the starting folder).
4. **Work out what `AppRun` starts** (`evaluate_apprun`, src/Program/AppRunScript.hpp).
   - An ELF `AppRun`, or a link to one, is the program itself.
   - A shell-script `AppRun` (electron-builder, linuxdeploy, most hand-made AppImages) is interpreted by a small built-in shell. The Linux
     layer has no fork or exec, so real bash could neither run the command substitutions these scripts use nor hand over to the program.
     - The built-in shell covers variables, exports, arrays, parameter expansion, quoting, globbing, `if`/`while`/`for`, functions,
       `trap … EXIT`, `source`, `eval` and `test`/`[[ ]]`.
     - In `$( )` it supports dirname, basename, readlink -f, realpath, pwd, echo, printf, uname and id.
     - The script hands over through `exec prog …`, or by running a program that lies inside the image. That program is the result, with
       every exported variable. If that program is itself a script, it is interpreted in turn.
     - Any other command fails with status 127 and the script carries on, as a shell would.
   - If `AppRun` leads nowhere, the program named on the `Exec=` line of the root `.desktop` entry is started instead. It is looked for in
     `usr/bin`, the root, `bin` and `usr/sbin`, and gets the variables AppImageKit's `AppRun` binary would set (`PATH`, `LD_LIBRARY_PATH`,
     `XDG_DATA_DIRS`, …).
5. **Electron apps** (a `chrome-sandbox` next to the program) get `--no-sandbox`. Chromium's sandbox needs user namespaces or a setuid
   helper, and this OS has neither.

The program then runs as any dynamically linked Linux program (see LinuxDynamicPrograms.md): its interpreter and system libraries
(glibc, `ld-linux-x86-64.so.2`) come from `/Linux` as usual, and the libraries the AppImage bundles come from the mount through
`LD_LIBRARY_PATH` or the program's RPATH.

## SquashFS

All SquashFS 4.0 features mksquashfs writes are read:

- every compressor: gzip, LZMA, LZO, XZ (with BCJ filters), LZ4 and Zstandard. The decoders are in src/Media (`Zlib`, `Xz`, `Lzo`, `Lz4`,
  `Zstd`);
- basic and extended inodes, holes, fragments, large directories, hard links;
- symbolic links, followed in every component.

Extended attributes and the export table are ignored. Decompressed metadata and data blocks are cached (32 and 8 blocks). Decompression
runs outside the cache lock.

## DwarFS

AppImages built with [uruntime](https://github.com/VHSgunzo/uruntime), such as the "anylinux" builds of pkgforge-dev that the App Manager
installs (FeatherPad and many others), carry a DwarFS image instead of SquashFS. `DwarfsFileSystem` reads format 2.3 to 2.5 (mkdwarfs
0.5 and later):

- the sections: data blocks, the metadata schema and the metadata, each checked against its XXH3-64 checksum (src/Media/Xxh3) when read;
- block compression: none, Zstandard, LZMA (xz, with BCJ filters), LZ4 and LZ4HC;
- the metadata (src/FileSystem/Dwarfs/DwarfsMetadata.cpp): the Thrift compact-encoded Frozen2 schema, the bit-packed structure it
  describes, the packed directory, chunk and shared-file tables, and names and link targets compressed with FSST;
- deduplicated files, holes, hard links (the same `file_id`), devices, FIFOs and sockets (listed as specials), entries without a name
  (written by an mkdwarfs bug, skipped as DwarFS itself does), link targets written on Windows with `\`.

The metadata is decoded once at mount into plain tables. Blocks are often 16 or 32 MiB; a few decompressed blocks (as many as fit 64 MiB,
2 to 16) are cached. Brotli, FLAC and ricepp blocks give Unsupported when read. Older formats (2.2 and before) and images with a feature
flag this reader does not know do not mount (Unsupported).

## ISO 9660

`IsoFileSystem` reads the most capable tree an image has:

- **Rock Ridge:** POSIX names, modes, symbolic links, continuation areas, and the deep folders mkisofs relocates (CL/RE). Files compressed
  with zisofs (zlib blocks of 32, 64 or 128 KiB) are decompressed, keeping one block per open file.
- **Joliet** otherwise: UCS-2 names, turned into UTF-8.
- **Plain names** otherwise: without `;1`, in lower case, as Linux shows them.

Files of several extents are read as one. Interleaved files, zisofs2 and sparse (SF) files are refused with Unsupported.

## Tests

- `tests/run_host_tests.sh AppImageTest` builds with ASan and UBSan and covers:
  - SquashFS images the test writes itself with the reference compressor libraries (zlib, libzstd, liblzma, liblz4, liblzo2, each used when
    pkg-config finds it): 4 KiB and 128 KiB blocks, with and without fragments, holes and extended inodes, and stored uncompressed;
  - truncated and bit-flipped images;
  - 8 threads reading at once;
  - the AppImage header parser, including 32-bit, big-endian and damaged headers;
  - the graft;
  - AppRun scripts in the style of electron-builder and linuxdeploy, hand-written launchers and fuzzed scripts;
  - an end-to-end `AppImageSession` on a RAM file system;
  - ISO images made by `genisoimage` (Rock Ridge with relocated deep folders, Rock Ridge + Joliet, Joliet only, plain) and `xorriso`
    (Rock Ridge + Joliet, and zisofs with 32, 64 and 128 KiB blocks), plus truncated and bit-flipped ones and 8 threads on one shared
    compressed file. Each tool is used when it is installed;
  - a type 1 `AppImageSession` (an xorriso image with the runtime written over its system area);
  - links leaving a SquashFS image through the graft: absolute targets, `..` above the root, out and back in, a loop between the two,
    listings, `stat` and `readlink` of such links.
- `tests/run_host_tests.sh DwarfsTest` covers the DwarFS reader with the images of tests/data/dwarfs (from the DwarFS project, CC0),
  compared with the contents and times its own tests expect; those images with their blocks re-compressed (liblzma, liblz4) or stored
  plain; truncated and bit-flipped images and scrambled metadata with fixed-up checksums; 8 threads; XXH3 against libxxhash; a DwarFS
  AppImage through `open_appimage_image`. `DwarfsTest --dump <file>` lists a DwarFS image or AppImage.
- `ZstdTest` and `XzLz4Test` test the decoders against the zstd, xz and lz4 command-line tools and liblzo2.
- `tests/appimage_qemu.sh` packs `tests/linux/appimagetest.c` (static) with a bash-style `AppRun` twice: as a type 2 AppImage
  (`/Desktop/Downloads/Hello.AppImage`) and as a type 1 (`Hello1.AppImage`, ISO 9660 from xorriso). The kernel autoruns each in a QEMU boot
  of its own (its own build folder, `build-appimage-qemu`). Both pass 14 checks on the real kernel: the mount, the variables, AppRun's
  export, the arguments, a 1 MB file over many blocks, a listing, a link, an absolute link into `/Linux/etc`, and refusing writes.
- `tests/appimage_interop.sh <file.AppImage>[:pattern] ...` checks the reader against real AppImages. It extracts each one with its own
  runtime (`--appimage-extract`), compares every file, folder and link, then prints what AppRun would start. `AppImageTest --dump` and
  `--apprun` do the two halves without running the AppImage.

## What is not done

- **Type 1 AppImages have not been tried on a real one.** None is installed on the development machine, so the tests use images that
  xorriso makes, with a stand-in runtime.
- **Links in the base back into the mount.** A link elsewhere in `/Linux` whose target lies inside `/tmp/.mount_…` is not followed into
  the image, because the OS file system resolves it before the graft sees it.
- **AppRun scripts get no real process.** A script that needs a real external command to decide what to start (`sed`, `grep`, `awk` for
  something that matters) may pick the wrong program. `case` blocks are skipped, and `$(( ))` arithmetic and `${X:offset}` are not
  supported (the command that uses them is skipped).
- **No `fork` in the Linux layer.** `execve` works (LinuxDynamicPrograms.md), but a program that forks helper processes (Electron's zygote
  and renderers, a real shell running commands) still fails at that point.
- **No `/bin/sh`.** sharun-based AppImages (uruntime "anylinux" builds such as FeatherPad) now start: sharun finds itself through
  `/proc/self/exe`. Their sharun then runs `AppRun.sh` with the system shell and stops with "Failed to find a shell". A real shell would need
  `fork` for every `$( )`; the built-in AppRun interpreter could stand in, but that is not wired up (and its result for FeatherPad's hooks is
  not right yet: the hooks also `ln -s` into `/tmp`).
- **Desktop integration only through the App Manager** (AppManager.md): an AppImage started from a file gets no menu entry; there is no AppImageUpdate.
- **Only a small static test program has run in the OS.** Real desktop AppImages (Electron, GTK) have not been started in QEMU yet;
  they also need glibc and the system libraries in `/Linux` (see LinuxDynamicPrograms.md).
