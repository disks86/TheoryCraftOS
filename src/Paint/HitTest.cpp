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

#include "Paint/HitTest.hpp"

namespace paint {

bool hit_test(const DisplayList& list, const dom::Document* doc, int32_t x, int32_t y, HitResult* out) {
  // front to back is the order of the list read from the end, but whether a clip holds the point is known where the clip starts: so go forward and
  // keep the last candidate that is not hidden by a clip
  bool inside[96];                // per open clip: does it hold the point?
  uint32_t depth = 0, failing = 0;   // clips open, and how many of them do not hold the point (nothing in them can be hit)
  uint64_t best = 0;
  bool found = false;
  for (uint64_t i = 0; i < list.count(); ++i) {
    const Op& op = list.op(i);
    if (op.kind == OpKind::PushClip) {
      const bool holds = op.rect.contains(x, y);
      if (depth < 96) inside[depth] = holds;
      ++depth;
      if (!holds) ++failing;
      continue;
    }
    if (op.kind == OpKind::PopClip) {
      if (depth > 0) {
        --depth;
        if (depth < 96 && !inside[depth] && failing > 0) --failing;
      }
      continue;
    }
    if (op.kind != OpKind::Hit && op.kind != OpKind::Text && op.kind != OpKind::Object) continue;
    if (failing > 0 || !op.node || !op.rect.contains(x, y)) continue;
    best = i;
    found = true;
  }
  if (!found) return false;
  {
    const Op& op = list.op(best);
    out->node = op.node;
    out->text = op.kind == OpKind::Text;
    out->object = op.kind == OpKind::Object;
    out->cursor = op.cursor;
    out->link = 0;
    if (doc) {
      const dom::Node* n = doc->by_serial(op.node);
      if (n && n->type == dom::NodeType::Text) {
        n = n->parent;
        if (n) out->node = n->serial;
      }
      for (const dom::Node* a = n; a; a = a->parent) {
        if (a->is_tag("a") && dom::has_attribute(a, "href")) {
          out->link = a->serial;
          break;
        }
      }
    }
    return true;
  }
}

}  // namespace paint
