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

#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Desktop/AppMan/AppManCatalog.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Program/AppImageLaunch.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

using filesystem::Status;

constexpr StringView kWorldRoot = "/Linux";
constexpr uint64_t kMaxScriptBytes = 1u << 20;
constexpr uint64_t kMaxDesktopBytes = 1u << 16;
constexpr uint64_t kMaxIconBytes = 4u << 20;
constexpr StringView kEnvironment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/root", "LANG=C.UTF-8"};

bool is_identifier(StringView name) {
  if (name.empty() || !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_')) return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

// Paths of the layout ("/Linux/opt/<name>", ...).
bool join(String* out, StringView a, StringView b = {}, StringView c = {}, StringView d = {}) {
  return out->assign(a) && out->append(b) && out->append(c) && out->append(d);
}

// What a download that is not an AppImage is, by its first bytes.
StringView packed_kind(ConstByteView head) {
  if (head.size() >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) return "a .zip archive";
  if (head.size() >= 2 && head[0] == 0x1F && head[1] == 0x8B) return "a gzip-packed file (.tar.gz)";
  if (head.size() >= 6 && head[0] == 0xFD && head[1] == '7' && head[2] == 'z' && head[3] == 'X' && head[4] == 'Z') return "an xz-packed file (.tar.xz)";
  if (head.size() >= 6 && head[0] == '7' && head[1] == 'z' && head[2] == 0xBC && head[3] == 0xAF) return "a .7z archive";
  if (head.size() >= 4 && head[0] == 0x28 && head[1] == 0xB5 && head[2] == 0x2F && head[3] == 0xFD) return "a zstd-packed file";
  if (head.size() >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') return "a plain Linux program, not an AppImage";
  if (head.size() >= 1 && head[0] == '<') return "a web page";
  return "not an AppImage";
}

// Makes the folder at `path` and every folder above it that is missing.
bool make_folders(filesystem::IFileSystem& files, StringView path) {
  for (uint64_t at = 1; at <= path.size(); ++at) {
    if (at < path.size() && path[at] != '/') continue;
    const Status s = files.create_directory(path.first(at));
    if (s != Status::Ok && s != Status::AlreadyExists) return false;
  }
  return true;
}

struct Writer {
  filesystem::IFile* file;
  Progress* progress;
  const uint64_t* total = nullptr;      // (filled in by the network before the first bytes)
  uint64_t offset = 0, reported = 0;
  bool failed = false;
};

bool write_chunk(void* context, ConstByteView data) {
  Writer& w = *static_cast<Writer*>(context);
  if (w.progress && __atomic_load_n(&w.progress->cancel, __ATOMIC_RELAXED)) return false;
  uint64_t done = 0;
  if (w.file->write(w.offset, data, &done) != Status::Ok || done != data.size()) {
    w.failed = true;
    return false;
  }
  w.offset += done;
  if (w.progress) {
    __atomic_store_n(&w.progress->done, w.offset, __ATOMIC_RELAXED);
    if (w.total && *w.total) __atomic_store_n(&w.progress->total, *w.total, __ATOMIC_RELAXED);
    if (w.progress->changed && w.offset - w.reported >= (256u << 10)) {
      w.reported = w.offset;
      w.progress->changed(w.progress->context, *w.progress);
    }
  }
  return true;
}

}  // namespace

StringView result_name(Result result) {
  switch (result) {
    case Result::Ok: return "done";
    case Result::InvalidName: return "not a valid application name";
    case Result::NoScript: return "AppMan has no install script of that name (or it could not be fetched)";
    case Result::NoDownload: return "the install script's download link could not be worked out";
    case Result::DownloadFailed: return "the download failed";
    case Result::NotAnAppImage: return "the download is not an AppImage";
    case Result::NotInstalled: return "it is not installed";
    case Result::UpToDate: return "it is up to date";
    case Result::Cancelled: return "cancelled";
    case Result::FileError: return "the files could not be written";
    case Result::OutOfMemory: return "not enough memory";
  }
  return "";
}

bool Installer::download_lines(StringView script, String* out) {
  out->clear();
  String line(out->allocator());
  StringView rest = script, piece;
  while (rest.next_token('\n', &piece)) {
    if (piece.ends_with('\r')) piece = piece.first(piece.size() - 1);
    if (piece.ends_with('\\')) {      // continued on the next line
      if (!line.append(piece.first(piece.size() - 1))) return false;
      continue;
    }
    if (!line.append(piece)) return false;
    const StringView text = line.view().trimmed();
    const uint64_t eq = text.find('=');
    const bool assignment = eq != StringView::npos && is_identifier(text.first(eq));
    if (assignment && (!out->append(text) || !out->push_back('\n'))) return false;
    if (assignment && text.starts_with("version=")) return true;
    line.clear();
  }
  return false;
}

bool Installer::read_file(StringView path, uint64_t limit, String* text) const {
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(path, filesystem::OpenMode::Read, &file) != Status::Ok) return false;
  const uint64_t size = file->size() < limit ? file->size() : limit;
  ByteBuffer bytes(allocator_);
  if (!bytes.put_zeros(size)) return false;
  uint64_t done = 0;
  while (done < size) {
    uint64_t got = 0;
    if (file->read(done, bytes.writable_view().subview(done), &got) != Status::Ok || got == 0) return false;
    done += got;
  }
  return text->assign(StringView(bytes.view()));
}

bool Installer::write_file(StringView path, StringView text) {
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) return false;
  uint64_t done = 0;
  return file->write(0, text.bytes(), &done) == Status::Ok && done == text.size() && file->flush() == Status::Ok;
}

bool Installer::installed(StringView name) const {
  if (!valid_app_name(name)) return false;
  String version(allocator_), remover(allocator_);
  filesystem::DirectoryEntry entry;
  return join(&version, filesystem::layout::kLinuxOpt, "/", name, "/version") && join(&remover, filesystem::layout::kLinuxOpt, "/", name, "/remove") &&
         files_.stat(version.view(), &entry) == Status::Ok && files_.stat(remover.view(), &entry) == Status::Ok;
}

bool Installer::list_installed(List<InstalledApp>* apps) const {
  apps->clear();
  UniquePointer<filesystem::IDirectory> listing;
  if (files_.open_directory(filesystem::layout::kLinuxOpt, &listing) != Status::Ok) return true;      // (nothing installed yet)
  filesystem::DirectoryEntry entry;
  for (uint32_t guard = 0; guard < 100000 && listing->next(&entry) == Status::Ok; ++guard) {
    const StringView name = entry.name.view();
    if (entry.type != filesystem::NodeType::Directory || !installed(name)) continue;
    InstalledApp app(allocator_);
    String path(allocator_);
    if (!app.name.assign(name) || !join(&path, filesystem::layout::kLinuxOpt, "/", name, "/version")) return false;
    String version(allocator_);
    if (read_file(path.view(), 4096, &version) && !app.version.assign(version.view().trimmed())) return false;
    if (!apps->push_back(util::move(app))) return false;
  }
  // By name.
  ArrayView<InstalledApp> list = apps->view();
  for (uint64_t i = 1; i < list.size(); ++i)
    for (uint64_t j = i; j > 0 && list[j].name.view() < list[j - 1].name.view(); --j) {
      InstalledApp t = util::move(list[j]);
      list[j] = util::move(list[j - 1]);
      list[j - 1] = util::move(t);
    }
  return true;
}

Result Installer::resolve(StringView name, String* url, String* detail) {
  detail->clear();
  url->clear();
  if (!valid_app_name(name)) return Result::InvalidName;
  String address(allocator_), script(allocator_), lines(allocator_);
  if (!join(&address, kScriptsUrl, name)) return Result::OutOfMemory;
  if (!network_.fetch(address.view(), kMaxScriptBytes, &script) || script.empty() || !script.view().starts_with("#!")) {
    detail->assign(address.view());
    return Result::NoScript;
  }
  if (!download_lines(script.view(), &lines)) {
    detail->assign("the script has no version= line");
    return Result::NoDownload;
  }
  if (!evaluate_script_variable(allocator_, files_, kWorldRoot, lines.view(), "version", ArrayView<const StringView>(kEnvironment), &network_, url)) {
    detail->assign("the version= line gave nothing");
    return Result::NoDownload;
  }
  const StringView link = url->view().trimmed();
  bool clean = link.starts_with("https://") || link.starts_with("http://");
  for (char c : link) clean &= c > ' ' && c != '"' && c != '\'';
  if (!clean) {
    detail->assign("the script's link is not a web address: ");
    detail->append(link.first(link.size() < 200 ? link.size() : 200));
    url->clear();
    return Result::NoDownload;
  }
  String trimmed(allocator_);
  if (!trimmed.assign(link)) return Result::OutOfMemory;
  *url = util::move(trimmed);
  return Result::Ok;
}

Result Installer::install(StringView name, Progress* progress, String* detail) {
  String url(allocator_);
  const Result r = resolve(name, &url, detail);
  if (r != Result::Ok) return r;
  return fetch_and_install(name, url.view(), progress, detail);
}

Result Installer::update(StringView name, Progress* progress, String* detail) {
  detail->clear();
  if (!installed(name)) return Result::NotInstalled;
  String url(allocator_), path(allocator_), current(allocator_);
  const Result r = resolve(name, &url, detail);
  if (r != Result::Ok) return r;
  if (!join(&path, filesystem::layout::kLinuxOpt, "/", name, "/version")) return Result::OutOfMemory;
  if (read_file(path.view(), 4096, &current) && current.view().trimmed().equals(url.view())) return Result::UpToDate;
  return fetch_and_install(name, url.view(), progress, detail);
}

Result Installer::fetch_and_install(StringView name, StringView url, Progress* progress, String* detail) {
  String dir(allocator_), icons(allocator_), part(allocator_), program(allocator_);
  if (!join(&dir, filesystem::layout::kLinuxOpt, "/", name) || !join(&icons, dir.view(), "/icons") || !join(&part, dir.view(), "/", name) || !part.append(".part") ||
      !join(&program, dir.view(), "/", name))
    return Result::OutOfMemory;
  if (!make_folders(files_, icons.view())) {
    detail->assign(dir.view());
    return Result::FileError;
  }

  // The download, into a .part file.
  {
    UniquePointer<filesystem::IFile> file;
    if (files_.open_file(part.view(), filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) {
      detail->assign(part.view());
      return Result::FileError;
    }
    uint64_t total = 0;
    Writer writer{file.get(), progress, &total};
    if (progress) {
      __atomic_store_n(&progress->done, 0, __ATOMIC_RELAXED);
      __atomic_store_n(&progress->total, 0, __ATOMIC_RELAXED);
    }
    const bool ok = network_.download(url, &write_chunk, &writer, &total);
    if (progress && total) __atomic_store_n(&progress->total, total, __ATOMIC_RELAXED);
    if (progress && progress->changed) progress->changed(progress->context, *progress);
    const bool flushed = file->flush() == Status::Ok;
    file = UniquePointer<filesystem::IFile>();
    if (!ok || !flushed || writer.offset == 0) {
      (void)files_.remove(part.view());
      if (progress && __atomic_load_n(&progress->cancel, __ATOMIC_RELAXED)) return Result::Cancelled;
      if (writer.failed || !flushed) {
        detail->assign("writing ");
        detail->append(part.view());
        return Result::FileError;
      }
      detail->assign(url);
      return Result::DownloadFailed;
    }
  }
  if (appimage_type(files_, part.view()) == exe::AppImageType::None) {
    uint8_t head[8] = {};
    UniquePointer<filesystem::IFile> file;
    uint64_t got = 0;
    if (files_.open_file(part.view(), filesystem::OpenMode::Read, &file) == Status::Ok) (void)file->read(0, ByteView(head), &got);
    file = UniquePointer<filesystem::IFile>();
    (void)files_.remove(part.view());
    detail->assign(packed_kind(ConstByteView(head).first(got)));
    return Result::NotAnAppImage;
  }
  // In place of the old one (an update).
  const Status gone = files_.remove(program.view());
  if (gone != Status::Ok && gone != Status::NotFound) {
    detail->assign(program.view());
    return Result::FileError;
  }
  if (files_.rename(part.view(), program.view()) != Status::Ok) {
    detail->assign(program.view());
    return Result::FileError;
  }

  // AppMan's bookkeeping: where it came from, and how to take it away again.
  String version(allocator_), remover(allocator_), text(allocator_);
  if (!join(&version, dir.view(), "/version") || !join(&remover, dir.view(), "/remove") || !join(&text, url, "\n")) return Result::OutOfMemory;
  if (!write_file(version.view(), text.view())) {
    detail->assign(version.view());
    return Result::FileError;
  }
  if (!join(&text, "#!/bin/sh\nset -e\nrm -f /usr/local/bin/", name, "\nrm -R -f /opt/") || !join(&text, text.view(), name, "\nrm -f /usr/local/share/applications/", name) ||
      !text.append("-AM.desktop\n"))
    return Result::OutOfMemory;
  if (!write_file(remover.view(), text.view())) {
    detail->assign(remover.view());
    return Result::FileError;
  }
  return install_launcher(name, detail);
}

// The desktop entry and the icon, out of the AppImage, as AppMan's scripts make them.
Result Installer::install_launcher(StringView name, String* detail) {
  String program(allocator_), linux_program(allocator_), icon_path(allocator_), linux_icon(allocator_);
  if (!join(&program, filesystem::layout::kLinuxOpt, "/", name, "/") || !program.append(name) || !join(&linux_program, "/opt/", name, "/", name) ||
      !join(&icon_path, filesystem::layout::kLinuxOpt, "/", name, "/icons/") || !icon_path.append(name) || !join(&linux_icon, "/opt/", name, "/icons/", name))
    return Result::OutOfMemory;
  String entry(allocator_), message(allocator_);
  bool have_icon = false;
  UniquePointer<filesystem::IFileSystem> image;
  if (open_appimage_image(allocator_, files_, program.view(), &image, &message)) {
    // The first desktop entry at the root of the image, and its icon (.DirIcon).
    UniquePointer<filesystem::IDirectory> root;
    String desktop(allocator_);
    if (image->open_directory("/", &root) == Status::Ok) {
      filesystem::DirectoryEntry item;
      for (uint32_t guard = 0; guard < 10000 && root->next(&item) == Status::Ok; ++guard)
        if (item.name.view().ends_with(".desktop") && desktop.empty() && (!desktop.assign("/") || !desktop.append(item.name.view()))) return Result::OutOfMemory;
    }
    auto read_image = [&](StringView path, uint64_t limit, ByteBuffer* out) {
      UniquePointer<filesystem::IFile> file;
      if (image->open_file(path, filesystem::OpenMode::Read, &file) != Status::Ok || file->size() == 0 || file->size() > limit) return false;
      if (!out->put_zeros(file->size())) return false;
      uint64_t done = 0;
      while (done < out->size()) {
        uint64_t got = 0;
        if (file->read(done, out->writable_view().subview(done), &got) != Status::Ok || got == 0) return false;
        done += got;
      }
      return true;
    };
    ByteBuffer bytes(allocator_);
    if (!desktop.empty() && read_image(desktop.view(), kMaxDesktopBytes, &bytes) && !entry.assign(StringView(bytes.view()))) return Result::OutOfMemory;
    ByteBuffer icon(allocator_);
    if (read_image("/.DirIcon", kMaxIconBytes, &icon)) {
      UniquePointer<filesystem::IFile> out;
      uint64_t done = 0;
      have_icon = files_.open_file(icon_path.view(), filesystem::OpenMode::CreateTruncate, &out) == Status::Ok && out->write(0, icon.view(), &done) == Status::Ok &&
                  done == icon.size() && out->flush() == Status::Ok;
    }
  }

  // Exec= the AppImage (without --no-sandbox: the OS adds it when it starts an Electron program), Icon= the icon taken out, TryExec= the AppImage.
  String rewritten(allocator_);
  bool any_exec = false, any_name = false;
  StringView rest = entry.view(), line;
  while (rest.next_token('\n', &line)) {
    if (line.ends_with('\r')) line = line.first(line.size() - 1);
    if (line.starts_with("Exec=")) {
      StringView command = line.substr(5).trimmed(), arguments;
      if (command.starts_with('"')) {
        const uint64_t close = command.find('"', 1);
        arguments = close == StringView::npos ? StringView() : command.substr(close + 1);
      } else {
        const uint64_t space = command.find(' ');
        arguments = space == StringView::npos ? StringView() : command.substr(space);
      }
      if (!rewritten.append("Exec=") || !rewritten.append(linux_program.view())) return Result::OutOfMemory;
      StringView word, words = arguments;
      while (words.next_token(' ', &word)) {
        if (word.empty() || word.equals("--no-sandbox")) continue;
        if (!rewritten.push_back(' ') || !rewritten.append(word)) return Result::OutOfMemory;
      }
      if (!rewritten.push_back('\n')) return Result::OutOfMemory;
      any_exec = true;
      continue;
    }
    if (line.starts_with("TryExec=")) {
      if (!join(&rewritten, rewritten.view(), "TryExec=", linux_program.view(), "\n")) return Result::OutOfMemory;
      continue;
    }
    if (line.starts_with("Icon=") && have_icon) {
      if (!join(&rewritten, rewritten.view(), "Icon=", linux_icon.view(), "\n")) return Result::OutOfMemory;
      continue;
    }
    if (line.starts_with("Name=")) any_name = true;
    if (!rewritten.append(line) || !rewritten.push_back('\n')) return Result::OutOfMemory;
  }
  if (!any_exec) {      // no desktop entry in the image (or one without a program): a plain one
    rewritten.clear();
    if (!join(&rewritten, "[Desktop Entry]\nType=Application\nName=", name, "\nExec=", linux_program.view()) || !rewritten.push_back('\n')) return Result::OutOfMemory;
    if (have_icon && !join(&rewritten, rewritten.view(), "Icon=", linux_icon.view(), "\n")) return Result::OutOfMemory;
    any_name = true;
  }
  if (!any_name) {
    String named(allocator_);
    const uint64_t header = rewritten.view().find("[Desktop Entry]\n");
    if (header != StringView::npos) {
      const uint64_t after = header + 16;
      if (!join(&named, rewritten.view().first(after), "Name=", name, "\n") || !named.append(rewritten.view().substr(after))) return Result::OutOfMemory;
      rewritten = util::move(named);
    }
  }
  String applications(allocator_), desktop_path(allocator_);
  if (!applications.assign(filesystem::layout::kLinuxLocalApplications) || !join(&desktop_path, applications.view(), "/", name, "-AM.desktop")) return Result::OutOfMemory;
  if (!make_folders(files_, applications.view()) || !write_file(desktop_path.view(), rewritten.view())) {
    detail->assign(desktop_path.view());
    return Result::FileError;
  }
  // /usr/local/bin/<name>, where the file system has links (relative, so that it is right both in the Linux world and in the OS's tree).
  String link(allocator_), target(allocator_);
  if (join(&link, filesystem::layout::kLinuxLocalBin, "/", name) && join(&target, "../../../opt/", name, "/", name) &&
      make_folders(files_, filesystem::layout::kLinuxLocalBin)) {
    (void)files_.remove(link.view());
    (void)files_.create_symbolic_link(link.view(), target.view(), false);
  }
  return Result::Ok;
}

bool Installer::remove_tree(StringView path, uint32_t depth) {
  if (depth > 32) return false;
  filesystem::DirectoryEntry entry;
  const Status s = files_.stat(path, &entry);
  if (s == Status::NotFound) return true;
  if (s == Status::Ok && entry.type == filesystem::NodeType::Directory && !entry.is_symlink) {
    List<String> names(allocator_);
    {
      UniquePointer<filesystem::IDirectory> listing;
      if (files_.open_directory(path, &listing) != Status::Ok) return false;
      filesystem::DirectoryEntry child;
      for (uint32_t guard = 0; guard < 100000 && listing->next(&child) == Status::Ok; ++guard) {
        String full(allocator_);
        if (!join(&full, path, "/", child.name.view()) || !names.push_back(util::move(full))) return false;
      }
    }
    for (const String& child : names.view())
      if (!remove_tree(child.view(), depth + 1)) return false;
  }
  const Status r = files_.remove(path);
  return r == Status::Ok || r == Status::NotFound;
}

Result Installer::remove(StringView name, String* detail) {
  detail->clear();
  if (!valid_app_name(name)) return Result::InvalidName;
  if (!installed(name)) return Result::NotInstalled;
  String dir(allocator_), desktop(allocator_), link(allocator_);
  if (!join(&dir, filesystem::layout::kLinuxOpt, "/", name) || !join(&desktop, filesystem::layout::kLinuxLocalApplications, "/", name, "-AM.desktop") ||
      !join(&link, filesystem::layout::kLinuxLocalBin, "/", name))
    return Result::OutOfMemory;
  (void)files_.remove(link.view());
  (void)files_.remove(desktop.view());
  if (!remove_tree(dir.view(), 0)) {
    detail->assign(dir.view());
    return Result::FileError;
  }
  return Result::Ok;
}

}  // namespace desktop::appman
