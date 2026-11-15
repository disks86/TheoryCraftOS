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
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// The part of YAML that winget's manifests and version lists are written in (what yaml-cpp emits): block mappings and block sequences by indentation
// (a sequence may sit at its key's own indentation), and scalars that are plain, 'single quoted', "double quoted" (with escapes) or block scalars
// (| and >), any of which may run over several lines. Flow collections ([a, b], {a: b}) are kept as the text they are; anchors, aliases, tags and
// several documents in one file are not understood. The text is untrusted: depth and the number of nodes are limited.
namespace desktop::appman {

struct YamlNode {
  enum class Kind : uint8_t { Scalar, Map, Sequence };
  explicit YamlNode(allocators::IAllocator& allocator) : key(allocator), value(allocator), children(allocator) {}

  Kind kind = Kind::Scalar;
  String key;                                  // of an entry of a mapping
  String value;                                // of a scalar
  List<UniquePointer<YamlNode>> children;      // the entries of a mapping, the items of a sequence

  // The entry of this mapping with that key, or nullptr.
  const YamlNode* find(StringView name) const;
  // The scalar value of that entry ("" if there is none or it is not a scalar).
  StringView text(StringView name) const;
};

inline constexpr uint64_t kMaxYamlBytes = 8ull << 20;
inline constexpr uint32_t kMaxYamlDepth = 32;
inline constexpr uint64_t kMaxYamlNodes = 200000;

// Parses `text` into `root`. False if it is not YAML of the kind above, is too large or too deeply nested, or memory ran out.
[[nodiscard]] bool parse_yaml(allocators::IAllocator& allocator, StringView text, YamlNode* root);

}  // namespace desktop::appman
