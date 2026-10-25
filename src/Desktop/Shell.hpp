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

// A small command shell for the terminal: it keeps a current folder on a filesystem and runs one command line at a time, writing what
// the command prints through the host's write function. Commands: help, pwd, cd, ls [-l], cat, echo, mkdir, touch, rm [-r], cp, mv, stat,
// clear, exit. Arguments are separated by blanks; "double" or 'single' quotes keep blanks inside an argument. Paths may be absolute or
// relative to the current folder and may use "." and ".." (resolved here: the filesystems do not know them). It works on any IFileSystem, so
// it also goes into mounted network shares. Everything runs in the calling thread.
#pragma once
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/String.hpp"

namespace desktop {

class Shell {
 public:
  struct Host {
    void (*write)(void* context, StringView text) = nullptr;
    void (*clear)(void* context) = nullptr;   // the "clear" command
    void (*exit)(void* context) = nullptr;    // the "exit" command
    // A command that names an ELF or PE file was typed: `path` is its absolute path. The shell has already printed a line about it.
    void (*unsupported_executable)(void* context, StringView path, exe::ExecutableKind kind) = nullptr;
    void* context = nullptr;
  };
  static constexpr uint32_t kMaxArguments = 16;

  explicit Shell(allocators::IAllocator& allocator) : allocator_(allocator), cwd_(allocator) { cwd_.assign("/"); }

  void set_host(const Host& host) { host_ = host; }
  // The filesystem to work on (nullptr: none); the current folder goes back to the root. It must outlive the shell.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; cwd_.assign("/"); }

  StringView cwd() const { return cwd_.view(); }
  // "/folder> "
  bool prompt(String* out) const { return out->assign(cwd_.view()) && out->append("> "); }
  // Makes `path` (absolute or relative) the current folder. The status says why not (NotFound, NotADirectory, ...).
  filesystem::Status change_directory(StringView path);
  // Turns `path` into an absolute path without "." and ".." parts. False if it goes above the root or memory ran out.
  bool resolve(StringView path, String* out) const;
  // Runs one command line.
  void execute(StringView line);

 private:
  void print(StringView text) const { if (host_.write) host_.write(host_.context, text); }
  void say(StringView a, StringView b = StringView(), StringView c = StringView()) const;
  void fail(StringView command, StringView subject, filesystem::Status status) const;
  bool parse(StringView line, String* storage, StringView* args, uint32_t* count) const;
  bool path_of(StringView command, StringView argument, String* out) const;
  void ls(StringView* args, uint32_t count);
  void cat(StringView path);
  void rm(StringView* args, uint32_t count);
  void cp_or_mv(StringView command, StringView* args, uint32_t count, bool move);
  void stat(StringView path);
  bool run_program(StringView command);
  void help() const;

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem* fs_ = nullptr;
  String cwd_;
  Host host_;
};

}  // namespace desktop
