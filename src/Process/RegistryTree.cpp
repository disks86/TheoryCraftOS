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
#include "Process/RegistryTree.hpp"

namespace process::windowsabi {

uint32_t utf16_units(StringView text) {
  uint32_t units = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    if ((b & 0xC0) == 0x80) continue;      // a continuation byte
    units += b >= 0xF0 ? 2 : 1;
  }
  return units;
}

namespace {
char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
int compare_names(StringView a, StringView b) {
  const uint64_t n = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < n; ++i) {
    const char x = fold(a[i]), y = fold(b[i]);
    if (x != y) return static_cast<uint8_t>(x) < static_cast<uint8_t>(y) ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
}  // namespace

bool RegistryTree::names_equal(StringView a, StringView b) { return compare_names(a, b) == 0; }
bool RegistryTree::valid_key_name(StringView name) { return !name.empty() && utf16_units(name) <= kRegMaxNameChars; }

RegistryTree::RegistryTree(allocators::IAllocator& allocator, uint64_t now) : allocator_(allocator), keys_(allocator) {
  auto root = UniquePointer<RegKey>::make(allocator_, allocator_);
  if (!root) return;
  root->name.assign("REGISTRY");
  root->write_time = now;
  keys_.push_back(static_cast<UniquePointer<RegKey>&&>(root));
  static const char* const kHives[] = {"MACHINE\\HARDWARE", "MACHINE\\SAM", "MACHINE\\SECURITY", "MACHINE\\SOFTWARE\\Classes", "MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
                                       "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "MACHINE\\SYSTEM\\CurrentControlSet\\Control", "MACHINE\\SYSTEM\\CurrentControlSet\\Services",
                                       "MACHINE\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug", "USER\\.DEFAULT", "USER\\S-1-5-21-0-0-0-1000\\Software\\Microsoft\\Windows\\CurrentVersion",
                                       "USER\\S-1-5-21-0-0-0-1000\\Environment", "USER\\S-1-5-21-0-0-0-1000\\Volatile Environment", "USER\\S-1-5-21-0-0-0-1000\\Control Panel", "USER\\S-1-5-21-0-0-0-1000\\Control Panel\\International\\Geo"};
  for (const char* path : kHives) {      // every level, one after the other: a path is made one key at a time
    const StringView whole(path);
    for (uint64_t end = whole.find('\\'); ; end = whole.find('\\', end + 1)) {
      uint32_t id = 0;
      bool created = false;
      resolve(kRoot, end == StringView::npos ? whole : whole.first(end), true, now, &id, &created);
      if (end == StringView::npos) break;
    }
  }
  uint32_t version = 0;
  bool created = false;
  if (resolve(kRoot, StringView("MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"), false, now, &version, &created) == kRegSuccess) {
    auto text = [&](const char* name, const char* value) {
      ByteBuffer data(allocator_);
      for (const char* c = value; *c; ++c) data.put16(static_cast<uint8_t>(*c));
      data.put16(0);
      set_value(version, StringView(name), 1 /* REG_SZ */, data.view(), now);
    };
    text("ProductName", "TheoryCraftOS Windows");
    text("CurrentVersion", "10.0");
    text("CurrentBuildNumber", "19045");
    text("CurrentBuild", "19045");
    text("SystemRoot", "C:\\Windows");
    text("PathName", "C:\\Windows");
    text("CSDVersion", "");
  }
  uint32_t geo = 0;
  if (resolve(kRoot, StringView("USER\\S-1-5-21-0-0-0-1000\\Control Panel\\International\\Geo"), false, now, &geo, &created) == kRegSuccess) {      // the user's place (GetUserGeoID)
    auto text = [&](const char* name, const char* value) {
      ByteBuffer data(allocator_);
      for (const char* c = value; *c; ++c) data.put16(static_cast<uint8_t>(*c));
      data.put16(0);
      set_value(geo, StringView(name), 1 /* REG_SZ */, data.view(), now);
    };
    text("Nation", "244");
    text("Name", "US");
  }
  uint32_t profiles = 0;
  if (resolve(kRoot, StringView("MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList"), false, now, &profiles, &created) == kRegSuccess) {
    auto expandable = [&](const char* name, const char* value) {      // REG_EXPAND_SZ: the folders every profile API answers with
      ByteBuffer data(allocator_);
      for (const char* c = value; *c; ++c) data.put16(static_cast<uint8_t>(*c));
      data.put16(0);
      set_value(profiles, StringView(name), 2 /* REG_EXPAND_SZ */, data.view(), now);
    };
    expandable("ProfilesDirectory", "%SystemDrive%\\Users");
    expandable("Default", "%SystemDrive%\\Users\\Default");
    expandable("Public", "%SystemDrive%\\Users\\Public");
    expandable("ProgramData", "%SystemDrive%\\ProgramData");
  }
}

uint32_t RegistryTree::find_child(uint32_t parent, StringView name) const {
  const RegKey* p = key(parent);
  if (!p) return ~0u;
  for (uint64_t i = 0; i < p->subkeys.count(); ++i) {
    const uint32_t id = p->subkeys[i];
    const RegKey* k = key(id);
    if (k && !k->deleted && names_equal(k->name.view(), name)) return id;
  }
  return ~0u;
}

bool RegistryTree::link_child(uint32_t parent, uint32_t child) {
  RegKey* p = key(parent);
  const RegKey* c = key(child);
  if (!p || !c) return false;
  uint64_t at = 0;
  while (at < p->subkeys.count()) {
    const RegKey* other = key(p->subkeys[at]);
    if (other && compare_names(other->name.view(), c->name.view()) > 0) break;
    ++at;
  }
  return p->subkeys.insert(at, child);
}

uint32_t RegistryTree::new_key(uint32_t parent, StringView name, uint64_t now) {
  auto fresh = UniquePointer<RegKey>::make(allocator_, allocator_);
  if (!fresh || !fresh->name.assign(name)) return ~0u;
  fresh->parent = parent;
  fresh->write_time = now;
  const uint32_t id = static_cast<uint32_t>(keys_.count());
  if (!keys_.push_back(static_cast<UniquePointer<RegKey>&&>(fresh))) return ~0u;
  if (!link_child(parent, id)) {
    keys_.remove_at(id);
    return ~0u;
  }
  if (RegKey* p = key(parent)) p->write_time = now;
  return id;
}

uint32_t RegistryTree::resolve(uint32_t base, StringView path, bool create, uint64_t now, uint32_t* id, bool* created) {
  if (created) *created = false;
  if (!alive(base)) return kRegKeyDeleted;
  uint32_t at = base;
  uint64_t pos = 0;
  if (path.starts_with('\\')) {
    pos = 1;
    uint64_t end = path.find('\\', pos);
    if (end == StringView::npos) end = path.size();
    if (!names_equal(path.substr(pos, end - pos), key(kRoot)->name.view())) return kRegNameNotFound;
    at = kRoot;
    pos = end < path.size() ? end + 1 : end;
  }
  uint32_t depth = 0;
  bool made = false;
  while (pos < path.size()) {
    uint64_t end = path.find('\\', pos);
    if (end == StringView::npos) end = path.size();
    const StringView part = path.substr(pos, end - pos);
    pos = end < path.size() ? end + 1 : end;
    if (part.empty()) return kRegNameNotFound;      // "a\\b" and a trailing '\' name nothing
    if (!valid_key_name(part) || ++depth > kRegMaxDepth) return kRegInvalidParameter;
    uint32_t child = find_child(at, part);
    if (child == ~0u) {
      if (!create || pos < path.size()) return kRegNameNotFound;      // only the last key of a path is made: its parents have to be there
      child = new_key(at, part, now);
      if (child == ~0u) return kRegNoMemory;
      made = true;
    } else {
      made = false;
    }
    at = child;
  }
  *id = at;
  if (created) *created = made;
  return kRegSuccess;
}

bool RegistryTree::full_name(uint32_t id, String* out) const {
  const RegKey* k = key(id);
  if (!k) return false;
  if (id == kRoot) return out->assign("\\") && out->append(k->name.view());
  return full_name(k->parent, out) && out->append("\\") && out->append(k->name.view());
}

void RegistryTree::add_handle(uint32_t id) { if (RegKey* k = key(id)) ++k->open_handles; }

void RegistryTree::drop_if_unused(uint32_t id) {
  RegKey* k = key(id);
  if (!k || !k->deleted || k->open_handles != 0) return;
  k->values.clear();
}

void RegistryTree::release_handle(uint32_t id) {
  RegKey* k = key(id);
  if (!k) return;
  if (k->open_handles != 0) --k->open_handles;
  drop_if_unused(id);
}

uint32_t RegistryTree::delete_key(uint32_t id) {
  RegKey* k = key(id);
  if (!k || k->deleted) return kRegKeyDeleted;
  if (id == kRoot) return kRegCannotDelete;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) if (alive(k->subkeys[i])) return kRegCannotDelete;
  k->deleted = true;
  RegKey* parent = key(k->parent);
  if (parent) {
    for (uint64_t i = 0; i < parent->subkeys.count(); ++i) if (parent->subkeys[i] == id) { parent->subkeys.remove_at(i); break; }
  }
  drop_if_unused(id);
  return kRegSuccess;
}

const RegValue* RegistryTree::find_value(uint32_t id, StringView name) const {
  const RegKey* k = key(id);
  if (!k) return nullptr;
  for (uint64_t i = 0; i < k->values.count(); ++i) {
    const UniquePointer<RegValue>& v = k->values[i];
    if (v && names_equal(v->name.view(), name)) return v.get();
  }
  return nullptr;
}

uint32_t RegistryTree::set_value(uint32_t id, StringView name, uint32_t type, ConstByteView data, uint64_t now) {
  RegKey* k = key(id);
  if (!k || k->deleted) return kRegKeyDeleted;
  if (utf16_units(name) > kRegMaxValueNameChars || data.size() > kRegMaxValueBytes) return kRegInvalidParameter;
  for (uint64_t i = 0; i < k->values.count(); ++i) {
    UniquePointer<RegValue>& v = k->values[i];
    if (v && names_equal(v->name.view(), name)) {
      auto replacement = UniquePointer<RegValue>::make(allocator_, allocator_);
      if (!replacement || !replacement->name.assign(v->name.view()) || !replacement->data.append(data)) return kRegNoMemory;
      replacement->type = type;
      v = static_cast<UniquePointer<RegValue>&&>(replacement);
      k->write_time = now;
      return kRegSuccess;
    }
  }
  auto fresh = UniquePointer<RegValue>::make(allocator_, allocator_);
  if (!fresh || !fresh->name.assign(name) || !fresh->data.append(data)) return kRegNoMemory;
  fresh->type = type;
  if (!k->values.push_back(static_cast<UniquePointer<RegValue>&&>(fresh))) return kRegNoMemory;
  k->write_time = now;
  return kRegSuccess;
}

uint32_t RegistryTree::delete_value(uint32_t id, StringView name, uint64_t now) {
  RegKey* k = key(id);
  if (!k || k->deleted) return kRegKeyDeleted;
  for (uint64_t i = 0; i < k->values.count(); ++i) {
    const UniquePointer<RegValue>& v = k->values[i];
    if (v && names_equal(v->name.view(), name)) {
      k->values.remove_at(i);
      k->write_time = now;
      return kRegSuccess;
    }
  }
  return kRegNameNotFound;
}

RegistryTree::Statistics RegistryTree::statistics(uint32_t id) const {
  Statistics s;
  const RegKey* k = key(id);
  if (!k) return s;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) {
    const RegKey* c = key(k->subkeys[i]);
    if (!c || c->deleted) continue;
    ++s.subkeys;
    const uint32_t bytes = utf16_units(c->name.view()) * 2;
    if (bytes > s.max_subkey_name_bytes) s.max_subkey_name_bytes = bytes;
  }
  for (uint64_t i = 0; i < k->values.count(); ++i) {
    const UniquePointer<RegValue>& v = k->values[i];
    if (!v) continue;
    ++s.values;
    const uint32_t bytes = utf16_units(v->name.view()) * 2;
    if (bytes > s.max_value_name_bytes) s.max_value_name_bytes = bytes;
    if (v->data.size() > s.max_value_data_bytes) s.max_value_data_bytes = static_cast<uint32_t>(v->data.size());
  }
  return s;
}

}  // namespace process::windowsabi
