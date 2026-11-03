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

uint32_t RegistryTree::rename_key(uint32_t id, StringView new_name, uint64_t now) {
  RegKey* k = key(id);
  if (!k || k->deleted) return kRegKeyDeleted;
  if (id == kRoot || k->parent == kRoot) return kRegCannotDelete;
  if (!valid_key_name(new_name)) return kRegInvalidParameter;
  const uint32_t clash = find_child(k->parent, new_name);
  if (clash != ~0u) return clash == id && compare_names(k->name.view(), new_name) == 0 ? kRegSuccess : kRegNameCollision;
  RegKey* parent = key(k->parent);
  if (!parent) return kRegKeyDeleted;
  for (uint64_t i = 0; i < parent->subkeys.count(); ++i) if (parent->subkeys[i] == id) { parent->subkeys.remove_at(i); break; }
  if (!k->name.assign(new_name) || !link_child(k->parent, id)) return kRegNoMemory;
  k->write_time = now;
  parent->write_time = now;
  return kRegSuccess;
}

uint32_t RegistryTree::delete_tree(uint32_t id) {
  RegKey* k = key(id);
  if (!k || k->deleted) return kRegKeyDeleted;
  if (id == kRoot) return kRegCannotDelete;
  while (k->subkeys.count() != 0) {      // (delete_key takes the child out of the list, so the last one is always the next)
    const uint32_t child = k->subkeys[k->subkeys.count() - 1];
    const uint32_t status = alive(child) ? delete_tree(child) : kRegSuccess;
    if (status != kRegSuccess) return status;
    if (k->subkeys.count() != 0 && k->subkeys[k->subkeys.count() - 1] == child) k->subkeys.remove_at(k->subkeys.count() - 1);
  }
  return delete_key(id);
}

bool RegistryTree::has_open_handles(uint32_t id) const {
  const RegKey* k = key(id);
  if (!k || k->deleted) return false;
  if (k->open_handles != 0) return true;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) if (has_open_handles(k->subkeys[i])) return true;
  return false;
}

uint32_t RegistryTree::count_open_keys(uint32_t id) const {
  const RegKey* k = key(id);
  if (!k || k->deleted) return 0;
  uint32_t n = k->open_handles != 0 ? 1 : 0;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) n += count_open_keys(k->subkeys[i]);
  return n;
}

namespace {
constexpr uint32_t kImageMagic = 0x48524354u;      // "TCRH"
constexpr uint32_t kImageVersion = 1;
bool put_text(ByteBuffer* out, StringView text) { return text.size() <= 0xFFFFFFFFu && out->put32(static_cast<uint32_t>(text.size())) && out->append(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }
}  // namespace

bool RegistryTree::save_node(uint32_t id, ByteBuffer* out, uint32_t depth) const {
  const RegKey* k = key(id);
  if (!k || k->deleted || depth > kRegMaxDepth) return false;
  if (!put_text(out, k->name.view()) || !out->put32(k->volatile_key ? 1u : 0u) || !out->put32(static_cast<uint32_t>(k->write_time)) || !out->put32(static_cast<uint32_t>(k->write_time >> 32))) return false;
  if (!out->put32(static_cast<uint32_t>(k->values.count()))) return false;
  for (uint64_t i = 0; i < k->values.count(); ++i) {
    const UniquePointer<RegValue>& v = k->values[i];
    if (!v || !put_text(out, v->name.view()) || !out->put32(v->type) || !out->put32(static_cast<uint32_t>(v->data.size())) || !out->append(v->data.view())) return false;
  }
  uint32_t live = 0;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) if (alive(k->subkeys[i])) ++live;
  if (!out->put32(live)) return false;
  for (uint64_t i = 0; i < k->subkeys.count(); ++i) if (alive(k->subkeys[i]) && !save_node(k->subkeys[i], out, depth + 1)) return false;
  return true;
}

bool RegistryTree::save(uint32_t id, ByteBuffer* out) const {
  return out->put32(kImageMagic) && out->put32(kImageVersion) && save_node(id, out, 0);
}

// Walks one node without making anything: every length is checked against what is left, so that restore() can run on an image that is known to parse.
bool RegistryTree::check_node(ByteReader& in, uint32_t depth) {
  if (depth > kRegMaxDepth) return false;
  const uint32_t name_size = in.le32();
  if (!in.ok() || name_size > in.remaining()) return false;
  const ConstByteView name = in.take(name_size);
  if (depth != 0 && !valid_key_name(StringView(reinterpret_cast<const char*>(name.data()), name.size()))) return false;
  in.le32();
  in.le64();
  const uint32_t values = in.le32();
  if (!in.ok() || values > in.remaining() / 12) return false;
  for (uint32_t i = 0; i < values; ++i) {
    const uint32_t value_name = in.le32();
    if (!in.ok() || value_name > in.remaining()) return false;
    const ConstByteView text = in.take(value_name);
    if (utf16_units(StringView(reinterpret_cast<const char*>(text.data()), text.size())) > kRegMaxValueNameChars) return false;
    in.le32();
    const uint32_t size = in.le32();
    if (!in.ok() || size > kRegMaxValueBytes || size > in.remaining()) return false;
    in.skip(size);
  }
  const uint32_t subkeys = in.le32();
  if (!in.ok() || subkeys > in.remaining() / 24) return false;
  for (uint32_t i = 0; i < subkeys; ++i) if (!check_node(in, depth + 1)) return false;
  return in.ok();
}

bool RegistryTree::valid_image(ConstByteView image) {
  ByteReader in(image);
  if (in.le32() != kImageMagic || in.le32() != kImageVersion || !in.ok()) return false;
  return check_node(in, 0) && in.at_end();
}

uint32_t RegistryTree::restore_node(uint32_t id, ByteReader& in, uint64_t now, uint32_t depth) {
  const uint32_t name_size = in.le32();
  in.skip(name_size);      // (the top key keeps its own name; a subkey's name was read by the caller)
  const uint32_t flags = in.le32();
  in.le64();
  if (RegKey* k = key(id)) k->volatile_key = (flags & 1) != 0;
  const uint32_t values = in.le32();
  for (uint32_t i = 0; i < values; ++i) {
    const uint32_t value_name = in.le32();
    const ConstByteView text = in.take(value_name);
    const uint32_t type = in.le32();
    const uint32_t size = in.le32();
    const ConstByteView data = in.take(size);
    if (!in.ok()) return kRegInvalidParameter;
    const uint32_t status = set_value(id, StringView(reinterpret_cast<const char*>(text.data()), text.size()), type, data, now);
    if (status != kRegSuccess) return status;
  }
  const uint32_t subkeys = in.le32();
  for (uint32_t i = 0; i < subkeys; ++i) {
    ByteReader peek = in;      // the child's name comes first in its node: read it here, then let the child's own pass skip it
    const uint32_t child_name = peek.le32();
    const ConstByteView text = peek.take(child_name);
    if (!peek.ok()) return kRegInvalidParameter;
    const StringView name(reinterpret_cast<const char*>(text.data()), text.size());
    uint32_t child = find_child(id, name);
    if (child == ~0u) child = new_key(id, name, now);
    if (child == ~0u) return kRegNoMemory;
    const uint32_t status = restore_node(child, in, now, depth + 1);
    if (status != kRegSuccess) return status;
  }
  return in.ok() ? kRegSuccess : kRegInvalidParameter;
}

uint32_t RegistryTree::restore(uint32_t id, ConstByteView image, bool replace, uint64_t now) {
  if (!alive(id)) return kRegKeyDeleted;
  if (!valid_image(image)) return kRegInvalidParameter;
  if (replace) {
    RegKey* k = key(id);
    k->values.clear();
    while (k->subkeys.count() != 0) {
      const uint32_t child = k->subkeys[k->subkeys.count() - 1];
      const uint64_t before = k->subkeys.count();
      if (alive(child)) delete_tree(child);
      if (k->subkeys.count() == before) k->subkeys.remove_at(before - 1);
    }
  }
  ByteReader in(image);
  in.le32();
  in.le32();
  return restore_node(id, in, now, 0);
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
