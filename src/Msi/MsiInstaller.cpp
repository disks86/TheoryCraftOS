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

#include "Msi/MsiInstaller.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Media/Cabinet.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"
#include "Utility/Utf16.hpp"

namespace msi {

using filesystem::Status;

struct Installer::Named {
  explicit Named(allocators::IAllocator& allocator) : name(allocator), value(allocator) {}
  String name, value;
};

struct Installer::Folder {
  explicit Folder(allocators::IAllocator& allocator) : key(allocator), parent(allocator), name(allocator), path(allocator) {}
  String key, parent;
  String name;              // the folder's own name under its parent ("." or "": the parent itself)
  String path;              // a Windows path that ends in '\'
  bool resolved = false;
  bool given = false;       // set from outside (a folder of the system, a property, a custom action), not made of its parent's
  bool busy = false;        // being resolved (a parent that leads back here is a damaged table)
};

struct Installer::Feature {
  explicit Feature(allocators::IAllocator& allocator) : key(allocator), parent(allocator) {}
  String key, parent;
  int32_t level = 1;
  bool selected = false;
};

struct Installer::Component {
  explicit Component(allocators::IAllocator& allocator) : key(allocator), folder(allocator), condition(allocator), key_path(allocator) {}
  String key, folder, condition, key_path;
  int32_t attributes = 0;
  bool wanted = false;      // a chosen feature has it, and its condition holds
};

struct Installer::FileItem {
  explicit FileItem(allocators::IAllocator& allocator) : key(allocator), component(allocator), name(allocator) {}
  String key, component, name;
  uint64_t size = 0;
  uint32_t sequence = 0;
  int32_t attributes = 0;
  bool placed = false;
};

namespace {

constexpr uint32_t kMaxFolderDepth = 64;
constexpr uint32_t kFileNoncompressed = 0x2000;
constexpr uint64_t kMaxActionProgramBytes = 1ull << 30;      // a program kept in the package for a custom action

// "short|long" -> "long" (file and folder names come in both forms).
StringView long_name(StringView text) {
  const uint64_t bar = text.find('|');
  return bar == StringView::npos ? text : text.substr(bar + 1);
}

// A name Windows could give a file or a folder: no path in it, nothing it does not allow.
bool valid_name(StringView name) {
  if (name.empty() || name.size() > 255 || name.equals(".") || name.equals("..") || name.ends_with(' ') || name.ends_with('.')) return false;
  for (const char c : name)
    if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  return true;
}

bool write_all(filesystem::IFile& file, uint64_t offset, ConstByteView bytes) {
  uint64_t at = 0;
  while (at < bytes.size()) {
    uint64_t done = 0;
    if (file.write(offset + at, bytes.subview(at), &done) != Status::Ok || done == 0) return false;
    at += done;
  }
  return true;
}

// Makes the folder at `path` (a path of the OS file system) and every folder above it that is missing.
bool make_folders(filesystem::IFileSystem& files, StringView path) {
  filesystem::DirectoryEntry entry;
  if (files.stat(path, &entry) == Status::Ok) return entry.type == filesystem::NodeType::Directory;
  const uint64_t slash = path.rfind('/');
  if (slash != StringView::npos && slash > 0 && !make_folders(files, path.first(slash))) return false;
  const Status made = files.create_directory(path);
  return made == Status::Ok || made == Status::AlreadyExists;
}

bool exists(filesystem::IFileSystem& files, StringView path) {
  filesystem::DirectoryEntry entry;
  return files.stat(path, &entry) == Status::Ok;
}

StringView parent_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? StringView() : path.first(slash);
}

int compare_folded(StringView a, StringView b) {
  const uint64_t shared = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < shared; ++i) {
    const uint8_t x = static_cast<uint8_t>(fold(a[i])), y = static_cast<uint8_t>(fold(b[i]));
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

// Does `name` match `pattern`, where '*' stands for any run of characters and '?' for one (without regard to case)?
bool matches_wildcard(StringView name, StringView pattern, uint32_t depth = 0) {
  if (depth > 16) return false;
  uint64_t n = 0, p = 0;
  while (p < pattern.size()) {
    if (pattern[p] == '*') {
      for (uint64_t skip = n; skip <= name.size(); ++skip)
        if (matches_wildcard(name.substr(skip), pattern.substr(p + 1), depth + 1)) return true;
      return false;
    }
    if (n >= name.size() || (pattern[p] != '?' && fold(pattern[p]) != fold(name[n]))) return false;
    ++n, ++p;
  }
  return n == name.size();
}

// The actions that are tables this engine does not act on: named when the package has rows for them.
constexpr struct {
  StringView action, table, what;
} kTableActions[] = {
    {"InstallServices", "ServiceInstall", "its services were not installed"},
    {"WriteEnvironmentStrings", "Environment", "its environment variables were not set"},
    {"RegisterClassInfo", "Class", "its COM classes were not registered"},
    {"RegisterExtensionInfo", "Extension", "its file types were not registered"},
    {"RegisterProgIdInfo", "ProgId", "its program identifiers were not registered"},
    {"RegisterMIMEInfo", "MIME", "its content types were not registered"},
    {"RegisterTypeLibraries", "TypeLib", "its type libraries were not registered"},
    {"SelfRegModules", "SelfReg", "its self-registering libraries were not registered"},
    {"RegisterFonts", "Font", "its fonts were not registered"},
    {"WriteIniValues", "IniFile", "its .ini values were not written"},
    {"DuplicateFiles", "DuplicateFile", "its duplicate files were not made"},
    {"MoveFiles", "MoveFile", "its files to move were not moved"},
    {"InstallODBC", "ODBCDriver", "its ODBC drivers were not installed"},
    {"MsiPublishAssemblies", "MsiAssembly", "its assemblies were not published"},
    {"PublishComponents", "PublishComponent", "its qualified components were not published"},
};

// The actions of Windows Installer that have nothing to do here: bookkeeping of the real installer, or work this engine does as it goes.
constexpr StringView kQuietActions[] = {
    "CostInitialize", "FileCost", "InstallValidate", "InstallInitialize", "ProcessComponents", "UnpublishComponents", "UnpublishFeatures", "PublishFeatures",
    "PublishProduct", "RegisterUser", "InstallFinalize", "FindRelatedProducts", "MigrateFeatureStates", "RemoveExistingProducts", "ValidateProductID",
    "AllocateRegistrySpace", "IsolateComponents", "BindImage", "SetODBCFolders", "ScheduleReboot", "ForceReboot", "ResolveSource", "CCPSearch", "RMCCPSearch",
    "UnregisterClassInfo", "UnregisterExtensionInfo", "UnregisterProgIdInfo", "UnregisterMIMEInfo", "UnregisterTypeLibraries", "SelfUnregModules", "UnregisterFonts",
    "RemoveIniValues", "RemoveDuplicateFiles", "RemoveODBC", "RemoveEnvironmentStrings", "StopServices", "StartServices", "DeleteServices", "MsiUnpublishAssemblies",
    "InstallExecute", "InstallExecuteAgain", "InstallSFPCatalogFile", "PatchFiles", "RegisterComPlus", "UnregisterComPlus", "DisableRollback", "InstallAdminPackage",
    "MsiConfigureServices", "ExecuteAction",
};

}  // namespace

Installer::Installer(allocators::IAllocator& allocator, filesystem::IFileSystem& files, IHost& host, Database& database)
    : allocator_(allocator), files_(files), host_(host), database_(database), properties_(allocator), folders_(allocator), features_(allocator), components_(allocator),
      file_items_(allocator), files_by_key_(allocator), notes_(allocator), shortcuts_(allocator), system_folders_(allocator), detail_(allocator) {}

Installer::~Installer() = default;

bool Installer::system_path(StringView windows_path, String* out) {
  if (windows_path.size() < 3 || fold(windows_path[0]) != 'c' || windows_path[1] != ':' || (windows_path[2] != '\\' && windows_path[2] != '/')) return false;
  StringView rest = windows_path.without_prefix(3), part;
  String clean(out->allocator());
  if (!clean.assign("C:")) return false;
  while (!rest.empty()) {
    uint64_t end = 0;
    while (end < rest.size() && rest[end] != '\\' && rest[end] != '/') ++end;
    part = rest.first(end);
    rest = rest.without_prefix(end + 1);
    if (part.empty()) continue;      // ("a\\b", a '\' at the end)
    if (!valid_name(part) || !clean.push_back('\\') || !clean.append(part)) return false;
  }
  if (clean.size() == 2 && !clean.push_back('\\')) return false;
  return filesystem::layout::system_path_of_windows(clean.view(), out);
}

// --- properties

StringView Installer::property(StringView name) const {
  for (const UniquePointer<Named>& p : properties_.view())
    if (p->name.view().equals(name)) return p->value.view();
  return StringView();
}

bool Installer::set_property(StringView name, StringView value) {
  for (uint64_t i = 0; i < properties_.count(); ++i) {
    if (!properties_[i]->name.view().equals(name)) continue;
    if (value.empty()) return properties_.remove_at(i);      // (a property with no value is no property)
    String copy(allocator_);      // (the value may be a piece of the old one)
    return copy.assign(value) && properties_[i]->value.assign(copy.view());
  }
  if (value.empty()) return true;
  UniquePointer<Named> p = UniquePointer<Named>::make(allocator_, allocator_);
  return p && p->name.assign(name) && p->value.assign(value) && properties_.push_back(util::move(p));
}

// A property that is a folder of the Windows world: `system_folder` (a path of the OS file system under /Windows) and `more` under it, as "C:\...\".
bool Installer::set_folder_property(StringView name, StringView system_folder, StringView more) {
  String path(allocator_), full(allocator_);
  if (!full.assign(system_folder) || !full.append(more) || !filesystem::layout::windows_view_of(full.view(), &path)) return false;
  if (!path.view().ends_with('\\') && !path.push_back('\\')) return false;
  String kept(allocator_);
  return set_property(name, path.view()) && kept.assign(path.view()) && system_folders_.push_back(util::move(kept));
}

bool Installer::load_properties(const Options& options) {
  namespace layout = filesystem::layout;
  if (const Table* table = database_.table("Property")) {
    for (uint32_t row = 0; row < table->rows(); ++row)
      if (!set_property(table->text(row, "Property"), table->text(row, "Value"))) return false;
  }
  // The folders. (A Windows process's user is "Default": Process/ProgramEnvironment.cpp.)
  String user(allocator_), start_menu(allocator_);
  if (!user.assign(layout::kWindowsUsers) || !user.append("/Default") || !start_menu.assign(parent_of(layout::kWindowsStartMenuPrograms))) return false;
  const bool folders =
      set_property("ROOTDRIVE", "C:\\") && set_property("WindowsVolume", "C:\\") && set_folder_property("ProgramFiles64Folder", layout::kWindowsProgramFiles) &&
      set_folder_property("ProgramFilesFolder", layout::kWindowsProgramFilesX86) && set_folder_property("CommonFiles64Folder", layout::kWindowsProgramFiles, "/Common Files") &&
      set_folder_property("CommonFilesFolder", layout::kWindowsProgramFilesX86, "/Common Files") && set_folder_property("WindowsFolder", layout::kWindowsDirectory) &&
      set_folder_property("System64Folder", layout::kWindowsSystem32) && set_folder_property("SystemFolder", layout::kWindowsSysWow64) &&
      set_folder_property("FontsFolder", layout::kWindowsDirectory, "/Fonts") && set_folder_property("TempFolder", layout::kWindowsDirectory, "/Temp") &&
      set_folder_property("CommonAppDataFolder", layout::kWindowsProgramData) && set_folder_property("ProgramMenuFolder", layout::kWindowsStartMenuPrograms) &&
      set_folder_property("StartupFolder", layout::kWindowsStartMenuPrograms, "/Startup") && set_folder_property("AdminToolsFolder", layout::kWindowsStartMenuPrograms, "/Administrative Tools") &&
      set_folder_property("StartMenuFolder", start_menu.view()) && set_folder_property("DesktopFolder", layout::kDesktop) && set_folder_property("AppDataFolder", user.view(), "/AppData/Roaming") &&
      set_folder_property("LocalAppDataFolder", user.view(), "/AppData/Local") && set_folder_property("PersonalFolder", user.view(), "/Documents") &&
      set_folder_property("USERPROFILE", user.view());
  if (!folders) return false;
  // The system: Windows 10 as Windows Installer reports it, on a 64-bit x86 processor, run by an administrator with no windows.
  static constexpr struct {
    StringView name, value;
  } kSystem[] = {{"VersionNT", "603"},     {"VersionNT64", "603"},    {"WindowsBuild", "9600"},  {"ServicePackLevel", "0"}, {"MsiNTProductType", "1"}, {"VersionMsi", "5.00"},
                 {"Msix64", "6"},          {"Intel", "6"},            {"Privileged", "1"},       {"AdminUser", "1"},        {"PhysicalMemory", "8192"}, {"ColorBits", "32"},
                 {"ScreenX", "1920"},      {"ScreenY", "1080"},       {"ComputerName", "TCOS"},  {"LogonUser", "Default"},  {"USERNAME", "Default"},    {"UserLanguageID", "1033"},
                 {"SystemLanguageID", "1033"}};
  for (const auto& p : kSystem)
    if (!set_property(p.name, p.value)) return false;
  if (!set_property("PackageCode", database_.package_code())) return false;
  if (!options.package_path.empty()) {
    const uint64_t slash = options.package_path.rfind('\\');
    if (!set_property("OriginalDatabase", options.package_path) || !set_property("DATABASE", options.package_path) ||
        (slash != StringView::npos && (!set_property("SourceDir", options.package_path.first(slash + 1)) || !set_property("SOURCEDIR", options.package_path.first(slash + 1)))))
      return false;
  }
  // The command line's.
  for (const StringView pair : options.properties) {
    StringView name, value;
    if (!pair.split_once('=', &name, &value)) continue;
    name = name.trimmed(), value = value.trimmed();
    if (value.size() >= 2 && value.starts_with('"') && value.ends_with('"')) value = value.substr(1, value.size() - 2);
    if (!name.empty() && !set_property(name, value)) return false;
  }
  // What is not the package's or the caller's to say.
  if (!set_property("ALLUSERS", "1") || !set_property("UILevel", "2") || !set_property("CLIENTUILEVEL", "3")) return false;
  if (remove_ && (!set_property("REMOVE", "ALL") || !set_property("Installed", "1"))) return false;
  if (!options.upgrading_product.empty() && !set_property("UPGRADINGPRODUCTCODE", options.upgrading_product)) return false;
  return true;
}

// --- the tables, as the things they describe

bool Installer::load_model() {
  if (const Table* table = database_.table("Directory")) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      UniquePointer<Folder> f = UniquePointer<Folder>::make(allocator_, allocator_);
      // DefaultDir is "target:source", each of them "short|long": where the folder goes is the target's long name.
      StringView target = table->text(row, "DefaultDir");
      const uint64_t colon = target.find(':');
      if (colon != StringView::npos) target = target.first(colon);
      if (!f || !f->key.assign(table->text(row, "Directory")) || !f->parent.assign(table->text(row, "Directory_Parent")) || !f->name.assign(long_name(target)) ||
          !folders_.push_back(util::move(f)))
        return false;
    }
  }
  if (const Table* table = database_.table("Feature")) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      UniquePointer<Feature> f = UniquePointer<Feature>::make(allocator_, allocator_);
      if (!f || !f->key.assign(table->text(row, "Feature")) || !f->parent.assign(table->text(row, "Feature_Parent"))) return false;
      const int32_t level = table->integer(row, "Level");
      f->level = level == kNull ? 0 : level;
      if (!features_.push_back(util::move(f))) return false;
    }
  }
  if (const Table* table = database_.table("Component")) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      UniquePointer<Component> c = UniquePointer<Component>::make(allocator_, allocator_);
      if (!c || !c->key.assign(table->text(row, "Component")) || !c->folder.assign(table->text(row, "Directory_")) || !c->condition.assign(table->text(row, "Condition")) ||
          !c->key_path.assign(table->text(row, "KeyPath")))
        return false;
      const int32_t attributes = table->integer(row, "Attributes");
      c->attributes = attributes == kNull ? 0 : attributes;
      if (!components_.push_back(util::move(c))) return false;
    }
  }
  if (const Table* table = database_.table("File")) {
    if (table->rows() > kMaxFiles) return false;
    for (uint32_t row = 0; row < table->rows(); ++row) {
      UniquePointer<FileItem> f = UniquePointer<FileItem>::make(allocator_, allocator_);
      if (!f || !f->key.assign(table->text(row, "File")) || !f->component.assign(table->text(row, "Component_")) || !f->name.assign(long_name(table->text(row, "FileName")))) return false;
      const int32_t size = table->integer(row, "FileSize"), sequence = table->integer(row, "Sequence"), attributes = table->integer(row, "Attributes");
      f->size = size == kNull ? 0 : static_cast<uint32_t>(size);
      f->sequence = sequence == kNull || sequence < 0 ? 0 : static_cast<uint32_t>(sequence);
      f->attributes = attributes == kNull ? 0 : attributes;
      if (!file_items_.push_back(util::move(f))) return false;
    }
  }
  // The files in the order of their keys (without regard to case, as a cabinet names them): a heap sort of their numbers.
  const uint64_t count = file_items_.count();
  if (!files_by_key_.reserve(count)) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (!files_by_key_.push_back(i)) return false;
  ArrayView<uint32_t> order = files_by_key_.view();
  auto before = [&](uint32_t a, uint32_t b) { return compare_folded(file_items_[a]->key.view(), file_items_[b]->key.view()) < 0; };
  auto sift = [&](uint64_t root, uint64_t end) {
    for (;;) {
      uint64_t child = root * 2 + 1;
      if (child >= end) return;
      if (child + 1 < end && before(order[child], order[child + 1])) ++child;
      if (!before(order[root], order[child])) return;
      const uint32_t t = order[root];
      order[root] = order[child];
      order[child] = t;
      root = child;
    }
  };
  for (uint64_t start = count / 2; start > 0; --start) sift(start - 1, count);
  for (uint64_t end = count; end > 1; --end) {
    const uint32_t t = order[0];
    order[0] = order[end - 1];
    order[end - 1] = t;
    sift(0, end - 1);
  }
  return true;
}

Installer::Folder* Installer::folder(StringView key) {
  for (const UniquePointer<Folder>& f : folders_.view())
    if (f->key.view().equals(key)) return f.get();
  return nullptr;
}
const Installer::Folder* Installer::folder(StringView key) const {
  for (const UniquePointer<Folder>& f : folders_.view())
    if (f->key.view().equals(key)) return f.get();
  return nullptr;
}
Installer::Component* Installer::component(StringView key) {
  for (const UniquePointer<Component>& c : components_.view())
    if (c->key.view().equals(key)) return c.get();
  return nullptr;
}
const Installer::Component* Installer::component(StringView key) const {
  for (const UniquePointer<Component>& c : components_.view())
    if (c->key.view().equals(key)) return c.get();
  return nullptr;
}

// The place in file_items_ of the file with that key (without regard to case), or the number of files if there is none.
uint64_t Installer::file_index(StringView key) const {
  uint64_t low = 0, high = files_by_key_.count();
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2;
    const int order = compare_folded(file_items_[files_by_key_[middle]]->key.view(), key);
    if (order == 0) return files_by_key_[middle];
    if (order < 0) low = middle + 1;
    else high = middle;
  }
  return file_items_.count();
}

const Installer::FileItem* Installer::file(StringView key) const {
  const uint64_t index = file_index(key);
  return index < file_items_.count() ? file_items_[index].get() : nullptr;
}

bool Installer::installing(StringView component_key) const {
  const Component* c = component(component_key);
  return c && c->wanted && !remove_;
}
bool Installer::removing(StringView component_key) const {
  const Component* c = component(component_key);
  return c && c->wanted && remove_;
}

// --- IExpressionContext

int32_t Installer::state(char kind, StringView name) const {
  bool chosen = false;
  if (kind == '$' || kind == '?') {
    const Component* c = component(name);
    if (!c) return kStateNone;
    chosen = c->wanted;
  } else {
    bool found = false;
    for (const UniquePointer<Feature>& f : features_.view())
      if (f->key.view().equals(name)) found = true, chosen = f->selected;
    if (!found) return kStateNone;
  }
  if (!costed_) return kStateNone;
  const bool action = kind == '$' || kind == '&';
  if (action) return !chosen ? kStateNone : remove_ ? kStateAbsent : kStateLocal;
  return chosen && remove_ ? kStateLocal : kStateAbsent;      // (what is there before the run: all of it when removing, nothing when installing)
}

bool Installer::component_folder(StringView key, String* out) const {
  const Component* c = component(key);
  const Folder* f = c ? folder(c->folder.view()) : nullptr;
  return f && f->resolved && out->assign(f->path.view());
}

bool Installer::file_path(StringView key, String* out) const {
  const FileItem* item = file(key);
  return item && component_folder(item->component.view(), out) && out->append(item->name.view());
}

bool Installer::format(StringView text, String* out) const {
  out->clear();
  return format_text(allocator_, text, *this, out);
}

bool Installer::condition(StringView text) const {
  bool result = false;
  (void)evaluate_condition(text, *this, &result);      // (one that cannot be read is false)
  return result;
}

bool Installer::note(StringView what, StringView name) {
  if (notes_.count() >= kMaxNotes) return true;
  String line(allocator_);
  if (!line.assign(what) || (!name.empty() && (!line.append(": ") || !line.append(name)))) return false;
  for (const String& known : static_cast<const List<String>&>(notes_).view())
    if (known.view().equals(line.view())) return true;
  return notes_.push_back(util::move(line));
}

Outcome Installer::fail(Outcome outcome, StringView what, StringView name, StringView more) {
  (void)(detail_.assign(what) && detail_.append(name) && detail_.append(more));
  return outcome;
}

// --- folders

bool Installer::resolve_folder(Folder& f, uint32_t depth) {
  if (f.resolved) return true;
  if (f.busy || depth > kMaxFolderDepth) return false;
  f.busy = true;
  bool ok = true;
  const StringView given = property(f.key.view());
  if (!given.empty()) {
    // A folder of the system, or one the command line or a custom action placed.
    ok = f.path.assign(given) && (f.path.view().ends_with('\\') || f.path.push_back('\\'));
    f.given = true;
  } else if (f.parent.empty() || f.parent.view().equals(f.key.view())) {
    ok = f.path.assign(property("ROOTDRIVE"));      // the root of the package's tree (TARGETDIR)
  } else {
    Folder* parent = folder(f.parent.view());
    ok = parent && resolve_folder(*parent, depth + 1) && f.path.assign(parent->path.view());
    if (ok && !f.name.empty() && !f.name.view().equals(".")) ok = valid_name(f.name.view()) && f.path.append(f.name.view()) && f.path.push_back('\\');
  }
  f.busy = false;
  // Every folder must be one this system can hold.
  String checked(allocator_);
  f.resolved = ok && system_path(f.path.view(), &checked);
  return f.resolved;
}

bool Installer::resolve_folders() {
  // (a folder that was made of its parent's name is made again, in case the parent moved: its property is this engine's own writing)
  for (const UniquePointer<Folder>& f : folders_.view()) {
    if (f->resolved && !f->given && !set_property(f->key.view(), StringView())) return false;
    f->resolved = false;
  }
  for (const UniquePointer<Folder>& f : folders_.view()) {
    if (!resolve_folder(*f, 0)) return false;
    if (!set_property(f->key.view(), f->path.view())) return false;
  }
  return true;
}

// --- the actions

Outcome Installer::run(const Options& options, String* detail) {
  detail->clear();
  detail_.clear();
  properties_.clear(), folders_.clear(), features_.clear(), components_.clear(), file_items_.clear(), files_by_key_.clear(), notes_.clear(), shortcuts_.clear(), system_folders_.clear();
  remove_ = options.remove;
  undo_ = options.remove && options.undo;
  cancel_ = options.cancel;
  costed_ = false;
  auto done = [&](Outcome outcome) {
    (void)detail->assign(detail_.view());
    return outcome;
  };

  // The processor the package is for: "x64" (also written "AMD64"), "Intel" (32-bit x86, which installs the same way) or none.
  const StringView platform = database_.platform();
  package64_ = equals_folded(platform, "x64") || equals_folded(platform, "AMD64");
  if (!package64_ && !platform.empty() && !equals_folded(platform, "Intel"))
    return done(fail(Outcome::NotSupported, "it is a package for another kind of processor: ", platform));
  if (!database_.table("InstallExecuteSequence")) return done(fail(Outcome::NotSupported, "it has no list of actions to install it with (it is a patch or a merge module, not a product)"));
  if (!load_properties(options) || !load_model()) return done(fail(Outcome::OutOfMemory, "there is not enough memory to read the package"));

  // The actions with a place in the sequence, in its order.
  const Table& sequence = *database_.table("InstallExecuteSequence");
  List<uint32_t> order(allocator_);
  for (uint32_t row = 0; row < sequence.rows(); ++row) {
    const int32_t number = sequence.integer(row, "Sequence");
    if (number == kNull || number <= 0) continue;      // (not run, or run only when the installation ends)
    uint64_t at = order.count();
    while (at > 0 && sequence.integer(order[at - 1], "Sequence") > number) --at;
    if (!order.insert(at, row)) return done(Outcome::OutOfMemory);
  }
  for (const uint32_t row : static_cast<const List<uint32_t>&>(order).view()) {
    if (cancelled()) return done(Outcome::Cancelled);
    if (!condition(sequence.text(row, "Condition"))) continue;
    const Outcome outcome = run_action(sequence.text(row, "Action"));
    if (outcome != Outcome::Ok) return done(outcome);
  }
  if (remove_) unregister_product();
  return done(Outcome::Ok);
}

Outcome Installer::run_action(StringView action) {
  if (action.equals("AppSearch")) return app_search();
  if (action.equals("LaunchConditions")) return launch_conditions();
  if (action.equals("CostFinalize")) return cost_finalize();
  if (action.equals("InstallFiles")) return install_files();
  if (action.equals("CreateFolders")) return create_folders();
  if (action.equals("RemoveFiles")) return remove_files();
  if (action.equals("RemoveFolders")) return remove_folders();
  if (action.equals("WriteRegistryValues")) return write_registry();
  if (action.equals("RemoveRegistryValues")) return remove_registry();
  if (action.equals("CreateShortcuts")) return create_shortcuts();
  if (action.equals("RemoveShortcuts")) return remove_shortcuts();
  if (action.equals("RegisterProduct")) return register_product();
  for (const StringView quiet : kQuietActions)
    if (action.equals(quiet)) return Outcome::Ok;
  for (const auto& known : kTableActions) {
    if (!action.equals(known.action)) continue;
    const Table* table = database_.table(known.table);
    if (!remove_ && table && table->rows() > 0 && !note(known.what)) return Outcome::OutOfMemory;
    return Outcome::Ok;
  }
  if (const Table* custom = database_.table("CustomAction")) {
    const uint32_t row = custom->find("Action", action);
    if (row != kNoColumn) return custom_action(action, row);
  }
  return note("an action this system does not know was skipped", action) ? Outcome::Ok : Outcome::OutOfMemory;
}

// AppSearch: properties set from what is found on the computer. Of the ways to look, the registry's is done (RegLocator).
Outcome Installer::app_search() {
  const Table* search = database_.table("AppSearch");
  const Table* locator = database_.table("RegLocator");
  if (!search || !locator) return Outcome::Ok;
  for (uint32_t row = 0; row < search->rows(); ++row) {
    const StringView name = search->text(row, "Property");
    if (!property(name).empty()) continue;      // (the first signature that finds something wins)
    const uint32_t at = locator->find("Signature_", search->text(row, "Signature_"));
    if (at == kNoColumn) continue;
    const int32_t root = locator->integer(at, "Root");
    String key(allocator_), value_name(allocator_), text(allocator_);
    if (root < 0 || root > 3 || !format(locator->text(at, "Key"), &key) || !format(locator->text(at, "Name"), &value_name)) continue;
    uint32_t type = 0;
    ByteBuffer data(allocator_);
    if (!host_.registry_get(static_cast<RegistryRoot>(root), key.view(), value_name.view(), &type, &data)) continue;
    if (type == kRegDword) {
      // A number is "#" and the number, as a package's own registry values are written.
      if (data.size() < 4 || !text.push_back('#') || !text.append_uint(ByteReader(data.view()).le32())) continue;
    } else if (type == kRegString || type == kRegExpandString) {
      ByteReader in(data.view());
      bool ok = true;
      while (ok && in.remaining() >= 2) {
        const uint16_t unit = in.le16();
        if (unit == 0) break;
        // (as UTF-8; a surrogate pair's halves are kept apart, which only matters for a name outside the basic plane)
        if (unit < 0x80) ok = text.push_back(static_cast<char>(unit));
        else if (unit < 0x800) ok = text.push_back(static_cast<char>(0xC0 | (unit >> 6))) && text.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        else ok = text.push_back(static_cast<char>(0xE0 | (unit >> 12))) && text.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F))) && text.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
      }
      if (!ok) return Outcome::OutOfMemory;
    } else {
      continue;
    }
    if (!set_property(name, text.view())) return Outcome::OutOfMemory;
  }
  return Outcome::Ok;
}

Outcome Installer::launch_conditions() {
  const Table* table = database_.table("LaunchCondition");
  if (!table) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    if (condition(table->text(row, "Condition"))) continue;
    String message(allocator_);
    if (!format(table->text(row, "Description"), &message)) return Outcome::OutOfMemory;
    return fail(Outcome::Refused, "the package says: ", message.view());
  }
  return Outcome::Ok;
}

// Where every folder is, which features are chosen, and which components that brings.
Outcome Installer::cost_finalize() {
  if (!resolve_folders()) return fail(Outcome::Failed, "the package names a folder this system cannot make (outside drive C, or not a name)");
  // A feature is chosen by its level (1 to INSTALLLEVEL; 0 is "never"), which a row of the Condition table changes when its condition holds;
  // ADDLOCAL names the chosen ones itself. A feature under one that is not chosen is not chosen.
  int64_t install_level = 1;
  if (!parse_integer(property("INSTALLLEVEL"), &install_level) || install_level < 1) install_level = 1;
  if (const Table* table = database_.table("Condition")) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      const int32_t level = table->integer(row, "Level");
      if (level == kNull || !condition(table->text(row, "Condition"))) continue;
      for (const UniquePointer<Feature>& f : features_.view())
        if (f->key.view().equals(table->text(row, "Feature_"))) f->level = level;
    }
  }
  const StringView add_local = property("ADDLOCAL");
  for (const UniquePointer<Feature>& f : features_.view()) {
    f->selected = f->level > 0 && f->level <= install_level;
    if (!add_local.empty() && !add_local.equals("ALL")) {
      f->selected = false;
      StringView rest = add_local, one;
      while (rest.next_token(',', &one)) f->selected |= f->level > 0 && one.trimmed().equals(f->key.view());
    } else if (add_local.equals("ALL")) {
      f->selected = f->level > 0;
    }
  }
  for (uint32_t pass = 0; pass < kMaxFolderDepth; ++pass) {
    bool changed = false;
    for (const UniquePointer<Feature>& f : features_.view()) {
      if (!f->selected || f->parent.empty()) continue;
      for (const UniquePointer<Feature>& parent : features_.view())
        if (parent->key.view().equals(f->parent.view()) && !parent->selected) f->selected = false, changed = true;
    }
    if (!changed) break;
  }
  costed_ = true;
  if (const Table* table = database_.table("FeatureComponents")) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      bool chosen = false;
      for (const UniquePointer<Feature>& f : features_.view())
        if (f->selected && f->key.view().equals(table->text(row, "Feature_"))) chosen = true;
      Component* c = chosen ? component(table->text(row, "Component_")) : nullptr;
      if (c) c->wanted = true;
    }
  }
  // (Conditions of components may ask about features, so they come after; when removing, what was installed is not known better than "all of it".)
  for (const UniquePointer<Component>& c : components_.view())
    if (c->wanted && !remove_ && !condition(c->condition.view())) c->wanted = false;
  return Outcome::Ok;
}

namespace {

// Writes the files of a cabinet that the installation wants, each into its place, as the cabinet is unpacked.
class FileSink final : public media::ICabinetSink {
 public:
  struct Wanted {
    explicit Wanted(allocators::IAllocator& allocator) : key(allocator), path(allocator) {}
    Wanted(Wanted&&) = default;
    Wanted& operator=(Wanted&&) = default;
    String key;       // the file's key: its name in the cabinet
    String path;      // where it goes, a path of the OS file system
    uint64_t size = 0;
    bool placed = false;
    bool size_matches = false;
  };

  FileSink(allocators::IAllocator& allocator, filesystem::IFileSystem& files, List<Wanted>& wanted, const bool* cancel)
      : allocator_(allocator), files_(files), wanted_(wanted), cancel_(cancel) {}

  bool begin_file(const media::CabinetEntry& entry, bool* wanted) override {
    *wanted = false;
    current_ = nullptr;
    if (cancel_ && __atomic_load_n(cancel_, __ATOMIC_RELAXED)) return false;
    Wanted* w = find(entry);
    if (!w) return true;
    if (!w->size_matches) return false;
    if (!make_folders(files_, parent_of(w->path.view())) || files_.open_file(w->path.view(), filesystem::OpenMode::CreateTruncate, &file_) != Status::Ok) return file_failed(*w);
    current_ = w;
    offset_ = 0;
    *wanted = true;
    return true;
  }
  bool write(ConstByteView bytes) override {
    if (!current_ || !file_ || bytes.size() > current_->size - offset_ || !write_all(*file_, offset_, bytes)) {
      file_error = current_ != nullptr;
      if (current_) failed_path = current_->path.view();
      return false;
    }
    offset_ += bytes.size();
    return true;
  }
  bool end_file() override {
    if (!current_ || !file_) return true;
    const bool ok = offset_ == current_->size && file_->flush() == Status::Ok;
    file_.reset();
    current_->placed = ok;
    last_ = ok ? current_ : nullptr;
    current_ = nullptr;
    return ok;
  }
  // The same bytes as the file written last, under another key (packers keep one copy of files that are identical): a copy of that file.
  bool same_file(const media::CabinetEntry& entry, const media::CabinetEntry&) override {
    if (cancel_ && __atomic_load_n(cancel_, __ATOMIC_RELAXED)) return false;
    Wanted* w = find(entry);
    if (!w) return true;
    if (!w->size_matches || !last_ || last_->size != w->size) return false;
    UniquePointer<filesystem::IFile> from, to;
    ByteBuffer block(allocator_);
    if (!block.put_zeros(64 * 1024)) return false;
    if (files_.open_file(last_->path.view(), filesystem::OpenMode::Read, &from) != Status::Ok || !make_folders(files_, parent_of(w->path.view())) ||
        files_.open_file(w->path.view(), filesystem::OpenMode::CreateTruncate, &to) != Status::Ok)
      return file_failed(*w);
    for (uint64_t at = 0; at < w->size;) {
      const uint64_t piece = w->size - at < block.size() ? w->size - at : block.size();
      uint64_t got = 0;
      if (from->read(at, block.writable_view().first(piece), &got) != Status::Ok || got != piece || !write_all(*to, at, block.view().first(piece))) return file_failed(*w);
      at += piece;
    }
    if (to->flush() != Status::Ok) return file_failed(*w);
    w->placed = true;
    return true;
  }

  bool file_error = false;      // the stop was a file that could not be written (not the cabinet)
  StringView failed_path;

 private:
  // The wanted file a cabinet's entry is, if it is one that is still to be placed. (They are in the order of their keys.)
  Wanted* find(const media::CabinetEntry& entry) {
    uint64_t low = 0, high = wanted_.count();
    while (low < high) {
      const uint64_t middle = low + (high - low) / 2;
      if (compare_folded(wanted_[middle].key.view(), entry.name.view()) < 0) low = middle + 1;
      else high = middle;
    }
    if (low >= wanted_.count()) return nullptr;
    Wanted& w = wanted_[low];
    if (w.placed || !equals_folded(w.key.view(), entry.name.view())) return nullptr;
    // (the size the package's table gives is the one that must arrive: a cabinet that says otherwise is not this package's)
    w.size_matches = entry.size == w.size;
    return &w;
  }
  bool file_failed(const Wanted& w) {
    file_error = true;
    failed_path = w.path.view();
    return false;
  }

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  List<Wanted>& wanted_;
  const bool* cancel_;
  Wanted* current_ = nullptr;
  Wanted* last_ = nullptr;      // the file that was written last, whole
  UniquePointer<filesystem::IFile> file_;
  uint64_t offset_ = 0;
};

}  // namespace

// The files of the components being installed whose sequence numbers are first..last, out of the cabinet stream `cabinet`.
Outcome Installer::extract_cabinet(StringView cabinet, uint32_t first_sequence, uint32_t last_sequence) {
  List<FileSink::Wanted> wanted(allocator_);
  for (const uint32_t index : static_cast<const List<uint32_t>&>(files_by_key_).view()) {      // (in the order of their keys, which is how the sink looks them up)
    const FileItem* item = file_items_[index].get();
    if (item->sequence < first_sequence || item->sequence > last_sequence || item->placed || !installing(item->component.view())) continue;
    FileSink::Wanted w(allocator_);
    String windows(allocator_);
    if (!valid_name(item->name.view()) || !file_path(item->key.view(), &windows) || !system_path(windows.view(), &w.path))
      return fail(Outcome::Failed, "the package names a file this system cannot hold: ", item->name.view());
    if (item->size > kMaxFileBytes) return fail(Outcome::NotSupported, "it has a file larger than this system installs: ", item->name.view());
    w.size = item->size;
    if (!w.key.assign(item->key.view()) || !wanted.push_back(util::move(w))) return Outcome::OutOfMemory;
  }
  if (wanted.count() == 0) return Outcome::Ok;
  if (cabinet.empty()) return fail(Outcome::NotSupported, "its files are not inside the package but beside it, where a download has nothing");
  if (!cabinet.starts_with('#')) return fail(Outcome::NotSupported, "its files are in a cabinet beside the package, which a download does not have: ", cabinet);
  media::CompoundStream stream(allocator_);
  if (!database_.open_stream(cabinet.without_prefix(1), &stream)) return fail(Outcome::Failed, "the package is damaged: it does not hold its cabinet ", cabinet.without_prefix(1));
  media::CabinetReader reader(allocator_, stream);
  FileSink sink(allocator_, files_, wanted, cancel_);
  if (!reader.open() || !reader.extract_all(sink)) {
    if (cancelled()) return Outcome::Cancelled;
    if (sink.file_error) return fail(Outcome::FileError, "a file could not be written: ", sink.failed_path);
    return fail(Outcome::Failed, "the package is damaged, or packed in a way this system cannot unpack: its cabinet ", cabinet.without_prefix(1));
  }
  for (const FileSink::Wanted& w : static_cast<const List<FileSink::Wanted>&>(wanted).view()) {
    if (!w.placed) return fail(Outcome::Failed, "the package is damaged: its cabinet does not hold the file ", w.key.view());
    const uint64_t index = file_index(w.key.view());
    if (index < file_items_.count()) file_items_[index]->placed = true;
  }
  return Outcome::Ok;
}

// InstallFiles: the Media table says which cabinet holds the files up to which sequence number.
Outcome Installer::install_files() {
  if (remove_) return Outcome::Ok;
  if (!costed_) return fail(Outcome::Failed, "the package is damaged: it installs files before it has worked out where they go");
  for (const UniquePointer<FileItem>& item : file_items_.view())
    if (installing(item->component.view()) && (static_cast<uint32_t>(item->attributes) & kFileNoncompressed) != 0)
      return fail(Outcome::NotSupported, "its files are not inside the package but beside it, where a download has nothing");
  const Table* media = database_.table("Media");
  if (!media) {
    for (const UniquePointer<FileItem>& item : file_items_.view())
      if (installing(item->component.view())) return fail(Outcome::Failed, "the package is damaged: it has files and no list of where they are kept");
    return Outcome::Ok;
  }
  // The disks in the order of their last sequence numbers.
  List<uint32_t> disks(allocator_);
  for (uint32_t row = 0; row < media->rows(); ++row) {
    uint64_t at = disks.count();
    while (at > 0 && media->integer(disks[at - 1], "LastSequence") > media->integer(row, "LastSequence")) --at;
    if (!disks.insert(at, row)) return Outcome::OutOfMemory;
  }
  uint32_t first = 0;
  for (const uint32_t row : static_cast<const List<uint32_t>&>(disks).view()) {
    const int32_t last = media->integer(row, "LastSequence");
    if (last == kNull || last < 0) continue;
    const Outcome outcome = extract_cabinet(media->text(row, "Cabinet"), first, static_cast<uint32_t>(last));
    if (outcome != Outcome::Ok) return outcome;
    first = static_cast<uint32_t>(last) + 1;
  }
  for (const UniquePointer<FileItem>& item : file_items_.view())
    if (installing(item->component.view()) && !item->placed) return fail(Outcome::Failed, "the package is damaged: none of its cabinets holds the file ", item->name.view());
  return Outcome::Ok;
}

Outcome Installer::create_folders() {
  const Table* table = database_.table("CreateFolder");
  if (!table || remove_) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    if (!installing(table->text(row, "Component_"))) continue;
    const Folder* f = folder(table->text(row, "Directory_"));
    String path(allocator_);
    if (!f || !f->resolved || !system_path(f->path.view(), &path)) continue;
    if (!make_folders(files_, path.view())) return fail(Outcome::FileError, "a folder could not be made: ", path.view());
  }
  return Outcome::Ok;
}

// RemoveFiles: the files of the components that go, and what the RemoveFile table names for this kind of run (1: when its component is installed,
// 2: when it is removed, 3: both).
Outcome Installer::remove_files() {
  if (!costed_) return Outcome::Ok;
  if (remove_) {
    for (const UniquePointer<FileItem>& item : file_items_.view()) {
      String windows(allocator_), path(allocator_);
      if (!removing(item->component.view()) || !file_path(item->key.view(), &windows) || !system_path(windows.view(), &path)) continue;
      (void)files_.remove(path.view());
    }
  }
  const Table* table = database_.table("RemoveFile");
  for (uint32_t row = 0; table && row < table->rows(); ++row) {
    const int32_t mode = table->integer(row, "InstallMode");
    const StringView owner = table->text(row, "Component_");
    if (mode == kNull || !((installing(owner) && (mode & 1)) || (removing(owner) && (mode & 2)))) continue;
    // DirProperty: a folder of the Directory table, or a property that holds a path.
    String windows(allocator_), path(allocator_);
    const Folder* f = folder(table->text(row, "DirProperty"));
    if (!windows.assign(f && f->resolved ? f->path.view() : property(table->text(row, "DirProperty"))) || windows.empty() || !system_path(windows.view(), &path)) continue;
    const StringView pattern = long_name(table->text(row, "FileName"));
    if (pattern.empty()) {      // (the folder itself, if nothing is left in it)
      (void)files_.remove(path.view());
      continue;
    }
    List<String> names(allocator_);
    {
      UniquePointer<filesystem::IDirectory> listing;
      if (files_.open_directory(path.view(), &listing) != Status::Ok) continue;
      filesystem::DirectoryEntry entry;
      for (uint32_t guard = 0; guard < 100000 && listing->next(&entry) == Status::Ok; ++guard) {
        if (entry.type == filesystem::NodeType::Directory || !matches_wildcard(entry.name.view(), pattern)) continue;
        String full(allocator_);
        if (!full.assign(path.view()) || !full.push_back('/') || !full.append(entry.name.view()) || !names.push_back(util::move(full))) return Outcome::OutOfMemory;
      }
    }
    for (const String& name : static_cast<const List<String>&>(names).view()) (void)files_.remove(name.view());
  }
  return Outcome::Ok;
}

// RemoveFolders: the folders of the package's Directory table, where they are empty now - the deepest first. A folder of the system (Program Files,
// the Start menu, ...) or the drive's root is never the package's to take away, whatever name the package gives it.
Outcome Installer::remove_folders() {
  if (!remove_ || !costed_) return Outcome::Ok;
  for (uint32_t pass = 0; pass < kMaxFolderDepth; ++pass) {
    bool removed = false;
    for (const UniquePointer<Folder>& f : folders_.view()) {
      String path(allocator_);
      bool system = !f->resolved || f->path.size() <= 3;
      for (const String& known : static_cast<const List<String>&>(system_folders_).view()) system |= equals_folded(known.view(), f->path.view());
      if (system || !system_path(f->path.view(), &path)) continue;
      // (a folder with something in it is not removed by the file system)
      if (exists(files_, path.view()) && files_.remove(path.view()) == Status::Ok) removed = true;
    }
    if (!removed) break;
  }
  return Outcome::Ok;
}

// Where a row of the Registry (or RemoveRegistry) table points: the root (-1: the machine's, as every installation here is for all users), the key and
// the value's name.
bool Installer::registry_place(const Table& table, uint32_t row, RegistryRoot* root, String* key, String* name) const {
  const int32_t r = table.integer(row, "Root");
  if (r == kNull || r < -1 || r > 3) return false;
  *root = r == -1 ? RegistryRoot::LocalMachine : static_cast<RegistryRoot>(r);
  return format(table.text(row, "Key"), key) && format(table.text(row, "Name"), name) && !key->empty();
}

// WriteRegistryValues. A value is text, unless it starts with '#': "#x" and hexadecimal digits (bytes), "#%" (text with %variables%), "#" and a number,
// "##" (text that starts with '#'). "[~]" inside makes it a list of strings. With no value, the names "+" and "*" just make the key.
Outcome Installer::write_registry() {
  const Table* table = database_.table("Registry");
  if (!table || remove_) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    if (!installing(table->text(row, "Component_"))) continue;
    RegistryRoot root = RegistryRoot::LocalMachine;
    String key(allocator_), name(allocator_), value(allocator_);
    if (!registry_place(*table, row, &root, &key, &name) || !format(table->text(row, "Value"), &value)) continue;
    const StringView raw = table->text(row, "Value");
    ByteBuffer data(allocator_);
    uint32_t type = kRegString;
    StringView text = value.view();
    if (raw.empty()) {
      if (name.view().equals("-")) continue;      // (a key to take away when the product is removed)
      if (name.view().equals("+") || name.view().equals("*")) {
        // (the key itself: made by giving it an empty default value, unless it has one)
        name.clear();
        uint32_t old_type = 0;
        ByteBuffer old(allocator_);
        if (host_.registry_get(root, key.view(), "", &old_type, &old)) continue;
      }
      text = StringView();
    } else if (text.starts_with("##")) {
      text = text.without_prefix(1);
    } else if (text.starts_with("#x") || text.starts_with("#X")) {
      type = kRegBinary;
      uint32_t pending = 0, digits = 0;
      for (const char c : text.without_prefix(2)) {
        const uint32_t digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit == 16) break;
        pending = pending * 16 + digit;
        if (++digits == 2) {
          if (!data.put8(static_cast<uint8_t>(pending))) return Outcome::OutOfMemory;
          pending = 0, digits = 0;
        }
      }
    } else if (text.starts_with("#%")) {
      type = kRegExpandString;
      text = text.without_prefix(2);
    } else if (text.starts_with('#')) {
      int64_t number = 0;
      if (!parse_integer(text.without_prefix(1).trimmed(), &number)) number = 0;
      type = kRegDword;
      if (!data.put32(static_cast<uint32_t>(number))) return Outcome::OutOfMemory;
    }
    if (type == kRegString || type == kRegExpandString) {
      // "[~]" separates the strings of a list (REG_MULTI_SZ), which ends with one more terminator.
      static constexpr StringView kSeparator = "[~]";
      if (!text.contains(kSeparator)) {
        if (!util::append_utf16le(&data, text) || !data.put16(0)) return Outcome::OutOfMemory;
      } else {
        type = kRegMultiString;
        for (StringView rest = text;;) {
          const uint64_t at = rest.find(kSeparator);
          const StringView piece = at == StringView::npos ? rest : rest.first(at);
          if (!piece.empty() && (!util::append_utf16le(&data, piece) || !data.put16(0))) return Outcome::OutOfMemory;
          if (at == StringView::npos) break;
          rest = rest.substr(at + kSeparator.size());
        }
        if (!data.put16(0)) return Outcome::OutOfMemory;
      }
    }
    if (!host_.registry_set(root, key.view(), name.view(), type, data.view())) return fail(Outcome::Failed, "a value could not be written to the registry: ", key.view());
  }
  return Outcome::Ok;
}

// RemoveRegistryValues: what the package wrote goes with its component (and a key that is empty afterwards goes too); the RemoveRegistry table
// names values to delete when its component is installed.
Outcome Installer::remove_registry() {
  if (const Table* table = database_.table("RemoveRegistry"); table && !remove_) {
    for (uint32_t row = 0; row < table->rows(); ++row) {
      RegistryRoot root = RegistryRoot::LocalMachine;
      String key(allocator_), name(allocator_);
      if (!installing(table->text(row, "Component_")) || !registry_place(*table, row, &root, &key, &name)) continue;
      if (name.view().equals("-")) host_.registry_delete_key(root, key.view(), false);
      else host_.registry_delete_value(root, key.view(), name.view());
    }
  }
  const Table* table = database_.table("Registry");
  if (!table || !remove_) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    RegistryRoot root = RegistryRoot::LocalMachine;
    String key(allocator_), name(allocator_);
    if (!removing(table->text(row, "Component_")) || !registry_place(*table, row, &root, &key, &name)) continue;
    const bool no_value = table->text(row, "Value").empty();
    if (no_value && (name.view().equals("-") || name.view().equals("*"))) {
      host_.registry_delete_key(root, key.view(), false);      // (the key and everything under it)
      continue;
    }
    if (no_value && name.view().equals("+")) continue;      // (a key that was only made stays)
    host_.registry_delete_value(root, key.view(), name.view());
    host_.registry_delete_key(root, key.view(), true);
  }
  return Outcome::Ok;
}

// Where a row of the Shortcut table puts its file: the folder of its Directory_, its long name and ".lnk", as a path of the OS file system.
bool Installer::shortcut_path(const Table& table, uint32_t row, String* link) const {
  const Folder* f = folder(table.text(row, "Directory_"));
  const StringView name = long_name(table.text(row, "Name"));
  String windows(allocator_);
  return f && f->resolved && valid_name(name) && windows.assign(f->path.view()) && windows.append(name) && windows.append(".lnk") && system_path(windows.view(), link);
}

// CreateShortcuts. The target is formatted text, or the name of a feature: then the shortcut is to the key file of its component.
Outcome Installer::create_shortcuts() {
  const Table* table = database_.table("Shortcut");
  if (!table || remove_) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    const StringView owner = table->text(row, "Component_");
    if (!installing(owner)) continue;
    String link(allocator_), target(allocator_), working(allocator_);
    if (!shortcut_path(*table, row, &link)) continue;
    const StringView raw = table->text(row, "Target");
    bool feature = false;
    for (const UniquePointer<Feature>& f : features_.view()) feature |= f->key.view().equals(raw);
    if (feature) {
      const Component* c = component(owner);
      if (!c || !file_path(c->key_path.view(), &target)) continue;
    } else if (!format(raw, &target)) {
      return Outcome::OutOfMemory;
    }
    String checked(allocator_);
    if (target.empty() || !system_path(target.view(), &checked)) continue;      // (a shortcut to something that is not a file on this disk)
    // Started in the folder WkDir names (a folder of the package, or a property), else the target's own.
    const StringView wk = table->text(row, "WkDir");
    const Folder* f = folder(wk);
    if (!working.assign(f && f->resolved ? f->path.view() : property(wk))) return Outcome::OutOfMemory;
    if (working.empty()) {
      const uint64_t slash = target.view().rfind('\\');
      if (slash == StringView::npos || !working.assign(target.view().first(slash))) continue;
    }
    while (working.size() > 3 && working.view().ends_with('\\')) working.truncate(working.size() - 1);
    if (!make_folders(files_, parent_of(link.view())) || !host_.write_shortcut(link.view(), target.view(), working.view()))
      return fail(Outcome::FileError, "a shortcut could not be written: ", link.view());
    if (!shortcuts_.push_back(util::move(link))) return Outcome::OutOfMemory;
  }
  return Outcome::Ok;
}

Outcome Installer::remove_shortcuts() {
  const Table* table = database_.table("Shortcut");
  if (!table || !remove_) return Outcome::Ok;
  for (uint32_t row = 0; row < table->rows(); ++row) {
    String link(allocator_);
    if (removing(table->text(row, "Component_")) && shortcut_path(*table, row, &link)) (void)files_.remove(link.view());
  }
  return Outcome::Ok;
}

// RegisterProduct: the entry in the list of installed programs, under the product's code. (This system's "msiexec" is the App Manager, which keeps
// the package and removes the product with it; the command is the one Windows would have.)
Outcome Installer::register_product() {
  if (remove_) return Outcome::Ok;
  const StringView code = property("ProductCode");
  if (code.empty()) return Outcome::Ok;
  String key(allocator_), uninstall(allocator_);
  if (!key.assign("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") || !key.append(code) || !uninstall.assign("MsiExec.exe /X") || !uninstall.append(code)) return Outcome::OutOfMemory;
  auto text = [&](StringView name, StringView value) {
    ByteBuffer data(allocator_);
    return value.empty() || (util::append_utf16le(&data, value) && data.put16(0) && host_.registry_set(RegistryRoot::LocalMachine, key.view(), name, kRegString, data.view()));
  };
  auto number = [&](StringView name, uint32_t value) {
    ByteBuffer data(allocator_);
    return data.put32(value) && host_.registry_set(RegistryRoot::LocalMachine, key.view(), name, kRegDword, data.view());
  };
  const bool ok = text("DisplayName", property("ProductName")) && text("DisplayVersion", property("ProductVersion")) && text("Publisher", property("Manufacturer")) &&
                  text("UninstallString", uninstall.view()) && text("InstallLocation", property("ARPINSTALLLOCATION")) && text("URLInfoAbout", property("ARPURLINFOABOUT")) &&
                  text("HelpLink", property("ARPHELPLINK")) && number("WindowsInstaller", 1) && (property("ARPSYSTEMCOMPONENT").empty() || number("SystemComponent", 1));
  // (a system with no registry yet has no list of installed programs to be in)
  return ok || note("the product is not in the registry's list of installed programs") ? Outcome::Ok : Outcome::OutOfMemory;
}

void Installer::unregister_product() {
  const StringView code = property("ProductCode");
  String key(allocator_);
  if (!code.empty() && key.assign("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") && key.append(code)) host_.registry_delete_key(RegistryRoot::LocalMachine, key.view(), false);
}

// --- custom actions

Outcome Installer::run_program(StringView action, StringView program, StringView arguments, StringView working_folder, bool ignore_result) {
  uint32_t code = 0;
  String message(allocator_);
  if (!host_.run(program, arguments, working_folder, &code, &message)) {
    if (cancelled()) return Outcome::Cancelled;
    if (ignore_result) return note("a program of the package could not be run", action) ? Outcome::Ok : Outcome::OutOfMemory;
    (void)(detail_.assign("the program of its step ") && detail_.append(action) && detail_.append(" could not be run: ") && detail_.append(message.view()));
    return Outcome::Failed;
  }
  if (code != 0 && !ignore_result) {
    (void)(detail_.assign("the program of its step ") && detail_.append(action) && detail_.append(" ended with code ") && detail_.append_uint(code));
    return Outcome::Failed;
  }
  return Outcome::Ok;
}

// A custom action: its type says what it is (the low bits: 1 a DLL, 2 a program, 3 text, 5 and 6 scripts), where it comes from (0x00 the Binary
// table, 0x10 an installed file, 0x20 a folder, 0x30 a property) and how it is run (0x40: its result does not matter; 0x400 with 0x100: it undoes a
// failed installation, which this engine never rolls back).
Outcome Installer::custom_action(StringView action, uint32_t row) {
  const Table& table = *database_.table("CustomAction");
  const int32_t raw_type = table.integer(row, "Type");
  if (raw_type == kNull) return Outcome::Ok;
  const uint32_t type = static_cast<uint32_t>(raw_type), kind = type & 0x07, source_kind = type & 0x30;
  const bool ignore_result = (type & 0x40) != 0;
  const StringView source = table.text(row, "Source");
  if ((type & 0x400) != 0 && (type & 0x100) != 0) return Outcome::Ok;      // a rollback action
  String target(allocator_);
  if (!format(table.text(row, "Target"), &target)) return Outcome::OutOfMemory;

  if (kind == 3) {
    if (source_kind == 0x30) return set_property(source, target.view()) ? Outcome::Ok : Outcome::OutOfMemory;      // type 51: set a property
    if (source_kind == 0x20) {      // type 35: place a folder
      Folder* f = folder(source);
      if (!f) return Outcome::Ok;
      f->given = true;      // (from here on it is where the action put it, not where its parent is)
      if (!set_property(source, target.view())) return Outcome::OutOfMemory;
      if (costed_ && !resolve_folders()) return fail(Outcome::Failed, "the package places a folder where this system cannot make one: ", target.view());
      return Outcome::Ok;
    }
    if (source_kind == 0x10) {      // type 19: stop with a message (a number is a row of the Error table)
      int64_t number = 0;
      String message(allocator_);
      const Table* errors = database_.table("Error");
      if (parse_integer(target.view().trimmed(), &number) && errors) {
        for (uint32_t e = 0; e < errors->rows(); ++e)
          if (errors->integer(e, "Error") == number && !format(errors->text(e, "Message"), &message)) return Outcome::OutOfMemory;
      }
      return fail(Outcome::Refused, "the package says: ", message.empty() ? target.view() : message.view());
    }
    return Outcome::Ok;
  }
  if (kind == 2) {
    if (undo_) return Outcome::Ok;      // (the product was never whole: there is nothing of it to tell that it goes)
    String program(allocator_), arguments(allocator_), working(allocator_);
    if (source_kind == 0x00) {
      // Type 2: a program kept in the package. It is unpacked, run and deleted again.
      // (into the folder Windows Installer keeps its own files in, under a name made of the action's)
      String stream(allocator_), temp(allocator_);
      if (!stream.assign("Binary.") || !stream.append(source) || !temp.assign(filesystem::layout::kWindowsInstallerCache) || !program.assign(temp.view()) || !program.append("/MSI"))
        return Outcome::OutOfMemory;
      for (const char c : action)
        if (!program.push_back((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_')) return Outcome::OutOfMemory;
      if (!program.append(".exe")) return Outcome::OutOfMemory;
      ByteBuffer bytes(allocator_);
      if (!database_.read_stream(stream.view(), kMaxActionProgramBytes, &bytes)) return fail(Outcome::Failed, "the package is damaged: it does not hold the program of its step ", action);
      UniquePointer<filesystem::IFile> file;
      if (!make_folders(files_, temp.view()) || files_.open_file(program.view(), filesystem::OpenMode::CreateTruncate, &file) != Status::Ok || !write_all(*file, 0, bytes.view()) ||
          file->flush() != Status::Ok)
        return fail(Outcome::FileError, "a file could not be written: ", program.view());
      file.reset();
      bytes.clear();
      const Outcome outcome = run_program(action, program.view(), target.view(), temp.view(), ignore_result);
      (void)files_.remove(program.view());
      return outcome;
    }
    if (source_kind == 0x10) {      // type 18: one of the installed files
      String windows(allocator_);
      if (!file_path(source, &windows) || !system_path(windows.view(), &program)) return ignore_result ? Outcome::Ok : fail(Outcome::Failed, "the package is damaged: its step runs a file it does not have: ", action);
      if (!exists(files_, program.view())) {
        // (when the product is removed and its files are gone already, or its component was not chosen)
        return note("a program of the package was not there to run", action) ? Outcome::Ok : Outcome::OutOfMemory;
      }
      return run_program(action, program.view(), target.view(), StringView(), ignore_result);
    }
    // Types 50 (a property holds the program's path; the target is its arguments) and 34 (the target is a whole command line, run in a folder).
    StringView path, rest;
    if (source_kind == 0x30) {
      path = property(source);
      rest = target.view();
      if (path.size() >= 2 && path.starts_with('"') && path.ends_with('"')) path = path.substr(1, path.size() - 2);
    } else {
      const StringView line = target.view().trimmed();
      if (line.starts_with('"')) {
        const uint64_t close = line.find('"', 1);
        if (close == StringView::npos) return ignore_result ? Outcome::Ok : fail(Outcome::Failed, "the package is damaged: the command of its step cannot be read: ", action);
        path = line.substr(1, close - 1);
        rest = line.substr(close + 1).trimmed();
      } else {
        uint64_t end = line.find(' ');
        for (uint64_t at = 0; at + 4 <= line.size(); ++at) {
          if (!equals_folded(line.substr(at, 4), ".exe") || (at + 4 < line.size() && line[at + 4] != ' ')) continue;
          end = at + 4;
          break;
        }
        path = end == StringView::npos ? line : line.first(end);
        rest = end == StringView::npos ? StringView() : line.substr(end).trimmed();
      }
      const Folder* f = folder(source);
      if (f && f->resolved && !system_path(f->path.view(), &working)) working.clear();
    }
    if (!system_path(path, &program) || !exists(files_, program.view())) {
      if (ignore_result || remove_) return note("a program of the package was not there to run", action) ? Outcome::Ok : Outcome::OutOfMemory;
      return fail(Outcome::Failed, "its step runs a program that is not on this computer's disk: ", action);
    }
    return run_program(action, program.view(), rest, working.view(), ignore_result);
  }
  // DLLs and scripts work through the installer's own programming interface from inside its process, which this engine is not.
  if (remove_) return Outcome::Ok;
  return note(kind == 1 ? "a step of the package that is a library (DLL) was skipped" : "a step of the package that is a script was skipped", action) ? Outcome::Ok : Outcome::OutOfMemory;
}

}  // namespace msi
