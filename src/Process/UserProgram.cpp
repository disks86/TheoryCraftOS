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

namespace {
// The library file LoadLibrary found (under its name), then the usual places.
class SingleFileProvider final : public ILibraryProvider {
 public:
  SingleFileProvider(StringView name, ConstByteView file, ILibraryProvider* fallback) : name_(name), file_(file), fallback_(fallback) {}
  bool find(StringView canonical_name, ConstByteView* image) override {
    if (canonical_name.equals(name_)) { *image = file_; return true; }
    return fallback_ && fallback_->find(canonical_name, image);
  }

 private:
  StringView name_;
  ConstByteView file_;
  ILibraryProvider* fallback_;
};
}  // namespace

namespace {

uint64_t read_u64(AddressSpace& space, uint64_t address, bool* ok) {
  uint8_t raw[8] = {};
  if (!space.read_privileged(address, ByteView(raw, 8))) { *ok = false; return 0; }
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i);
  return v;
}
bool write_u64(AddressSpace& space, uint64_t address, uint64_t v) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(v >> (8 * i));
  return space.write_privileged(address, ConstByteView(raw, 8));
}

// The thread-local data a library brought in has: a slot in every thread's array and a block for every thread that exists (threads made later get theirs from the process's
// templates). False (nothing changed) if there is no room.
bool give_tls(AddressSpace& space, Process& process, Module& module) {
  uint32_t rva = 0, size = 0;
  if (!module.pe->directory(9, &rva, &size) || size < 0x28) return true;
  bool ok = true;
  const uint64_t directory = module.base + rva;
  const uint64_t start = read_u64(space, directory, &ok), end = read_u64(space, directory + 8, &ok), index_at = read_u64(space, directory + 0x10, &ok);
  uint64_t zero_fill = read_u64(space, directory + 0x20, &ok) & 0xFFFFFFFFu;
  if (!ok) return false;
  if (end <= start && zero_fill == 0) return true;      // (only callbacks)
  const uint64_t bytes = end - start;
  Process::ThreadSupport& support = process.thread_support_mutable();
  uint32_t index = 0;
  while (index < kMaxTlsModules && support.tls_taken[index]) ++index;      // (the lowest free slot)
  if (index >= kMaxTlsModules || bytes > (1u << 20) || zero_fill > (1u << 20)) return false;
  uint64_t block_size = 0;
  if (!AddressSpace::round_up(bytes + zero_fill, &block_size)) return false;
  // a block for every thread
  uint32_t done = 0;
  bool failed = false;
  for (uint32_t t = 0; t < Process::kMaxThreads && !failed; ++t) {
    Process::ThreadRecord* thread = process.thread_at(t);
    if (!thread->used || thread->finished || thread->teb == 0) continue;
    uint64_t array = read_u64(space, thread->teb + 0x58, &ok), at = 0;
    if (ok && array == 0) array = read_u64(space, thread->teb + 0x1788, &ok);      // (a thread that has not started: see write_tls)
    if (!ok || array == 0 || !space.find_free(block_size, 0x00007FF000000000ull, &at) || !space.map(at, block_size, kProtRead | kProtWrite)) { failed = true; break; }
    uint8_t chunk[256];
    for (uint64_t copied = 0; copied < bytes && !failed;) {
      const uint64_t k = bytes - copied < sizeof(chunk) ? bytes - copied : sizeof(chunk);
      if (!space.read_privileged(start + copied, ByteView(chunk, k)) || !space.write_privileged(at + copied, ConstByteView(chunk, k))) failed = true;
      copied += k;
    }
    if (failed || !write_u64(space, array + 8 * uint64_t{index}, at)) { space.unmap(at, block_size); failed = true; break; }
    thread->extra_tls[index] = at;
    thread->extra_tls_size[index] = block_size;
    ++done;
  }
  if (!failed) {
    uint8_t raw[4] = {uint8_t(index), uint8_t(index >> 8), uint8_t(index >> 16), uint8_t(index >> 24)};
    if (index_at != 0 && !space.write_privileged(index_at, ConstByteView(raw, 4))) failed = true;
  }
  if (failed) {
    for (uint32_t t = 0; t < Process::kMaxThreads; ++t) {
      Process::ThreadRecord* thread = process.thread_at(t);
      if (thread->extra_tls[index] != 0) { space.unmap(thread->extra_tls[index], thread->extra_tls_size[index]); thread->extra_tls[index] = 0; }
    }
    return false;
  }
  support.tls[index] = TlsTemplate{start, bytes, zero_fill};
  support.tls_taken[index] = true;
  if (index + 1 > support.tls_count) support.tls_count = index + 1;
  module.tls_index = static_cast<int32_t>(index);
  (void)done;
  return true;
}

// The library goes: its blocks are freed, and threads made from now on get an empty one in its slot.
void take_tls(AddressSpace& space, Process& process, const Module& module) {
  if (module.tls_index < 0) return;
  const uint32_t index = static_cast<uint32_t>(module.tls_index);
  for (uint32_t t = 0; t < Process::kMaxThreads; ++t) {
    Process::ThreadRecord* thread = process.thread_at(t);
    if (thread->extra_tls[index] == 0) continue;
    space.unmap(thread->extra_tls[index], thread->extra_tls_size[index]);
    thread->extra_tls[index] = 0;
    thread->extra_tls_size[index] = 0;
  }
  process.thread_support_mutable().tls[index] = TlsTemplate{};
  process.thread_support_mutable().tls_taken[index] = false;
}

}  // namespace

uint32_t UserProgram::load_module(ConstByteView file, StringView canonical_name, LoadedModule* out, uint32_t capacity, uint32_t* count, bool bind) {
  constexpr uint32_t kNoMemory = 0xC0000017u, kDllNotFound = 0xC0000135u, kBadImage = 0xC000007Bu, kNotSupported = 0xC00000BBu, kBufferTooSmall = 0xC0000023u, kInvalidParameter = 0xC000000Du;
  *count = 0;
  if (!modules_ || !process_) return kNotSupported;
  if (modules_->find(canonical_name)) return kInvalidParameter;      // (the caller looks first)
  const uint64_t first = modules_->count(), init_first = modules_->init_count();
  SingleFileProvider provider(canonical_name, file, libraries_);
  String detail(allocator_);
  switch (load_additional_library(space_, allocator_, provider, modules_.get(), canonical_name, &detail, bind)) {
    case LoadStatus::Ok: break;
    case LoadStatus::OutOfMemory: process_log(detail.view()); process_log(canonical_name); return kNoMemory;
    case LoadStatus::Unsupported: return kNotSupported;
    case LoadStatus::Malformed: return kBadImage;
    default: return kDllNotFound;
  }
  const uint64_t fresh = modules_->init_count() - init_first;
  if (fresh > capacity) {
    modules_->drop_from(space_, first);
    return kBufferTooSmall;
  }
  for (uint64_t i = first; i < modules_->count(); ++i) {      // thread-local data of their own
    if (modules_->modules_[i]->no_bind) continue;
    if (!give_tls(space_, *process_, *modules_->modules_[i])) {
      for (uint64_t k = first; k < i; ++k) take_tls(space_, *process_, *modules_->modules_[k]);
      modules_->drop_from(space_, first);
      return kNotSupported;
    }
  }
  for (uint64_t i = 0; i < fresh; ++i) {
    const Module& m = modules_->init_at(init_first + i);
    LoadedModule& o = out[i];
    o = LoadedModule();
    o.base = m.base;
    o.size = m.size;
    o.entry = m.entry == m.base ? 0 : m.entry;
    for (uint64_t k = 0; k + 1 < sizeof(o.name) && k < m.name.size(); ++k) o.name[k] = m.name.view()[k];
  }
  *count = static_cast<uint32_t>(fresh);
  return 0;
}

bool UserProgram::set_standard_handles(const uint64_t handles[3]) {
  if (!process_ || process_->abi() != SyscallAbi::Windows) return false;
  bool ok = true;
  const uint64_t peb = read_u64(space_, process_->gs_base() + 0x60, &ok);
  const uint64_t parameters = ok ? read_u64(space_, peb + 0x20, &ok) : 0;
  if (!ok || parameters == 0) return false;
  for (uint32_t i = 0; i < 3; ++i) {
    if (handles[i] == 0 || handles[i] == ~uint64_t{0}) continue;
    if (!write_u64(space_, parameters + 0x20 + 8 * i, handles[i])) return false;
  }
  return true;
}

bool UserProgram::module_name_at(uint64_t address, char (&name)[40]) const {
  if (!modules_) return false;
  for (uint64_t i = 0; i < modules_->count(); ++i) {
    const Module& m = modules_->at(i);
    if (address < m.base || address - m.base >= m.size) continue;
    uint32_t n = 0;
    for (; n < 39 && n < m.name.size(); ++n) { const char c = m.name.view()[n]; name[n] = c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
    name[n] = 0;
    return true;
  }
  return false;
}

bool UserProgram::reference_module(StringView canonical_name, uint64_t* base) { return modules_ && reference_library(modules_.get(), canonical_name, base); }

uint32_t UserProgram::release_module(uint64_t base, LoadedModule* out, uint32_t capacity, uint32_t* count) {
  *count = 0;
  if (!modules_) return 0xC00000BBu;
  List<Module*> doomed(allocator_);
  if (!release_library(modules_.get(), allocator_, base, &doomed)) return 0xC0000008u;
  uint32_t n = 0;
  for (Module* m : doomed.view()) {
    if (n >= capacity) break;      // (more than the caller has room for stays marked; the caller asks for few at a time)
    LoadedModule& o = out[n++];
    o = LoadedModule();
    o.base = m->base;
    o.size = m->size;
    o.entry = m->entry == m->base ? 0 : m->entry;
    for (uint64_t k = 0; k + 1 < sizeof(o.name) && k < m->name.size(); ++k) o.name[k] = m->name.view()[k];
  }
  *count = n;
  return 0;
}

uint32_t UserProgram::unload_module(uint64_t base) {
  if (!modules_) return 0xC00000BBu;
  if (Module* m = modules_->find_base(base); m && process_) take_tls(space_, *process_, *m);
  return unload_library(space_, modules_.get(), base) ? 0 : 0xC0000008u;
}

LoadStatus UserProgram::load(ConstByteView file, ArrayView<const StringView> arguments, ArrayView<const StringView> environment, ConstByteView random) {
  if (process_) return LoadStatus::Malformed;      // already loaded
  const exe::ExecutableKind kind = exe::identify_executable(file.at_most(exe::kIdentifyBytes));
  ProcessBinding binding;
  LoadedImage image;
  uint64_t stack_top = 0, stack_pointer = 0, heap_start = 0, fs_base = 0, gs_base = 0, argument = 0, argument2 = 0, dispatcher = 0, thread_thunk = 0, thread_teb = 0;
  Process::ThreadSupport thread_support;
  uint8_t image_information[64] = {};
  UniquePointer<ModuleSet> loaded_modules;      // (kept by the program once everything worked)
  const StringView path = arguments.empty() ? StringView("") : arguments[0];
  if (kind == exe::ExecutableKind::Elf) {
    exe::ElfFile elf(allocator_);
    if (!elf.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(elf, &binding)) return LoadStatus::Unsupported;
    LoadStatus status = load_elf(elf, file, space_, &image);
    if (status == LoadStatus::Ok) status = map_stack(space_, kDefaultStackBytes, &stack_top);
    if (status == LoadStatus::Ok) status = build_linux_stack(space_, stack_top, image, arguments, environment, path, random, &stack_pointer,
                                                                       binding.arch == UserArch::AArch64 ? StringView("aarch64") : binding.arch == UserArch::RiscV64 ? StringView("riscv64") : StringView("x86_64"));
    if (status != LoadStatus::Ok) return status;
    if (!AddressSpace::round_up(image.end, &heap_start)) return LoadStatus::Malformed;
  } else if (kind == exe::ExecutableKind::Pe) {
    exe::PeFile pe(allocator_);
    if (!pe.parse(file)) return LoadStatus::Malformed;
    if (!bind_executable(pe, &binding)) return LoadStatus::Unsupported;
    describe_pe_image(pe, file.size(), image_information);
    if (!pe.imports().empty() && !libraries_) return LoadStatus::NeedsLibraries;      // nothing resolves imports: it would jump to address 0
    LoadStatus status = load_pe(pe, file, space_, &image, false);
    if (status != LoadStatus::Ok) return status;
    describe_image_pages(space_, pe, image.base);      // (the pages are an image, committed, whatever their access)
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
    if (!command_line_.empty() ? !command_line.assign(command_line_.view()) : !join_command_line(arguments, &command_line)) return LoadStatus::OutOfMemory;
    WindowsStartupInput input;
    input.allocator = &allocator_;
    input.process_id = id_;
    input.thread_id = id_;
    input.stack_top = stack_top;
    input.stack_size = kDefaultStackBytes;
    input.image_path = path.empty() ? StringView("C:\\program.exe") : path;
    input.command_line = command_line.view();
    input.environment = environment;
    input.processor_count = processor_count_;
    input.current_directory = working_directory_.empty() ? StringView("/") : working_directory_.view();
    // The module lists for PEB.Ldr: the program, then the libraries in load order; the initialization order as indexes.
    WindowsModule listed[64];
    uint32_t init_order[64];
    uint32_t listed_count = 0, init_count = 0;
    if (modules) {
      for (uint64_t i = 0; i < modules->count() && listed_count < 64; ++i) {
        const Module& m = modules->at(i);
        WindowsModule entry{m.main ? program_file : m.name.view(), m.base, m.size, m.entry, m.main, true};
        (m.main ? &pe : m.pe)->directory(9, &entry.tls_rva, &entry.tls_size);      // IMAGE_DIRECTORY_ENTRY_TLS (the program's headers are the caller's)
        listed[listed_count++] = entry;
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
    for (uint32_t i = 0; i < startup.tls_count; ++i) thread_support.tls[i] = startup.tls[i];
    thread_support.tls_count = startup.tls_count;
    for (uint32_t i = 0; i < startup.tls_count; ++i) thread_support.tls_taken[i] = true;
    thread_support.start_thunk = thread_thunk;
    thread_teb = startup.teb;
    loaded_modules = static_cast<UniquePointer<ModuleSet>&&>(modules);
  } else {
    return LoadStatus::NotExecutable;
  }
  UniquePointer<Process> process = UniquePointer<Process>::make(allocator_, id_, binding, space_, host_);
  if (!process) return LoadStatus::OutOfMemory;
  process->set_image_information(image_information);
  process->create_time = host_.realtime_ns() / 100 + 11644473600ull * 10000000ull;      // (FILETIME)
  process->attach_address_space(space_, heap_start);
  process->set_initial_thread_pointers(fs_base, gs_base);
  process->set_exception_dispatcher(dispatcher);
  if (loaded_modules) process->set_module_host(this);
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
