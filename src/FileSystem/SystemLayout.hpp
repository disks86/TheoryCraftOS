/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The layout of the system filesystem (the tree SystemFileSystem presents). Code that needs one of these places uses the names here
// instead of spelling the path out, so the layout can be followed and changed in one place. See docs/FileSystemLayout.md.
//
//   /                  the root: the primary partition (the first writable, built-in, non-boot volume)
//   /EFI               a mount of the EFI system partition (not a folder of the primary partition)
//   /Desktop           everything on the user's desktop
//   /Desktop/Downloads where the browser puts what it downloads
//   /Desktop/Documents spreadsheets, text files and the like
//   /Desktop/Pictures  image files
//   /Desktop/Video     video files
//   /Desktop/Audio     audio files such as music
//   /Windows           the root of Windows user-mode processes: they see its contents as the C: drive
//   /Linux             the root of Linux processes: they see its contents as /
//   /AppStore          one sub-folder per application for the files it keeps (the browser's cache is /AppStore/Browser/Cache)
//
// Inside /Windows is the usual Windows tree (Program Files, Program Files (x86), ProgramData, Users, Windows\System32, ...) and inside /Linux
// the usual FHS tree (bin, etc, home, usr/bin, var/log, ...); the lists are kWindowsFolders and kLinuxFolders below.
//
// Volumes other than those above (USB sticks, network shares) are folders of "/" named by their label.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace filesystem::layout {

inline constexpr const char* kEfi = "/EFI";
inline constexpr const char* kDesktop = "/Desktop";
inline constexpr const char* kDownloads = "/Desktop/Downloads";
inline constexpr const char* kDocuments = "/Desktop/Documents";
inline constexpr const char* kPictures = "/Desktop/Pictures";
inline constexpr const char* kVideo = "/Desktop/Video";
inline constexpr const char* kAudio = "/Desktop/Audio";
inline constexpr const char* kWindows = "/Windows";
inline constexpr const char* kLinux = "/Linux";
inline constexpr const char* kAppStore = "/AppStore";

// The system database: where the OS and its programs keep their settings (the "System" application's folder in the AppStore).
inline constexpr const char* kSystemDatabase = "/AppStore/System/system.db";

// The calendar application's appointments (the "Calendar" application's folder in the AppStore).
inline constexpr const char* kCalendarDatabase = "/AppStore/Calendar/calendar.db";

// The to-do application's items (the "Todo" application's folder in the AppStore).
inline constexpr const char* kTodoDatabase = "/AppStore/Todo/todo.db";

// The App Manager (the AppImage package manager, src/Desktop/AppMan): its copy of the AppMan catalog (the "AppMan" application's folder in the AppStore).
// The programs it installs live in the Linux world, laid out as AppMan ("am") lays them out: /opt/<app>/ (the AppImage, its version and remove files,
// icons/<app>), a desktop entry in /usr/local/share/applications/<app>-AM.desktop and a link in /usr/local/bin.
inline constexpr const char* kAppManFolder = "/AppStore/AppMan";
inline constexpr const char* kAppManCatalog = "/AppStore/AppMan/stats-appimages";
inline constexpr const char* kLinuxOpt = "/Linux/opt";
inline constexpr const char* kLinuxLocalBin = "/Linux/usr/local/bin";

// The Thunderbolt driver's one-time keys, one file of 32 bytes per device named by its UUID in hex (the "Thunderbolt" application's folder in the AppStore).
inline constexpr const char* kThunderboltKeys = "/AppStore/Thunderbolt/Keys";

// The Bluetooth bonds: the keys of the devices this machine has paired with, one file (the "Bluetooth" application's folder in the AppStore).
inline constexpr const char* kBluetoothBonds = "/AppStore/Bluetooth/bonds.bin";

// When the volumes were last trimmed by the schedule (FileSystem/TrimSchedule; the "FileSystem" application's folder in the AppStore).
inline constexpr const char* kFstrimFolder = "/AppStore/FileSystem";
inline constexpr const char* kFstrimState = "/AppStore/FileSystem/fstrim.state";

// The name the EFI system partition is mounted under (the folder "/EFI").
inline constexpr const char* kEfiMountName = "EFI";

// What a Windows process sees as C:\Program Files, C:\Program Files (x86), C:\Users and C:\Windows.
inline constexpr const char* kWindowsProgramFiles = "/Windows/Program Files";
inline constexpr const char* kWindowsProgramFilesX86 = "/Windows/Program Files (x86)";
inline constexpr const char* kWindowsProgramData = "/Windows/ProgramData";
inline constexpr const char* kWindowsUsers = "/Windows/Users";
inline constexpr const char* kWindowsDirectory = "/Windows/Windows";
inline constexpr const char* kWindowsSystem32 = "/Windows/Windows/System32";
inline constexpr const char* kWindowsSysWow64 = "/Windows/Windows/SysWOW64";
// Where installed Windows programs put their Start menu shortcuts (.lnk): for every user (C:\ProgramData\...) and, under each folder of
// kWindowsUsers, for one user (kWindowsUserStartMenuPrograms is relative to the user's folder). The desktop's Start menu lists them.
inline constexpr const char* kWindowsStartMenuPrograms = "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs";
inline constexpr const char* kWindowsUserStartMenuPrograms = "AppData/Roaming/Microsoft/Windows/Start Menu/Programs";

// What a Linux process sees as /bin, /etc, /home, /tmp and /usr/bin.
inline constexpr const char* kLinuxBin = "/Linux/bin";
inline constexpr const char* kLinuxEtc = "/Linux/etc";
inline constexpr const char* kLinuxHome = "/Linux/home";
inline constexpr const char* kLinuxTmp = "/Linux/tmp";
inline constexpr const char* kLinuxUsrBin = "/Linux/usr/bin";
inline constexpr const char* kLinuxRootHome = "/Linux/root";   // root's home folder (Linux programs run as root)
// Where installed Linux programs put their desktop entries (.desktop files, the freedesktop.org menu): the desktop's Start menu lists them.
inline constexpr const char* kLinuxApplications = "/Linux/usr/share/applications";
inline constexpr const char* kLinuxLocalApplications = "/Linux/usr/local/share/applications";
inline constexpr const char* kLinuxRootApplications = "/Linux/root/.local/share/applications";
// Where a Linux process sees the Windows tree (/Windows, the C: drive), as under WSL: a path in the Linux process's own view, not one of the OS's file
// system. Files (and unix sockets, see Process/UnixSockets.hpp) made there by one kind of program are the other's.
inline constexpr const char* kLinuxWindowsDrive = "/mnt/c";
// Where a Linux process sees the user's files (/Desktop with Documents, Downloads, ...), the same way: what the desktop hands a Linux program (a file
// dropped on its window) is there for it to open.
inline constexpr const char* kLinuxDesktopDrive = "/mnt/desktop";
// The X server's sockets (src/X11/X11SocketServer.hpp): /tmp/.X11-unix/X<display> as Linux programs see it, and where that is on the system's file system.
inline constexpr const char* kLinuxX11SocketFolder = "/tmp/.X11-unix";
inline constexpr const char* kX11SocketFolder = "/Linux/tmp/.X11-unix";
// The runtime folder of the Linux programs' user ($XDG_RUNTIME_DIR, root's: there are no other users yet) as they see it and where it is on the
// system's file system. The Wayland compositor's socket is there (src/Wayland/WaylandSocketServer.hpp: WAYLAND_DISPLAY, wayland-0), and the
// kernel's PipeWire listens there (src/PipeWire/SocketServer.hpp): pipewire-0 (PipeWire's protocol) and pulse/native (PulseAudio's).
inline constexpr const char* kLinuxRuntimeFolder = "/run/user/0";
inline constexpr const char* kLinuxRun = "/Linux/run";
inline constexpr const char* kLinuxRunUser = "/Linux/run/user";
inline constexpr const char* kRuntimeFolder = "/Linux/run/user/0";
inline constexpr const char* kWaylandDisplay = "wayland-0";
inline constexpr const char* kPipeWireSocket = "pipewire-0";
inline constexpr const char* kPulseSocket = "pulse/native";
// The kernel's D-Bus (src/DBus/SocketServer.hpp): the session bus is $XDG_RUNTIME_DIR/bus (DBUS_SESSION_BUS_ADDRESS), the system bus the standard
// /run/dbus/system_bus_socket (DBUS_SYSTEM_BUS_ADDRESS's default), as Linux programs see them; kSystemBusFolder is where that is on the system's disk.
inline constexpr const char* kSessionBusSocket = "bus";
inline constexpr const char* kLinuxSystemBusSocket = "/run/dbus/system_bus_socket";
inline constexpr const char* kSystemBusFolder = "/Linux/run/dbus";
inline constexpr const char* kSystemBusSocket = "/Linux/run/dbus/system_bus_socket";
// Where the buses find the .service files of the programs they start on demand (dbus-daemon's standard folders; src/DBus/Activation.hpp).
inline constexpr const char* kSessionServicesFolder = "/Linux/usr/share/dbus-1/services";
inline constexpr const char* kSystemServicesFolder = "/Linux/usr/share/dbus-1/system-services";
inline constexpr const char* kLocalSessionServicesFolder = "/Linux/usr/local/share/dbus-1/services";
inline constexpr const char* kLocalSystemServicesFolder = "/Linux/usr/local/share/dbus-1/system-services";
// systemd unit folders, where D-Bus looks for a service's SystemdService= unit (there is no systemd: the bus runs the unit's ExecStart itself). The
// administrator's (/etc) come before the packages' (/usr/lib), as systemd reads them; the user folders are the session bus's.
inline constexpr const char* kLocalSystemUnitsFolder = "/Linux/etc/systemd/system";
inline constexpr const char* kSystemUnitsFolder = "/Linux/usr/lib/systemd/system";
inline constexpr const char* kLocalUserUnitsFolder = "/Linux/etc/systemd/user";
inline constexpr const char* kUserUnitsFolder = "/Linux/usr/lib/systemd/user";
// The machine's id (/etc/machine-id: 32 hex digits, made at the first start; D-Bus's Peer.GetMachineId, libdbus and sd-bus read it).
inline constexpr const char* kLinuxMachineId = "/Linux/etc/machine-id";
// Where device firmware is kept, as on Linux (/lib/firmware): the Intel NPU's is in intel/vpu.
inline constexpr const char* kLinuxFirmware = "/Linux/lib/firmware";
inline constexpr const char* kLinuxFirmwareIntelVpu = "/Linux/lib/firmware/intel/vpu";

// The folders under /Windows (what C:\ holds on a Windows installation), parents before children.
inline constexpr const char* kWindowsFolders[] = {
    kWindowsProgramFiles,
    "/Windows/Program Files/Common Files",
    kWindowsProgramFilesX86,
    "/Windows/Program Files (x86)/Common Files",
    kWindowsProgramData,
    "/Windows/ProgramData/Microsoft",
    kWindowsUsers,
    "/Windows/Users/Default",
    "/Windows/Users/Public",
    "/Windows/Users/Public/Desktop",
    "/Windows/Users/Public/Documents",
    "/Windows/Users/Public/Downloads",
    "/Windows/Users/Public/Music",
    "/Windows/Users/Public/Pictures",
    "/Windows/Users/Public/Videos",
    kWindowsDirectory,
    "/Windows/Windows/Fonts",
    "/Windows/Windows/Logs",
    kWindowsSystem32,
    "/Windows/Windows/System32/drivers",
    "/Windows/Windows/System32/config",
    kWindowsSysWow64,
    "/Windows/Windows/Temp",
};

// The folders under /Linux (the Filesystem Hierarchy Standard), parents before children. /dev, /proc and /sys are the places the kernel
// presents devices and process information; here they start out empty.
inline constexpr const char* kLinuxFolders[] = {
    kLinuxBin, "/Linux/boot", "/Linux/dev", kLinuxEtc, "/Linux/etc/opt", kLinuxHome, "/Linux/lib", kLinuxFirmware, "/Linux/lib/firmware/intel", kLinuxFirmwareIntelVpu, "/Linux/lib64", "/Linux/media",
    "/Linux/mnt", "/Linux/opt", "/Linux/proc", "/Linux/root", "/Linux/run", kSystemBusFolder, "/Linux/run/user", kRuntimeFolder, "/Linux/run/user/0/pulse", "/Linux/sbin", "/Linux/srv", "/Linux/sys", kLinuxTmp,
    "/Linux/usr", kLinuxUsrBin, "/Linux/usr/include", "/Linux/usr/lib", "/Linux/usr/lib64", "/Linux/usr/local", "/Linux/usr/local/bin",
    "/Linux/usr/local/lib", "/Linux/usr/sbin", "/Linux/usr/share", "/Linux/usr/share/dbus-1", kSessionServicesFolder, kSystemServicesFolder, "/Linux/var", "/Linux/var/cache", "/Linux/var/lib", "/Linux/var/log",
    "/Linux/var/tmp",
};

// Every folder of the layout that lives on the primary partition, parents before children. "/EFI" is not one: it is a mount.
inline constexpr const char* kFolders[] = {kDesktop, kDownloads, kDocuments, kPictures, kVideo, kAudio, kWindows, kLinux, kAppStore};

inline bool create_folder(filesystem::IFileSystem& fs, const char* folder) {
  const Status status = fs.create_directory(folder);
  return status == Status::Ok || status == Status::AlreadyExists;
}

// Creates the folders above (kFolders, then the Windows and Linux trees) in `fs` (the primary partition's own filesystem, or one standing in for it) when they are missing. Folders that
// exist stay as they are, and so does everything else in them. False if one could not be made (read-only medium, no space).
inline bool create_folders(filesystem::IFileSystem& fs) {
  bool ok = true;
  for (const char* folder : kFolders) ok &= create_folder(fs, folder);
  for (const char* folder : kWindowsFolders) ok &= create_folder(fs, folder);
  for (const char* folder : kLinuxFolders) ok &= create_folder(fs, folder);
  return ok;
}

// A system path as a Linux process names it, into *out: /Linux/x is /x, /Windows/x is /mnt/c/x, /Desktop/x is /mnt/desktop/x. False for a path a Linux
// process cannot see (/AppStore, /EFI, another volume) or on out-of-memory.
inline bool linux_view_of(StringView system_path, String* out) {
  struct Mount { const char* system; const char* seen; };
  const Mount mounts[] = {{kWindows, kLinuxWindowsDrive}, {kDesktop, kLinuxDesktopDrive}, {kLinux, ""}};
  for (const Mount& m : mounts) {
    const StringView root(m.system);
    if (system_path.size() < root.size() || system_path.first(root.size()) != root) continue;
    const StringView rest = system_path.without_prefix(root.size());
    if (!rest.empty() && rest[0] != '/') continue;      // ("/Linuxish" is not)
    if (!out->assign(StringView(m.seen)) || !out->append(rest)) return false;
    if (out->empty()) return out->push_back('/');
    return true;
  }
  return false;
}

// The other way: what a Linux process names /x (/mnt/c/x, /mnt/desktop/x) on the system. False for a relative path or on out-of-memory. (Windows'
// names are not folded to the case the files have: the caller opens them through the file system, which compares names as it does.)
inline bool system_path_of_linux(StringView linux_path, String* out) {
  if (linux_path.empty() || linux_path[0] != '/') return false;
  struct Mount { const char* seen; const char* system; };
  const Mount mounts[] = {{kLinuxWindowsDrive, kWindows}, {kLinuxDesktopDrive, kDesktop}};
  for (const Mount& m : mounts) {
    const StringView root(m.seen);
    if (linux_path.size() < root.size() || linux_path.first(root.size()) != root) continue;
    const StringView rest = linux_path.without_prefix(root.size());
    if (!rest.empty() && rest[0] != '/') continue;
    return out->assign(StringView(m.system)) && out->append(rest);
  }
  if (!out->assign(StringView(kLinux))) return false;
  return linux_path == StringView("/") || out->append(linux_path);
}

// "/AppStore/<app>" or "/AppStore/<app>/<sub>" in *path (replacing what it held). `app` is one folder name ("Browser"); `sub` may be a
// path ("Cache", "Cache/images") or empty. False if `app` is not a valid folder name, or on out-of-memory.
inline bool app_store_path(String* path, StringView app, StringView sub = StringView()) {
  if (!filesystem::valid_name(app)) return false;
  if (!path->assign(kAppStore) || !path->push_back('/') || !path->append(app)) return false;
  if (sub.empty()) return true;
  return path->push_back('/') && path->append(sub);
}

// Creates the folder "/AppStore/<app>" (and "/AppStore" itself if it is missing) in `fs`. Call once when an application first needs to
// store something; an existing folder is fine.
inline Status create_app_folder(filesystem::IFileSystem& fs, StringView app, allocators::IAllocator& allocator) {
  String path(allocator);
  if (!app_store_path(&path, app)) return Status::InvalidPath;
  const Status parent = fs.create_directory(kAppStore);
  if (parent != Status::Ok && parent != Status::AlreadyExists) return parent;
  const Status status = fs.create_directory(path.view());
  return status == Status::AlreadyExists ? Status::Ok : status;
}

}  // namespace filesystem::layout
