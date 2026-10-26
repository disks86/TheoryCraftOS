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
#include "Process/ModuleLoader.hpp"
#include "Utility/ByteBuffer.hpp"

namespace process {

namespace {

constexpr uint64_t kPage = AddressSpace::kPageSize;
constexpr uint64_t kDllHint = 0x00007FF800000000ull;      // where libraries are looked for room, from here upwards (a Windows-like place)
constexpr uint32_t kMaxModules = 64;
constexpr uint32_t kMaxDepth = 16;
constexpr uint64_t kStubSize = 8, kStubsPerPage = kPage / kStubSize, kMaxStubs = kStubsPerPage * 4;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool starts_with(StringView text, const char* prefix) {
  uint64_t n = 0;
  while (prefix[n]) ++n;
  if (text.size() < n) return false;
  for (uint64_t i = 0; i < n; ++i) if (lower(text[i]) != prefix[i]) return false;
  return true;
}

}  // namespace

bool canonical_library_name(StringView name, String* out) {
  out->clear();
  if (name.empty() || name.size() > 100) return false;
  for (char c : name) if (!out->push_back(lower(c))) return false;
  if (out->view().rfind('.') == StringView::npos && !out->append(".dll")) return false;
  const StringView text = out->view();
  if (starts_with(text, "api-ms-win-core-") || starts_with(text, "api-ms-win-eventing-") || text.equals("kernelbase.dll")) return out->assign("kernel32.dll");
  if (starts_with(text, "api-ms-win-security-")) return out->assign("advapi32.dll");
  if (text.equals("wsock32.dll") || text.equals("winsock.dll")) return out->assign("ws2_32.dll");
  // All the C runtimes are one library here: it exports both the classic msvcrt names and the Universal CRT's.
  if (starts_with(text, "api-ms-win-crt-") || text.equals("ucrtbase.dll") || text.equals("ucrtbased.dll") || text.equals("crtdll.dll") || starts_with(text, "msvcr") ||
      starts_with(text, "vcruntime") || starts_with(text, "msvcp"))
    return out->assign("msvcrt.dll");
  return true;
}

struct FileLibraryProvider::Entry {
  Entry(allocators::IAllocator& allocator, StringView n) : name(allocator), bytes(allocator) { name.assign(n); }
  String name;
  ByteBuffer bytes;
  Entry* next = nullptr;
};

FileLibraryProvider::FileLibraryProvider(allocators::IAllocator& allocator, filesystem::IFileSystem* files) : allocator_(allocator), files_(files) {}

FileLibraryProvider::~FileLibraryProvider() {
  while (entries_) {
    Entry* next = entries_->next;
    entries_->~Entry();
    allocator_.deallocate(entries_);
    entries_ = next;
  }
}

bool FileLibraryProvider::find(StringView canonical_name, ConstByteView* image) {
  for (Entry* e = entries_; e; e = e->next) {
    if (e->name.view().equals(canonical_name)) {
      *image = e->bytes.view();
      return true;
    }
  }
  if (!files_ || canonical_name.contains('/') || canonical_name.contains('\\')) return false;      // a plain file name only
  String path(allocator_);
  if (!path.assign("/Windows/Windows/System32/") || !path.append(canonical_name)) return false;
  UniquePointer<filesystem::IFile> file;
  if (files_->open_file(path.view(), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > (64ull << 20)) return false;
  void* memory = allocator_.allocate(sizeof(Entry));
  if (!memory) return false;
  Entry* entry = new (memory) Entry(allocator_, canonical_name);
  if (!entry->bytes.put_zeros(size)) { entry->~Entry(); allocator_.deallocate(entry); return false; }
  ByteView bytes = entry->bytes.writable_view();
  for (uint64_t done = 0; done < size;) {
    uint64_t got = 0;
    if (file->read(done, bytes.subview(done, size - done < (1u << 20) ? size - done : (1u << 20)), &got) != filesystem::Status::Ok || got == 0) {
      entry->~Entry();
      allocator_.deallocate(entry);
      return false;
    }
    done += got;
  }
  entry->next = entries_;
  entries_ = entry;
  *image = entry->bytes.view();
  return true;
}

Module* ModuleSet::find_base(uint64_t base) const {
  for (const UniquePointer<Module>& m : modules_.view()) if (m->base == base) return m.get();
  return nullptr;
}

void ModuleSet::drop_from(AddressSpace& space, uint64_t first) {
  for (uint64_t i = order_.count(); i > 0; --i) {
    for (uint64_t k = first; k < modules_.count(); ++k) {
      if (order_[i - 1] == modules_[k].get()) { order_.remove_at(i - 1); break; }
    }
  }
  while (modules_.count() > first) {
    const Module& m = *modules_[modules_.count() - 1];
    space.unmap(m.base, m.size);
    modules_.remove_at(modules_.count() - 1);
  }
}

Module* ModuleSet::find(StringView canonical_name) const {
  for (const UniquePointer<Module>& m : modules_.view()) if (!m->doomed && m->name.view().equals(canonical_name)) return m.get();
  return nullptr;
}

bool ModuleSet::describe_missing(uint64_t address, String* out) const {
  if (stub_base_ == 0 || address < stub_base_ || (address - stub_base_) / kStubSize >= missing_.count()) return false;
  return out->assign(missing_[(address - stub_base_) / kStubSize].view());
}

namespace {

struct Context {
  AddressSpace& space;
  allocators::IAllocator& allocator;
  ILibraryProvider& provider;
  ModuleSet& set;
  String* detail;
  uint64_t next_base = kDllHint;
};

}  // namespace

bool apply_relocations(AddressSpace& space, const exe::PeFile& pe, uint64_t base, uint64_t delta) {
  if (delta == 0) return true;
  if (!pe.relocations_ok()) return false;
  for (const exe::PeRelocation& r : pe.relocations().view()) {
    uint8_t raw[8];
    if (r.rva >= pe.size_of_image() || pe.size_of_image() - r.rva < (r.type == exe::kPeRelocationDir64 ? 8u : 4u)) return false;
    if (r.type == exe::kPeRelocationDir64) {
      if (!space.read_privileged(base + r.rva, ByteView(raw, 8))) return false;
      uint64_t v = 0;
      for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i);
      v += delta;
      for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(v >> (8 * i));
      if (!space.write_privileged(base + r.rva, ConstByteView(raw, 8))) return false;
    } else {
      if (!space.read_privileged(base + r.rva, ByteView(raw, 4))) return false;
      uint32_t v = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
      v += static_cast<uint32_t>(delta);
      for (uint32_t i = 0; i < 4; ++i) raw[i] = static_cast<uint8_t>(v >> (8 * i));
      if (!space.write_privileged(base + r.rva, ConstByteView(raw, 4))) return false;
    }
  }
  return true;
}

namespace {

void say(Context& c, StringView a, StringView b = StringView()) {
  if (!c.detail || !c.detail->empty()) return;
  c.detail->assign(a);
  c.detail->append(b);
}

// Loads the library (if not loaded yet) and, first, the libraries it imports and forwards to.
LoadStatus ensure_loaded(Context& c, StringView canonical, uint32_t depth, Module** result) {
  if (Module* have = c.set.find(canonical)) {
    *result = have;
    return LoadStatus::Ok;
  }
  *result = nullptr;
  if (depth > kMaxDepth || c.set.count() >= kMaxModules) {
    say(c, "too many libraries: ", canonical);
    return LoadStatus::NeedsLibraries;
  }
  ConstByteView file;
  if (!c.provider.find(canonical, &file)) return LoadStatus::Ok;      // not available: its imports become stubs
  UniquePointer<Module> module = UniquePointer<Module>::make(c.allocator, c.allocator);
  if (!module || !module->name.assign(canonical)) return LoadStatus::OutOfMemory;
  if (!module->owned.parse(file) || !module->owned.is_dll() || !module->owned.is_pe32_plus() || module->owned.machine() != exe::kPeMachineAmd64) {
    say(c, canonical, " is not a 64-bit x86 DLL");
    return LoadStatus::Malformed;
  }
  module->pe = &module->owned;
  const exe::PeFile& pe = module->owned;
  uint64_t size = 0;
  if (!AddressSpace::round_up(pe.size_of_image(), &size) || size == 0) return LoadStatus::Malformed;

  // The preferred base if nothing is there, else the next free place, with the relocations applied.
  uint64_t base = pe.image_base();
  LoadedImage image;
  bool relocate = false;
  if (base == 0 || base % 0x10000 != 0 || !c.space.find_free(size, base, &base) || base != pe.image_base()) {
    uint64_t at = 0;
    if (!c.space.find_free(size, c.next_base, &at)) return LoadStatus::OutOfMemory;
    base = (at + 0xFFFF) & ~uint64_t{0xFFFF};
    if (!c.space.find_free(size, base, &at) || at != base) return LoadStatus::OutOfMemory;
    c.next_base = base + ((size + 0xFFFF) & ~uint64_t{0xFFFF});
    relocate = true;
  }
  // A DLL linked with dynamic base (0x40) lists every address that has to change; one that lists none needs none. Without the flag, a missing list means it cannot move.
  if (relocate && (!pe.relocations_ok() || (pe.relocations().count() == 0 && !(pe.dll_characteristics() & 0x40)))) {
    say(c, canonical, " cannot be moved (it has no relocations)");
    return LoadStatus::Malformed;
  }
  LoadStatus status = map_pe_image(pe, file, c.space, base, false, &image);
  if (status != LoadStatus::Ok) {
    say(c, canonical, " could not be mapped");
    return status;
  }
  if (!apply_relocations(c.space, pe, base, base - pe.image_base())) {
    say(c, canonical, " has damaged relocations");
    return LoadStatus::Malformed;
  }
  module->base = base;
  module->size = size;
  module->entry = image.entry;
  Module* raw = module.get();
  if (!c.set.modules_.push_back(static_cast<UniquePointer<Module>&&>(module))) return LoadStatus::OutOfMemory;
  *result = raw;

  // Its own dependencies: imported libraries, and the libraries its forwarded exports name.
  String name(c.allocator);
  for (const exe::PeImport& imp : raw->pe->imports().view()) {
    if (!canonical_library_name(imp.dll.view(), &name)) continue;
    Module* dependency = nullptr;
    status = ensure_loaded(c, name.view(), depth + 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  for (const exe::PeExport& e : raw->pe->exports().view()) {
    if (e.forwarder.empty()) continue;
    const uint64_t dot = e.forwarder.view().find('.');
    if (dot == StringView::npos || !canonical_library_name(e.forwarder.view().first(dot), &name)) continue;
    Module* dependency = nullptr;
    status = ensure_loaded(c, name.view(), depth + 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  return LoadStatus::Ok;
}

// The address of an export, following forwarders ("NTDLL.RtlFoo", "NTDLL.#12"). 0 if there is none.
uint64_t resolve_export(const ModuleSet& set, const Module& module, StringView name, bool by_ordinal, uint32_t ordinal, uint32_t depth, allocators::IAllocator& allocator) {
  if (depth > 8 || !module.pe) return 0;
  const exe::PeExport* e = by_ordinal ? module.pe->find_export_by_ordinal(ordinal) : module.pe->find_export(name);
  if (!e) return 0;
  if (e->forwarder.empty()) return module.base + e->rva;
  const StringView forward = e->forwarder.view();
  const uint64_t dot = forward.find('.');
  if (dot == StringView::npos) return 0;
  String library(allocator);
  if (!canonical_library_name(forward.first(dot), &library)) return 0;
  const Module* target = set.find(library.view());
  if (!target || target == &module) return 0;
  const StringView function = forward.without_prefix(dot + 1);
  if (!function.empty() && function[0] == '#') {
    uint32_t number = 0;
    for (uint64_t i = 1; i < function.size(); ++i) {
      if (function[i] < '0' || function[i] > '9') return 0;
      number = number * 10 + static_cast<uint32_t>(function[i] - '0');
    }
    return resolve_export(set, *target, StringView(), true, number, depth + 1, allocator);
  }
  return resolve_export(set, *target, function, false, 0, depth + 1, allocator);
}

bool put_u64(AddressSpace& space, uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  return space.write_privileged(address, ConstByteView(raw, 8));
}

// Fills the import address table of one module.
LoadStatus bind_imports(Context& c, Module& module) {
  String library(c.allocator);
  for (const exe::PeImport& imp : module.pe->imports().view()) {
    uint64_t address = 0;
    if (canonical_library_name(imp.dll.view(), &library)) {
      if (const Module* target = c.set.find(library.view())) address = resolve_export(c.set, *target, imp.name.view(), imp.by_ordinal, imp.ordinal, 0, c.allocator);
    }
    if (address == 0) {
      // Nothing provides it: a stub that stops the program with a message, if (and only if) it is ever called.
      if (c.set.missing_.count() >= kMaxStubs) {
        say(c, "too many missing functions in ", module.name.view());
        return LoadStatus::NeedsLibraries;
      }
      if (c.set.stub_base_ == 0) {
        uint64_t at = 0;
        if (!c.space.find_free(kMaxStubs * kStubSize, kDllHint - (1ull << 32), &at) || !c.space.map(at, kMaxStubs * kStubSize, kProtRead | kProtExecute)) return LoadStatus::OutOfMemory;
        static const uint8_t kTrap[kStubSize] = {0x0F, 0x0B, 0x0F, 0x0B, 0x0F, 0x0B, 0x0F, 0x0B};      // ud2: the processor reports it as an invalid instruction at the stub
        for (uint64_t i = 0; i < kMaxStubs; ++i) c.space.write_privileged(at + i * kStubSize, ConstByteView(kTrap, kStubSize));
        c.set.stub_base_ = at;
      }
      String described(c.allocator);
      described.assign(imp.dll.view());
      described.push_back('!');
      if (imp.by_ordinal) {
        described.push_back('#');
        described.append_uint(imp.ordinal);
      } else {
        described.append(imp.name.view());
      }
      address = c.set.stub_base_ + c.set.missing_.count() * kStubSize;
      if (!c.set.missing_.push_back(static_cast<String&&>(described))) return LoadStatus::OutOfMemory;
    }
    if (!put_u64(c.space, module.base + imp.slot_rva, address)) return LoadStatus::Malformed;
  }
  return LoadStatus::Ok;
}

// A library after the libraries it imports (depth-first, each once).
void order(Context& c, Module& module, uint32_t depth) {
  if (module.visit != 0 || depth > kMaxDepth) return;
  module.visit = 1;      // (also what ends a cycle)
  String name(c.allocator);
  for (const exe::PeImport& imp : module.pe->imports().view()) {
    if (!canonical_library_name(imp.dll.view(), &name)) continue;
    if (Module* dependency = c.set.find(name.view())) order(c, *dependency, depth + 1);
  }
  if (!module.main) c.set.order_.push_back(&module);
}

}  // namespace

namespace {
// The loaded libraries a module imports or forwards to.
void dependencies_of(const ModuleSet& set, const Module& module, allocators::IAllocator& allocator, List<Module*>* out) {
  String name(allocator);
  for (const exe::PeImport& imp : module.pe->imports().view()) {
    if (!canonical_library_name(imp.dll.view(), &name)) continue;
    if (Module* d = set.find(name.view())) out->push_back(d);
  }
  for (const exe::PeExport& e : module.pe->exports().view()) {
    if (e.forwarder.empty()) continue;
    const uint64_t dot = e.forwarder.view().find('.');
    if (dot == StringView::npos || !canonical_library_name(e.forwarder.view().first(dot), &name)) continue;
    if (Module* d = set.find(name.view())) out->push_back(d);
  }
}

void doom(const ModuleSet& set, allocators::IAllocator& allocator, Module& module, List<Module*>* out) {
  module.doomed = true;
  out->push_back(&module);
  List<Module*> dependencies(allocator);
  dependencies_of(set, module, allocator, &dependencies);
  for (Module* d : dependencies.view()) {
    if (d->pinned || d->doomed || d->refs == 0) continue;
    if (--d->refs == 0) doom(set, allocator, *d, out);
  }
}
}  // namespace

LoadStatus load_additional_library(AddressSpace& space, allocators::IAllocator& allocator, ILibraryProvider& provider, ModuleSet* set, StringView canonical_name, String* detail) {
  Context c{space, allocator, provider, *set, detail};
  const uint64_t first = set->count();
  const uint64_t init_first = set->init_count();
  (void)init_first;
  for (uint64_t i = 0; i < first; ++i) set->modules_[i]->visit = 1;      // (what was there is ordered already)
  Module* requested = nullptr;
  LoadStatus status = ensure_loaded(c, canonical_name, 1, &requested);
  if (status == LoadStatus::Ok && !requested) { say(c, canonical_name, " was not found"); status = LoadStatus::NeedsLibraries; }
  for (uint64_t i = first; i < set->count() && status == LoadStatus::Ok; ++i) status = bind_imports(c, *set->modules_[i]);
  if (status == LoadStatus::Ok) order(c, *requested, 0);
  if (status == LoadStatus::Ok) {      // the uses: one for the caller, one for each import of a new library
    ++requested->refs;
    for (uint64_t i = first; i < set->count(); ++i) {
      List<Module*> dependencies(allocator);
      dependencies_of(*set, *set->modules_[i], allocator, &dependencies);
      for (Module* d : dependencies.view()) if (!d->pinned) ++d->refs;
    }
  }
  if (status != LoadStatus::Ok) set->drop_from(space, first);
  return status;
}

bool reference_library(ModuleSet* set, StringView canonical_name, uint64_t* base) {
  Module* m = set->find(canonical_name);
  if (!m || m->main) return false;
  if (!m->pinned) ++m->refs;
  *base = m->base;
  return true;
}

bool release_library(ModuleSet* set, allocators::IAllocator& allocator, uint64_t base, List<Module*>* out) {
  Module* m = set->find_base(base);
  if (!m || m->main || m->doomed) return false;
  if (m->pinned) return true;
  if (m->refs == 0) return false;
  if (--m->refs == 0) doom(*set, allocator, *m, out);
  return true;
}

bool unload_library(AddressSpace& space, ModuleSet* set, uint64_t base) {
  for (uint64_t i = 0; i < set->modules_.count(); ++i) {
    Module& m = *set->modules_[i];
    if (m.base != base) continue;
    if (m.main) return false;
    for (uint64_t k = set->order_.count(); k > 0; --k) if (set->order_[k - 1] == &m) { set->order_.remove_at(k - 1); break; }
    space.unmap(m.base, m.size);
    set->modules_.remove_at(i);
    return true;
  }
  return false;
}

uint64_t ModuleSet::export_address(StringView module, StringView function) const {
  String name(allocator_);
  if (!canonical_library_name(module, &name)) return 0;
  const Module* m = find(name.view());
  return m ? resolve_export(*this, *m, function, false, 0, 0, allocator_) : 0;
}

LoadStatus load_windows_modules(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ConstByteView program_file, StringView program_name,
                                ILibraryProvider& provider, ModuleSet* out, String* detail) {
  (void)program_file;
  Context c{space, allocator, provider, *out, detail};
  UniquePointer<Module> main = UniquePointer<Module>::make(allocator, allocator);
  if (!main || !main->name.assign(program_name)) return LoadStatus::OutOfMemory;
  main->pe = &program;
  main->base = program.image_base();
  main->size = program.size_of_image();
  main->entry = program.image_base() + program.entry_rva();
  main->main = true;
  Module* main_raw = main.get();
  if (!out->modules_.push_back(static_cast<UniquePointer<Module>&&>(main))) return LoadStatus::OutOfMemory;

  // Phase 1: everything that is imported, directly or not.
  String name(allocator);
  for (const exe::PeImport& imp : program.imports().view()) {
    if (!canonical_library_name(imp.dll.view(), &name)) continue;
    Module* dependency = nullptr;
    const LoadStatus status = ensure_loaded(c, name.view(), 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  // Phase 2: bind.
  for (uint64_t i = 0; i < out->modules_.count(); ++i) {
    const LoadStatus status = bind_imports(c, *out->modules_[i]);
    if (status != LoadStatus::Ok) { main_raw->pe = nullptr; return status; }
  }
  // Phase 3: the order the DLLs start in.
  order(c, *main_raw, 0);
  for (UniquePointer<Module>& m : out->modules_.view()) m->pinned = true;      // (these stay as long as the process does)
  main_raw->pe = nullptr;      // (the program's headers belong to the caller and may go away)
  return LoadStatus::Ok;
}

}  // namespace process
