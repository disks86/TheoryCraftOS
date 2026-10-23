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

// Inline formatting (CSS 2.1 section 9.4.2 and chapter 10.8): the inline content of a block container is cut into tokens (words, spaces, the
// edges of inline boxes, atomic inlines, forced breaks), the tokens are broken into lines greedily, and each line gets its fragments, its
// height from the line-height of everything on it and the baselines of what sits on it, and its alignment.
#include "Layout/LayoutInternal.hpp"

namespace layout {

using browser::Style;

namespace {

bool is_space_char(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || static_cast<uint8_t>(c) >= 0x80; }

// What white-space (Style::white_space: 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line, 5 break-spaces) does to text.
struct WhiteSpace {
  bool collapse_spaces, preserve_newlines, wrap;
  explicit WhiteSpace(const Style& s) {
    const uint8_t ws = s.pre && s.white_space == 0 ? 2 : s.white_space;
    collapse_spaces = ws == 0 || ws == 1 || ws == 4;
    preserve_newlines = ws == 2 || ws == 3 || ws == 4 || ws == 5;
    wrap = ws == 0 || ws == 3 || ws == 4 || ws == 5;
  }
};

char transformed(char c, uint8_t transform, bool word_start) {
  if (transform == 1 && c >= 'a' && c <= 'z') return static_cast<char>(c - 32);
  if (transform == 2 && c >= 'A' && c <= 'Z') return static_cast<char>(c + 32);
  if (transform == 3 && word_start && c >= 'a' && c <= 'z') return static_cast<char>(c - 32);
  return c;
}

}  // namespace

LayoutUnit LayoutContext::line_height_of(const Style& style, const TextMetrics& metrics) const {
  if (style.line_height_px100 >= 0) return LayoutUnit::from_px100(style.line_height_px100);
  return metrics.height() + metrics.line_gap;
}

LayoutUnit LayoutContext::layout_atomic(BoxId id, LayoutUnit available) {
  WidthRequest request;
  request.shrink = true;
  request.available = available;
  layout_block(id, available, request);
  const Box& b = tree_.box(id);
  return b.margin.left + b.width + b.margin.right;
}

// Makes the tokens of the inline content of a block container.
struct InlineCollector {
  LayoutContext& ctx;
  BoxTree& tree;
  LayoutUnit available;
  TokenMode mode;
  List<Token>* tokens;
  bool last_space = true;   // the last thing emitted was a collapsible space, or nothing: another space collapses into it
  uint8_t valign = 0;
  bool ok = true;

  InlineCollector(LayoutContext& c, LayoutUnit avail, TokenMode m, List<Token>* out) : ctx(c), tree(c.tree_), available(avail), mode(m), tokens(out) {}

  void push(const Token& t) { ok &= tokens->push_back(t); }

  void walk(BoxId id) {
    Box& b = tree.box(id);
    if (b.floated && mode == TokenMode::Layout) {   // a float leaves the line: it is placed when the line it is on is made
      Token t;
      t.type = Token::Type::Float;
      t.box = id;
      push(t);
      return;
    }
    if (b.out_of_flow) {   // it leaves the line: placed against its containing block later, from where it would have been
      if (mode == TokenMode::Layout) {
        Token t;
        t.type = Token::Type::Abs;
        t.box = id;
        push(t);
      }
      return;
    }
    if (b.floated) {   // (a float in a measurement counts as an atomic inline: it takes room)
      atomic(id);
      return;
    }
    switch (b.kind) {
      case BoxKind::Text: text(id); break;
      case BoxKind::Inline: inline_box(id); break;
      case BoxKind::InlineBlock:
      case BoxKind::Replaced:
      case BoxKind::Block:
      case BoxKind::Anonymous: atomic(id); break;
      case BoxKind::LineBreak: {
        Token t;
        t.type = Token::Type::Break;
        t.box = id;
        push(t);
        last_space = true;
        break;
      }
      default: break;   // (a marker is not in the line)
    }
  }

  void inline_box(BoxId id) {
    Box& b = tree.box(id);
    ctx.resolve_edges(b, available);
    const uint8_t saved = valign;
    if (b.style.vertical_align != 0) valign = b.style.vertical_align;
    Token open;
    open.type = Token::Type::Open;
    open.box = id;
    open.width = b.margin.left + b.border.left + b.padding.left;
    open.vertical_align = valign;
    push(open);
    for (uint64_t i = 0; i < b.children.count(); ++i) walk(b.children[i]);
    Token close;
    close.type = Token::Type::Close;
    close.box = id;
    close.width = b.margin.right + b.border.right + b.padding.right;
    close.vertical_align = valign;
    push(close);
    valign = saved;
  }

  void atomic(BoxId id) {
    Token t;
    t.type = Token::Type::Atomic;
    t.box = id;
    t.width = mode == TokenMode::Layout ? ctx.layout_atomic(id, available) : ctx.atomic_outer_width(id, mode == TokenMode::MeasureMax);
    t.vertical_align = tree.box(id).style.vertical_align != 0 ? tree.box(id).style.vertical_align : valign;
    push(t);
    last_space = false;
  }

  // Text: white space collapses (or not) as the style says, the rest is cut into words, after hyphens too. The box keeps the collapsed
  // text, which the tokens and later the fragments point into.
  void text(BoxId id) {
    Box& b = tree.box(id);
    const Style& s = b.style;
    const WhiteSpace ws(s);
    String source = util::move(b.text);
    String out(tree.allocator());
    uint64_t word_start = 0;
    bool in_word = false, word_initial = true;
    auto flush_word = [&](bool break_after) {
      if (!in_word) return;
      in_word = false;
      emit_word(id, out, word_start, out.size() - word_start, break_after && ws.wrap, !ws.wrap);
    };
    const StringView src = source.view();
    for (uint64_t i = 0; i < src.size(); ++i) {
      const char c = src[i];
      if (is_space_char(c)) {
        if (c == '\n' && ws.preserve_newlines) {
          flush_word(false);
          Token t;
          t.type = Token::Type::Break;
          t.box = id;
          push(t);
          last_space = true;
          word_initial = true;
          continue;
        }
        if (ws.collapse_spaces || c == '\n') {
          flush_word(false);
          if (!last_space) {
            const uint64_t at = out.size();
            out.push_back(' ');
            Token t;
            t.type = Token::Type::Space;
            t.box = id;
            t.offset = static_cast<uint32_t>(at);
            t.length = 1;
            t.width = ctx.space_width(s);
            t.no_break = !ws.wrap;
            t.vertical_align = valign;
            push(t);
            last_space = true;
          }
          word_initial = true;
          continue;
        }
        // preserved white space (pre, pre-wrap, break-spaces): a tab runs to the next multiple of 8 columns, counted from this box's start
        flush_word(false);
        const uint64_t spaces = c == '\t' ? 8 - (out.size() % 8) : 1;
        const uint64_t at = out.size();
        for (uint64_t k = 0; k < spaces; ++k) out.push_back(' ');
        emit_word(id, out, at, spaces, ws.wrap, !ws.wrap);
        last_space = false;
        word_initial = true;
        continue;
      }
      if (!in_word) {
        in_word = true;
        word_start = out.size();
      }
      out.push_back(transformed(c, s.transform, word_initial));
      word_initial = false;
      last_space = false;
      if (c == '-' && i + 1 < src.size() && i > 0 && is_alnum(src[i - 1]) && is_alnum(src[i + 1])) flush_word(true);   // a line may end after a hyphen
    }
    flush_word(false);
    b.text = util::move(out);
  }

  // A word of the box's text at [offset, offset + length): measured, and cut into pieces that fit when the style lets words break anywhere.
  void emit_word(BoxId id, const String& text, uint64_t offset, uint64_t length, bool break_after, bool no_break) {
    const Style& s = tree.box(id).style;
    const StringView whole = text.view().substr(offset, length);
    LayoutUnit width = ctx.measurer_.width(s, whole);
    if (s.word_break && mode == TokenMode::Layout && width > available && length > 1 && available > LayoutUnit()) {
      uint64_t start = 0;
      while (start < length) {
        uint64_t end = start + 1;   // at least one character per piece (a UTF-8 character is kept whole)
        while (end < length && (static_cast<uint8_t>(whole[end]) & 0xC0) == 0x80) ++end;
        for (;;) {
          uint64_t next = end + 1;
          while (next < length && (static_cast<uint8_t>(whole[next]) & 0xC0) == 0x80) ++next;
          if (next > length || ctx.measurer_.width(s, whole.substr(start, next - start)) > available) break;
          end = next;
        }
        Token t;
        t.type = Token::Type::Word;
        t.box = id;
        t.offset = static_cast<uint32_t>(offset + start);
        t.length = static_cast<uint32_t>(end - start);
        t.width = ctx.measurer_.width(s, whole.substr(start, end - start));
        t.break_after = end < length || break_after;
        t.no_break = no_break;
        t.vertical_align = valign;
        push(t);
        start = end;
      }
      return;
    }
    Token t;
    t.type = Token::Type::Word;
    t.box = id;
    t.offset = static_cast<uint32_t>(offset);
    t.length = static_cast<uint32_t>(length);
    t.width = width;
    t.break_after = break_after;
    t.no_break = no_break;
    t.vertical_align = valign;
    push(t);
  }
};

bool LayoutContext::collect_tokens(BoxId container, LayoutUnit available, TokenMode mode, List<Token>* tokens) {
  InlineCollector collector(*this, available, mode, tokens);
  const uint64_t count = tree_.box(container).children.count();
  for (uint64_t i = 0; i < count && collector.ok; ++i) collector.walk(tree_.box(container).children[i]);
  return collector.ok;
}

LayoutUnit LayoutContext::layout_inline_content(BoxId id, LayoutUnit content_width) {
  Box& c = tree_.box(id);
  const Style& style = c.style;
  List<Token> tokens(tree_.allocator());
  if (!collect_tokens(id, content_width, TokenMode::Layout, &tokens)) {
    failed_ = true;
    return LayoutUnit();
  }
  const WhiteSpace ws(style);
  const uint64_t n = tokens.count();
  const LayoutUnit indent = LayoutUnit::from_px100(style.text_indent_px100);
  // the container's content box in the coordinates of the block formatting context (where floats are kept)
  const LayoutUnit content_left = origin_x_ + c.content_left();
  const LayoutUnit content_top = origin_y_ + c.content_top();

  struct OpenBox {
    BoxId box;
    LayoutUnit start;
    bool starts;
  };
  struct Placed {
    uint8_t kind;   // 0 text, 1 inline box, 2 atomic
    BoxId box;
    uint32_t offset, length;
    LayoutUnit x, width, above, below, lh_height;
    uint8_t valign;
    bool starts, ends;
  };
  List<OpenBox> open(tree_.allocator());
  List<BoxId> deferred(tree_.allocator());   // floats met on a line that already has content: they go at the top of the next line
  const TextMetrics strut_metrics = measurer_.metrics(style);
  const LayoutUnit strut_line = line_height_of(style, strut_metrics);
  const LayoutUnit strut_half_leading = (strut_line - strut_metrics.height()) / 2;
  const LayoutUnit strut_above = strut_metrics.ascent + strut_half_leading, strut_below = strut_line - strut_above;
  LayoutUnit cursor_y;
  uint64_t line_number = 0;

  // spaces before the first word of a line collapse away (the edges of inline boxes stay and take room); returns where the line's content starts
  auto skip_leading = [&](uint64_t at, LayoutUnit* edges) {
    uint64_t j = at;
    for (; j < n; ++j) {
      Token& t = tokens[j];
      if (t.type == Token::Type::Space && !t.no_break) {
        t.hidden = true;
        t.width = LayoutUnit();
      } else if (t.type == Token::Type::Open || t.type == Token::Type::Close) {
        *edges += t.width;
      } else if (!(t.type == Token::Type::Float && t.hidden)) {
        break;
      }
    }
    return j;
  };
  LayoutUnit leading_edges;
  uint64_t next_line = 0;   // where the next line's tokens start (its edges of inline boxes included)

  while (!failed_ && skip_leading(next_line, &leading_edges) < n) {
    // --- one line: the room beside the floats at its top, then as many chunks as fit
    uint64_t line_start = next_line;
    uint64_t end = line_start;
    bool forced = false;
    LayoutUnit avail_width, line_offset;
    for (uint64_t attempt = 0; attempt < 128; ++attempt) {
      LayoutUnit band_left = content_left, band_right = content_left + content_width;
      if (floats_) floats_->band(content_top + cursor_y, strut_line, content_left, content_left + content_width, &band_left, &band_right);
      avail_width = band_right - band_left - (line_number == 0 ? indent : LayoutUnit());
      line_offset = band_left - content_left;
      LayoutUnit line_width;
      bool has_content = false, restart = false;
      forced = false;
      LayoutUnit edges;
      uint64_t j = skip_leading(line_start, &edges);
      line_width = edges;
      uint64_t chunk_pos = j;
      end = chunk_pos;
      while (chunk_pos < n && !restart) {
        // a float at the start of a chunk is placed now (at the top of this line, if nothing is on it yet) or after the line
        if (tokens[chunk_pos].type == Token::Type::Float) {
          Token& ft = tokens[chunk_pos];
          if (!has_content && floats_) {
            place_float(ft.box, id, content_top + cursor_y);
            ft.hidden = true;
            restart = true;   // (the room beside the line changed)
            break;
          }
          failed_ |= !deferred.push_back(ft.box);
          ft.hidden = true;
          ++chunk_pos;
          end = chunk_pos;
          continue;
        }
        uint64_t k = chunk_pos;
        LayoutUnit chunk_width, trailing;
        bool chunk_forced = false, content_in_chunk = false;
        for (; k < n; ++k) {
          const Token& t = tokens[k];
          if (t.type == Token::Type::Break) {
            ++k;
            chunk_forced = true;
            break;
          }
          if (t.type == Token::Type::Float) break;
          chunk_width += t.width;
          if (t.type == Token::Type::Word || t.type == Token::Type::Atomic) content_in_chunk = true;
          if (t.type == Token::Type::Space && !t.no_break) {
            trailing = t.width;
            ++k;
            break;
          }
          if (break_after_token(tokens, k, ws.wrap)) {
            ++k;
            break;
          }
        }
        if (line_width + chunk_width - trailing <= avail_width || !has_content) {
          if (!has_content && line_width + chunk_width - trailing > avail_width && floats_ && !floats_->empty()) {
            // nothing fits beside the floats: the line goes down to where there is more room
            const LayoutUnit next = floats_->next_edge_below(content_top + cursor_y);
            if (!next.is_max() && next > content_top + cursor_y) {
              cursor_y = next - content_top;
              restart = true;
              break;
            }
          }
          line_width += chunk_width;
          has_content = has_content || content_in_chunk;
          chunk_pos = k;
          end = k;
          if (chunk_forced) {
            forced = true;
            break;
          }
        } else {
          break;   // the chunk goes on the next line
        }
      }
      if (!restart) break;
    }
    // the spaces at the end of the line hang: they take no room
    for (uint64_t j = end; j > line_start; --j) {
      Token& t = tokens[j - 1];
      if (t.type == Token::Type::Open || t.type == Token::Type::Close || (t.type == Token::Type::Float && t.hidden)) continue;
      if (t.type == Token::Type::Space && !t.no_break) {
        t.hidden = true;
        t.width = LayoutUnit();
      }
      break;
    }
    bool any = forced;
    for (uint64_t j = line_start; j < end && !any; ++j)
      any = !tokens[j].hidden && (tokens[j].type == Token::Type::Word || tokens[j].type == Token::Type::Atomic || tokens[j].type == Token::Type::Space);
    if (end == line_start) {   // (nothing could be placed: only float tokens were handled)
      next_line = line_start + 1;   // (a line always takes one chunk, so this does not happen; it keeps the loop finite)
      continue;
    }
    if (!any) {   // a line of nothing but the edges of inline boxes: no line box (the boxes stay open for the next line)
      for (uint64_t j = line_start; j < end; ++j)   // (positioned boxes on it still need their place)
        if (tokens[j].type == Token::Type::Abs) register_absolute(tokens[j].box, id, c.content_left() + line_offset, c.content_top() + cursor_y);
      next_line = end;
      continue;
    }

    // --- its fragments
    List<Placed> placed(tree_.allocator());
    LayoutUnit x;
    for (uint64_t k = 0; k < open.count(); ++k) open[k].start = LayoutUnit();
    for (uint64_t k = line_start; k < end; ++k) {
      const Token& t = tokens[k];
      if (t.hidden) continue;
      switch (t.type) {
        case Token::Type::Open: {
          OpenBox o;
          o.box = t.box;
          o.start = x;
          o.starts = true;
          failed_ |= !open.push_back(o);
          x += t.width;
          break;
        }
        case Token::Type::Close: {
          if (open.count() > 0) {
            const OpenBox o = open[open.count() - 1];
            open.remove_at(open.count() - 1);
            Placed p{};
            p.kind = 1;
            p.box = o.box;
            p.x = o.start;
            p.width = x + t.width - o.start;
            p.starts = o.starts;
            p.ends = true;
            p.valign = t.vertical_align;
            failed_ |= !placed.push_back(p);
          }
          x += t.width;
          break;
        }
        case Token::Type::Word:
        case Token::Type::Space: {
          if (placed.count() > 0) {
            Placed& last = placed[placed.count() - 1];
            if (last.kind == 0 && last.box == t.box && last.offset + last.length == t.offset) {
              last.length += t.length;
              last.width += t.width;
              x += t.width;
              break;
            }
          }
          Placed p{};
          p.kind = 0;
          p.box = t.box;
          p.offset = t.offset;
          p.length = t.length;
          p.x = x;
          p.width = t.width;
          p.valign = t.vertical_align;
          failed_ |= !placed.push_back(p);
          x += t.width;
          break;
        }
        case Token::Type::Atomic: {
          Placed p{};
          p.kind = 2;
          p.box = t.box;
          p.x = x;
          p.width = t.width;
          p.valign = t.vertical_align;
          failed_ |= !placed.push_back(p);
          x += t.width;
          break;
        }
        case Token::Type::Abs: {
          Placed p{};
          p.kind = 3;
          p.box = t.box;
          p.x = x;
          failed_ |= !placed.push_back(p);
          break;
        }
        case Token::Type::Break:
        case Token::Type::Float: break;
      }
    }
    // inline boxes still open at the end of the line carry on in the next one
    for (uint64_t k = 0; k < open.count(); ++k) {
      if (x > open[k].start || open[k].starts) {
        Placed p{};
        p.kind = 1;
        p.box = open[k].box;
        p.x = open[k].start;
        p.width = x - open[k].start;
        p.starts = open[k].starts;
        p.ends = false;
        failed_ |= !placed.push_back(p);
      }
      open[k].starts = false;
    }

    // --- its height: how far each fragment reaches above and below the baseline (or, for top and bottom alignment, how tall it is)
    LayoutUnit baseline_pos = strut_above, below_max = strut_below;
    LayoutUnit top_tall, bottom_tall;
    for (uint64_t k = 0; k < placed.count(); ++k) {
      Placed& p = placed[k];
      if (p.kind == 3) continue;
      Box& pb = tree_.box(p.box);
      if (p.kind == 2) {
        const LayoutUnit baseline = pb.has_baseline ? pb.baseline : pb.height;
        p.above = pb.margin.top + baseline;
        p.below = (pb.height - baseline) + pb.margin.bottom;
      } else {
        const TextMetrics m = measurer_.metrics(pb.style);
        const LayoutUnit lh = line_height_of(pb.style, m);
        const LayoutUnit half = (lh - m.height()) / 2;
        p.above = m.ascent + half;
        p.below = lh - p.above;
      }
      p.lh_height = p.above + p.below;
      if (p.valign == 1) {
        top_tall = max(top_tall, p.lh_height);
      } else if (p.valign == 3) {
        bottom_tall = max(bottom_tall, p.lh_height);
      } else {
        if (p.valign == 2) {   // middle: the box's centre is half an x-height above the baseline
          const LayoutUnit half_x = LayoutUnit::from_px100(static_cast<int64_t>(style.font_px) * 25);
          const LayoutUnit half = p.lh_height / 2;
          p.above = half + half_x;
          p.below = max(p.lh_height - p.above, LayoutUnit());
        }
        baseline_pos = max(baseline_pos, p.above);
        below_max = max(below_max, p.below);
      }
    }
    LayoutUnit line_height = baseline_pos + below_max;
    if (top_tall > line_height) {
      below_max += top_tall - line_height;
      line_height = top_tall;
    }
    if (bottom_tall > line_height) {
      baseline_pos += bottom_tall - line_height;
      line_height = bottom_tall;
    }

    // --- its alignment within the room beside the floats (justified text is left aligned for now)
    const LayoutUnit room = avail_width - x;
    LayoutUnit shift = line_offset + (line_number == 0 ? indent : LayoutUnit());
    if (room > LayoutUnit()) {
      if (style.align == 1) shift += room / 2;
      else if (style.align == 2) shift += room;
    }

    LineInfo info;
    info.x = c.content_left() + shift;
    info.y = c.content_top() + cursor_y;
    info.width = x;
    info.height = line_height;
    info.baseline = baseline_pos;
    info.first_fragment = static_cast<uint32_t>(c.fragments.count());
    for (uint64_t k = 0; k < placed.count(); ++k) {
      const Placed& p = placed[k];
      Box& pb = tree_.box(p.box);
      if (p.kind == 3) {   // a positioned box: no fragment; where it would have been is where this line is
        register_absolute(p.box, id, info.x + p.x, info.y);
        continue;
      }
      Fragment f;
      f.box = p.box;
      f.text_offset = p.offset;
      f.text_length = p.length;
      f.x = info.x + p.x;
      f.width = p.width;
      f.starts_box = p.starts;
      f.ends_box = p.ends;
      LayoutUnit top;   // the top of the fragment's line-height box, from the top of the line
      if (p.valign == 1) top = LayoutUnit();
      else if (p.valign == 3) top = line_height - p.lh_height;
      else top = baseline_pos - p.above;
      if (p.kind == 2) {
        f.y = info.y + top + pb.margin.top;
        f.height = pb.height;
        pb.x = f.x + pb.margin.left;
        pb.y = f.y;
        apply_relative(pb);
        f.x = pb.x;
        f.y = pb.y;
        f.width = pb.width;
      } else {
        const TextMetrics m = measurer_.metrics(pb.style);
        const LayoutUnit lh = line_height_of(pb.style, m);
        const LayoutUnit half = (lh - m.height()) / 2;
        f.y = info.y + top + half;
        f.height = m.height();
        if (p.kind == 1) {   // an inline box's rectangle takes in its padding and border
          f.y -= pb.padding.top + pb.border.top;
          f.height += pb.padding.vertical() + pb.border.vertical();
        }
      }
      failed_ |= !c.fragments.push_back(f);
    }
    info.fragment_count = static_cast<uint32_t>(c.fragments.count()) - info.first_fragment;
    failed_ |= !c.lines.push_back(info);
    cursor_y += line_height;
    ++line_number;

    // floats that came after content on that line start at the top of the next one
    if (floats_) {
      for (uint64_t k = 0; k < deferred.count(); ++k) place_float(deferred[k], id, content_top + cursor_y);
    }
    deferred.clear();
    next_line = end;
  }
  // floats at the very end of the content (after the last line)
  if (floats_) {
    for (uint64_t k = 0; k < deferred.count(); ++k) place_float(deferred[k], id, content_top + cursor_y);
  }
  return cursor_y;
}

}  // namespace layout
