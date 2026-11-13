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

// Reads a display list for the walk below.
struct ListSource {
  const DisplayList& list;
  uint64_t count() const { return list.count(); }
  OpKind kind(uint64_t i) const { return list.op(i).kind; }
  const IRect& rect(uint64_t i) const { return list.op(i).rect; }
  uint32_t node(uint64_t i) const { return list.op(i).node; }
  const Affine& transform(uint64_t i) const { return list.transform(list.op(i).data); }
  const ClipShape& shape(uint64_t i) const { return list.shape(list.op(i).data); }
};

bool is_target(OpKind kind) { return kind == OpKind::Hit || kind == OpKind::Text || kind == OpKind::Object; }
bool is_push(OpKind kind) { return kind == OpKind::PushClip || kind == OpKind::PushClipPath || kind == OpKind::PushMask || kind == OpKind::PushTransform; }

// Calls `on_hit(i)` for every op that answers for an element and holds page position (x, y), back to front (paint order). Front to back is the order of the
// list read from the end, but whether a clip holds the point is known where the clip starts: so the walk goes forward and skips what a clip hides. A transform
// maps the point back into the space of what it holds (through the inverse of its matrix) for as long as it is open. Returns false if `on_hit` did.
template <typename Source, typename OnHit>
bool walk(const Source& source, int32_t x, int32_t y, OnHit&& on_hit) {
  struct Entry {
    bool is_transform = false;
    bool failed = false;        // clip: does not hold the point; transform: cannot be inverted
    double px = 0, py = 0;      // transform: the point before it
  };
  Entry open[96];
  uint32_t depth = 0, failing = 0;   // groups open, and how many of them hide the point (nothing in them can be hit)
  double px = x, py = y;             // the point in the space of the ops being read
  for (uint64_t i = 0; i < source.count(); ++i) {
    const OpKind kind = source.kind(i);
    if (is_push(kind)) {
      Entry e;
      if (kind == OpKind::PushTransform) {
        e.is_transform = true;
        e.px = px;
        e.py = py;
        Affine inverse;
        if (source.transform(i).invert(&inverse)) {
          const double nx = inverse.x_of(px, py), ny = inverse.y_of(px, py);
          px = nx;
          py = ny;
        } else {
          e.failed = true;
        }
      } else if (kind == OpKind::PushClipPath) {
        e.failed = !shape_contains(source.shape(i), px, py);
      } else {
        e.failed = !source.rect(i).contains(floor_to_int(px), floor_to_int(py));
      }
      if (depth < 96) open[depth] = e;
      ++depth;
      if (e.failed) ++failing;
      continue;
    }
    if (kind == OpKind::PopClip || kind == OpKind::PopTransform) {
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
    if (!is_target(kind)) continue;
    if (failing > 0 || !source.node(i) || !source.rect(i).contains(floor_to_int(px), floor_to_int(py))) continue;
    if (!on_hit(i)) return false;
  }
  return true;
}

}  // namespace

// Reads a hit map for the walk.
struct HitMapSource {
  const HitMap& map;
  uint64_t count() const { return map.entries_.count(); }
  OpKind kind(uint64_t i) const { return map.entries_[i].kind; }
  const IRect& rect(uint64_t i) const { return map.entries_[i].rect; }
  uint32_t node(uint64_t i) const { return map.entries_[i].node; }
  const Affine& transform(uint64_t i) const { return map.transforms_[map.entries_[i].data]; }
  const ClipShape& shape(uint64_t i) const { return map.shapes_[map.entries_[i].data]; }
};

bool hit_test(const DisplayList& list, const dom::Document* doc, int32_t x, int32_t y, HitResult* out) {
  uint64_t best = 0;
  bool found = false;
  (void)walk(ListSource{list}, x, y, [&](uint64_t i) {
    best = i;
    found = true;
    return true;
  });
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

void HitMap::clear() {
  entries_.clear();
  transforms_.clear();
  shapes_.clear();
}

bool HitMap::build(const DisplayList& list) {
  clear();
  for (uint64_t i = 0; i < list.count(); ++i) {
    const Op& op = list.op(i);
    const bool keep = is_push(op.kind) || op.kind == OpKind::PopClip || op.kind == OpKind::PopTransform || (is_target(op.kind) && op.node != 0);
    if (!keep) continue;
    Entry e;
    e.kind = op.kind == OpKind::PushMask ? OpKind::PushClip : op.kind;   // (a mask is hit tested as its rectangle, as hit_test() does)
    e.rect = op.rect;
    e.node = op.node;
    bool ok = true;
    if (op.kind == OpKind::PushTransform) {
      e.data = static_cast<uint32_t>(transforms_.count());
      ok = transforms_.push_back(list.transform(op.data));
    } else if (op.kind == OpKind::PushClipPath) {
      e.data = static_cast<uint32_t>(shapes_.count());
      ok = shapes_.push_back(list.shape(op.data));
    }
    if (!ok || !entries_.push_back(e)) {
      clear();
      return false;
    }
  }
  return true;
}

bool HitMap::nodes_at(int32_t x, int32_t y, List<uint32_t>* out) const {
  out->clear();
  List<uint32_t> back_to_front(allocator_);
  if (!walk(HitMapSource{*this}, x, y, [&](uint64_t i) { return back_to_front.push_back(entries_[i].node); })) return false;
  for (uint64_t i = back_to_front.count(); i > 0; --i) {   // (reverse walk: the index itself is needed)
    const uint32_t node = back_to_front[i - 1];
    bool seen = false;
    for (const uint32_t had : out->view()) seen |= had == node;
    if (!seen && !out->push_back(node)) return false;
  }
  return true;
}

}  // namespace paint
