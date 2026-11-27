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
#include "Kernel/Sync.hpp"
#include "Process/ImageLoader.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// Where a library is looked for.
enum class LibrarySearch : uint8_t {
  Normal,          // Windows' search: the program's own folders first, then System32 (SysWOW64 for a 32-bit program)
  SystemOnly,      // only System32 (or SysWOW64): what an internal layer of this OS's own libraries is built on (Windows/tcos_dll_layering.h)
};

// Where the DLLs a Windows program imports come from.
class ILibraryProvider {
 public:
  // The file of a library by its canonical name ("kernel32.dll", see canonical_library_name()). False if there is none. The bytes must stay valid until the
  // provider is destroyed.
  virtual bool find(StringView canonical_name, ConstByteView* image) = 0;
  // The same for a 32-bit program: its libraries are the 32-bit ones (C:\Windows\SysWOW64, where a 64-bit program has C:\Windows\System32). False where
  // there are none.
  virtual bool find_32bit(StringView canonical_name, ConstByteView* image) { (void)canonical_name; (void)image; return false; }
  // The file of a library and where it is in the OS file system (`os_path`: "/Windows/Program Files/App/d3d9.dll", "/Windows/Windows/System32/d3d9.dll"). Two
  // libraries of one name from different files are two libraries of the process (Module::file). This default knows only find() and find_32bit() and says the
  // file is the system folder's.
  virtual bool locate(StringView canonical_name, bool wow, LibrarySearch where, ConstByteView* image, String* os_path) {
    (void)where;
    if (!(wow ? find_32bit(canonical_name, image) : find(canonical_name, image))) return false;
    return os_path->assign(wow ? "/Windows/Windows/SysWOW64/" : "/Windows/Windows/System32/") && os_path->append(canonical_name);
  }
  // Did the program bring this library itself: a file of that name (lower case, "msvcr100.dll") in its own folders, not System32 or SysWOW64? A C runtime
  // it brought is loaded instead of being folded into msvcrt.dll (see ModuleSet::library_name()).
  virtual bool has_own_copy(StringView file_name, bool wow) { (void)file_name; (void)wow; return false; }

 protected:
  ILibraryProvider() = default;
  ~ILibraryProvider() = default;
};

// The name a library is known by here: lower case, ".dll" added if there is no extension, and the aliases folded: the API-set names ("api-ms-win-core-file-l1-1-0.dll"
// and the like) and "kernelbase.dll" are kernel32.dll, which implements all of those. False if the name is empty or too long.
bool canonical_library_name(StringView name, String* out);
// The name as it is, made canonical only in its form: lower case, the file name of a path, ".dll" added. False if it is empty or too long.
bool plain_library_name(StringView name, String* out);
// A C runtime of one Visual C++ version (msvcr100.dll, msvcp120.dll, vcruntime140.dll ...): one canonical_library_name() folds into msvcrt.dll unless the
// program brings its own copy.
bool is_versioned_runtime(StringView plain_name);
// Is the OS path of a library file ("/Windows/Windows/System32/d3d9.dll") directly in the system folder, System32 or SysWOW64 (any case)?
bool is_system_library_path(StringView os_path);
// The Windows folder of a library file of the OS file system, as Module::directory has it: "C:\\Games\\X\\" for "/Windows/Games/X/d3d9.dll"; empty for the
// system folders and for a file outside the Windows world (which has no drive letter). False if memory ran out.
bool windows_directory_of_library(StringView os_path, String* out);
// Is the import of `imported` by `importer` (canonical names) one of the internal layers of this OS's libraries (Windows/tcos_dll_layering.h)?
bool is_internal_layer(StringView importer, StringView imported);

// Tries one provider, then the next.
class ChainedLibraryProvider final : public ILibraryProvider {
 public:
  ChainedLibraryProvider(ILibraryProvider* first, ILibraryProvider* second) : first_(first), second_(second) {}
  bool find(StringView canonical_name, ConstByteView* image) override {
    return (first_ && first_->find(canonical_name, image)) || (second_ && second_->find(canonical_name, image));
  }
  bool find_32bit(StringView canonical_name, ConstByteView* image) override {
    return (first_ && first_->find_32bit(canonical_name, image)) || (second_ && second_->find_32bit(canonical_name, image));
  }
  bool locate(StringView canonical_name, bool wow, LibrarySearch where, ConstByteView* image, String* os_path) override {
    return (first_ && first_->locate(canonical_name, wow, where, image, os_path)) || (second_ && second_->locate(canonical_name, wow, where, image, os_path));
  }
  bool has_own_copy(StringView file_name, bool wow) override {
    return (first_ && first_->has_own_copy(file_name, wow)) || (second_ && second_->has_own_copy(file_name, wow));
  }

 private:
  ILibraryProvider* first_;
  ILibraryProvider* second_;
};

// Libraries in the folder C:\Windows\System32 of the Windows world (/Windows/Windows/System32 in the OS file system): DLLs a user put there, built for this OS. Folders
// added with add_directory() are searched first, in the order given (the program's own folder: an application's DLLs next to its .exe, as Windows looks there first).
// The files are read when first asked for and kept, by their path, until the provider is destroyed. The provider may be shared by programs that run at once (a program
// and the ones it starts), so finding is serialised.
// Side-by-side manifests, as far as the loader needs them. A program's manifest (its RT_MANIFEST resource) names the private assemblies it depends on
// (a <dependentAssembly> whose <assemblyIdentity> has no publicKeyToken); such an assembly is a folder next to the program with a manifest of its own
// whose <file name=.../> entries are DLLs found there (Chrome: chrome.exe beside "131.0.6778.86\131.0.6778.86.manifest" listing chrome_elf.dll).
// Only plain names come out (no folder separators, not "." or ".."). False if memory ran out.
[[nodiscard]] bool manifest_private_assemblies(StringView xml, allocators::IAllocator& allocator, List<String>* names);
[[nodiscard]] bool manifest_files(StringView xml, allocators::IAllocator& allocator, List<String>* names);

class FileLibraryProvider final : public ILibraryProvider {
 public:
  FileLibraryProvider(allocators::IAllocator& allocator, filesystem::IFileSystem* files);
  ~FileLibraryProvider();
  bool find(StringView canonical_name, ConstByteView* image) override;
  bool find_32bit(StringView canonical_name, ConstByteView* image) override;
  bool locate(StringView canonical_name, bool wow, LibrarySearch where, ConstByteView* image, String* os_path) override;
  bool has_own_copy(StringView file_name, bool wow) override;
  // A folder of the OS file system ("/Windows/Program Files/App") to look in before System32.
  bool add_directory(StringView directory);
  // The program at `path` (OS file system) with the file `image`: its folder (add_directory), and the DLLs of the private assemblies its manifest
  // depends on, found in the assembly's folder ("<folder>/<assembly>/") before anywhere else. False if memory ran out; a missing or unreadable
  // manifest is no failure (the program runs with what is found).
  bool add_program(StringView path, ConstByteView image);

 private:
  struct Entry;
  struct Redirect {
    explicit Redirect(allocators::IAllocator& allocator) : name(allocator), directory(allocator) {}
    String name;           // canonical (lower case) file name
    String directory;      // OS folder, ending in '/'
  };
  bool load(StringView path, ConstByteView* image);      // the file at `path`: read once, then kept (lock held)
  bool read_text(StringView path, uint64_t limit, String* out);
  allocators::IAllocator& allocator_;
  filesystem::IFileSystem* files_;
  Entry* entries_ = nullptr;
  List<String> directories_;
  List<Redirect> redirects_;
  kernel::Mutex lock_;
};

// One image of the process: the program itself or a DLL it (indirectly) imports.
struct Module {
  explicit Module(allocators::IAllocator& allocator) : name(allocator), directory(allocator), file(allocator), owned(allocator), import_deps(allocator), forward_deps(allocator) {}
  String name;                          // canonical
  String directory;                     // the Windows folder its file is in, when not System32 ("C:\\Program Files\\App\\"); empty for System32
  // The file it was loaded from, in the OS file system ("/Windows/Windows/System32/d3d9.dll"; empty for the program). A library is known by its file: a load by
  // path of another file of the same name is another library (as on Windows), a load by bare name is the first library of that name (ModuleSet::find()).
  String file;
  bool system = false;                  // the file is in System32 or SysWOW64
  // Loaded only as an internal layer of a System32 library (Windows/tcos_dll_layering.h): not found by its base name, as it is not there on Windows.
  bool private_layer = false;
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
  // The libraries its imports were bound to, each once (which library a name means can change later: a private layer that becomes an ordinary library may be
  // found first by that name from then on, but this library's uses stay where they were counted).
  List<Module*> import_deps;
  List<Module*> forward_deps;           // libraries that a forwarded export of this one was resolved through: they stay loaded as long as this one does (like its imports)
  int32_t tls_index = -1;               // the slot of its thread-local data in every thread's array (a library loaded after start-up), or -1
};

// Everything loaded into one process, with the stubs that stand in for imports that nothing provides.
class ModuleSet {
 public:
  explicit ModuleSet(allocators::IAllocator& allocator) : allocator_(allocator), modules_(allocator), order_(allocator), missing_(allocator), own_runtimes_(allocator), folded_runtimes_(allocator) {}
  ModuleSet(const ModuleSet&) = delete;
  ModuleSet& operator=(const ModuleSet&) = delete;

  // A library by its base name, as a load by bare name finds it: the first loaded of that name, leaving out private layers.
  Module* find(StringView canonical_name) const;
  // The library loaded from the file at `os_path` (any case), private layers too.
  Module* find_file(StringView os_path) const;
  // The library of that name whose file is in System32 or SysWOW64, private layers too.
  Module* find_system(StringView canonical_name) const;
  // The library an import (or a forwarder) of `importer` names: find_system() for an internal layer of a System32 library, else find().
  Module* import_target(const Module& importer, StringView canonical_name) const;
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
  // The name a library is known by in this process: canonical_library_name(), except that a C runtime the program brought itself ("msvcr100.dll" in its
  // folder, as Unigine Heaven has) keeps its own name and is that file, as on Windows, where the program's folder is searched first and these are not
  // known DLLs. Which runtimes those are is decided when the program's imports are first seen (own_runtimes_). False if the name is empty or too long.
  bool library_name(StringView name, String* out) const;
  bool owns_runtime(StringView plain_name) const;

 // (Filled in by load_windows_modules(); not for anyone else to change.)
  allocators::IAllocator& allocator_;
  List<UniquePointer<Module>> modules_;
  List<Module*> order_;
  List<String> missing_;
  List<String> own_runtimes_;      // plain names of the C runtimes the program brought ("msvcr100.dll")
  List<String> folded_runtimes_;   // and of those it did not bring: msvcrt.dll stands for them
  uint64_t stub_base_ = 0;
};

// Loads the DLLs the program imports (and theirs) next to the program into `space`: each is mapped at its preferred base if that is free, else somewhere else with
// its relocations applied; then every import address table is filled in. An import no library has (or a library that is missing, with `provider` null) does not stop
// the load: its slot points at a stub that stops the program with a message when it is called, which describe_missing() explains. `main_image` is the program as
// load_pe() mapped it. Fails (NeedsLibraries, Malformed, OutOfMemory) only for a library file that is damaged, built for another CPU, or has no relocations when its
// base is taken. `detail` gets a text for the user on failure.
// With `bind_program` false the program's own import table is left as the file has it (bind_program_imports() fills it in later: a process that is created suspended has not
// resolved its imports yet).
LoadStatus load_windows_modules(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ConstByteView program_file, StringView program_name,
                                ILibraryProvider& provider, ModuleSet* out, String* detail, bool bind_program = true);
// The import table of the program (the first module of the set), when load_windows_modules() left it alone.
LoadStatus bind_program_imports(AddressSpace& space, allocators::IAllocator& allocator, const exe::PeFile& program, ModuleSet* set);

// After start-up: loads one more library (the file `provider` has for `canonical_name`) and the libraries it imports that are not loaded yet, into the process that `set` describes,
// and binds their imports. What was loaded is appended to `set` (modules_ and, in the order their entry points must run, order_): the caller reads set->count() and set->init_count()
// from before the call. On failure nothing stays loaded. (A library's thread-local data is the caller's business: see UserProgram::load_module().)
// The library itself is always a new one, even when another of its name is loaded: the caller has looked for a loaded one by name or by file.
LoadStatus load_additional_library(AddressSpace& space, allocators::IAllocator& allocator, ILibraryProvider& provider, ModuleSet* set, StringView canonical_name, String* detail, bool bind = true);
// LoadLibrary of a library that is loaded already: counts one more use. False if no such library is there (or it is on its way out). By base name (a private layer is
// not found), or by its file (a private layer found so is an ordinary library from then on: the program asked for it).
bool reference_library(ModuleSet* set, StringView canonical_name, uint64_t* base);
bool reference_library_file(ModuleSet* set, StringView os_path, uint64_t* base);
// The same for the library whose image starts at `base`, whatever its name or file (a private layer stays one: nothing was asked for by name or path).
bool reference_library_base(ModuleSet* set, uint64_t base);
// The library at `from` depends on the one at `to` from now on (once for a pair); false if either is not there.
bool link_libraries(ModuleSet* set, uint64_t from, uint64_t to);
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
