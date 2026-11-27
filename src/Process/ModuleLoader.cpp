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
#include "Process/ImageCache.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Windows/tcos_dll_layering.h"

namespace process {

namespace {

constexpr uint64_t kPage = AddressSpace::kPageSize;
constexpr uint64_t kDllHint = 0x00007FF800000000ull;      // where libraries are looked for room, from here upwards (a Windows-like place)
constexpr uint32_t kMaxModules = 512;
constexpr uint64_t kMaxLibraryBytes = 1ull << 30;      // (a browser's main DLL is a third of that)
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

bool plain_library_name(StringView name, String* out) {
  out->clear();
  const uint64_t slash = name.rfind('\\'), slash2 = name.rfind('/');      // a library named with a path is known by its file name
  if (slash != StringView::npos) name = name.without_prefix(slash + 1);
  if (slash2 != StringView::npos && name.rfind('/') != StringView::npos) name = name.without_prefix(name.rfind('/') + 1);
  if (name.empty() || name.size() > 100) return false;
  for (char c : name) if (!out->push_back(lower(c))) return false;
  return out->view().rfind('.') != StringView::npos || out->append(".dll");
}

bool is_versioned_runtime(StringView plain_name) {
  return (starts_with(plain_name, "msvcr") || starts_with(plain_name, "msvcp") || starts_with(plain_name, "vcruntime")) && !plain_name.equals("msvcrt.dll");
}

bool canonical_library_name(StringView name, String* out) {
  if (!plain_library_name(name, out)) return false;
  const StringView text = out->view();
  // The API sets of other libraries than kernel32 (checked first: the WinRT ones start with api-ms-win-core- too).
  if (starts_with(text, "api-ms-win-core-winrt-")) return out->assign("combase.dll");
  if (starts_with(text, "api-ms-win-power-")) return out->assign("powrprof.dll");
  if (starts_with(text, "api-ms-win-shcore-")) return out->assign("shcore.dll");
  // (the "downlevel" sets name the library they came out of: api-ms-win-downlevel-shell32-l1-1-0 is shell32, and so on; Chromium takes CommandLineToArgvW from that one)
  if (starts_with(text, "api-ms-win-downlevel-shell32-")) return out->assign("shell32.dll");
  if (starts_with(text, "api-ms-win-downlevel-advapi32-")) return out->assign("advapi32.dll");
  if (starts_with(text, "api-ms-win-downlevel-kernel32-") || starts_with(text, "api-ms-win-appmodel-")) return out->assign("kernel32.dll");
  if (starts_with(text, "api-ms-win-downlevel-ole32-")) return out->assign("ole32.dll");
  if (starts_with(text, "api-ms-win-downlevel-user32-")) return out->assign("user32.dll");
  if (starts_with(text, "api-ms-win-downlevel-shlwapi-")) return out->assign("shlwapi.dll");
  if (starts_with(text, "api-ms-win-downlevel-version-")) return out->assign("version.dll");
  // Libraries with other extensions than .dll: here they are DLLs like the others (the print spooler's winspool.drv is winspool.dll).
  if (text.equals("winspool.drv")) return out->assign("winspool.dll");
  if (text.equals("bthprops.cpl")) return out->assign("bthprops.dll");
  if (starts_with(text, "api-ms-win-core-") || starts_with(text, "api-ms-win-eventing-") || text.equals("kernelbase.dll")) return out->assign("kernel32.dll");
  if (starts_with(text, "api-ms-win-security-")) return out->assign("advapi32.dll");
  if (text.equals("wsock32.dll") || text.equals("winsock.dll")) return out->assign("ws2_32.dll");
  // Every version of XInput is the one library (xinput1_4.dll exports the older ones' names and ordinals too).
  if (starts_with(text, "xinput1_") || text.equals("xinput9_1_0.dll") || text.equals("xinputuap.dll")) return out->assign("xinput1_4.dll");
  // All the C runtimes are one library here: it exports both the classic msvcrt names and the Universal CRT's.
  if (starts_with(text, "api-ms-win-crt-") || text.equals("ucrtbase.dll") || text.equals("ucrtbased.dll") || text.equals("crtdll.dll") || starts_with(text, "msvcr") ||
      starts_with(text, "vcruntime") || starts_with(text, "msvcp"))
    return out->assign("msvcrt.dll");
  return true;
}

namespace {
bool equals_ignore_case(StringView a, StringView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
}  // namespace

bool is_system_library_path(StringView os_path) {
  const StringView folders[2] = {StringView("/windows/windows/system32/"), StringView("/windows/windows/syswow64/")};
  for (StringView folder : folders) {
    if (os_path.size() <= folder.size() || !equals_ignore_case(os_path.first(folder.size()), folder)) continue;
    const StringView rest = os_path.without_prefix(folder.size());
    if (!rest.contains('/')) return true;      // (directly in it, not in a folder below)
  }
  return false;
}

bool windows_directory_of_library(StringView os_path, String* out) {
  out->clear();
  // (System32 is the folder an empty one means; a 32-bit program's libraries of SysWOW64 are said to be there, as Windows' GetModuleFileName says)
  if (is_system_library_path(os_path)) return !equals_ignore_case(os_path.substr(17, 9), "SysWOW64/") || out->assign("C:\\Windows\\SysWOW64\\");
  const uint64_t slash = os_path.rfind('/');
  // The Windows world is the OS folder /Windows, drive C; a file elsewhere has no Windows folder.
  if (slash == StringView::npos || os_path.size() < 9 || !equals_ignore_case(os_path.first(9), "/Windows/")) return true;
  if (!out->assign("C:\\")) return false;
  for (char ch : os_path.substr(9, slash + 1 - 9)) if (!out->push_back(ch == '/' ? '\\' : ch)) return false;
  return true;
}

bool is_internal_layer(StringView importer, StringView imported) {
  return tcos_is_internal_layer(importer.data(), importer.size(), imported.data(), imported.size()) != 0;
}

struct FileLibraryProvider::Entry {
  Entry(allocators::IAllocator& allocator, StringView p) : path(allocator), bytes(allocator) { path.assign(p); }
  String path;           // the file in the OS file system: one entry for each file, whatever it was looked for as
  ByteBuffer bytes;
  Entry* next = nullptr;
};

FileLibraryProvider::FileLibraryProvider(allocators::IAllocator& allocator, filesystem::IFileSystem* files)
    : allocator_(allocator), files_(files), directories_(allocator), redirects_(allocator) {}

namespace {
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The value of the attribute `key` of the start tag `tag` ("<file name='x.dll' hash='...'/"), or false. Values in ' or " quotes.
bool tag_attribute(StringView tag, StringView key, StringView* value) {
  uint64_t i = 1;
  while (i < tag.size() && !is_space(tag[i]) && tag[i] != '/' && tag[i] != '>') ++i;      // (the element's name)
  while (i < tag.size()) {
    while (i < tag.size() && (is_space(tag[i]) || tag[i] == '/')) ++i;
    const uint64_t name_at = i;
    while (i < tag.size() && !is_space(tag[i]) && tag[i] != '=' && tag[i] != '/' && tag[i] != '>') ++i;
    const StringView name = tag.substr(name_at, i - name_at);
    while (i < tag.size() && is_space(tag[i])) ++i;
    if (i >= tag.size() || tag[i] != '=') return false;
    ++i;
    while (i < tag.size() && is_space(tag[i])) ++i;
    if (i >= tag.size() || (tag[i] != '\'' && tag[i] != '"')) return false;
    const char quote = tag[i++];
    const uint64_t end = tag.find(quote, i);
    if (end == StringView::npos) return false;
    if (name.equals(key)) {
      *value = tag.substr(i, end - i);
      return true;
    }
    i = end + 1;
  }
  return false;
}

// The start tags of the element `element` ("<file"), each up to its '>', after `from` and before `end`.
template <typename Each>
bool for_each_tag(StringView xml, StringView element, uint64_t from, uint64_t end, Each each) {
  for (uint64_t at = xml.find(element, from); at != StringView::npos && at < end; at = xml.find(element, at + 1)) {
    const uint64_t after = at + element.size();
    if (after >= xml.size() || !(is_space(xml[after]) || xml[after] == '/' || xml[after] == '>')) continue;      // ("<fileX")
    const uint64_t close = xml.find('>', after);
    if (close == StringView::npos || close >= end) return true;
    if (!each(xml.substr(at, close - at))) return false;
  }
  return true;
}

bool plain_name(StringView name) {
  return !name.empty() && name.size() <= 255 && !name.contains('/') && !name.contains('\\') && !name.contains(':') && !name.equals(".") && !name.equals("..");
}

bool add_name(StringView name, List<String>* names, allocators::IAllocator& allocator) {
  if (!plain_name(name)) return true;
  for (const String& have : names->view()) if (have.view().equals_ignore_case(name)) return true;
  String copy(allocator);
  return copy.assign(name) && names->push_back(static_cast<String&&>(copy));
}
}  // namespace

bool manifest_private_assemblies(StringView xml, allocators::IAllocator& allocator, List<String>* names) {
  for (uint64_t at = xml.find("<dependentAssembly"); at != StringView::npos; at = xml.find("<dependentAssembly", at + 1)) {
    uint64_t end = xml.find("</dependentAssembly", at);
    if (end == StringView::npos) end = xml.size();
    bool ok = true;
    if (!for_each_tag(xml, "<assemblyIdentity", at, end, [&](StringView tag) {
          StringView name, token, type;
          if (!tag_attribute(tag, "name", &name) || tag_attribute(tag, "publicKeyToken", &token)) return true;      // (a shared assembly: not a folder of the program's)
          if (tag_attribute(tag, "type", &type) && !type.equals_ignore_case("win32")) return true;
          ok = add_name(name, names, allocator);
          return ok;
        }))
      return false;
    if (!ok) return false;
  }
  return true;
}

bool manifest_files(StringView xml, allocators::IAllocator& allocator, List<String>* names) {
  return for_each_tag(xml, "<file", 0, xml.size(), [&](StringView tag) {
    StringView name;
    return !tag_attribute(tag, "name", &name) || add_name(name, names, allocator);
  });
}

bool FileLibraryProvider::read_text(StringView path, uint64_t limit, String* out) {
  UniquePointer<filesystem::IFile> file;
  if (!files_ || files_->open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > limit) return false;
  ByteBuffer bytes(allocator_);
  if (!bytes.put_zeros(size)) return false;
  for (uint64_t done = 0; done < size;) {
    uint64_t got = 0;
    if (file->read(done, bytes.writable_view().subview(done), &got) != filesystem::Status::Ok || got == 0) return false;
    done += got;
  }
  return out->assign(StringView(bytes.view()));
}

bool FileLibraryProvider::add_program(StringView path, ConstByteView image) {
  const uint64_t slash = path.rfind('/');
  if (slash == StringView::npos || slash == 0) return true;
  const StringView folder = path.first(slash);
  if (!add_directory(folder)) return false;
  exe::PeFile pe(allocator_);
  ConstByteView manifest;
  if (!pe.parse(image) || !pe.find_resource(image, 24, 1, &manifest)) return true;      // RT_MANIFEST, CREATEPROCESS_MANIFEST_RESOURCE_ID
  List<String> assemblies(allocator_);
  if (!manifest_private_assemblies(StringView(manifest), allocator_, &assemblies)) return false;
  constexpr uint64_t kMaxManifestBytes = 1u << 20;
  for (const String& assembly : assemblies.view()) {
    // Windows looks for "<assembly>.manifest" next to the program, then in the folder "<assembly>"; the DLLs are next to the manifest found.
    String directory(allocator_), manifest_path(allocator_), text(allocator_);
    bool found = false;
    for (int place = 0; place < 2 && !found; ++place) {
      if (!directory.assign(folder) || !directory.push_back('/')) return false;
      if (place == 1 && (!directory.append(assembly.view()) || !directory.push_back('/'))) return false;
      if (!manifest_path.assign(directory.view()) || !manifest_path.append(assembly.view()) || !manifest_path.append(".manifest")) return false;
      found = read_text(manifest_path.view(), kMaxManifestBytes, &text);
    }
    if (!found) continue;
    List<String> files(allocator_);
    if (!manifest_files(text.view(), allocator_, &files)) return false;
    kernel::ScopedMutex hold(lock_);
    for (const String& name : files.view()) {
      Redirect redirect(allocator_);
      if (!redirect.name.assign(name.view()) || !redirect.directory.assign(directory.view())) return false;
      for (uint64_t i = 0; i < redirect.name.size(); ++i) redirect.name.set(i, lower(redirect.name[i]));
      bool have = false;
      for (const Redirect& r : redirects_.view()) have = have || r.name.view().equals(redirect.name.view());
      if (!have && !redirects_.push_back(static_cast<Redirect&&>(redirect))) return false;
    }
  }
  return true;
}

FileLibraryProvider::~FileLibraryProvider() {
  while (entries_) {
    Entry* next = entries_->next;
    entries_->~Entry();
    allocator_.deallocate(entries_);
    entries_ = next;
  }
}

bool FileLibraryProvider::add_directory(StringView directory) {
  String copy(allocator_);
  if (directory.empty() || !copy.assign(directory)) return false;
  if (copy.view()[copy.size() - 1] != '/' && !copy.push_back('/')) return false;
  kernel::ScopedMutex hold(lock_);
  for (const String& have : directories_.view()) if (have.view().equals(copy.view())) return true;
  return directories_.push_back(static_cast<String&&>(copy));
}

bool FileLibraryProvider::find(StringView canonical_name, ConstByteView* image) {
  String path(allocator_);
  return locate(canonical_name, false, LibrarySearch::Normal, image, &path);
}

bool FileLibraryProvider::find_32bit(StringView canonical_name, ConstByteView* image) {
  String path(allocator_);
  return locate(canonical_name, true, LibrarySearch::Normal, image, &path);
}

// Windows' order: a DLL of a private assembly the program's manifest names (64-bit programs), the program's own folders, then System32 (SysWOW64 for a 32-bit
// program: the libraries a 32-bit program brought in its folder are of its kind). Each file is read once and kept by its path.
bool FileLibraryProvider::locate(StringView canonical_name, bool wow, LibrarySearch where, ConstByteView* image, String* os_path) {
  kernel::ScopedMutex hold(lock_);
  if (!files_ || canonical_name.empty() || canonical_name.contains('/') || canonical_name.contains('\\')) return false;      // a plain file name only
  if (where == LibrarySearch::Normal) {
    if (!wow) {
      for (const Redirect& r : redirects_.view()) {
        if (r.name.view().equals(canonical_name) && os_path->assign(r.directory.view()) && os_path->append(canonical_name) && load(os_path->view(), image)) return true;
      }
    }
    for (const String& directory : directories_.view()) {
      if (os_path->assign(directory.view()) && os_path->append(canonical_name) && load(os_path->view(), image)) return true;
    }
  }
  return os_path->assign(wow ? "/Windows/Windows/SysWOW64/" : "/Windows/Windows/System32/") && os_path->append(canonical_name) && load(os_path->view(), image);
}

bool FileLibraryProvider::has_own_copy(StringView file_name, bool wow) {
  (void)wow;      // (a program's folder holds the libraries of its own kind)
  kernel::ScopedMutex hold(lock_);
  if (!files_ || file_name.empty() || file_name.contains('/') || file_name.contains('\\')) return false;
  String path(allocator_);
  for (const String& directory : directories_.view()) {
    const StringView d = directory.view();
    if (d.equals("/Windows/Windows/System32/") || d.equals("/Windows/Windows/SysWOW64/")) continue;      // (the system's are not the program's)
    filesystem::DirectoryEntry entry;
    if (path.assign(d) && path.append(file_name) && files_->stat(path.view(), &entry) == filesystem::Status::Ok && entry.type == filesystem::NodeType::File) return true;
  }
  return false;
}

bool FileLibraryProvider::load(StringView path, ConstByteView* image) {
  for (Entry* e = entries_; e; e = e->next) {
    if (e->path.view().equals(path)) {
      *image = e->bytes.view();
      return true;
    }
  }
  UniquePointer<filesystem::IFile> file;
  if (files_->open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxLibraryBytes) return false;
  void* memory = allocator_.allocate(sizeof(Entry));
  if (!memory) return false;
  Entry* entry = new (memory) Entry(allocator_, path);
  if (entry->path.size() != path.size() || !entry->bytes.put_zeros(size)) { entry->~Entry(); allocator_.deallocate(entry); return false; }
  ByteView bytes = entry->bytes.writable_view();
  for (uint64_t done = 0; done < size;) {
    uint64_t got = 0;
    if (file->read(done, bytes.subview(done, size - done < (4u << 20) ? size - done : (4u << 20)), &got) != filesystem::Status::Ok || got == 0) {
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

bool ModuleSet::owns_runtime(StringView plain_name) const {
  for (const String& own : own_runtimes_.view()) if (own.view().equals(plain_name)) return true;
  return false;
}

bool ModuleSet::library_name(StringView name, String* out) const {
  if (!canonical_library_name(name, out)) return false;
  if (own_runtimes_.count() == 0 || !out->view().equals("msvcrt.dll")) return true;
  String plain(allocator_);
  if (!plain_library_name(name, &plain)) return false;
  return !owns_runtime(plain.view()) || out->assign(plain.view());
}

Module* ModuleSet::find(StringView canonical_name) const {
  for (const UniquePointer<Module>& m : modules_.view()) if (!m->doomed && !m->private_layer && m->name.view().equals(canonical_name)) return m.get();
  return nullptr;
}

Module* ModuleSet::find_file(StringView os_path) const {
  if (os_path.empty()) return nullptr;
  for (const UniquePointer<Module>& m : modules_.view()) if (!m->doomed && !m->main && equals_ignore_case(m->file.view(), os_path)) return m.get();
  return nullptr;
}

Module* ModuleSet::find_system(StringView canonical_name) const {
  for (const UniquePointer<Module>& m : modules_.view()) if (!m->doomed && m->system && m->name.view().equals(canonical_name)) return m.get();
  return nullptr;
}

Module* ModuleSet::import_target(const Module& importer, StringView canonical_name) const {
  if (importer.system && is_internal_layer(importer.name.view(), canonical_name)) return find_system(canonical_name);
  return find(canonical_name);
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
  bool allow_program = false;      // the library asked for by name may be a program image (it is then only mapped)
  bool bind = true;      // false: only the library itself is mapped (no imports, no dependencies)
};

// An import of a versioned C runtime that the program brought (a file of that name in its own folders) makes that runtime the process's own: from then
// on the name is that file, not msvcrt.dll. Decided once per name, the first time it is seen and before anything is bound to it; a name that was
// folded once stays folded (what was bound to msvcrt.dll for it stays right). False if memory ran out.
bool note_own_runtime(Context& c, StringView imported) {
  String plain(c.allocator);
  if (!plain_library_name(imported, &plain) || !is_versioned_runtime(plain.view())) return true;
  if (c.set.owns_runtime(plain.view())) return true;
  for (const String& folded : c.set.folded_runtimes_.view()) if (folded.view().equals(plain.view())) return true;
  if (c.provider.has_own_copy(plain.view(), c.space.is_32bit())) return c.set.own_runtimes_.push_back(static_cast<String&&>(plain));
  return c.set.folded_runtimes_.push_back(static_cast<String&&>(plain));
}

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

void record_image_base(AddressSpace& space, uint64_t base) {
  uint8_t raw[4];
  if (!space.read_privileged(base + 0x3C, ByteView(raw, 4))) return;
  const uint64_t nt = raw[0] | (uint64_t{raw[1]} << 8) | (uint64_t{raw[2]} << 16) | (uint64_t{raw[3]} << 24);
  uint8_t value[8];
  for (uint32_t i = 0; i < 8; ++i) value[i] = uint8_t(base >> (8 * i));
  if (space.is_32bit()) space.write_privileged(base + nt + 24 + 28, ConstByteView(value, 4));      // OptionalHeader.ImageBase of a PE32 image: 32 bits, after BaseOfData
  else space.write_privileged(base + nt + 24 + 24, ConstByteView(value, 8));                        // OptionalHeader.ImageBase
}

// What Windows says about the pages of a mapped image: one allocation of type image, the headers read-only, each section with the protection its characteristics ask for (a
// writable one is copy-on-write: the first write gives that page a copy of its own).
void describe_image_pages(AddressSpace& space, const exe::PeFile& pe, uint64_t base) {
  PageAttributes attributes;
  attributes.type = PageAttributes::Image;
  attributes.allocation_protect = 0x80;      // PAGE_EXECUTE_WRITECOPY
  const uint64_t header_bytes = pe.size_of_headers() < pe.size_of_image() ? pe.size_of_headers() : pe.size_of_image();
  attributes.protect = 0x02;
  space.set_attributes(base, header_bytes, attributes);
  for (const exe::PeSection& section : pe.sections().view()) {
    const uint64_t span = section.virtual_size ? section.virtual_size : section.raw_size;
    if (span == 0) continue;
    const bool r = section.characteristics & exe::kPeSectionRead, w = section.characteristics & exe::kPeSectionWrite, x = section.characteristics & exe::kPeSectionExecute;
    attributes.protect = !(r || w || x) ? 0x01 : w ? (x ? 0x80 : 0x08) : x ? (r ? 0x20 : 0x10) : 0x02;
    const uint64_t at = base + section.virtual_address;
    space.set_attributes(at, span, attributes);
    space.protect(at, span, uint8_t(((r || w) ? kProtRead : 0) | (x ? kProtExecute : 0)));      // (a writable section is read-only until it is written to)
    space.join_allocation(at, span, base);
  }
  space.join_allocation(base, header_bytes, base);
}

namespace {

void say(Context& c, StringView a, StringView b = StringView()) {
  if (!c.detail || !c.detail->empty()) return;
  c.detail->assign(a);
  c.detail->append(b);
}

// Loads the library (if not loaded yet) and, first, the libraries it imports and forwards to.
// The load configuration of an image names a security cookie; one that still has the compiler's default (or none) gets a value of its own, written even into a read-only page.
void initialise_security_cookie(AddressSpace& space, const exe::PeFile& pe, uint64_t base) {
  static uint64_t counter = 0x9E3779B97F4A7C15ull;
  uint32_t rva = 0, size = 0;
  if (!pe.is_pe32_plus()) {      // a 32-bit image: the cookie's address is 32 bits at 0x3C, and so is the cookie
    if (!pe.directory(10, &rva, &size) || size < 0x40 || rva == 0) return;
    uint8_t raw[4];
    if (!space.read_privileged(base + rva + 0x3C, ByteView(raw, 4))) return;
    const uint64_t address = raw[0] | (uint64_t{raw[1]} << 8) | (uint64_t{raw[2]} << 16) | (uint64_t{raw[3]} << 24);
    if (address < base || address - base > pe.size_of_image() || pe.size_of_image() - (address - base) < 4) return;
    if (!space.read_privileged(address, ByteView(raw, 4))) return;
    const uint32_t current = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
    constexpr uint32_t kDefault32 = 0xBB40E64Eu;
    if (current != kDefault32 && current != 0) return;
    counter = counter * 6364136223846793005ull + 1442695040888963407ull + base;
    uint32_t value = static_cast<uint32_t>(counter >> 32) ^ static_cast<uint32_t>(base << 3);
    if (value == 0 || value == kDefault32) value = kDefault32 + 1;
    for (uint32_t i = 0; i < 4; ++i) raw[i] = uint8_t(value >> (8 * i));
    space.write_privileged(address, ConstByteView(raw, 4));
    return;
  }
  if (!pe.directory(10, &rva, &size) || size < 0x60 || rva == 0) return;      // IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG; the cookie is at 0x58
  uint8_t raw[8];
  if (!space.read_privileged(base + rva + 0x58, ByteView(raw, 8))) return;
  uint64_t address = 0;
  for (uint32_t i = 0; i < 8; ++i) address |= uint64_t{raw[i]} << (8 * i);
  if (address < base || address - base > pe.size_of_image() || pe.size_of_image() - (address - base) < 8) return;
  if (!space.read_privileged(address, ByteView(raw, 8))) return;
  uint64_t current = 0;
  for (uint32_t i = 0; i < 8; ++i) current |= uint64_t{raw[i]} << (8 * i);
  constexpr uint64_t kDefault = 0x00002B992DDFA232ull;
  if (current != kDefault && current != 0) return;
  counter = counter * 6364136223846793005ull + 1442695040888963407ull + base;
  uint64_t value = (counter ^ (counter >> 29) ^ (base << 17)) & 0x0000FFFFFFFFFFFFull;
  if (value == 0 || value == kDefault) value = kDefault + 1;
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(value >> (8 * i));
  space.write_privileged(address, ConstByteView(raw, 8));
}

// `importer` is the library whose import (or forwarder) names it, null for the program's and for the library a LoadLibrary asked for. `requested`: that library,
// which is loaded anew from the file the provider has for it (the caller looked for a loaded one).
LoadStatus ensure_loaded(Context& c, StringView canonical, const Module* importer, bool requested, uint32_t depth, Module** result) {
  // An internal layer of a System32 library is the System32 file of the library it is built on (Windows/tcos_dll_layering.h); anything else is looked for as
  // Windows does: a loaded library of that name, else the search.
  const bool layer = importer && importer->system && is_internal_layer(importer->name.view(), canonical);
  if (!requested) {
    if (Module* have = layer ? c.set.find_system(canonical) : c.set.find(canonical)) {
      *result = have;
      return LoadStatus::Ok;
    }
  }
  *result = nullptr;
  if (depth > kMaxDepth || c.set.count() >= kMaxModules) {
    say(c, "too many libraries: ", canonical);
    return LoadStatus::NeedsLibraries;
  }
  ConstByteView file;
  String path(c.allocator);
  const bool wow = c.space.is_32bit();      // a 32-bit program: its libraries are 32-bit ones
  if (!c.provider.locate(canonical, wow, layer ? LibrarySearch::SystemOnly : LibrarySearch::Normal, &file, &path)) return LoadStatus::Ok;      // not available: its imports become stubs
  if (!requested) {
    if (Module* same = c.set.find_file(path.view())) {      // that file is loaded (as a private layer, which it is no longer when found by the search)
      if (!layer) same->private_layer = false;
      *result = same;
      return LoadStatus::Ok;
    }
  }
  UniquePointer<Module> module = UniquePointer<Module>::make(c.allocator, c.allocator);
  if (!module || !module->name.assign(canonical) || !module->file.assign(path.view()) || !windows_directory_of_library(path.view(), &module->directory)) return LoadStatus::OutOfMemory;
  module->system = is_system_library_path(path.view());
  module->private_layer = layer;
  const bool parsed_ok = module->owned.parse(file);
  const bool data_only_32bit = parsed_ok && !module->owned.is_pe32_plus() && module->owned.machine() == 0x14C && !pe_image_contains_code(module->owned);
  const bool right_kind = parsed_ok && (wow ? !module->owned.is_pe32_plus() && module->owned.machine() == exe::kPeMachineI386
                                            : module->owned.is_pe32_plus() && module->owned.machine() == exe::kPeMachineAmd64);
  if (!parsed_ok || !(right_kind || (data_only_32bit && !wow)) || (!module->owned.is_dll() && !(c.allow_program && depth == 1))) {
    say(c, canonical, wow ? " is not a 32-bit x86 DLL" : " is not a 64-bit x86 DLL");
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
    uint64_t at = 0, hint = c.next_base;
    if (c.space.is_32bit()) {      // (a 32-bit program's moved library from the top down, as Windows places them: the middle of its 2 GiB stays free)
      if (!c.space.find_free_windows32(size, true, &base)) { say(c, "no room for ", canonical); return LoadStatus::OutOfMemory; }
    } else {
      for (uint32_t attempt = 0;; ++attempt) {      // a free place that starts on a 64 KiB boundary (the holes between libraries do not)
        if (attempt >= 256 || !c.space.find_free(size, hint, &at)) { say(c, "no room for ", canonical); return LoadStatus::OutOfMemory; }
        base = (at + 0xFFFF) & ~uint64_t{0xFFFF};
        if (base == at) break;
        hint = base;
      }
      c.next_base = base + ((size + 0xFFFF) & ~uint64_t{0xFFFF});
    }
    relocate = true;
  }
  // A DLL linked with dynamic base (0x40) lists every address that has to change; one that lists none needs none. Without the flag, a missing list means it cannot move.
  if (relocate && (!pe.relocations_ok() || (pe.relocations().count() == 0 && !(pe.dll_characteristics() & 0x40)))) {
    say(c, canonical, " cannot be moved (it has no relocations)");
    return LoadStatus::Malformed;
  }
  // At a given base an image is the same in every process (the processes of one program load their libraries alike, moved ones included): its pages are shared
  // (ImageCache, with the relocations for that base applied once), and only the ones written get copies of their own.
  bool shared = false;
  if (pe.entry_rva() < size) {
    if (ImageCache* cache = image_cache()) {
      if (ImageCache::Image* cached = cache->acquire(pe, file, base)) {
        shared = ImageCache::map(*cached, c.space);
        cached->release();      // (the mappings hold it now)
      }
    }
  }
  if (shared) {
    image.base = base;
    image.end = base + size;
    image.entry = pe.entry_rva() ? base + pe.entry_rva() : 0;
  } else if (const LoadStatus status = map_pe_image(pe, file, c.space, base, false, &image); status != LoadStatus::Ok) {
    say(c, canonical, " could not be mapped");
    return status;
  }
  LoadStatus status = LoadStatus::Ok;
  if (!shared && !apply_relocations(c.space, pe, base, base - pe.image_base())) {      // (a shared image was relocated when it was made)
    say(c, canonical, " has damaged relocations");
    return LoadStatus::Malformed;
  }
  initialise_security_cookie(c.space, pe, base);
  if (!shared && relocate && (pe.dll_characteristics() & 0x40)) record_image_base(c.space, base);
  describe_image_pages(c.space, pe, base);
  module->base = base;
  module->size = size;
  module->no_bind = !c.bind || !pe.is_dll() || !right_kind;      // (a program image loaded as a library is only mapped)
  module->entry = pe.is_dll() ? image.entry : 0;
  Module* raw = module.get();
  if (!c.set.modules_.push_back(static_cast<UniquePointer<Module>&&>(module))) return LoadStatus::OutOfMemory;
  *result = raw;

  if (raw->no_bind) return LoadStatus::Ok;
  // Its own dependencies: imported libraries, and the libraries its forwarded exports name.
  String name(c.allocator);
  for (const exe::PeImport& imp : raw->pe->imports().view()) {
    if (!note_own_runtime(c, imp.dll.view())) return LoadStatus::OutOfMemory;
    if (!c.set.library_name(imp.dll.view(), &name)) continue;
    Module* dependency = nullptr;
    status = ensure_loaded(c, name.view(), raw, false, depth + 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  for (const exe::PeExport& e : raw->pe->exports().view()) {
    if (e.forwarder.empty()) continue;
    const uint64_t dot = e.forwarder.view().find('.');
    if (dot == StringView::npos || !note_own_runtime(c, e.forwarder.view().first(dot)) || !c.set.library_name(e.forwarder.view().first(dot), &name)) continue;
    Module* dependency = nullptr;
    status = ensure_loaded(c, name.view(), raw, false, depth + 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  return LoadStatus::Ok;
}

// The address of an export, following forwarders ("NTDLL.RtlFoo", "NTDLL.#12"). 0 if there is none.
// `hops` (if given) gets, for each forwarder followed, the module with the forwarded export and then the module it was forwarded to.
uint64_t resolve_export(const ModuleSet& set, const Module& module, StringView name, bool by_ordinal, uint32_t ordinal, uint32_t depth, allocators::IAllocator& allocator,
                        List<Module*>* hops = nullptr) {
  if (depth > 8 || !module.pe) return 0;
  const exe::PeExport* e = by_ordinal ? module.pe->find_export_by_ordinal(ordinal) : module.pe->find_export(name);
  if (!e) return 0;
  if (e->forwarder.empty()) return module.base + e->rva;
  const StringView forward = e->forwarder.view();
  const uint64_t dot = forward.find('.');
  if (dot == StringView::npos) return 0;
  String library(allocator);
  if (!set.library_name(forward.first(dot), &library)) return 0;
  const Module* target = set.import_target(module, library.view());      // (by base name, as Windows does; an internal layer is the System32 file)
  if (!target || target == &module) return 0;
  const StringView function = forward.without_prefix(dot + 1);
  if (!function.empty() && function[0] == '#') {
    uint32_t number = 0;
    for (uint64_t i = 1; i < function.size(); ++i) {
      if (function[i] < '0' || function[i] > '9') return 0;
      number = number * 10 + static_cast<uint32_t>(function[i] - '0');
    }
    const uint64_t found = resolve_export(set, *target, StringView(), true, number, depth + 1, allocator, hops);
    if (found != 0 && hops) { hops->push_back(const_cast<Module*>(&module)); hops->push_back(const_cast<Module*>(target)); }
    return found;
  }
  const uint64_t found = resolve_export(set, *target, function, false, 0, depth + 1, allocator, hops);
  if (found != 0 && hops) { hops->push_back(const_cast<Module*>(&module)); hops->push_back(const_cast<Module*>(target)); }
  return found;
}

bool put_u64(AddressSpace& space, uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  return space.write_privileged(address, ConstByteView(raw, 8));
}

bool put_u32(AddressSpace& space, uint64_t address, uint64_t value) {
  if (value > 0xFFFFFFFFull) return false;
  uint8_t raw[4];
  for (uint32_t i = 0; i < 4; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  return space.write_privileged(address, ConstByteView(raw, 4));
}

// Fills the import address table of one module.
LoadStatus bind_imports(Context& c, Module& module) {
  String library(c.allocator);
  for (const exe::PeImport& imp : module.pe->imports().view()) {
    uint64_t address = 0;
    if (c.set.library_name(imp.dll.view(), &library)) {
      if (Module* target = c.set.import_target(module, library.view())) {
        address = resolve_export(c.set, *target, imp.name.view(), imp.by_ordinal, imp.ordinal, 0, c.allocator);
        bool listed = target == &module;
        for (const Module* d : module.import_deps.view()) listed = listed || d == target;
        if (!listed && !module.import_deps.push_back(target)) return LoadStatus::OutOfMemory;
      }
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
    // (a slot of the table is as wide as a pointer of the program)
    if (!(module.pe->is_pe32_plus() ? put_u64(c.space, module.base + imp.slot_rva, address) : put_u32(c.space, module.base + imp.slot_rva, address))) return LoadStatus::Malformed;
  }
  return LoadStatus::Ok;
}

// A library after the libraries it imports (depth-first, each once).
void order(Context& c, Module& module, uint32_t depth) {
  if (module.visit != 0 || depth > kMaxDepth) return;
  module.visit = 1;      // (also what ends a cycle)
  String name(c.allocator);
  for (const exe::PeImport& imp : module.pe->imports().view()) {
    if (!c.set.library_name(imp.dll.view(), &name)) continue;
    if (Module* dependency = c.set.import_target(module, name.view())) order(c, *dependency, depth + 1);
  }
  if (!module.main) c.set.order_.push_back(&module);
}

}  // namespace

namespace {
// The loaded libraries a module imports or forwards to.
void dependencies_of(const ModuleSet& set, const Module& module, allocators::IAllocator& allocator, List<Module*>* out) {
  (void)set; (void)allocator;
  for (Module* d : module.import_deps.view()) out->push_back(d);      // (what its imports were bound to when it was loaded)
  for (Module* d : module.forward_deps.view()) out->push_back(d);      // (a forwarded export is no dependency until it is resolved: see link_libraries)
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

namespace {
bool add_forward_dependency(Module& from, Module& to) {
  if (&from == &to || from.pinned || to.pinned || from.doomed || to.doomed) return true;
  for (Module* d : from.forward_deps.view()) if (d == &to) return true;
  if (!from.forward_deps.push_back(&to)) return false;
  ++to.refs;
  return true;
}
}  // namespace

LoadStatus load_additional_library(AddressSpace& space, allocators::IAllocator& allocator, ILibraryProvider& provider, ModuleSet* set, StringView canonical_name, String* detail, bool bind) {
  Context c{space, allocator, provider, *set, detail};
  c.bind = bind;
  c.allow_program = true;
  const uint64_t first = set->count();
  const uint64_t init_first = set->init_count();
  (void)init_first;
  for (uint64_t i = 0; i < first; ++i) set->modules_[i]->visit = 1;      // (what was there is ordered already)
  Module* requested = nullptr;
  LoadStatus status = ensure_loaded(c, canonical_name, nullptr, true, 1, &requested);
  if (status == LoadStatus::Ok && !requested) { say(c, canonical_name, " was not found"); status = LoadStatus::NeedsLibraries; }
  for (uint64_t i = first; i < set->count() && status == LoadStatus::Ok; ++i) if (!set->modules_[i]->no_bind) status = bind_imports(c, *set->modules_[i]);
  if (status == LoadStatus::Ok) order(c, *requested, 0);
  if (status == LoadStatus::Ok) {      // the uses: one for the caller, one for each import of a new library
    ++requested->refs;
    String name(allocator);
    for (uint64_t i = first; i < set->count(); ++i) {
      Module& m = *set->modules_[i];
      List<Module*> dependencies(allocator);
      dependencies_of(*set, m, allocator, &dependencies);
      for (Module* d : dependencies.view()) if (!d->pinned) ++d->refs;
      if (m.no_bind) continue;
      // Resolving an import through a forwarder makes the libraries on the way dependencies of the one that forwards; a forwarder to a library that was loaded for it does too.
      for (const exe::PeImport& imp : m.pe->imports().view()) {
        if (!set->library_name(imp.dll.view(), &name)) continue;
        const Module* target = set->import_target(m, name.view());
        if (!target) continue;
        List<Module*> hops(allocator);
        if (resolve_export(*set, *target, imp.name.view(), imp.by_ordinal, imp.ordinal, 0, allocator, &hops) == 0) continue;
        for (uint64_t h = 0; h + 1 < hops.count(); h += 2) add_forward_dependency(*hops[h], *hops[h + 1]);
      }
      for (const exe::PeExport& e : m.pe->exports().view()) {
        if (e.forwarder.empty()) continue;
        const uint64_t dot = e.forwarder.view().find('.');
        if (dot == StringView::npos || !set->library_name(e.forwarder.view().first(dot), &name)) continue;
        Module* d = set->import_target(m, name.view());
        bool added_now = false;
        for (uint64_t k = first; d && k < set->count(); ++k) if (set->modules_[k].get() == d) added_now = true;
        if (d && added_now) add_forward_dependency(m, *d);
      }
    }
  }
  if (status != LoadStatus::Ok) set->drop_from(space, first);
  return status;
}

bool link_libraries(ModuleSet* set, uint64_t from, uint64_t to) {
  Module* a = set->find_base(from);
  Module* b = set->find_base(to);
  if (!a || !b) return false;
  return add_forward_dependency(*a, *b);
}

bool reference_library(ModuleSet* set, StringView canonical_name, uint64_t* base) {
  Module* m = set->find(canonical_name);
  if (!m || m->main) return false;
  if (!m->pinned) ++m->refs;
  *base = m->base;
  return true;
}

bool reference_library_file(ModuleSet* set, StringView os_path, uint64_t* base) {
  Module* m = set->find_file(os_path);
  if (!m) return false;
  m->private_layer = false;      // (the program asked for that file: from now on it is a library like the others)
  if (!m->pinned) ++m->refs;
  *base = m->base;
  return true;
}

bool reference_library_base(ModuleSet* set, uint64_t base) {
  Module* m = set->find_base(base);
  if (!m || m->main || m->doomed) return false;
  if (!m->pinned) ++m->refs;
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
  if (!library_name(module, &name)) return 0;
  const Module* m = find(name.view());
  return m ? resolve_export(*this, *m, function, false, 0, 0, allocator_) : 0;
}

LoadStatus load_windows_modules(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ConstByteView program_file, StringView program_name,
                                ILibraryProvider& provider, ModuleSet* out, String* detail, bool bind_program) {
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
    if (!note_own_runtime(c, imp.dll.view())) return LoadStatus::OutOfMemory;
    if (!out->library_name(imp.dll.view(), &name)) continue;
    Module* dependency = nullptr;
    const LoadStatus status = ensure_loaded(c, name.view(), main_raw, false, 1, &dependency);
    if (status != LoadStatus::Ok) return status;
  }
  // Phase 2: bind.
  for (uint64_t i = bind_program ? 0 : 1; i < out->modules_.count(); ++i) {
    const LoadStatus status = bind_imports(c, *out->modules_[i]);
    if (status != LoadStatus::Ok) { main_raw->pe = nullptr; return status; }
  }
  // Phase 3: the order the DLLs start in.
  order(c, *main_raw, 0);
  for (UniquePointer<Module>& m : out->modules_.view()) m->pinned = true;      // (these stay as long as the process does)
  main_raw->pe = nullptr;      // (the program's headers belong to the caller and may go away)
  return LoadStatus::Ok;
}

namespace {
class NoLibraries final : public ILibraryProvider {
 public:
  bool find(StringView, ConstByteView*) override { return false; }
};
}  // namespace

LoadStatus bind_program_imports(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ModuleSet* set) {
  if (set->modules_.count() == 0 || !set->modules_[0]->main) return LoadStatus::Malformed;
  NoLibraries none;
  String detail(allocator);
  Context c{space, allocator, none, *set, &detail};
  Module& main = *set->modules_[0];
  main.pe = &program;      // (the headers are the caller's, and only here)
  const LoadStatus status = bind_imports(c, main);
  main.pe = nullptr;
  return status;
}

}  // namespace process
