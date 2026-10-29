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
#include "Process/ProgramEnvironment.hpp"

namespace process {

namespace {

// `path` lies in `root` (equal to it, or below it): the part after it, which is empty for the root itself.
bool inside(StringView root, StringView path, StringView* rest) {
  if (path.size() < root.size() || !path.first(root.size()).equals(root)) return false;
  const StringView tail = path.without_prefix(root.size());
  if (!tail.empty() && tail[0] != '/') return false;      // "/Linuxes" is not in "/Linux"
  *rest = tail;
  return true;
}

StringView base_name(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? path : path.without_prefix(slash + 1);
}

}  // namespace

StringView world_root(SyscallAbi abi) { return abi == SyscallAbi::Windows ? kWindowsRoot : kLinuxRoot; }

bool program_name(SyscallAbi abi, StringView shell_path, String* out) {
  StringView rest;
  const bool windows = abi == SyscallAbi::Windows;
  if (!inside(world_root(abi), shell_path, &rest) || rest.empty()) {
    rest = StringView();
    out->clear();
    if (!out->append(windows ? "C:" : "")) return false;
    return out->push_back(windows ? '\\' : '/') && out->append(base_name(shell_path));
  }
  out->clear();
  if (!windows) return out->assign(rest);
  if (!out->assign("C:")) return false;
  for (char c : rest) {
    if (!out->push_back(c == '/' ? '\\' : c)) return false;
  }
  return true;
}

bool world_directory(SyscallAbi abi, StringView shell_cwd, String* out) {
  StringView rest;
  if (!inside(world_root(abi), shell_cwd, &rest) || rest.empty()) return out->assign("/");
  return out->assign(rest);
}

ArrayView<const StringView> default_environment(SyscallAbi abi) {
  static const StringView kLinux[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/root", "USER=root", "LOGNAME=root", "TERM=dumb", "LANG=C.UTF-8", "SHELL=/bin/sh"};
  static const StringView kWindows[] = {"SystemDrive=C:", "SystemRoot=C:\\Windows", "windir=C:\\Windows", "PATH=C:\\Windows\\System32;C:\\Windows", "TEMP=C:\\Temp",
                                         "TMP=C:\\Temp", "OS=Windows_NT", "PROCESSOR_ARCHITECTURE=AMD64", "NUMBER_OF_PROCESSORS=1", "COMSPEC=C:\\Windows\\System32\\cmd.exe",
                                         "USERPROFILE=C:\\Users\\Default", "USERNAME=Default", "HOMEDRIVE=C:", "HOMEPATH=\\Users\\Default",
                                         "ALLUSERSPROFILE=C:\\ProgramData", "ProgramData=C:\\ProgramData", "COMPUTERNAME=TCOS", "PROCESSOR_IDENTIFIER=Intel64 Family 6 Model 1 Stepping 1, GenuineIntel",
                                         "PROCESSOR_LEVEL=6", "PROCESSOR_REVISION=0101", "ProgramFiles=C:\\Program Files", "CommonProgramFiles=C:\\Program Files\\Common Files",
                                         "ProgramFiles(x86)=C:\\Program Files (x86)", "CommonProgramFiles(x86)=C:\\Program Files (x86)\\Common Files", "ProgramW6432=C:\\Program Files",
                                         "CommonProgramW6432=C:\\Program Files\\Common Files", "APPDATA=C:\\Users\\Default\\AppData\\Roaming", "LOCALAPPDATA=C:\\Users\\Default\\AppData\\Local"};
  if (abi == SyscallAbi::Windows) return ArrayView<const StringView>(kWindows, sizeof(kWindows) / sizeof(kWindows[0]));
  return ArrayView<const StringView>(kLinux, sizeof(kLinux) / sizeof(kLinux[0]));
}

}  // namespace process
