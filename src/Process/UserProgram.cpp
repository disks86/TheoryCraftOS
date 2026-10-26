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
#include "Process/UserProgram.hpp"
#include "Executable/ElfFile.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Executable/PeFile.hpp"
#include "Process/WindowsStartup.hpp"
#include "Utility/String.hpp"

namespace process {

namespace {
// The arguments as a Windows command line: joined by spaces, an argument with a space in quotes.
bool join_command_line(ArrayView<const StringView> arguments, String* out) {
  bool first = true;
  for (const StringView& argument : arguments) {
    const bool quote = argument.find(' ') != StringView::npos;
    if (!first && !out->append(" ")) return false;
    if ((quote && !out->append("\"")) || !out->append(argument) || (quote && !out->append("\""))) return false;
    first = false;
  }
  return true;
}
}  // namespace

LoadStatus UserProgram::load(ConstByteView file, ArrayView<const StringView> arguments, ArrayView<const StringView> environment, ConstByteView random) {
  if (process_) return LoadStatus::Malformed;      // already loaded
  const exe::ExecutableKind kind = exe::identify_executable(file.at_most(exe::kIdentifyBytes));
  ProcessBinding binding;
  LoadedImage image;
  uint64_t stack_top = 0, stack_pointer = 0, heap_start = 0, fs_base = 0, gs_base = 0, argument = 0, argument2 = 0, dispatcher = 0, thread_thunk = 0, thread_teb = 0;
  Process::ThreadSupport thread_support;
  UniquePointer<ModuleSet> loaded_modules;      // (kept by the program once everything worked)
  const StringView path = arguments.empty() ? StringView("") : arguments[0];
  if (kind == exe::ExecutableKind::Elf) {
    exe::ElfFile elf(allocator_);
    if (!elf.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(elf, &binding)) return LoadStatus::Unsupported;
    LoadStatus status = load_elf(elf, file, space_, &image);
    if (status == LoadStatus::Ok) status = map_stack(space_, kDefaultStackBytes, &stack_top);
    if (status == LoadStatus::Ok) status = build_linux_stack(space_, stack_top, image, arguments, environment, path, random, &stack_pointer);
    if (status != LoadStatus::Ok) return status;
    if (!AddressSpace::round_up(image.end, &heap_start)) return LoadStatus::Malformed;
  } else if (kind == exe::ExecutableKind::Pe) {
    exe::PeFile pe(allocator_);
    if (!pe.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(pe, &binding)) return LoadStatus::Unsupported;
    if (!pe.imports().empty() && !libraries_) return LoadStatus::NeedsLibraries;      // nothing resolves imports: it would jump to address 0
    LoadStatus status = load_pe(pe, file, space_, &image);
    if (status != LoadStatus::Ok) return status;
    const StringView program_file = path.empty() ? StringView("program.exe") : path.substr(path.rfind('\\') == StringView::npos ? 0 : path.rfind('\\') + 1);
    UniquePointer<ModuleSet> modules;
    if (libraries_) {
      modules = UniquePointer<ModuleSet>::make(allocator_, allocator_);
      if (!modules) return LoadStatus::OutOfMemory;
      status = load_windows_modules(space_, allocator_, pe, file, program_file, *libraries_, modules.get(), &detail_);
      if (status != LoadStatus::Ok) return status;
    }
    status = map_stack(space_, kDefaultStackBytes, &stack_top);
    if (status != LoadStatus::Ok) return status;
    stack_pointer = windows_initial_stack_pointer(stack_top);
    String command_line(allocator_);
    if (!join_command_line(arguments, &command_line)) return LoadStatus::OutOfMemory;
    WindowsStartupInput input;
    input.allocator = &allocator_;
    input.process_id = id_;
    input.thread_id = id_;
    input.stack_top = stack_top;
    input.stack_size = kDefaultStackBytes;
    input.image_path = path.empty() ? StringView("C:\\program.exe") : path;
    input.command_line = command_line.view();
    input.environment = environment;
    input.processor_count = 1;
    input.current_directory = working_directory_.empty() ? StringView("/") : working_directory_.view();
    // The module lists for PEB.Ldr: the program, then the libraries in load order; the initialization order as indexes.
    WindowsModule listed[64];
    uint32_t init_order[64];
    uint32_t listed_count = 0, init_count = 0;
    if (modules) {
      for (uint64_t i = 0; i < modules->count() && listed_count < 64; ++i) {
        const Module& m = modules->at(i);
        listed[listed_count++] = WindowsModule{m.main ? program_file : m.name.view(), m.base, m.size, m.entry, m.main, true};
      }
      for (uint64_t i = 0; i < modules->init_count() && init_count < 64; ++i) {
        for (uint32_t j = 0; j < listed_count; ++j) {
          if (&modules->at(j) == &modules->init_at(i)) init_order[init_count++] = j;
        }
      }
      input.modules = ArrayView<const WindowsModule>(listed, listed_count);
      input.init_order = ArrayView<const uint32_t>(init_order, init_count);
    }
    WindowsStartup startup;
    status = build_windows_startup(space_, pe, image, input, &startup);
    if (status != LoadStatus::Ok) return status;
    gs_base = startup.teb;
    argument = startup.peb;
    // With ntdll the thread starts in RtlUserThreadStart(entry, parameter), which runs the libraries' entry points and then the program's.
    const uint64_t thunk = modules ? modules->export_address("ntdll.dll", "RtlUserThreadStart") : 0;
    if (thunk != 0) {
      argument = image.entry;
      argument2 = startup.peb;
      image.entry = thunk;
    }
    dispatcher = modules ? modules->export_address("ntdll.dll", "KiUserExceptionDispatcher") : 0;
    thread_thunk = modules ? modules->export_address("ntdll.dll", "TcosThreadStart") : 0;
    thread_support.peb = startup.peb;
    thread_support.tls_template = startup.tls_template;
    thread_support.tls_template_bytes = startup.tls_template_bytes;
    thread_support.tls_zero_fill = startup.tls_zero_fill;
    thread_support.start_thunk = thread_thunk;
    thread_teb = startup.teb;
    loaded_modules = static_cast<UniquePointer<ModuleSet>&&>(modules);
  } else {
    return LoadStatus::NotExecutable;
  }
  UniquePointer<Process> process = UniquePointer<Process>::make(allocator_, id_, binding, space_, host_);
  if (!process) return LoadStatus::OutOfMemory;
  process->attach_address_space(space_, heap_start);
  process->set_initial_thread_pointers(fs_base, gs_base);
  process->set_exception_dispatcher(dispatcher);
  if (binding.abi == SyscallAbi::Windows) {
    process->set_thread_support(thread_support);
    process->register_main_thread(id_, thread_teb);
  }
  if (files_) {
    process->attach_file_system(allocator_, *files_);
    if (!working_directory_.empty()) process->files().change_directory(working_directory_.view());
  }
  modules_ = static_cast<UniquePointer<ModuleSet>&&>(loaded_modules);
  start_ = ProgramStart{binding.arch, image.entry, stack_pointer, argument, argument2, fs_base, gs_base};
  image_ = image;
  process_ = static_cast<UniquePointer<Process>&&>(process);
  return LoadStatus::Ok;
}

}  // namespace process
