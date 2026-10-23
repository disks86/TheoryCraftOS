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
#include "Paint/Raster.hpp"

namespace paint {

namespace {

int32_t floor_to_int(double v) {
  if (!(v > -1e9)) return -1000000000;
  if (v > 1e9) return 1000000000;
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<double>(i) > v ? i - 1 : i;
}

}  // namespace

bool hit_test(const DisplayList& list, const dom::Document* doc, int32_t x, int32_t y, HitResult* out) {
  // front to back is the order of the list read from the end, but whether a clip holds the point is known where the clip starts: so go forward and
  // keep the last candidate that is not hidden by a clip. A transform maps the point back into the space of what it holds (through the inverse of its
  // matrix) for as long as it is open.
  struct Entry {
    bool is_transform = false;
    bool failed = false;        // clip: does not hold the point; transform: cannot be inverted
    double px = 0, py = 0;      // transform: the point before it
  };
  Entry open[96];
  uint32_t depth = 0, failing = 0;   // groups open, and how many of them hide the point (nothing in them can be hit)
  double px = x, py = y;             // the point in the space of the ops being read
  uint64_t best = 0;
  bool found = false;
  for (uint64_t i = 0; i < list.count(); ++i) {
    const Op& op = list.op(i);
    if (op.kind == OpKind::PushClip || op.kind == OpKind::PushClipPath || op.kind == OpKind::PushTransform) {
      Entry e;
      if (op.kind == OpKind::PushTransform) {
        e.is_transform = true;
        e.px = px;
        e.py = py;
        Affine inverse;
        if (list.transform(op.data).invert(&inverse)) {
          const double nx = inverse.x_of(px, py), ny = inverse.y_of(px, py);
          px = nx;
          py = ny;
        } else {
          e.failed = true;
        }
      } else if (op.kind == OpKind::PushClipPath) {
        e.failed = !shape_contains(list.shape(op.data), px, py);
      } else {
        e.failed = !op.rect.contains(floor_to_int(px), floor_to_int(py));
      }
      if (depth < 96) open[depth] = e;
      ++depth;
      if (e.failed) ++failing;
      continue;
    }
    if (op.kind == OpKind::PopClip || op.kind == OpKind::PopTransform) {
      if (depth > 0) {
        --depth;
        if (depth < 96) {
          if (open[depth].failed && failing > 0) --failing;
          if (open[depth].is_transform) {
            px = open[depth].px;
            py = open[depth].py;
          }
        }
      }
      continue;
    }
    if (op.kind != OpKind::Hit && op.kind != OpKind::Text && op.kind != OpKind::Object) continue;
    if (failing > 0 || !op.node || !op.rect.contains(floor_to_int(px), floor_to_int(py))) continue;
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
    out->rect = op.rect;
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
