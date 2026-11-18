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
#include "FileSystem/IFileSystem.hpp"
#include "Msi/MsiDatabase.hpp"
#include "Msi/MsiExpression.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// Installs and removes what a Windows Installer package (.msi, MsiDatabase.hpp) describes, the way "msiexec /i package.msi /qn" and
// "msiexec /x package.msi /qn" do: with no questions and no windows. A package is not a program but a description: tables of folders, features,
// components, files, registry values and shortcuts, and a list of actions (InstallExecuteSequence) to run in order, each under a condition. This
// engine runs that list:
//
//   - the properties: the package's own (the Property table), the ones Windows Installer defines (the folders of the system, the version of
//     Windows, ALLUSERS=1: every installation is for all users) and the ones the caller gives;
//   - AppSearch (registry values), LaunchConditions, the costing actions (where each folder of the Directory table is, which features are chosen
//     by their level and conditions, which components those bring);
//   - InstallFiles: the files of the chosen components, out of the cabinets inside the package (Media/Cabinet.hpp), into their folders on drive C
//     (which is /Windows here); CreateFolders, WriteRegistryValues, CreateShortcuts, RegisterProduct (the entry of "installed programs");
//   - the package's own custom actions that set a property or a folder, stop with a message, or run a program (one kept in the package, one of
//     the installed files, or one a property names), waiting for it and checking how it ended;
//   - removal runs the same list with REMOVE=ALL and Installed set: RemoveFiles, RemoveFolders, RemoveRegistryValues, RemoveShortcuts, and the
//     custom actions the package runs when it is removed.
//
// What it does not do is said rather than guessed: custom actions that are DLLs or scripts are skipped (they need the installer's programming
// interface, msi.dll, inside the action's process) and noted; so are the actions for services, environment variables, COM classes, file types, fonts
// and .ini files when their tables have rows (notes()). There is no rollback: a failure leaves what was done so far, and the caller removes it
// (Options::undo).
// Packages whose files lie beside them, or that are for another processor, are refused. Everything the package says is untrusted: a folder or
// file name that would leave drive C or go through "..", and sizes that do not match, stop the installation.
namespace msi {

enum class RegistryRoot : uint8_t { ClassesRoot, CurrentUser, LocalMachine, Users };
inline constexpr uint32_t kRegString = 1, kRegExpandString = 2, kRegBinary = 3, kRegDword = 4, kRegMultiString = 7;

// What the engine needs of the system around it: the registry of the Windows world, programs run in it, and shortcut files.
class IHost {
 public:
  // Runs the Windows program at `program` (a path of the OS file system) with the rest of its command line in `arguments`, in `working_folder` (a
  // path of the OS file system; "": the program's own), and waits for it. False if it could not be started or was stopped: *message says which.
  [[nodiscard]] virtual bool run(StringView program, StringView arguments, StringView working_folder, uint32_t* exit_code, String* message) = 0;
  // The registry: `key` is the path under the root ("SOFTWARE\Vendor\Product"), `name` the value's ("" is the key's default value), `data` as the
  // registry keeps it (text is UTF-16LE with its terminator). Setting makes the key. False if there is no registry or the value is not there.
  [[nodiscard]] virtual bool registry_set(RegistryRoot root, StringView key, StringView name, uint32_t type, ConstByteView data) = 0;
  [[nodiscard]] virtual bool registry_get(RegistryRoot root, StringView key, StringView name, uint32_t* type, ByteBuffer* data) = 0;
  virtual void registry_delete_value(RegistryRoot root, StringView key, StringView name) = 0;
  // Deletes the key: with everything under it, or (`only_if_empty`) only if it has no values and no keys.
  virtual void registry_delete_key(RegistryRoot root, StringView key, bool only_if_empty) = 0;
  // Writes a shortcut file at `link` (a path of the OS file system) to the program or file `target` (a Windows path), started in `working_folder`.
  [[nodiscard]] virtual bool write_shortcut(StringView link, StringView target, StringView working_folder) = 0;

 protected:
  ~IHost() = default;
};

enum class Outcome : uint8_t {
  Ok,
  NotSupported,      // a package this engine cannot install (the detail says why)
  Refused,           // the package itself says no: a launch condition, or an action that stops with a message
  Failed,            // an action failed: a program it ran, or the package is damaged
  FileError,         // a file or a folder could not be written
  Cancelled,
  OutOfMemory,
};

struct Options {
  bool remove = false;                        // take the product away instead of installing it
  bool undo = false;                          // with `remove`: take away what an installation that failed left; the package's programs are not run
  ArrayView<const StringView> properties;     // "NAME=value" each: properties of the command line (they win over the package's)
  StringView package_path;                    // where the package is, as a Windows path (the properties OriginalDatabase and SourceDir)
  StringView upgrading_product;               // removing to make room for another version: that version's product code (UPGRADINGPRODUCTCODE)
  const bool* cancel = nullptr;               // becoming true stops the run between two files or two actions
};

class Installer final : private IExpressionContext {
 public:
  static constexpr uint64_t kMaxFileBytes = 16ull << 30;       // one installed file
  static constexpr uint32_t kMaxFiles = 200000, kMaxNotes = 64;

  // `database` is the opened package; everything must outlive this.
  Installer(allocators::IAllocator& allocator, filesystem::IFileSystem& files, IHost& host, Database& database);
  ~Installer();

  // Runs the package's execute sequence. On anything but Ok *detail says what stopped it.
  [[nodiscard]] Outcome run(const Options& options, String* detail);

  // After (or during) a run: a property's value ("ProductCode", "ProductName", "ProductVersion", "Manufacturer", ...; "" if it has none).
  StringView property(StringView name) const override;
  // What the run skipped that the package wanted (one line each), and the shortcut files it wrote (paths of the OS file system).
  ArrayView<const String> notes() const { return notes_.view(); }
  ArrayView<const String> shortcuts() const { return shortcuts_.view(); }

  // "C:\Program Files\App\a.exe" as a path of the OS file system ("/Windows/Program Files/App/a.exe"). False for another drive, a relative
  // path, one with ".." or a character Windows does not allow in a name, or if memory ran out.
  [[nodiscard]] static bool system_path(StringView windows_path, String* out);

 private:
  struct Named;
  struct Folder;
  struct Feature;
  struct Component;
  struct FileItem;

  // IExpressionContext
  int32_t state(char kind, StringView name) const override;
  [[nodiscard]] bool file_path(StringView key, String* out) const override;
  [[nodiscard]] bool component_folder(StringView key, String* out) const override;

  bool set_property(StringView name, StringView value);
  bool set_folder_property(StringView name, StringView system_folder, StringView more = {});
  bool load_properties(const Options& options);
  bool load_model();
  bool format(StringView text, String* out) const;
  bool condition(StringView text) const;
  bool note(StringView what, StringView name = {});
  Outcome fail(Outcome outcome, StringView what, StringView name = {}, StringView more = {});

  Folder* folder(StringView key);
  const Folder* folder(StringView key) const;
  bool resolve_folder(Folder& f, uint32_t depth);
  bool resolve_folders();
  Component* component(StringView key);
  const Component* component(StringView key) const;
  uint64_t file_index(StringView key) const;
  const FileItem* file(StringView key) const;
  bool installing(StringView component_key) const;
  bool removing(StringView component_key) const;

  Outcome run_action(StringView action);
  Outcome custom_action(StringView action, uint32_t row);
  Outcome run_program(StringView action, StringView program, StringView arguments, StringView working_folder, bool ignore_result);
  Outcome app_search();
  Outcome launch_conditions();
  Outcome cost_finalize();
  Outcome install_files();
  Outcome extract_cabinet(StringView cabinet, uint32_t first_sequence, uint32_t last_sequence);
  Outcome create_folders();
  Outcome remove_files();
  Outcome remove_folders();
  Outcome write_registry();
  Outcome remove_registry();
  Outcome create_shortcuts();
  Outcome remove_shortcuts();
  Outcome register_product();
  void unregister_product();
  bool shortcut_path(const Table& table, uint32_t row, String* link) const;
  bool registry_place(const Table& table, uint32_t row, RegistryRoot* root, String* key, String* name) const;
  bool cancelled() const { return cancel_ && __atomic_load_n(cancel_, __ATOMIC_RELAXED); }

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  IHost& host_;
  Database& database_;
  List<UniquePointer<Named>> properties_;
  List<UniquePointer<Folder>> folders_;
  List<UniquePointer<Feature>> features_;
  List<UniquePointer<Component>> components_;
  List<UniquePointer<FileItem>> file_items_;
  List<uint32_t> files_by_key_;      // file_items_ in the order of their keys, to look one up
  List<String> notes_, shortcuts_;
  List<String> system_folders_;      // the folders of the system (Windows paths ending in '\\'): never the package's to take away
  String detail_;
  const bool* cancel_ = nullptr;
  bool remove_ = false;
  bool undo_ = false;
  bool costed_ = false;
  bool package64_ = false;
};

}  // namespace msi
