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
#include "FileSystem/FileSystemTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// Path resolution with symbolic links for read-only file systems that keep their own tree (SquashFS, ISO 9660): the walk from the root,
// component by component, following links (relative ones from the link's folder, ".." allowed in their targets and taken physically, absolute
// ones from the root), with a limit on the links followed.
//
// A file system mounted inside another one's tree can instead be asked where a path leaves it (IFileSystem::resolve_exit): with `exit` given,
// an absolute target or a ".." above the root stops the walk and says where the path continues outside:
//   "/usr/share/zoneinfo/UTC"   an absolute target and the rest of the path, from the root of the tree the file system is mounted in;
//   "../lib/x"                  a climb above the root and the rest, from the root itself (so from the mount point: "../lib/x" is the folder
//                               "lib/x" next to the mount point).
// Neither ends with a slash.
namespace filesystem {

namespace link_walk {

constexpr uint32_t kMaxLinks = 40;

inline StringView next_component(StringView path, uint64_t* pos) {
  while (*pos < path.size() && path[*pos] == '/') ++*pos;
  const uint64_t start = *pos;
  while (*pos < path.size() && path[*pos] != '/') ++*pos;
  return path.substr(start, *pos - start);
}

// Does anything other than "." follow *pos?
inline bool more_components(StringView path, uint64_t pos) {
  for (;;) {
    const StringView part = next_component(path, &pos);
    if (part.empty()) return false;
    if (!part.equals(".")) return true;
  }
}

inline StringView last_component(StringView path) {
  StringView last;
  for (uint64_t pos = 0;;) {
    const StringView part = next_component(path, &pos);
    if (part.empty()) return last;
    last = part;
  }
}

// An ordinary path (not a link's target): no "..", names of at most kMaxNameLength bytes.
inline bool valid_path(StringView path) {
  for (uint64_t pos = 0;;) {
    const StringView part = next_component(path, &pos);
    if (part.empty()) return true;
    if (part.equals("..") || part.size() > kMaxNameLength) return false;
  }
}

}  // namespace link_walk

// Walks `path` from `root`. `Ops` answers for the file system:
//   Status lookup(const Node& directory, StringView name, Node* child);    NotFound if there is no such name
//   bool is_directory(const Node&); bool is_link(const Node&);
//   Status target(const Node& link, String* target);
// With `exit` null, links never leave the tree. With it, a walk that leaves ends with Ok, *exit set and *out unchanged; *exit stays empty otherwise.
// *exit = head + rest, without trailing slashes.
inline Status set_exit(String* exit, StringView head, StringView rest) {
  if (!exit->assign(head) || !exit->append(rest)) return Status::OutOfMemory;
  while (exit->size() > 1 && (*exit)[exit->size() - 1] == '/') exit->truncate(exit->size() - 1);
  return Status::Ok;
}

template <class Node, class Ops>
Status walk_path(allocators::IAllocator& allocator, StringView path, bool follow_last, const Node& root, Ops& ops, Node* out, String* exit = nullptr) {
  if (exit) exit->clear();
  if (!link_walk::valid_path(path)) return Status::InvalidPath;
  List<Node> stack(allocator);
  if (!stack.push_back(root)) return Status::OutOfMemory;
  String pending(allocator), spliced(allocator), target(allocator);
  if (!pending.assign(path)) return Status::OutOfMemory;
  uint32_t hops = 0;
  for (uint64_t pos = 0;;) {
    const StringView part = link_walk::next_component(pending.view(), &pos);
    if (part.empty()) break;
    if (part.equals(".")) continue;
    if (part.equals("..")) {
      if (stack.count() > 1) {
        stack.remove_at(stack.count() - 1);
      } else if (exit) {
        return set_exit(exit, "..", pending.view().substr(pos));
      }
      continue;
    }
    if (part.size() > kMaxNameLength) return Status::InvalidPath;
    const Node& current = stack.view().back();
    if (!ops.is_directory(current)) return Status::NotADirectory;
    Node child;
    if (const Status s = ops.lookup(current, part, &child); s != Status::Ok) return s;
    const bool more = link_walk::more_components(pending.view(), pos);
    if (ops.is_link(child) && (more || follow_last)) {
      if (++hops > link_walk::kMaxLinks) return Status::TooManyLinks;
      if (const Status s = ops.target(child, &target); s != Status::Ok) return s;
      if (target.empty()) return Status::NotFound;
      if (target.view().starts_with('/')) {
        if (exit) return set_exit(exit, target.view(), pending.view().substr(pos));
        while (stack.count() > 1) stack.remove_at(stack.count() - 1);
      }
      if (!spliced.assign(target.view()) || !spliced.push_back('/') || !spliced.append(pending.view().substr(pos))) return Status::OutOfMemory;
      pending = util::move(spliced);
      spliced = String(allocator);
      pos = 0;
      continue;
    }
    if (!stack.push_back(child)) return Status::OutOfMemory;
  }
  *out = stack.view().back();
  return Status::Ok;
}

}  // namespace filesystem
