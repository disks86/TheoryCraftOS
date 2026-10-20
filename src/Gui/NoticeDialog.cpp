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
#include "Gui/ClipboardText.hpp"
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
      top_(allocator), buttons_(allocator), gap_(allocator), copy_bar_(allocator), copy_gap_(allocator), ok_(allocator), toggle_(allocator), copy_(allocator),
      details_(allocator), fallback_(allocator), message_(allocator), storage_(allocator) {
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(10));

  top_.set_style(PanelStyle::Transparent);
  top_.set_dock(DockStyle::Top);
  icon_.set_dock(DockStyle::Left);
  icon_.set_size(kIconSize + 12, 0);
  for (Label& line : lines_) {
    line.set_dock(DockStyle::Top);
    line.set_size(0, kLineHeight);
  }
  top_.add(&icon_);
  for (Label& line : lines_) top_.add(&line);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, kButtonHeight);
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Fill);
  ok_.set_dock(DockStyle::Right);
  ok_.set_size(104, 0);
  ok_.set_text("OK");
  ok_.on_click(Handler::immediate(&NoticeDialog::on_ok, this));
  toggle_.set_dock(DockStyle::Left);
  toggle_.set_size(136, 0);
  toggle_.on_click(Handler::immediate(&NoticeDialog::on_toggle, this));
  toggle_.set_visible(false);
  buttons_.add(&ok_);
  buttons_.add(&toggle_);
  buttons_.add(&gap_);

  copy_bar_.set_style(PanelStyle::Transparent);
  copy_bar_.set_dock(DockStyle::Bottom);
  copy_bar_.set_size(0, 0);
  copy_.set_dock(DockStyle::Left);
  copy_.set_size(176, 0);
  copy_.set_text("Copy to clipboard");
  copy_.on_click(Handler::immediate(&NoticeDialog::on_copy, this));
  copy_gap_.set_style(PanelStyle::Transparent);
  copy_gap_.set_dock(DockStyle::Fill);
  copy_bar_.add(&copy_);
  copy_bar_.add(&copy_gap_);
  copy_bar_.set_visible(false);
  details_.set_read_only(true);
  details_.set_dock(DockStyle::Fill);
  details_.set_visible(false);

  add(&copy_bar_);
  add(&top_);
  add(&buttons_);
  add(&details_);   // Fill goes last
}

namespace {
// The message cut into lines of at most `columns` characters (words stay whole unless one is longer than a line), keeping its own line breaks.
void wrap_for_details(StringView text, uint32_t columns, String* out) {
  out->clear();
  uint64_t pos = 0;
  while (pos <= text.size()) {
    uint64_t end = text.find('\n', pos);
    if (end == StringView::npos) end = text.size();
    StringView line = text.substr(pos, end - pos);
    while (line.size() > columns) {
      uint64_t cut = columns;
      for (uint64_t i = columns; i > columns / 2; --i)
        if (line[i] == ' ') { cut = i; break; }
      out->append(line.first(cut));
      out->push_back('\n');
      line = line.substr(cut);
      while (line.starts_with(' ') && line.size() > 1 && cut != columns) line = line.substr(1);
    }
    out->append(line);
    if (end < text.size()) out->push_back('\n');
    pos = end + 1;
  }
}
}  // namespace

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
  shown_lines_ = count < 2 ? 2 : static_cast<int32_t>(count);
  for (uint32_t i = 0; i < kMaxLines; ++i) lines_[i].set_size(0, static_cast<int32_t>(i) < shown_lines_ ? kLineHeight : 0);
  // A message that did not fit the lines gets the details area, open to begin with.
  has_details_ = count == static_cast<uint32_t>(kMaxLines) && wrapped[count - 1].ends_with("...");
  details_open_ = true;
  if (has_details_) {
    String text(allocator());
    wrap_for_details(message_.view(), kDetailColumns, &text);
    details_.set_text(text.view());
  } else {
    details_.set_text(StringView());
  }
  arrange(manager);
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

// Sizes and places the dialog for the lines and the details it has now.
void NoticeDialog::arrange(WindowManager& manager) {
  const bool open = has_details_ && details_open_;
  toggle_.set_visible(has_details_);
  toggle_.set_text(open ? "Hide Details" : "Show Details");
  details_.set_visible(open);
  copy_bar_.set_visible(open);
  top_.set_size(0, shown_lines_ * kLineHeight);
  buttons_.set_dock(has_details_ ? DockStyle::Top : DockStyle::Bottom);
  copy_bar_.set_size(0, open ? kButtonHeight + 6 : 0);
  int32_t h = kFrameOverhead + shown_lines_ * kLineHeight + kButtonHeight + 8;
  if (open) h += kDetailsHeight + kButtonHeight + 12;
  else if (has_details_) h += 6;
  const int32_t w = has_details_ ? kWideWidth : kWidth;
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0),
              static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
}

void NoticeDialog::set_details_open(bool open) {
  if (!has_details_ || !manager()) return;
  UiGuard lock(this);
  details_open_ = open;
  arrange(*manager());
  if (Window* win = window()) win->layout();
}

bool NoticeDialog::copy_message() { return clipboard_put_text(clipboard(), &fallback_, message_.view()); }

void NoticeDialog::on_toggle(void* self, Control&) {
  auto* d = static_cast<NoticeDialog*>(self);
  d->set_details_open(!d->details_open_);
}
void NoticeDialog::on_copy(void* self, Control&) { static_cast<NoticeDialog*>(self)->copy_message(); }
void NoticeDialog::on_ok(void* self, Control&) { static_cast<NoticeDialog*>(self)->close(); }

bool NoticeDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    ok_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace gui
