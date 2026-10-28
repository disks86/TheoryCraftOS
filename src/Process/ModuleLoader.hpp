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
#include "Executable/PeFile.hpp"
#include "ObjectContainer/List.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// Where the DLLs a Windows program imports come from.
class ILibraryProvider {
 public:
  // The file of a library by its canonical name ("kernel32.dll", see canonical_library_name()). False if there is none. The bytes must stay valid until the
  // provider is destroyed or find() is called again for the same name.
  virtual bool find(StringView canonical_name, ConstByteView* image) = 0;

 protected:
  ILibraryProvider() = default;
  ~ILibraryProvider() = default;
};

// The name a library is known by here: lower case, ".dll" added if there is no extension, and the aliases folded: the API-set names ("api-ms-win-core-file-l1-1-0.dll"
// and the like) and "kernelbase.dll" are kernel32.dll, which implements all of those. False if the name is empty or too long.
bool canonical_library_name(StringView name, String* out);

// Tries one provider, then the next.
class ChainedLibraryProvider final : public ILibraryProvider {
 public:
  ChainedLibraryProvider(ILibraryProvider* first, ILibraryProvider* second) : first_(first), second_(second) {}
  bool find(StringView canonical_name, ConstByteView* image) override { return (first_ && first_->find(canonical_name, image)) || (second_ && second_->find(canonical_name, image)); }

 private:
  ILibraryProvider* first_;
  ILibraryProvider* second_;
};

// Libraries in the folder C:\Windows\System32 of the Windows world (/Windows/Windows/System32 in the OS file system): DLLs a user put there, built for this OS. The
// files are read when first asked for and kept until the provider is destroyed.
class FileLibraryProvider final : public ILibraryProvider {
 public:
  FileLibraryProvider(allocators::IAllocator& allocator, filesystem::IFileSystem* files);
  ~FileLibraryProvider();
  bool find(StringView canonical_name, ConstByteView* image) override;

 private:
  struct Entry;
  allocators::IAllocator& allocator_;
  filesystem::IFileSystem* files_;
  Entry* entries_ = nullptr;
};

// One image of the process: the program itself or a DLL it (indirectly) imports.
struct Module {
  explicit Module(allocators::IAllocator& allocator) : name(allocator), owned(allocator) {}
  String name;                          // canonical
  exe::PeFile owned;                    // the parsed headers of a DLL (the program's are the caller's)
  const exe::PeFile* pe = nullptr;      // the headers in use: &owned, or the program's
  uint64_t base = 0, size = 0;
  uint64_t entry = 0;                   // address of the entry point (DllMain for a DLL); 0 if none
  bool main = false;
  uint32_t visit = 0;                   // (scratch for the ordering)
  // Library lifetime: how many times it was asked for (LoadLibrary) plus how many loaded libraries import it. Libraries loaded when the program started are pinned.
  uint32_t refs = 0;
  bool pinned = false, doomed = false;
  bool no_bind = false;                 // mapped only: no imports bound, no dependencies, no thread-local data (an image that is not a DLL, or DONT_RESOLVE_DLL_REFERENCES)
  int32_t tls_index = -1;               // the slot of its thread-local data in every thread's array (a library loaded after start-up), or -1
};

// Everything loaded into one process, with the stubs that stand in for imports that nothing provides.
class ModuleSet {
 public:
  explicit ModuleSet(allocators::IAllocator& allocator) : allocator_(allocator), modules_(allocator), order_(allocator), missing_(allocator) {}
  ModuleSet(const ModuleSet&) = delete;
  ModuleSet& operator=(const ModuleSet&) = delete;

  Module* find(StringView canonical_name) const;
  Module* find_base(uint64_t base) const;
  // Takes the modules from index `first` on out of the set (and out of the start-up order), unmapping them.
  void drop_from(AddressSpace& space, uint64_t first);
  // In the order they were loaded: the program first.
  uint64_t count() const { return modules_.count(); }
  const Module& at(uint64_t index) const { return *modules_[index]; }
  // The order the entry points of the DLLs must run in (a DLL after the DLLs it imports); the program is not in it.
  uint64_t init_count() const { return order_.count(); }
  const Module& init_at(uint64_t index) const { return *order_[index]; }
  // The address of an exported function by name, 0 if there is none (forwarders are followed).
  uint64_t export_address(StringView module, StringView function) const;

  // A call into an import that nothing provides lands on a stub; this says which import it was ("KERNEL32.dll!Foo"). False if `address` is in no stub.
  bool describe_missing(uint64_t address, String* out) const;
  uint64_t missing_count() const { return missing_.count(); }

 // (Filled in by load_windows_modules(); not for anyone else to change.)
  allocators::IAllocator& allocator_;
  List<UniquePointer<Module>> modules_;
  List<Module*> order_;
  List<String> missing_;
  uint64_t stub_base_ = 0;
};

// Loads the DLLs the program imports (and theirs) next to the program into `space`: each is mapped at its preferred base if that is free, else somewhere else with
// its relocations applied; then every import address table is filled in. An import no library has (or a library that is missing, with `provider` null) does not stop
// the load: its slot points at a stub that stops the program with a message when it is called, which describe_missing() explains. `main_image` is the program as
// load_pe() mapped it. Fails (NeedsLibraries, Malformed, OutOfMemory) only for a library file that is damaged, built for another CPU, or has no relocations when its
// base is taken. `detail` gets a text for the user on failure.
LoadStatus load_windows_modules(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ConstByteView program_file, StringView program_name,
                                ILibraryProvider& provider, ModuleSet* out, String* detail);

// After start-up: loads one more library (the file `provider` has for `canonical_name`) and the libraries it imports that are not loaded yet, into the process that `set` describes,
// and binds their imports. What was loaded is appended to `set` (modules_ and, in the order their entry points must run, order_): the caller reads set->count() and set->init_count()
// from before the call. On failure nothing stays loaded. (A library's thread-local data is the caller's business: see UserProgram::load_module().)
LoadStatus load_additional_library(AddressSpace& space, allocators::IAllocator& allocator, ILibraryProvider& provider, ModuleSet* set, StringView canonical_name, String* detail, bool bind = true);
// LoadLibrary of a library that is loaded already: counts one more use. False if no such library is there (or it is on its way out).
bool reference_library(ModuleSet* set, StringView canonical_name, uint64_t* base);
// FreeLibrary: counts one use less. The libraries nobody uses any more (the one at `base`, then what only it kept alive) are marked and appended to `out` in the order their
// entry points must be called with DLL_PROCESS_DETACH; they stay mapped until unload_library(). Nothing is appended while uses remain, or for a pinned library.
bool release_library(ModuleSet* set, allocators::IAllocator& allocator, uint64_t base, List<Module*>* out);
// Removes the library at `base` from the process (its pages are unmapped). False if there is no such library or it is the program.
bool unload_library(AddressSpace& space, ModuleSet* set, uint64_t base);

// Applies the base relocations of a mapped image that was loaded `delta` (a wrapping difference) away from its preferred base.
bool apply_relocations(AddressSpace& space, const exe::PeFile& pe, uint64_t base, uint64_t delta);
// Records what Windows says about the pages of an image mapped at `base`: the image type, the protection of the headers and of each section (a writable one is copy-on-write),
// and that they are all one allocation.
void describe_image_pages(AddressSpace& space, const exe::PeFile& pe, uint64_t base);
// A relocated image (dynamic base) says in its headers where it really is.
void record_image_base(AddressSpace& space, uint64_t base);

}  // namespace process
