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

#include "Desktop/Shell.hpp"
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/FileStatus.hpp"
#include "Desktop/FileTransfer.hpp"
#include "Desktop/Format.hpp"

namespace desktop {

using filesystem::DirectoryEntry;
using filesystem::IFile;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

void Shell::say(StringView a, StringView b, StringView c) const {
  String line(allocator_);
  line.append(a);
  line.append(b);
  line.append(c);
  line.push_back('\n');
  print(line.view());
}

void Shell::fail(StringView command, StringView subject, Status status) const {
  String line(allocator_);
  line.append(command);
  line.append(": ");
  if (!subject.empty()) {
    line.append(subject);
    line.append(": ");
  }
  line.append(describe_status(status));
  line.push_back('\n');
  print(line.view());
}

bool Shell::resolve(StringView path, String* out) const {
  String joined(allocator_);
  if (path.empty() || path[0] != '/') {
    joined.assign(cwd_.view());
    joined.push_back('/');
  }
  joined.append(path);
  // Walk the components, dropping "." and letting ".." remove the one before it.
  List<uint64_t> starts(allocator_);   // where each kept component begins in `out`
  out->clear();
  uint64_t i = 0;
  const StringView text = joined.view();
  while (i < text.size()) {
    while (i < text.size() && text[i] == '/') ++i;
    uint64_t end = i;
    while (end < text.size() && text[end] != '/') ++end;
    const StringView part = text.substr(i, end - i);
    i = end;
    if (part.empty() || part.equals(".")) continue;
    if (part.equals("..")) {
      if (starts.count() == 0) continue;   // (above the root: stays at the root, like a real shell)
      out->erase(starts[starts.count() - 1], out->size() - starts[starts.count() - 1]);
      starts.remove_at(starts.count() - 1);
      continue;
    }
    starts.push_back(out->size());
    if (!out->push_back('/') || !out->append(part)) return false;
  }
  if (out->empty()) out->push_back('/');
  return true;
}

bool Shell::path_of(StringView command, StringView argument, String* out) const {
  if (!resolve(argument, out)) {
    fail(command, argument, Status::InvalidPath);
    return false;
  }
  return true;
}

Status Shell::change_directory(StringView path) {
  if (!fs_) return Status::NotFound;
  String target(allocator_);
  if (!resolve(path, &target)) return Status::InvalidPath;
  if (!target.view().equals("/")) {
    DirectoryEntry entry;
    const Status status = fs_->stat(target.view(), &entry);
    if (status != Status::Ok) return status;
    if (entry.type != NodeType::Directory) return Status::NotADirectory;
  }
  cwd_ = static_cast<String&&>(target);
  return Status::Ok;
}

// Splits at blanks; quotes group. `storage` holds the argument texts one after another, NUL-separated.
bool Shell::parse(StringView line, String* storage, StringView* args, uint32_t* count) const {
  *count = 0;
  uint64_t starts[kMaxArguments], lengths[kMaxArguments];
  uint64_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) break;
    if (*count == kMaxArguments) return false;
    starts[*count] = storage->size();
    char quote = 0;
    for (; i < line.size(); ++i) {
      const char c = line[i];
      if (quote) {
        if (c == quote) quote = 0;
        else storage->push_back(c);
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == ' ' || c == '\t') {
        break;
      } else {
        storage->push_back(c);
      }
    }
    lengths[*count] = storage->size() - starts[*count];
    ++*count;
  }
  for (uint32_t a = 0; a < *count; ++a) args[a] = storage->view().substr(starts[a], lengths[a]);
  return true;
}

void Shell::execute(StringView line) {
  String storage(allocator_);
  StringView args[kMaxArguments];
  uint32_t count = 0;
  if (!parse(line, &storage, args, &count)) {
    say("too many arguments");
    return;
  }
  if (count == 0) return;
  const StringView command = args[0];
  if (command.equals("help")) return help();
  if (command.equals("clear")) {
    if (host_.clear) host_.clear(host_.context);
    return;
  }
  if (command.equals("exit")) {
    if (host_.exit) host_.exit(host_.context);
    return;
  }
  if (command.equals("echo")) {
    String text(allocator_);
    for (uint32_t i = 1; i < count; ++i) {
      if (i > 1) text.push_back(' ');
      text.append(args[i]);
    }
    text.push_back('\n');
    print(text.view());
    return;
  }
  if (command.equals("pwd")) return say(cwd_.view());
  if (!fs_) return say(command, ": no storage available");
  if (command.equals("cd")) {
    const Status status = change_directory(count > 1 ? args[1] : StringView("/"));
    if (status != Status::Ok) fail("cd", count > 1 ? args[1] : StringView(), status);
    return;
  }
  if (command.equals("ls") || command.equals("dir")) return ls(args, count);
  if (command.equals("cat")) {
    if (count < 2) return say("usage: cat file...");
    for (uint32_t i = 1; i < count; ++i) cat(args[i]);
    return;
  }
  if (command.equals("mkdir") || command.equals("touch")) {
    if (count < 2) return say("usage: ", command, " name...");
    for (uint32_t i = 1; i < count; ++i) {
      String path(allocator_);
      if (!path_of(command, args[i], &path)) continue;
      Status status;
      if (command.equals("mkdir")) {
        status = fs_->create_directory(path.view());
      } else {
        UniquePointer<IFile> file;
        status = fs_->open_file(path.view(), OpenMode::CreateOrOpen, &file);
      }
      if (status != Status::Ok) fail(command, args[i], status);
    }
    fs_->flush();
    return;
  }
  if (command.equals("rm") || command.equals("rmdir")) return rm(args, count);
  if (command.equals("cp")) return cp_or_mv(command, args, count, false);
  if (command.equals("mv")) return cp_or_mv(command, args, count, true);
  if (command.equals("stat")) {
    if (count < 2) return say("usage: stat path");
    return stat(args[1]);
  }
  if (run_program(command, args, count)) return;
  say(command, ": command not found (try help)");
}

// A command that is not built in but names a file (./tool, /bin/tool, tool in the current folder): run it if the host can, else say it cannot be
// run. False if the word names no ELF or PE file.
bool Shell::run_program(StringView command, StringView* args, uint32_t count) {
  String path(allocator_);
  if (!resolve(command, &path)) return false;
  DirectoryEntry entry;
  if (fs_->stat(path.view(), &entry) != Status::Ok || entry.type == NodeType::Directory) return false;
  const exe::ExecutableKind kind = exe::identify_executable_file(*fs_, path.view());
  if (kind == exe::ExecutableKind::None) return false;
  if (host_.run_executable && host_.run_executable(host_.context, path.view(), kind, ArrayView<const StringView>(args + 1, count - 1))) return true;
  String reason(allocator_);
  if (reason.append(": unsupported executable (") && reason.append(exe::executable_kind_name(kind)) && reason.append(" programs cannot be run yet)")) say(command, reason.view());
  if (host_.unsupported_executable) host_.unsupported_executable(host_.context, path.view(), kind);
  return true;
}

void Shell::help() const {
  print("Commands:\n"
        "  pwd                 show the current folder\n"
        "  cd [folder]         go to a folder (.. is the parent)\n"
        "  ls [-l] [folder]    list a folder\n"
        "  cat file...         show files\n"
        "  echo text...        print text\n"
        "  mkdir folder...     make folders\n"
        "  touch file...       make empty files\n"
        "  rm [-r] path...     remove files (-r: folders and what is in them)\n"
        "  cp source dest      copy a file or folder\n"
        "  mv source dest      move or rename\n"
        "  stat path           show what a path is\n"
        "  clear, exit         clear the screen, close the terminal\n"
        "A word that names an ELF (Linux) or PE (Windows) program runs it: ./tool arg... (see the Linux and Windows folders).\n"
        "Drop a folder from the file explorer on the window to go there; a file puts its path on the line.\n");
}

void Shell::ls(StringView* args, uint32_t count) {
  bool longer = false;
  StringView target(".");
  for (uint32_t i = 1; i < count; ++i) {
    if (args[i].equals("-l")) longer = true;
    else target = args[i];
  }
  String path(allocator_);
  if (!path_of("ls", target, &path)) return;
  DirectoryEntry self;
  if (!path.view().equals("/")) {
    const Status status = fs_->stat(path.view(), &self);
    if (status != Status::Ok) return fail("ls", target, status);
    if (self.type != NodeType::Directory) {   // a file: just its name
      return say(target);
    }
  }
  List<DirectoryEntry> entries(allocator_);
  const Status status = read_sorted_directory(*fs_, path.view(), &entries);
  if (status != Status::Ok) return fail("ls", target, status);
  String out(allocator_), size(allocator_);
  for (const DirectoryEntry& e : entries) {
    const bool dir = e.type == NodeType::Directory;
    if (longer) {
      out.append(dir ? "d " : "- ");
      size.clear();
      if (!dir) format_size(e.size, &size);
      for (uint64_t pad = size.size(); pad < 10; ++pad) out.push_back(' ');
      out.append(size.view());
      out.append("  ");
    }
    out.append(e.name.view());
    if (dir) out.push_back('/');
    out.push_back('\n');
  }
  print(out.view());
}

void Shell::cat(StringView argument) {
  String path(allocator_);
  if (!path_of("cat", argument, &path)) return;
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path.view(), OpenMode::Read, &file);
  if (status != Status::Ok) return fail("cat", argument, status);
  constexpr uint64_t kChunk = 4096, kMaxBytes = 1024 * 1024;
  uint8_t buffer[kChunk];
  uint64_t offset = 0;
  bool newline_last = true;
  while (offset < kMaxBytes) {
    uint64_t got = 0;
    status = file->read(offset, ByteView(buffer, kChunk), &got);
    if (status != Status::Ok) return fail("cat", argument, status);
    if (got == 0) break;
    print(StringView(reinterpret_cast<const char*>(buffer), got));
    newline_last = buffer[got - 1] == '\n';
    offset += got;
  }
  if (offset >= kMaxBytes) say("\n[output stopped after 1 MB]");
  else if (!newline_last) print("\n");
}

void Shell::rm(StringView* args, uint32_t count) {
  bool recursive = false;
  uint32_t named = 0;
  for (uint32_t i = 1; i < count; ++i) {
    if (args[i].equals("-r") || args[i].equals("-rf")) { recursive = true; continue; }
    ++named;
    String path(allocator_);
    if (!path_of("rm", args[i], &path)) continue;
    if (path.view().equals("/")) { say("rm: refusing to remove the root"); continue; }
    DirectoryEntry entry;
    Status status = fs_->stat(path.view(), &entry);
    if (status == Status::Ok && entry.type == NodeType::Directory && !recursive) status = Status::IsADirectory;
    else if (status == Status::Ok) status = FileTransfer::remove_tree(allocator_, *fs_, path.view());
    if (status != Status::Ok) fail("rm", args[i], status);
    else if (cwd_.view().equals(path.view()) || (cwd_.size() > path.size() && cwd_.view().substr(0, path.size()).equals(path.view()) && cwd_[path.size()] == '/'))
      cwd_.assign("/");   // the current folder is gone
  }
  if (!named) say("usage: rm [-r] path...");
  fs_->flush();
}

void Shell::cp_or_mv(StringView command, StringView* args, uint32_t count, bool move) {
  StringView names[2];
  uint32_t n = 0;
  for (uint32_t i = 1; i < count; ++i) {
    if (args[i].equals("-r")) continue;
    if (n < 2) names[n] = args[i];
    ++n;
  }
  if (n != 2) return say("usage: ", command, " source destination");
  String from(allocator_), to(allocator_);
  if (!path_of(command, names[0], &from) || !path_of(command, names[1], &to)) return;
  DirectoryEntry entry;
  Status status = fs_->stat(from.view(), &entry);
  if (status != Status::Ok) return fail(command, names[0], status);
  // Into a folder that exists: keeps its name there.
  if (fs_->stat(to.view(), &entry) == Status::Ok && entry.type == NodeType::Directory) {
    if (!to.view().equals("/")) to.push_back('/');
    to.append(from.view().substr(from.view().rfind('/') + 1));
  }
  if (from.view().equals(to.view())) return;
  if (to.size() > from.size() && to.view().substr(0, from.size()).equals(from.view()) && to[from.size()] == '/')
    return say(command, ": a folder cannot go into itself");
  if (fs_->stat(to.view(), &entry) != Status::NotFound) return fail(command, names[1], Status::AlreadyExists);
  if (move) status = fs_->rename(from.view(), to.view());
  if (!move || status == Status::Unsupported) {
    FileTransfer transfer(allocator_, *fs_);
    status = transfer.measure(from.view());
    if (status == Status::Ok) status = transfer.copy(from.view(), to.view());
    if (status == Status::Ok && move) status = FileTransfer::remove_tree(allocator_, *fs_, from.view());
  }
  fs_->flush();
  if (status != Status::Ok) fail(command, names[0], status);
}

void Shell::stat(StringView argument) {
  String path(allocator_);
  if (!path_of("stat", argument, &path)) return;
  DirectoryEntry entry;
  if (path.view().equals("/")) return say("/: folder");
  const Status status = fs_->stat(path.view(), &entry);
  if (status != Status::Ok) return fail("stat", argument, status);
  String size(allocator_);
  if (entry.type == NodeType::Directory) return say(path.view(), ": folder");
  format_size(entry.size, &size);
  say(path.view(), ": file, ", size.view());
}

}  // namespace desktop
