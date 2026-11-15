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

#include "Desktop/AppMan/WingetInstaller.hpp"
#include "Crypto/Crypto.hpp"
#include "Desktop/AppMan/InstallFiles.hpp"
#include "Desktop/AppMan/Yaml.hpp"
#include "Desktop/InstalledApps.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Media/Zip.hpp"
#include "Media/Zlib.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

using filesystem::Status;

constexpr uint64_t kMaxRecordBytes = 1u << 20;
constexpr uint32_t kMszipBlock = 32768;
constexpr uint64_t kMaxShortcuts = 256;

bool equals_folded(StringView a, StringView b) { return compare_folded(a, b) == 0; }

bool append_hex(String* out, ConstByteView bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const uint8_t byte : bytes)
    if (!out->push_back(kHex[byte >> 4]) || !out->push_back(kHex[byte & 15])) return false;
  return true;
}

// `text` as part of a web address: everything but letters, digits and - . _ ~ (and '/', if it is kept) as %XX.
bool append_escaped(String* out, StringView text, bool keep_slash) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || (keep_slash && c == '/');
    const uint8_t byte = static_cast<uint8_t>(c);
    if (plain ? !out->push_back(c) : (!out->push_back('%') || !out->push_back(kHex[byte >> 4]) || !out->push_back(kHex[byte & 15]))) return false;
  }
  return true;
}

// Is the SHA-256 of `data` the one `hex` spells (any case)?
bool sha256_is(ConstByteView data, StringView hex, allocators::IAllocator& allocator) {
  uint8_t hash[32];
  crypto::hash(crypto::HashKind::Sha256, data, hash);
  String text(allocator);
  return append_hex(&text, ConstByteView(hash)) && equals_folded(text.view(), hex.trimmed());
}

bool web_address(StringView url) {
  if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
  for (const char c : url)
    if (static_cast<uint8_t>(c) <= ' ' || c == '"' || c == '\'' || c == 0x7F) return false;
  return true;
}

// A name for a file or a shortcut out of `text`: what Windows does not allow in a name becomes '_'. "" if nothing is left.
bool safe_name(StringView text, String* out) {
  out->clear();
  text = text.trimmed();
  if (text.size() > 100) text = text.first(100);
  for (const char c : text) {
    const bool bad = static_cast<uint8_t>(c) < 0x20 || c == 0x7F || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
    if (!out->push_back(bad ? '_' : c)) return false;
  }
  while (!out->empty() && ((*out)[out->size() - 1] == '.' || (*out)[out->size() - 1] == ' ')) out->truncate(out->size() - 1);
  if (out->view().starts_with('.')) out->clear();
  return true;
}

StringView parent_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? StringView() : path.first(slash);
}
StringView leaf_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? path : path.substr(slash + 1);
}
// "tool.exe" -> "tool"
StringView stem_of(StringView name) {
  const uint64_t dot = name.rfind('.');
  return dot == StringView::npos || dot == 0 ? name : name.first(dot);
}

// "/Windows/Program Files/x/y.exe" -> "C:\Program Files\x\y.exe"
bool windows_path(StringView system_path, String* out) {
  const StringView root(filesystem::layout::kWindows);
  if (!system_path.starts_with(root) || !out->assign("C:")) return false;
  for (const char c : system_path.substr(root.size()))
    if (!out->push_back(c == '/' ? '\\' : c)) return false;
  return true;
}

bool write_bytes(filesystem::IFileSystem& files, StringView path, ConstByteView bytes) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) return false;
  uint64_t at = 0;
  while (at < bytes.size()) {
    uint64_t done = 0;
    if (file->write(at, bytes.subview(at), &done) != Status::Ok || done == 0) return false;
    at += done;
  }
  return file->flush() == Status::Ok;
}

bool exists(filesystem::IFileSystem& files, StringView path) {
  filesystem::DirectoryEntry entry;
  return files.stat(path, &entry) == Status::Ok;
}

// The archive on the disk, read piece by piece.
class FileZipSource final : public media::IZipSource {
 public:
  explicit FileZipSource(filesystem::IFile& file) : file_(file) {}
  uint64_t size() const override { return file_.size(); }
  [[nodiscard]] bool read(uint64_t offset, ByteView out) override {
    uint64_t at = 0;
    while (at < out.size()) {
      uint64_t got = 0;
      if (file_.read(offset + at, out.subview(at), &got) != Status::Ok || got == 0) return false;
      at += got;
    }
    return true;
  }

 private:
  filesystem::IFile& file_;
};

// What the manifest calls the kinds of installer, for the message that says one cannot be installed.
StringView kind_name(StringView type) {
  if (equals_folded(type, "inno")) return "an Inno Setup program";
  if (equals_folded(type, "nullsoft")) return "an NSIS setup program";
  if (equals_folded(type, "msi") || equals_folded(type, "wix")) return "a Windows Installer package (.msi)";
  if (equals_folded(type, "msix") || equals_folded(type, "appx")) return "an MSIX package";
  if (equals_folded(type, "burn")) return "a WiX Burn setup program";
  if (equals_folded(type, "exe")) return "a setup program";
  if (equals_folded(type, "msstore")) return "a Microsoft Store application";
  if (equals_folded(type, "font")) return "a font";
  if (equals_folded(type, "zip")) return "an archive with a setup program in it";
  return "a setup program";
}

bool can_install(const WingetInstallerEntry& entry) {
  if (!web_address(entry.url.view()) || entry.sha256.size() != 64) return false;
  if (equals_folded(entry.type.view(), "portable")) return true;
  return equals_folded(entry.type.view(), "zip") && equals_folded(entry.nested_type.view(), "portable") && entry.nested.count() > 0;
}

// 0 for the architecture this computer runs best, higher for worse, -1 for one it cannot run.
int architecture_rank(StringView architecture) {
  if (equals_folded(architecture, "x64")) return 0;
  if (equals_folded(architecture, "neutral")) return 1;
  if (equals_folded(architecture, "x86")) return 2;
  return -1;
}

}  // namespace

struct WingetInstaller::Program {
  explicit Program(allocators::IAllocator& allocator) : path(allocator), label(allocator) {}
  String path;       // in the package's folder
  String label;      // of its shortcut
};

bool WingetInstaller::safe_member_path(StringView raw, String* out) {
  out->clear();
  if (raw.empty() || raw.size() > 1024) return false;
  StringView part;
  uint64_t start = 0;
  for (uint64_t i = 0; i <= raw.size(); ++i) {
    if (i < raw.size() && raw[i] != '/' && raw[i] != '\\') continue;
    part = raw.substr(start, i - start);
    start = i + 1;
    if (part.empty()) {
      if (i == 0) return false;                  // an absolute path
      if (i == raw.size()) break;                // a folder's own entry ("a/b/")
      return false;                              // "a//b"
    }
    if (part.equals(".") || part.equals("..") || part.size() > 255 || part.ends_with('.') || part.ends_with(' ')) return false;
    for (const char c : part)
      if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    if (!out->empty() && !out->push_back('/')) return false;
    if (!out->append(part)) return false;
  }
  return !out->empty();
}

bool WingetInstaller::decode_mszyml(allocators::IAllocator& allocator, ConstByteView packed, uint64_t max_bytes, String* text) {
  text->clear();
  ByteReader in(packed);
  // The header of Windows' compression interface: a magic number, the header's size, the algorithm (2: MSZIP), the size of the text.
  if (in.le32() != 0xC0E5510Au) return false;
  const uint16_t header_size = in.le16();
  in.skip(1);      // a check byte of the header
  const uint8_t algorithm = in.u8();
  const uint64_t size = in.le64();
  if (!in.ok() || header_size < 24 || algorithm != 2 || size > max_bytes || !in.seek(header_size)) return false;
  ByteBuffer out(allocator);
  while (out.size() < size) {
    const uint32_t block_size = in.le32();
    ByteReader block(in.take(block_size));
    if (!in.ok() || block.u8() != 'C' || block.u8() != 'K' || !block.ok()) return false;
    const uint64_t left = size - out.size(), before = out.size();
    if (!media::inflate_with_history(block.rest(), &out, left < kMszipBlock ? left : kMszipBlock, before < kMszipBlock ? before : kMszipBlock)) return false;
    if (out.size() == before) return false;      // (an empty block would never end)
  }
  return text->assign(StringView(out.view()));
}

bool WingetInstaller::find_version(allocators::IAllocator& allocator, StringView version_list, StringView version, String* found_version, String* path, String* sha256) {
  YamlNode root(allocator);
  if (!parse_yaml(allocator, version_list, &root)) return false;
  const YamlNode* versions = root.find("vD");
  if (!versions || versions->kind != YamlNode::Kind::Sequence || versions->children.count() == 0) return false;
  const YamlNode* chosen = versions->children.view()[0].get();
  for (const UniquePointer<YamlNode>& item : versions->children.view())
    if (item->text("v").equals(version)) {
      chosen = item.get();
      break;
    }
  const StringView where = chosen->text("rP");
  // The path is put into an address: it must be a plain path below the cache.
  String checked(allocator);
  if (where.empty() || !safe_member_path(where, &checked) || !checked.view().equals(where)) return false;
  return found_version->assign(chosen->text("v")) && path->assign(where) && sha256->assign(chosen->text("s256H")) && sha256->size() == 64;
}

bool WingetInstaller::parse_manifest(allocators::IAllocator& allocator, StringView yaml, WingetManifest* manifest) {
  manifest->installers.clear();
  YamlNode root(allocator);
  if (!parse_yaml(allocator, yaml, &root) || root.kind != YamlNode::Kind::Map) return false;
  if (!manifest->id.assign(root.text("PackageIdentifier")) || !manifest->name.assign(root.text("PackageName")) || !manifest->version.assign(root.text("PackageVersion")))
    return false;
  manifest->command.clear();
  const YamlNode* commands = root.find("Commands");
  if (commands && commands->kind == YamlNode::Kind::Sequence && commands->children.count() > 0 && commands->children.view()[0]->kind == YamlNode::Kind::Scalar &&
      !manifest->command.assign(commands->children.view()[0]->value.view()))
    return false;
  const YamlNode* installers = root.find("Installers");
  if (manifest->id.empty() || !installers || installers->kind != YamlNode::Kind::Sequence) return false;
  for (const UniquePointer<YamlNode>& item : installers->children.view()) {
    if (item->kind != YamlNode::Kind::Map) continue;
    UniquePointer<WingetInstallerEntry> entry = UniquePointer<WingetInstallerEntry>::make(allocator, allocator);
    if (!entry) return false;
    // What an installer does not say for itself, the manifest says for all of them.
    auto own_or_shared = [&](StringView key) { return item->find(key) ? item->text(key) : root.text(key); };
    if (!entry->architecture.assign(item->text("Architecture")) || !entry->type.assign(own_or_shared("InstallerType")) ||
        !entry->nested_type.assign(own_or_shared("NestedInstallerType")) || !entry->url.assign(item->text("InstallerUrl")) ||
        !entry->sha256.assign(item->text("InstallerSha256")))
      return false;
    const YamlNode* nested = item->find("NestedInstallerFiles");
    if (!nested) nested = root.find("NestedInstallerFiles");
    if (nested && nested->kind == YamlNode::Kind::Sequence) {
      for (const UniquePointer<YamlNode>& file : nested->children.view()) {
        if (file->kind != YamlNode::Kind::Map || entry->nested.count() >= kMaxShortcuts) continue;
        UniquePointer<WingetNestedFile> one = UniquePointer<WingetNestedFile>::make(allocator, allocator);
        if (!one || !one->path.assign(file->text("RelativeFilePath")) || !one->alias.assign(file->text("PortableCommandAlias")) || !entry->nested.push_back(util::move(one)))
          return false;
      }
    }
    if (!manifest->installers.push_back(util::move(entry))) return false;
  }
  return true;
}

const WingetInstallerEntry* WingetInstaller::choose_installer(const WingetManifest& manifest, String* why) {
  why->clear();
  const WingetInstallerEntry* best = nullptr;
  const WingetInstallerEntry* other = nullptr;      // one for this computer that is a setup program
  int best_rank = 0;
  for (const UniquePointer<WingetInstallerEntry>& entry : manifest.installers.view()) {
    const int rank = architecture_rank(entry->architecture.view());
    if (rank < 0) continue;
    if (!can_install(*entry)) {
      if (!other) other = entry.get();
      continue;
    }
    if (!best || rank < best_rank) best = entry.get(), best_rank = rank;
  }
  if (best) return best;
  if (other) {
    (void)(why->assign("it is installed by ") && why->append(kind_name(equals_folded(other->type.view(), "zip") && !other->nested_type.empty() && !equals_folded(other->nested_type.view(), "portable")
                                                                             ? other->nested_type.view()
                                                                             : other->type.view())) &&
           why->append(", which cannot be run here yet"));
  } else {
    (void)why->assign("there is no version of it for this kind of computer (x64)");
  }
  return nullptr;
}

bool WingetInstaller::folder_of(StringView id, String* out) const {
  return valid_package_id(id) && join(out, filesystem::layout::kWindowsWingetPackages, "/", id, kFolderSuffix);
}

bool WingetInstaller::read_record(StringView id, WingetInstalled* app, List<String>* shortcuts) const {
  String folder(allocator_), path(allocator_), text(allocator_);
  if (!folder_of(id, &folder) || !join(&path, folder.view(), "/", kRecordName) || !read_text_file(files_, allocator_, path.view(), kMaxRecordBytes, &text)) return false;
  bool ok = true, named = false;
  StringView rest = text.view(), line;
  while (ok && rest.next_token('\n', &line)) {
    StringView key, value;
    if (!line.split_once(':', &key, &value)) continue;
    value = value.trimmed();
    if (key.equals("PackageIdentifier")) ok = app->id.assign(value), named = !value.empty();
    else if (key.equals("PackageName")) ok = app->name.assign(value);
    else if (key.equals("PackageVersion")) ok = app->version.assign(value);
    else if (key.equals("Shortcut") && shortcuts && shortcuts->count() < kMaxShortcuts) {
      // (a name of a file in the Start menu's folder and nothing else: the record is a file anyone can edit)
      String name(allocator_), safe(allocator_);
      if (safe_member_path(value, &safe) && !safe.view().contains('/') && safe.view().ends_with(".lnk")) ok = name.assign(safe.view()) && shortcuts->push_back(util::move(name));
    }
  }
  return ok && named;
}

bool WingetInstaller::installed(StringView id) const {
  WingetInstalled app(allocator_);
  return read_record(id, &app, nullptr);
}

bool WingetInstaller::list_installed(List<WingetInstalled>* apps) const {
  apps->clear();
  UniquePointer<filesystem::IDirectory> listing;
  if (files_.open_directory(filesystem::layout::kWindowsWingetPackages, &listing) != Status::Ok) return true;      // (nothing installed yet)
  filesystem::DirectoryEntry entry;
  for (uint32_t guard = 0; guard < 100000 && listing->next(&entry) == Status::Ok; ++guard) {
    const StringView folder = entry.name.view();
    if (entry.type != filesystem::NodeType::Directory || !folder.ends_with(kFolderSuffix)) continue;
    const StringView id = folder.first(folder.size() - kFolderSuffix.size());
    WingetInstalled app(allocator_);
    if (!read_record(id, &app, nullptr)) continue;
    if (!app.id.assign(id)) return false;      // (the folder is the identifier, whatever the record says)
    if (app.name.empty() && !app.name.assign(id)) return false;
    if (!apps->push_back(util::move(app))) return false;
  }
  ArrayView<WingetInstalled> list = apps->view();
  for (uint64_t i = 1; i < list.size(); ++i)
    for (uint64_t j = i; j > 0 && compare_folded(list[j].name.view(), list[j - 1].name.view()) < 0; --j) {
      WingetInstalled t = util::move(list[j]);
      list[j] = util::move(list[j - 1]);
      list[j - 1] = util::move(t);
    }
  return true;
}

Result WingetInstaller::resolve(const WingetPackage& package, WingetManifest* manifest, String* detail) {
  detail->clear();
  if (!valid_package_id(package.id.view())) return Result::InvalidName;
  // 1. The version list, which must be the one the index names.
  String address(allocator_), packed(allocator_), list(allocator_);
  if (!address.assign(kWingetCache) || !address.append("packages/") || !append_escaped(&address, package.id.view(), false) || !address.push_back('/') ||
      !append_hex(&address, ConstByteView(package.hash).first(4)) || !address.append("/versionData.mszyml"))
    return Result::OutOfMemory;
  if (!network_.fetch(address.view(), kMaxVersionDataBytes, &packed) || packed.empty()) {
    detail->assign(address.view());
    return Result::NoManifest;
  }
  uint8_t hash[32];
  crypto::hash(crypto::HashKind::Sha256, packed.view().bytes(), hash);
  if (!crypto::equal_constant_time(hash, package.hash, 32)) {
    detail->assign("its list of versions is not the one the package list names; choose Refresh List and try again");
    return Result::NoManifest;
  }
  String version(allocator_), path(allocator_), sha256(allocator_);
  if (!decode_mszyml(allocator_, packed.view().bytes(), kMaxVersionListBytes, &list) || !find_version(allocator_, list.view(), package.version.view(), &version, &path, &sha256)) {
    detail->assign("its list of versions could not be read");
    return Result::NoManifest;
  }
  // 2. The manifest, which must be the one the version list names.
  String yaml(allocator_);
  if (!address.assign(kWingetCache) || !append_escaped(&address, path.view(), true)) return Result::OutOfMemory;
  if (!network_.fetch(address.view(), kMaxManifestBytes, &yaml) || yaml.empty()) {
    detail->assign(address.view());
    return Result::NoManifest;
  }
  if (!sha256_is(yaml.view().bytes(), sha256.view(), allocator_)) {
    detail->assign("its manifest is not the one its list of versions names");
    return Result::NoManifest;
  }
  if (!parse_manifest(allocator_, yaml.view(), manifest) || !equals_folded(manifest->id.view(), package.id.view())) {
    detail->assign("its manifest could not be read");
    return Result::NoManifest;
  }
  if (manifest->version.empty() && !manifest->version.assign(version.view())) return Result::OutOfMemory;
  if (manifest->name.empty() && !manifest->name.assign(package.name.view())) return Result::OutOfMemory;
  return Result::Ok;
}

Result WingetInstaller::install(const WingetPackage& package, Progress* progress, String* detail) {
  WingetManifest manifest(allocator_);
  const Result r = resolve(package, &manifest, detail);
  if (r != Result::Ok) return r;
  const WingetInstallerEntry* entry = choose_installer(manifest, detail);
  if (!entry) return Result::NotSupported;
  return place(manifest, *entry, installed(package.id.view()), progress, detail);
}

Result WingetInstaller::update(const WingetPackage& package, Progress* progress, String* detail) {
  detail->clear();
  WingetInstalled current(allocator_);
  if (!read_record(package.id.view(), &current, nullptr)) return Result::NotInstalled;
  if (current.version.view().equals(package.version.view())) return Result::UpToDate;
  WingetManifest manifest(allocator_);
  const Result r = resolve(package, &manifest, detail);
  if (r != Result::Ok) return r;
  if (current.version.view().equals(manifest.version.view())) return Result::UpToDate;
  const WingetInstallerEntry* entry = choose_installer(manifest, detail);
  if (!entry) return Result::NotSupported;
  return place(manifest, *entry, true, progress, detail);
}

void WingetInstaller::remove_shortcuts(const List<String>& shortcuts) {
  for (const String& name : shortcuts.view()) {
    String path(allocator_);
    if (join(&path, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view())) (void)files_.remove(path.view());
  }
}

// Empties the folder but for the file called `keep`.
bool WingetInstaller::clear_folder(StringView folder, StringView keep) {
  List<String> names(allocator_);
  {
    UniquePointer<filesystem::IDirectory> listing;
    if (files_.open_directory(folder, &listing) != Status::Ok) return false;
    filesystem::DirectoryEntry child;
    for (uint32_t guard = 0; guard < 100000 && listing->next(&child) == Status::Ok; ++guard) {
      String full(allocator_);
      if (child.name.view().equals(keep)) continue;
      if (!join(&full, folder, "/", child.name.view()) || !names.push_back(util::move(full))) return false;
    }
  }
  for (const String& child : names.view())
    if (!remove_tree(files_, allocator_, child.view())) return false;
  return true;
}

// Steps 4 and 5.
Result WingetInstaller::place(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool was_installed, Progress* progress, String* detail) {
  const StringView id = manifest.id.view();
  const bool archive = equals_folded(entry.type.view(), "zip");
  String folder(allocator_), part(allocator_);
  if (!folder_of(id, &folder)) return Result::InvalidName;
  if (!join(&part, folder.view(), "/download.part")) return Result::OutOfMemory;
  if (!make_folders(files_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  // Until the new files are in place a failure leaves what was there: the old version, or nothing.
  auto give_up = [&](Result result) {
    (void)files_.remove(part.view());
    if (!was_installed) (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };

  // 4. The download, and its checksum.
  crypto::Hasher hasher(crypto::HashKind::Sha256);
  const Result fetched = download_to_file(files_, network_, entry.url.view(), part.view(), progress, &hasher, detail);
  if (fetched != Result::Ok) return give_up(fetched);
  uint8_t hash[32];
  hasher.finish(hash);
  String hex(allocator_);
  if (!append_hex(&hex, ConstByteView(hash))) return give_up(Result::OutOfMemory);
  if (!equals_folded(hex.view(), entry.sha256.view())) {
    detail->assign(entry.url.view());
    return give_up(Result::BadChecksum);
  }
  if (!archive && exe::identify_executable_file(files_, part.view()) != exe::ExecutableKind::Pe) {
    detail->assign("it is not a Windows program");
    return give_up(Result::NotAnAppImage);
  }

  // The programs the package has, and what their shortcuts are called.
  List<UniquePointer<Program>> programs(allocator_);
  if (archive) {
    for (const UniquePointer<WingetNestedFile>& nested : entry.nested.view()) {
      UniquePointer<Program> program = UniquePointer<Program>::make(allocator_, allocator_);
      if (!program) return give_up(Result::OutOfMemory);
      if (!safe_member_path(nested->path.view(), &program->path)) {
        detail->assign("its manifest names a program outside the package");
        return give_up(Result::NotAnAppImage);
      }
      const StringView label = entry.nested.count() == 1 ? manifest.name.view() : !nested->alias.empty() ? nested->alias.view() : stem_of(leaf_of(program->path.view()));
      if (!safe_name(label, &program->label) || !programs.push_back(util::move(program))) return give_up(Result::OutOfMemory);
    }
  } else {
    // One program, called after its command (as winget calls it), else after the file of the download.
    UniquePointer<Program> program = UniquePointer<Program>::make(allocator_, allocator_);
    if (!program) return give_up(Result::OutOfMemory);
    StringView name = manifest.command.view();
    if (name.empty()) {
      name = entry.url.view();
      const uint64_t query = name.find('?');
      if (query != StringView::npos) name = name.first(query);
      name = leaf_of(name);
    }
    if (!safe_name(name, &program->path)) return give_up(Result::OutOfMemory);
    if (program->path.empty() && !safe_name(id, &program->path)) return give_up(Result::OutOfMemory);
    const StringView so_far = program->path.view();
    if ((so_far.size() < 4 || !equals_folded(so_far.substr(so_far.size() - 4), ".exe")) && !program->path.append(".exe")) return give_up(Result::OutOfMemory);
    if (!safe_name(manifest.name.view(), &program->label) || !programs.push_back(util::move(program))) return give_up(Result::OutOfMemory);
  }

  // 5. In place of the old version: its shortcuts and its files go (the download stays).
  List<String> old_shortcuts(allocator_);
  WingetInstalled old(allocator_);
  if (was_installed) (void)read_record(id, &old, &old_shortcuts);
  remove_shortcuts(old_shortcuts);
  List<String> shortcuts(allocator_);
  auto fail = [&](Result result) {      // (from here on a failure leaves nothing: half a program is worse)
    remove_shortcuts(shortcuts);
    (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };
  if (!clear_folder(folder.view(), "download.part")) {
    detail->assign(folder.view());
    return fail(Result::FileError);
  }
  if (archive) {
    const Result unpacked = unpack(part.view(), folder.view(), detail);
    (void)files_.remove(part.view());
    if (unpacked != Result::Ok) return fail(unpacked);
  } else {
    String program(allocator_);
    if (!join(&program, folder.view(), "/", programs.view()[0]->path.view())) return fail(Result::OutOfMemory);
    if (files_.rename(part.view(), program.view()) != Status::Ok) {
      detail->assign(program.view());
      return fail(Result::FileError);
    }
  }

  // The shortcuts (of the programs that are Windows programs), and the record.
  String record(allocator_);
  if (!join(&record, "PackageIdentifier: ", id, "\nPackageName: ", manifest.name.view()) || !record.append("\nPackageVersion: ") || !record.append(manifest.version.view()) ||
      !record.append("\nInstallerUrl: ") || !record.append(entry.url.view()) || !record.push_back('\n'))
    return fail(Result::OutOfMemory);
  if (!make_folders(files_, filesystem::layout::kWindowsStartMenuPrograms)) {
    detail->assign(filesystem::layout::kWindowsStartMenuPrograms);
    return fail(Result::FileError);
  }
  for (const UniquePointer<Program>& program : programs.view()) {
    String path(allocator_), target(allocator_), directory(allocator_), link(allocator_), name(allocator_);
    if (!join(&path, folder.view(), "/", program->path.view())) return fail(Result::OutOfMemory);
    if (exe::identify_executable_file(files_, path.view()) != exe::ExecutableKind::Pe) {
      if (programs.count() == 1) {
        detail->assign("the program its manifest names is not in the archive, or is not a Windows program");
        return fail(Result::NotAnAppImage);
      }
      continue;
    }
    if (program->label.empty() && !program->label.assign(stem_of(leaf_of(program->path.view())))) return fail(Result::OutOfMemory);
    // A shortcut of that name that is someone else's stays theirs: this one says whose it is.
    if (!join(&name, program->label.view(), ".lnk") || !join(&link, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view())) return fail(Result::OutOfMemory);
    bool taken = exists(files_, link.view());
    for (const String& mine : shortcuts.view()) taken |= equals_folded(mine.view(), name.view());
    if (taken) {
      String owner(allocator_);
      if (!safe_name(id, &owner) || !join(&name, program->label.view(), " (", owner.view(), ").lnk") || !join(&link, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view()))
        return fail(Result::OutOfMemory);
    }
    ByteBuffer bytes(allocator_);
    if (!windows_path(path.view(), &target) || !windows_path(parent_of(path.view()), &directory) || !write_shell_link(target.view(), directory.view(), &bytes) ||
        !write_bytes(files_, link.view(), bytes.view())) {
      detail->assign(link.view());
      return fail(Result::FileError);
    }
    if (!record.append("Shortcut: ") || !record.append(name.view()) || !record.push_back('\n') || !shortcuts.push_back(util::move(name))) return fail(Result::OutOfMemory);
  }
  String record_path(allocator_);
  if (!join(&record_path, folder.view(), "/", kRecordName)) return fail(Result::OutOfMemory);
  if (!write_text_file(files_, record_path.view(), record.view())) {
    detail->assign(record_path.view());
    return fail(Result::FileError);
  }
  return Result::Ok;
}

// Every file of the archive at `archive`, into `folder`.
Result WingetInstaller::unpack(StringView archive, StringView folder, String* detail) {
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(archive, filesystem::OpenMode::Read, &file) != Status::Ok) {
    detail->assign(archive);
    return Result::FileError;
  }
  FileZipSource source(*file);
  media::ZipReader zip(allocator_, source);
  if (!zip.open()) {
    detail->assign("the download is not a .zip archive");
    return Result::NotAnAppImage;
  }
  uint64_t total = 0;
  for (const media::ZipEntry& entry : zip.entries()) {
    String relative(allocator_), path(allocator_);
    if (!safe_member_path(entry.name.view(), &relative)) {
      detail->assign("the archive holds a file with a name that is not safe to unpack");
      return Result::NotAnAppImage;
    }
    if (!join(&path, folder, "/", relative.view())) return Result::OutOfMemory;
    if (entry.directory()) {
      if (!make_folders(files_, path.view())) {
        detail->assign(path.view());
        return Result::FileError;
      }
      continue;
    }
    if (entry.size > kMaxMemberBytes || entry.size > kMaxArchiveBytes - total) {
      detail->assign("the archive holds more than this program can unpack");
      return Result::NotAnAppImage;
    }
    total += entry.size;
    ByteBuffer bytes(allocator_);
    if (!zip.extract(entry, kMaxMemberBytes, &bytes)) {
      (void)(detail->assign("the archive is damaged, or there is not enough memory to unpack it: ") && detail->append(relative.view()));
      return Result::NotAnAppImage;
    }
    if (!make_folders(files_, parent_of(path.view())) || !write_bytes(files_, path.view(), bytes.view())) {
      detail->assign(path.view());
      return Result::FileError;
    }
  }
  return Result::Ok;
}

Result WingetInstaller::remove(StringView id, String* detail) {
  detail->clear();
  if (!valid_package_id(id)) return Result::InvalidName;
  WingetInstalled app(allocator_);
  List<String> shortcuts(allocator_);
  String folder(allocator_);
  if (!read_record(id, &app, &shortcuts)) return Result::NotInstalled;
  if (!folder_of(id, &folder)) return Result::OutOfMemory;
  remove_shortcuts(shortcuts);
  if (!remove_tree(files_, allocator_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  return Result::Ok;
}

}  // namespace desktop::appman
