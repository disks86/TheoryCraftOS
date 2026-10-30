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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Process/ModuleLoader.hpp"
#include "Process/Process.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// The state a program's first thread starts with.
struct ProgramStart {
  UserArch arch = UserArch::None;
  uint64_t entry = 0;
  uint64_t stack_pointer = 0;
  uint64_t argument = 0;      // first argument register (Windows without libraries: the PEB; with them, the program's entry point for RtlUserThreadStart)
  uint64_t argument2 = 0;     // second argument register (Windows with libraries: the PEB)
  uint64_t fs_base = 0;       // thread pointers the thread starts with (Windows: gs_base is the TEB)
  uint64_t gs_base = 0;
};

// A user-mode program ready to run: its address space, its Process (with the syscall interface its executable kind selects) and the start state of its
// first thread. load() is the one call that turns the bytes of a file into all of that:
//   identify (ELF or PE) -> parse -> choose the system-call interface -> map the image -> map the stack -> build the startup block -> attach the
//   address space to the process.
// Linux programs get arguments and environment on their stack; Windows programs get them in the PEB's process parameters (the command line is the
// arguments joined by spaces). The first argument is also the program's path.
class UserProgram final : public IModuleHost {
 public:
  UserProgram(allocators::IAllocator& allocator, uint64_t id, IProcessHost& host, uint64_t memory_limit = 256ull << 20)
      : allocator_(allocator), id_(id), host_(host), space_(allocator, memory_limit), detail_(allocator), working_directory_(allocator), command_line_(allocator), startup_title_(allocator), startup_desktop_(allocator) {}
  UserProgram(const UserProgram&) = delete;
  UserProgram& operator=(const UserProgram&) = delete;

  // The OS file system the program's tree lives in ("/Linux" for a Linux program, "/Windows" for a Windows one). Set before load(); without it the program
  // has no files. The file system must outlive the program.
  void set_file_system(filesystem::IFileSystem* fs) { files_ = fs; }

  // Where the DLLs a Windows program imports come from (nullptr: none, a program with imports is then refused). Set before load(). It must outlive the program.
  void set_library_provider(ILibraryProvider* provider) { libraries_ = provider; }
  // The program's current folder, as a path inside its world ("/" or "/home/me", also for a Windows program: C:\home\me). Set before load().
  void set_working_directory(StringView path) { working_directory_.assign(path); }

  // The command line a Windows program sees, when it is not simply the arguments joined (CreateProcess hands over the exact text). Set before load().
  void set_command_line(StringView text) { command_line_.assign(text); }
  // The startup information (STARTUPINFO, the console) a Windows program finds in its process parameters. Set before load().
  void set_startup(const StartupSpec& spec) {
    startup_ = spec;
    startup_title_.assign(spec.title);
    startup_desktop_.assign(spec.desktop);
    startup_.title = startup_title_.view();
    startup_.desktop = startup_desktop_.view();
    has_startup_ = true;
  }
  filesystem::IFileSystem* file_system() const { return files_; }
  ILibraryProvider* library_provider() const { return libraries_; }
  IProcessHost& host() const { return host_; }

  // How many processors the program is told the machine has (PEB, shared data). Set before load().
  void set_processor_count(uint32_t count) { processor_count_ = count ? count : 1; }

  // IModuleHost: LoadLibrary and FreeLibrary of a Windows program.
  uint32_t load_module(ConstByteView file, StringView canonical_name, LoadedModule* out, uint32_t capacity, uint32_t* count, bool bind) override;
  bool reference_module(StringView canonical_name, uint64_t* base) override;
  bool module_name_at(uint64_t address, char (&name)[40]) const override;
  uint32_t release_module(uint64_t base, LoadedModule* out, uint32_t capacity, uint32_t* count) override;
  uint32_t unload_module(uint64_t base) override;

  // The child's standard handles (RTL_USER_PROCESS_PARAMETERS): after load(), before it runs. 0 or -1 leaves the value as it was.
  bool set_standard_handles(const uint64_t handles[3]);

  // Why load() failed, when it can say more than the status ("kernel32.dll is not a 64-bit x86 DLL"); empty otherwise.
  StringView load_detail() const { return detail_.view(); }
  // A call into an import that nothing provides ends in a stub: which import was it ("KERNEL32.dll!Foo")? False if `address` is not in a stub.
  bool describe_missing_import(uint64_t address, String* out) const { return modules_ && modules_->describe_missing(address, out); }
  const ModuleSet* modules() const { return modules_.get(); }

  // `random` is the 16 bytes of AT_RANDOM. Call once; on failure the program is unusable (create another).
  LoadStatus load(ConstByteView file, ArrayView<const StringView> arguments, ArrayView<const StringView> environment, ConstByteView random);

  bool loaded() const { return process_ ? true : false; }
  Process* process() { return process_.get(); }
  AddressSpace& address_space() { return space_; }
  const ProgramStart& start() const { return start_; }
  const LoadedImage& image() const { return image_; }

 private:
  allocators::IAllocator& allocator_;
  uint64_t id_;
  IProcessHost& host_;
  AddressSpace space_;
  filesystem::IFileSystem* files_ = nullptr;
  ILibraryProvider* libraries_ = nullptr;
  UniquePointer<ModuleSet> modules_;
  String detail_, working_directory_, command_line_, startup_title_, startup_desktop_;
  StartupSpec startup_;
  bool has_startup_ = false;
  uint32_t processor_count_ = 1;
  UniquePointer<Process> process_;
  ProgramStart start_;
  LoadedImage image_;
};

}  // namespace process
