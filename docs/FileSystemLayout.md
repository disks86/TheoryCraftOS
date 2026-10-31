# Filesystem layout

The system filesystem (`SystemFileSystem`, the tree the file explorer and every program see) has a fixed layout. The paths are
named in [`src/FileSystem/SystemLayout.hpp`](../src/FileSystem/SystemLayout.hpp): use those names instead of spelling a path out.

| Path | What it is |
| --- | --- |
| `/` | The root of the filesystem: the primary partition (the first writable, built-in, non-boot volume). |
| `/EFI` | A mount of the EFI system partition. It is always called `EFI`, whatever its label says. |
| `/Desktop` | Everything that is on the user's desktop. |
| `/Desktop/Downloads` | Default location for what the browser downloads from the internet. |
| `/Desktop/Documents` | Default location for spreadsheets, text files and similar documents. |
| `/Desktop/Pictures` | Default location for image files. |
| `/Desktop/Video` | Default location for video files. |
| `/Desktop/Audio` | Default location for audio files such as music. |
| `/Windows` | Root for Windows user-mode processes: they see its contents as `C:\`. `/Windows/Windows/System32` (`C:\Windows\System32`) holds the system libraries (`ntdll.dll`, `kernel32.dll`, `msvcrt.dll`, `advapi32.dll`, `ws2_32.dll` and stub-only user32, gdi32, ... see [WindowsUserMode.md](WindowsUserMode.md)), which the loader reads from there (the kernel does not carry them); any other DLL or program put there is found the same way. `/Windows/Windows/System32/config` (`C:\Windows\System32\config`) is where `advapi32.dll` keeps the registry, a directory per key and a file per value (see [WindowsUserMode.md](WindowsUserMode.md)). |
| `/Linux` | Root for Linux processes: they see its contents as `/`. |
| `/AppStore` | Where applications keep the files they need. Each application has its own sub-folder, e.g. the browser's cache is `/AppStore/Browser/Cache` and the calendar's appointments are `/AppStore/Calendar/calendar.db` the to-do list is `/AppStore/Todo/todo.db` the Thunderbolt driver's device keys are `/AppStore/Thunderbolt/Keys/<uuid>` and the Bluetooth bonds (the keys of paired devices) are `/AppStore/Bluetooth/bonds.bin`. |

## Inside `/Windows` and `/Linux`

Both roots are created with the folders a fresh installation of the system has (`kWindowsFolders`, `kLinuxFolders`). Windows processes
see `/Windows` as `C:\`:

| Path | Seen as |
| --- | --- |
| `/Windows/Program Files`, `/Windows/Program Files (x86)` | `C:\Program Files`, `C:\Program Files (x86)` (each with `Common Files`) |
| `/Windows/ProgramData` | `C:\ProgramData` |
| `/Windows/Users` | `C:\Users` with `Default` and `Public` (`Desktop`, `Documents`, `Downloads`, `Music`, `Pictures`, `Videos`) |
| `/Windows/Windows` | `C:\Windows` with `Fonts`, `Logs`, `Temp`, `System32` (`drivers`, `config`) and `SysWOW64` |

Linux processes see `/Linux` as `/`, with the Filesystem Hierarchy Standard folders: `bin`, `boot`, `dev`, `etc`, `home`, `lib`, `lib64`,
`media`, `mnt`, `opt`, `proc`, `root`, `run`, `sbin`, `srv`, `sys`, `tmp`, `usr` (`bin`, `include`, `lib`, `lib64`, `local`, `sbin`, `share`)
and `var` (`cache`, `lib`, `log`, `tmp`). `dev`, `proc` and `sys` are plain empty folders for now. Use the `kWindows*` and `kLinux*`
constants for the places programs are installed (`kWindowsSystem32`, `kLinuxBin`, ...).

Other volumes (USB sticks, network shares) are folders of `/` named by their label.

## Rules for new code

- A default save location comes from the table above: browser downloads go to `/Desktop/Downloads`, documents to
  `/Desktop/Documents`, and so on. Use the constants in `filesystem::layout`, not string literals.
- An application stores its own files (caches, settings, databases, state) under `/AppStore/<Application>/`, never directly in `/` or in
  another application's folder. `layout::app_store_path()` builds the path and `layout::create_app_folder()` makes the folder.
- Do not create new top-level folders in `/`. If something does not fit the table, extend the table (this file and `SystemLayout.hpp`)
  in the same change.
- Windows and Linux processes get a view of their own root (`/Windows` as `C:\`, `/Linux` as `/`). Translate paths at the system-call
  boundary; a process must not be able to name a path outside its root.
- `/EFI` is a mount, not a folder of the primary partition. Do not create an `EFI` folder on the primary partition (it would be hidden by
  the mount).

## How it is built

`SystemFileSystem` mounts every volume on the machine. A volume that has an `/EFI` folder is the boot partition and is mounted as
`EFI`; the first writable built-in volume that is not a boot partition becomes the root volume, and the folders of the table (except
`/EFI`) are created on it when they are missing (`layout::create_folders()`). Existing folders and their contents are left alone.

With no primary partition (a machine with only the EFI partition, or no usable storage) the root is read-only and lists the volumes. On a
machine with no storage at all the in-memory fallback volume `Memory` holds the same folders itself (`/Memory/Desktop/...`); its contents
are lost at power-off.

## The primary disk image

The build makes `primary.img` (the `primary` target, part of the default build): a FAT32 volume that already holds the files an installed system has on its root volume, today the
three libraries in `/Windows/Windows/System32`. `tools/PrimaryImage.cpp` fills it with the OS's own FAT driver (`scripts/make-primary-image.sh` creates the volume with `mkfs.fat`
the first time). An existing image keeps its files, so the disk the OS writes to survives rebuilds; the build only puts the libraries on it again. The run targets and
`scripts/qemu-debug.sh` attach it as the primary disk. The size is `TCOS_PRIMARY_IMAGE_SIZE` (256M). A system without this disk has no Windows libraries, and Windows programs
that need them report that they cannot load them. The boot-time user-mode demo (`TCOS_USERMODE_DEMO`) has no disk and writes copies of the libraries into its RAM file system.
