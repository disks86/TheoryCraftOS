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

#include "Gui/NoticeDialog.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr int32_t kIconSize = 32, kLineHeight = 22, kButtonHeight = 28, kFrameOverhead = 56 + 10;
}

void NoticeIcon::paint(Painter& painter) {
  uint32_t fill, ink;
  const char* glyph;
  switch (kind_) {
    case NoticeKind::Error: fill = 0xFFC62828u; ink = 0xFFFFFFFFu; glyph = "X"; break;
    case NoticeKind::Warning: fill = 0xFFF9C02Du; ink = 0xFF000000u; glyph = "!"; break;
    default: fill = 0xFF1565C0u; ink = 0xFFFFFFFFu; glyph = "i"; break;
  }
  const int32_t x = (static_cast<int32_t>(bounds_.width) - kIconSize) / 2, y = 0;
  const Rect2D badge{x, y, static_cast<uint32_t>(kIconSize), static_cast<uint32_t>(kIconSize)};
  painter.fill(badge, fill);
  painter.outline(badge, painter.theme().text);
  painter.text_centered(badge, glyph, ink);
}

NoticeDialog::NoticeDialog(allocators::IAllocator& allocator)
    : Frame(allocator), icon_(allocator), lines_{Label(allocator), Label(allocator), Label(allocator), Label(allocator)},
      buttons_(allocator), gap_(allocator), ok_(allocator), message_(allocator), storage_(allocator) {
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(10));

  icon_.set_dock(DockStyle::Left);
  icon_.set_size(kIconSize + 12, 0);
  for (Label& line : lines_) {
    line.set_dock(DockStyle::Top);
    line.set_size(0, kLineHeight);
  }
  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, kButtonHeight);
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Fill);
  ok_.set_dock(DockStyle::Right);
  ok_.set_size(104, 0);
  ok_.set_text("OK");
  ok_.on_click(Handler::immediate(&NoticeDialog::on_ok, this));
  buttons_.add(&ok_);
  buttons_.add(&gap_);

  add(&buttons_);
  add(&icon_);
  for (Label& line : lines_) add(&line);
}

uint32_t NoticeDialog::wrap(StringView text, StringView* lines, uint32_t max_lines, String* storage) {
  // Lines are slices of `storage`, which holds the text of each line one after the other.
  storage->clear();
  uint32_t count = 0;
  uint64_t pos = 0;
  const uint64_t n = text.size();
  uint64_t starts[kMaxLines] = {}, lengths[kMaxLines] = {};
  if (max_lines > static_cast<uint32_t>(kMaxLines)) max_lines = kMaxLines;
  while (pos < n && count < max_lines) {
    while (pos < n && text[pos] == ' ') ++pos;
    if (pos >= n) break;
    uint64_t end = pos, last_space = StringView::npos;
    while (end < n && end - pos < static_cast<uint64_t>(kLineChars) && text[end] != '\n') {
      if (text[end] == ' ') last_space = end;
      ++end;
    }
    uint64_t next;
    if (end >= n) {
      next = end;
    } else if (text[end] == '\n' || text[end] == ' ') {
      next = end + 1;
    } else if (last_space != StringView::npos && last_space > pos) {
      end = last_space;
      next = last_space + 1;
    } else {
      next = end;  // one long word: broken in the middle
    }
    uint64_t rest = next;
    while (rest < n && (text[rest] == ' ' || text[rest] == '\n')) ++rest;
    const bool more = rest < n;
    starts[count] = storage->size();
    if (more && count + 1 == max_lines) {  // the last line available: what does not fit is replaced by "..."
      const uint64_t keep = end - pos > static_cast<uint64_t>(kLineChars) - 3 ? static_cast<uint64_t>(kLineChars) - 3 : end - pos;
      storage->append(text.substr(pos, keep));
      storage->append("...");
      lengths[count++] = keep + 3;
      break;
    }
    storage->append(text.substr(pos, end - pos));
    lengths[count++] = end - pos;
    pos = next;
  }
  for (uint32_t i = 0; i < count; ++i) lines[i] = storage->view().substr(starts[i], lengths[i]);
  return count;
}

bool NoticeDialog::show(WindowManager& manager, NoticeKind kind, StringView message) {
  UiGuard lock(&manager);
  kind_ = kind;
  message_.assign(message);
  StringView wrapped[kMaxLines];
  const uint32_t count = wrap(message_.view(), wrapped, kMaxLines, &storage_);
  icon_.set_kind(kind);
  set_title(kind == NoticeKind::Error ? "Error" : kind == NoticeKind::Warning ? "Warning" : "Information");
  for (uint32_t i = 0; i < kMaxLines; ++i) lines_[i].set_text(i < count ? wrapped[i] : StringView());
  const int32_t shown_lines = count < 2 ? 2 : static_cast<int32_t>(count);
  for (uint32_t i = 0; i < kMaxLines; ++i) lines_[i].set_size(0, static_cast<int32_t>(i) < shown_lines ? kLineHeight : 0);

  const int32_t w = kWidth, h = kFrameOverhead + shown_lines * kLineHeight + kButtonHeight + 8;
  const int32_t area_w = static_cast<int32_t>(manager.bounds().width), area_h = static_cast<int32_t>(manager.bounds().height);
  set_bounds({(area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0, (area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0, static_cast<uint32_t>(w),
              static_cast<uint32_t>(h)});
  if (showing()) {  // already open: the new text replaces the old
    if (Window* win = window()) win->layout();
    activate();
    return true;
  }
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&ok_);
  }
  return true;
}

void NoticeDialog::on_ok(void* self, Control&) { static_cast<NoticeDialog*>(self)->close(); }

bool NoticeDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    ok_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace gui
