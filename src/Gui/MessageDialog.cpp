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

#include "Gui/MessageDialog.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

MessageDialog::MessageDialog(allocators::IAllocator& allocator)
    : Frame(allocator), line1_(allocator), line2_(allocator), buttons_(allocator), gap_(allocator), accept_(allocator), decline_(allocator) {
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(10));

  line1_.set_dock(DockStyle::Top);
  line1_.set_size(0, 22);
  line2_.set_dock(DockStyle::Top);
  line2_.set_size(0, 22);

  // Buttons along the bottom, right-aligned: the first one added sits at the far right, so the
  // decline button goes first and the default one ends up on its left.
  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 28);
  gap_.set_style(PanelStyle::Transparent);
  decline_.set_dock(DockStyle::Right);
  decline_.set_size(104, 0);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(10, 0);
  accept_.set_dock(DockStyle::Right);
  accept_.set_size(104, 0);
  decline_.on_click(Handler::immediate(&MessageDialog::on_decline, this));
  accept_.on_click(Handler::immediate(&MessageDialog::on_accept, this));
  buttons_.add(&decline_);
  buttons_.add(&gap_);
  buttons_.add(&accept_);

  add(&line1_);
  add(&line2_);
  add(&buttons_);
}

void MessageDialog::set_texts(StringView title, StringView line1, StringView line2, StringView accept, StringView decline) {
  UiGuard lock(this);
  set_title(title);
  line1_.set_text(line1);
  line2_.set_text(line2);
  accept_.set_text(accept);
  decline_.set_text(decline);
}

bool MessageDialog::show(WindowManager& manager) {
  UiGuard lock(&manager);
  if (showing()) {
    activate();
    return true;
  }
  __atomic_store_n(&accepted_, false, __ATOMIC_RELEASE);
  const int32_t w = kWidth, h = kHeight;
  const int32_t area_w = static_cast<int32_t>(manager.bounds().width), area_h = static_cast<int32_t>(manager.bounds().height);
  set_bounds({(area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0, (area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0, static_cast<uint32_t>(w),
              static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&accept_);
  }
  return true;
}

void MessageDialog::on_accept(void* self, Control&) {
  auto* d = static_cast<MessageDialog*>(self);
  __atomic_store_n(&d->accepted_, true, __ATOMIC_RELEASE);
  d->close();
}

void MessageDialog::on_decline(void* self, Control&) {
  auto* d = static_cast<MessageDialog*>(self);
  __atomic_store_n(&d->accepted_, false, __ATOMIC_RELEASE);
  d->close();
}

bool MessageDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {  // a focused button handles Enter itself; this is for anything else
    accept_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace gui
