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

#include "Program/AppImageLaunch.hpp"
#include "FileSystem/Dwarfs/DwarfsFileSystem.hpp"
#include "FileSystem/Iso/IsoFileSystem.hpp"
#include "FileSystem/Squash/SquashFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Process/ProgramEnvironment.hpp"
#include "Utility/Move.hpp"

namespace desktop {

namespace {

constexpr StringView kWorldRoot = "/Linux";

bool read_head(filesystem::IFileSystem& files, StringView path, ByteView out, uint64_t* got) {
  *got = 0;
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  return file->read(0, out, got) == filesystem::Status::Ok;
}

bool is_elf(filesystem::IFileSystem& files, StringView host_path) {
  uint8_t head[4];
  uint64_t got = 0;
  return read_head(files, host_path, ByteView(head), &got) && got == 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
}

// A different name for every mount (the runtime uses six random characters).
uint64_t next_mount_id() {
  static uint64_t counter = 0;
  return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}

// "NAME=value" for `name`, from the last entry that sets it.
StringView variable(ArrayView<const StringView> environment, StringView name) {
  StringView found;
  for (const StringView entry : environment)
    if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=') found = entry.substr(name.size() + 1);
  return found;
}

}  // namespace

exe::AppImageType appimage_type(filesystem::IFileSystem& files, StringView path) {
  uint8_t head[16];
  uint64_t got = 0;
  if (!read_head(files, path, ByteView(head), &got) || got < sizeof(head)) return exe::AppImageType::None;
  return exe::identify_appimage(ConstByteView(head));
}

bool open_appimage_image(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView path, UniquePointer<filesystem::IFileSystem>* image, String* message) {
  auto fail = [&](StringView text) {
    message->assign(text);
    return false;
  };
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return fail("cannot open the AppImage");
  uint8_t head[16];
  uint64_t got = 0;
  if (file->read(0, ByteView(head), &got) != filesystem::Status::Ok || got != sizeof(head)) return fail("cannot read the AppImage");
  const exe::AppImageType type = exe::identify_appimage(ConstByteView(head));
  if (type == exe::AppImageType::None) return fail("this is not an AppImage");
  filesystem::Status mounted = filesystem::Status::Corrupt;
  if (type == exe::AppImageType::Type1) {
    // Type 1: the whole file is an ISO 9660 image, the runtime in its system area.
    if (!filesystem::IsoFileSystem::looks_like_iso(*file, 0)) return fail("the AppImage is damaged: a type 1 AppImage must be an ISO 9660 image");
    UniquePointer<filesystem::IsoFileSystem> iso;
    mounted = filesystem::IsoFileSystem::mount(allocator, util::move(file), 0, &iso);
    *image = util::move(iso);
  } else {
    uint64_t offset = 0;
    exe::AppImagePayload payload = exe::AppImagePayload::SquashFs;
    if (!exe::appimage_payload_offset(*file, &offset, &payload)) return fail("the AppImage is damaged: no SquashFS or DwarFS image follows its runtime");
    if (payload == exe::AppImagePayload::DwarFs) {
      // uruntime's AppImages (the "anylinux" builds) carry DwarFS.
      UniquePointer<filesystem::DwarfsFileSystem> dwarfs;
      mounted = filesystem::DwarfsFileSystem::mount(allocator, util::move(file), offset, &dwarfs);
      *image = util::move(dwarfs);
    } else {
      UniquePointer<filesystem::SquashFileSystem> squash;
      mounted = filesystem::SquashFileSystem::mount(allocator, util::move(file), offset, &squash);
      *image = util::move(squash);
    }
  }
  if (mounted == filesystem::Status::OutOfMemory) return fail("not enough memory to open the AppImage");
  if (mounted == filesystem::Status::Unsupported) return fail("the AppImage's image uses a format version or feature this OS does not know");
  if (mounted != filesystem::Status::Ok) return fail("the AppImage's image cannot be read (it is damaged, or of a kind this OS does not know)");
  return true;
}

bool AppImageSession::prepare(filesystem::IFileSystem& files, StringView path, StringView working_directory, ArrayView<const StringView> arguments,
                              ArrayView<const StringView> environment, String* message) {
  auto fail = [&](StringView text) {
    message->assign(text);
    return false;
  };
  if (!open_appimage_image(allocator_, files, path, &image_, message)) return false;

  // /tmp/.mount_<first six characters of the name><id>, as the runtime names it.
  StringView name = path.substr(path.rfind('/') + 1);
  if (!mount_.assign("/tmp/.mount_")) return fail("not enough memory");
  uint32_t taken = 0;
  for (char c : name) {
    if (taken == 6) break;
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (plain && !mount_.push_back(c)) return fail("not enough memory");
    taken += plain;
  }
  for (uint64_t id = next_mount_id(), i = 0; i < 6; ++i, id /= 36)
    if (!mount_.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[id % 36])) return fail("not enough memory");
  String host_mount(allocator_);
  if (!host_mount.assign(kWorldRoot) || !host_mount.append(mount_.view())) return fail("not enough memory");
  graft_ = UniquePointer<filesystem::GraftFileSystem>::make(allocator_, allocator_, files, *image_);
  if (!graft_ || !graft_->set_mount_point(host_mount.view()) || !graft_->set_link_root(kWorldRoot)) return fail("not enough memory");

  // The runtime's variables, then whatever the caller had.
  String appimage(allocator_), owd(allocator_), argv0(allocator_), appdir(allocator_), pwd(allocator_);
  if (path.starts_with(filesystem::layout::kLinux) && path.size() > kWorldRoot.size() && path[kWorldRoot.size()] == '/') {
    if (!appimage.assign("APPIMAGE=") || !appimage.append(path.substr(kWorldRoot.size()))) return fail("not enough memory");
  } else if (!appimage.assign("APPIMAGE=") || !appimage.append(path)) {
    return fail("not enough memory");
  }
  String world_cwd(allocator_);
  if (!process::world_directory(process::SyscallAbi::Linux, working_directory, &world_cwd)) return fail("not enough memory");
  if (!owd.assign("OWD=") || !owd.append(world_cwd.view()) || !pwd.assign("PWD=") || !pwd.append(world_cwd.view())) return fail("not enough memory");
  if (!argv0.assign("ARGV0=") || !argv0.append(appimage.view().substr(9))) return fail("not enough memory");
  if (!appdir.assign("APPDIR=") || !appdir.append(mount_.view())) return fail("not enough memory");
  List<StringView> start(allocator_);
  for (const StringView entry : environment)
    if (!start.push_back(entry)) return fail("not enough memory");
  if (!start.push_back(pwd.view()) || !start.push_back(appimage.view()) || !start.push_back(appdir.view()) || !start.push_back(argv0.view()) || !start.push_back(owd.view()))
    return fail("not enough memory");

  String entry(allocator_);
  if (!entry.assign(mount_.view()) || !entry.append("/AppRun")) return fail("not enough memory");
  const AppRunResult result = evaluate_apprun(allocator_, *graft_, kWorldRoot, entry.view(), arguments, ArrayView<const StringView>(start.view()), &launch_);
  if (result == AppRunResult::OutOfMemory) return fail("not enough memory to start the AppImage");
  if (result != AppRunResult::Launch && !desktop_entry_fallback(mount_.view(), arguments, ArrayView<const StringView>(start.view())))
    return fail("the AppImage's AppRun does not lead to a program this OS can start (see docs/AppImage.md)");

  // Electron's sandbox needs user namespaces or a setuid helper.
  const uint64_t slash = launch_.program.view().rfind('/');
  String sandbox(allocator_), host_sandbox(allocator_);
  if (!sandbox.assign(launch_.program.view().first(slash + 1)) || !sandbox.append("chrome-sandbox") || !host_sandbox.assign(kWorldRoot) || !host_sandbox.append(sandbox.view()))
    return fail("not enough memory");
  filesystem::DirectoryEntry probe;
  if (graft_->stat(host_sandbox.view(), &probe) == filesystem::Status::Ok) {
    bool has = false;
    for (const String& argument : launch_.arguments.view()) has |= argument.view().equals("--no-sandbox");
    String flag(allocator_);
    if (!has && (!flag.assign("--no-sandbox") || !launch_.arguments.insert(0, util::move(flag)))) return fail("not enough memory");
  }

  if (!program_.assign(kWorldRoot) || !program_.append(launch_.program.view())) return fail("not enough memory");
  for (const String& argument : launch_.arguments.view())
    if (!argument_views_.push_back(argument.view())) return fail("not enough memory");
  for (const String& variable_entry : launch_.environment.view())
    if (!environment_views_.push_back(variable_entry.view())) return fail("not enough memory");
  return true;
}

// The program named by the Exec line of the desktop entry at the image's root, with the variables the classic AppRun binary (AppImageKit's AppRun.c) sets.
bool AppImageSession::desktop_entry_fallback(StringView appdir, ArrayView<const StringView> arguments, ArrayView<const StringView> environment) {
  String host_root(allocator_);
  if (!host_root.assign(kWorldRoot) || !host_root.append(appdir)) return false;
  UniquePointer<filesystem::IDirectory> listing;
  if (graft_->open_directory(host_root.view(), &listing) != filesystem::Status::Ok) return false;
  filesystem::DirectoryEntry item;
  String desktop(allocator_);
  while (listing->next(&item) == filesystem::Status::Ok) {
    if (item.name.view().ends_with(".desktop")) {
      if (!desktop.assign(host_root.view()) || !desktop.push_back('/') || !desktop.append(item.name.view())) return false;
      break;
    }
  }
  if (desktop.empty()) return false;
  UniquePointer<filesystem::IFile> file;
  if (graft_->open_file(desktop.view(), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  uint8_t text[8192];
  uint64_t got = 0;
  if (file->read(0, ByteView(text), &got) != filesystem::Status::Ok) return false;
  StringView rest(ConstByteView(text).first(got)), line, exec;
  bool in_entry = false;
  while (rest.next_token('\n', &line)) {
    line = line.trimmed();
    if (line.starts_with('[')) in_entry = line.equals("[Desktop Entry]");
    else if (in_entry && line.starts_with("Exec=")) { exec = line.substr(5).trimmed(); break; }
  }
  StringView command;
  if (exec.starts_with('"')) {
    const uint64_t close = exec.find('"', 1);
    command = close == StringView::npos ? StringView() : exec.substr(1, close - 1);
  } else {
    const uint64_t space = exec.find(' ');
    command = space == StringView::npos ? exec : exec.first(space);
  }
  if (command.empty() || command.equals("AppRun") || command.ends_with("/AppRun")) return false;
  const StringView base = command.substr(command.rfind('/') + 1);
  static const char* const kFolders[] = {"/usr/bin/", "/", "/bin/", "/usr/sbin/"};
  String candidate(allocator_), host_candidate(allocator_);
  bool found = false;
  for (const char* folder : kFolders) {
    if (!candidate.assign(appdir) || !candidate.append(folder) || !candidate.append(base)) return false;
    if (!host_candidate.assign(kWorldRoot) || !host_candidate.append(candidate.view())) return false;
    if (is_elf(*graft_, host_candidate.view())) { found = true; break; }
  }
  if (!found || !launch_.program.assign(candidate.view())) return false;
  launch_.arguments.clear();
  for (const StringView argument : arguments) {
    String copy(allocator_);
    if (!copy.assign(argument) || !launch_.arguments.push_back(util::move(copy))) return false;
  }
  // AppRun.c: PATH, LD_LIBRARY_PATH, XDG_DATA_DIRS, PYTHONPATH, PERLLIB, GSETTINGS_SCHEMA_DIR, QT_PLUGIN_PATH, each before what was there.
  struct Prefix { const char* name; const char* folders[4]; };
  static const Prefix kPrefixes[] = {
      {"PATH", {"/usr/bin", "/usr/sbin", "/usr/games", "/bin"}},
      {"LD_LIBRARY_PATH", {"/usr/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/lib"}},
      {"XDG_DATA_DIRS", {"/usr/share", nullptr, nullptr, nullptr}},
      {"PYTHONPATH", {"/usr/share/pyshared", nullptr, nullptr, nullptr}},
      {"PERLLIB", {"/usr/share/perl5", "/usr/lib/perl5", nullptr, nullptr}},
      {"GSETTINGS_SCHEMA_DIR", {"/usr/share/glib-2.0/schemas", nullptr, nullptr, nullptr}},
      {"QT_PLUGIN_PATH", {"/usr/lib/qt4/plugins", "/usr/lib/qt5/plugins", nullptr, nullptr}},
  };
  launch_.environment.clear();
  for (const StringView entry : environment) {
    String copy(allocator_);
    if (!copy.assign(entry) || !launch_.environment.push_back(util::move(copy))) return false;
  }
  for (const Prefix& prefix : kPrefixes) {
    String value(allocator_);
    if (!value.assign(prefix.name) || !value.push_back('=')) return false;
    bool first = true;
    for (const char* folder : prefix.folders) {
      if (!folder) break;
      if ((!first && !value.push_back(':')) || !value.append(appdir) || !value.append(folder)) return false;
      first = false;
    }
    const StringView old = variable(environment, prefix.name);
    if (!old.empty() && (!value.push_back(':') || !value.append(old))) return false;
    if (!launch_.environment.push_back(util::move(value))) return false;
  }
  return true;
}

}  // namespace desktop
