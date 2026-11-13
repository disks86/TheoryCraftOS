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
| `/AppStore` | Where applications keep the files they need. Each application has its own sub-folder, e.g. the browser's cache is `/AppStore/Browser/Cache` and the calendar's appointments are `/AppStore/Calendar/calendar.db` the to-do list is `/AppStore/Todo/todo.db` the App Manager keeps its copy of the AppMan catalog in `/AppStore/AppMan/stats-appimages` the Thunderbolt driver's device keys are `/AppStore/Thunderbolt/Keys/<uuid>` and the Bluetooth bonds (the keys of paired devices) are `/AppStore/Bluetooth/bonds.bin`, the sound server's remembered stream volumes are `/AppStore/PipeWire/stream-restore.bin` and the last scheduled trim of the volumes is in `/AppStore/FileSystem/fstrim.state`. |

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
and `var` (`cache`, `lib`, `log`, `tmp`). `dev`, `proc` and `sys` are plain empty folders for now; the kernel answers a few names under `/dev` itself (`null`, `zero`, `random`, `urandom`, `tty`, and `dxg`: the
kernel-mode Direct3D layer, see [Vulkan.md](Vulkan.md)) without files behind them. The kernel's X server listens at `/tmp/.X11-unix/X0`
(`/Linux/tmp/.X11-unix/X0`, `kX11SocketFolder`; see [X11.md](X11.md)), which it makes at start. The kernel's Wayland compositor listens at `/run/user/0/wayland-0` (`XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` of every
Linux program; `/Linux/run/user/0/wayland-0`, `kRuntimeFolder`; see [Wayland.md](Wayland.md)), also made at start, and the kernel's PipeWire listens there too, at `pipewire-0` and `pulse/native` (see
[PipeWire.md](PipeWire.md)). The kernel's D-Bus session bus is at `/run/user/0/bus` (`DBUS_SESSION_BUS_ADDRESS`, `kSessionBusSocket`) and its system bus
at the standard `/run/dbus/system_bus_socket` (`/Linux/run/dbus`, `kSystemBusFolder`; see [DBus.md](DBus.md)). The buses start services from the
standard `dbus-1/services` and `dbus-1/system-services` folders (`kSessionServicesFolder`, ...), and run a service's systemd unit from
`/etc/systemd/{system,user}` or `/usr/lib/systemd/{system,user}` (`kSystemUnitsFolder`, ...). Use the `kWindows*` and `kLinux*`
constants for the places programs are installed (`kWindowsSystem32`, `kLinuxBin`, ...).

Installed applications appear in the desktop's Start menu as "Name (Windows)" or "Name (Linux)" (`src/Desktop/InstalledApps.hpp`). They are found
the way those systems' own menus find them: Windows shortcuts (`.lnk`) under `/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs`
(`kWindowsStartMenuPrograms`) and each user's `AppData/Roaming/Microsoft/Windows/Start Menu/Programs` (`kWindowsUserStartMenuPrograms`), and Linux
desktop entries (`.desktop`) under `/usr/share/applications`, `/usr/local/share/applications` and root's `~/.local/share/applications`
(`kLinuxApplications`, `kLinuxLocalApplications`, `kLinuxRootApplications`). An installer that puts its shortcut or desktop entry there is listed
the next time the Start menu opens; only entries whose program is a PE or ELF file on the disk are shown, and uninstallers are left out.

Device firmware follows the Linux convention: `/Linux/lib/firmware` (`kLinuxFirmware`), seen by Linux processes as `/lib/firmware`. The Intel NPU driver reads
`/Linux/lib/firmware/intel/vpu/vpu_<generation>_v1.bin` (`kLinuxFirmwareIntelVpu`; the file comes from the linux-firmware package, the OS does not carry it).

A Linux process also sees the Windows tree at `/mnt/c` (`kLinuxWindowsDrive`), as under WSL: `/mnt/c/Users/Public` is the Windows process's
`C:\Users\Public`, with names compared without regard to case as Windows does. It is the one place the two views overlap, so that files and unix
sockets (`src/Process/UnixSockets.hpp`) one kind of program makes are there for the other. `/mnt` itself stays a plain folder of `/Linux`;
`c` is not listed in it.

The user's files are there for it too, at `/mnt/desktop` (`kLinuxDesktopDrive`): `/mnt/desktop/Documents/a.txt` is `/Desktop/Documents/a.txt`, so a file
the desktop hands a Linux program (dropped on its window, `src/Wayland/WaylandDragBridge.cpp`) is one it can open. `layout::linux_view_of()` and
`layout::system_path_of_linux()` translate paths between the two views.

Windows processes see the user's files at `C:\Users\Public\Desktop` (`kWindowsDesktopFolder`), where Windows keeps the files every user's desktop
shows: `C:\Users\Public\Desktop\Documents\a.txt` is `/Desktop/Documents/a.txt` (names compared without regard to case), so a file dropped on a
Windows program's window is one it can open. It is a mount: the folder of that name in `/Windows` is hidden by it (a Linux process's
`/mnt/c/Users/Public/Desktop` is `/Desktop` too). `layout::windows_view_of()` and `layout::system_path_of_windows()` translate paths.

Other volumes (USB sticks, network shares) are folders of `/` named by their label.

## Rules for new code

- A default save location comes from the table above: browser downloads go to `/Desktop/Downloads`, documents to
  `/Desktop/Documents`, and so on. Use the constants in `filesystem::layout`, not string literals.
- An application stores its own files (caches, settings, databases, state) under `/AppStore/<Application>/`, never directly in `/` or in
  another application's folder. `layout::app_store_path()` builds the path and `layout::create_app_folder()` makes the folder.
- Do not create new top-level folders in `/`. If something does not fit the table, extend the table (this file and `SystemLayout.hpp`)
  in the same change.
- Windows and Linux processes get a view of their own root (`/Windows` as `C:\`, `/Linux` as `/`). Translate paths at the system-call
  boundary; a process must not be able to name a path outside its root (a Linux process's `/mnt/c` and `/mnt/desktop` and a Windows process's `C:\Users\Public\Desktop` are the only ways out of their own).
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
