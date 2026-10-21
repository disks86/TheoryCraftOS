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

#include "Browser/HtmlDocument.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/Entities.hpp"

namespace browser {

bool HtmlDocument::add_run(StringView text, uint8_t flags, uint8_t level, uint8_t indent, uint8_t breaks, int32_t link, uint8_t align, uint32_t color,
                           uint32_t background, uint32_t node) {
  if (runs_.count() >= kMaxRuns || pool_.size() + text.size() > kMaxText) {
    truncated_ = true;
    return true;   // (dropped: not an error, the page is just cut off)
  }
  Run run;
  run.offset = static_cast<uint32_t>(pool_.size());
  run.length = static_cast<uint32_t>(text.size());
  run.flags = flags;
  run.level = level;
  run.indent = indent;
  run.breaks = breaks;
  run.align = align;
  run.link = link;
  run.color = color;
  run.background = background;
  run.node = node;
  return pool_.append(text) && runs_.push_back(run);
}

bool HtmlDocument::add_object(uint8_t kind, uint32_t w, uint32_t h, uint32_t pct, uint8_t indent, uint8_t breaks, int32_t link, uint8_t align, uint32_t background, uint32_t node) {
  if (runs_.count() >= kMaxRuns) {
    truncated_ = true;
    return true;
  }
  Run run;
  run.offset = static_cast<uint32_t>(pool_.size());
  run.indent = indent;
  run.breaks = breaks;
  run.align = align;
  run.link = link;
  run.background = background;
  run.node = node;
  run.object = kind;
  run.object_w = static_cast<uint16_t>(w > 8192 ? 8192 : w < 1 ? 1 : w);
  run.object_h = static_cast<uint16_t>(h > 8192 ? 8192 : h < 1 ? 1 : h);
  run.object_pct = static_cast<uint8_t>(pct > 100 ? 100 : pct);
  return runs_.push_back(run);
}

int32_t HtmlDocument::add_link(StringView target) {
  if (links_.count() >= kMaxRuns || pool_.size() + target.size() > kMaxText) return -1;
  LinkRef ref{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(target.size())};
  if (!pool_.append(target) || !links_.push_back(ref)) return -1;
  return static_cast<int32_t>(links_.count() - 1);
}

void HtmlDocument::clear() {
  page_background_ = 0;
  runs_.clear();
  links_.clear();
  pool_.clear();
  title_.clear();
  truncated_ = false;
}

bool HtmlDocument::parse_text(StringView text) {
  clear();
  String shown(allocator_);
  for (uint64_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '\r') {
      ++i;
    } else if (c == '\t') {
      shown.append("    ");
      ++i;
    } else if (c == '\n' || (c >= 32 && c < 127)) {
      shown.push_back(c);
      ++i;
    } else if (static_cast<uint8_t>(c) >= 0x80) {
      const char* look = dom::ascii_look_alike(dom::next_code_point(text, &i));
      shown.append(look ? StringView(look) : StringView("?"));
    } else {
      ++i;   // (other control characters are dropped)
    }
  }
  if (shown.size() && shown[shown.size() - 1] == '\n') shown.truncate(shown.size() - 1);
  if (shown.empty()) return true;
  return add_run(shown.view(), kRunPre, 0, 0, 0, -1, 0, 0, 0, 0);
}

bool HtmlDocument::parse_html(StringView html, StringView extra_css, IScriptRunner* scripts) {
  clear();
  dom::Document doc(allocator_);
  if (!dom::build_document(doc, html, scripts)) return false;
  RenderOptions options;
  options.extra_css = extra_css;
  if (!render_document(doc, options, this)) {
    clear();
    return false;
  }
  return true;
}

}  // namespace browser
