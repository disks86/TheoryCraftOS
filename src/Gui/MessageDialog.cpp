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
#include "Utility/String.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

MessageDialog::MessageDialog(allocators::IAllocator& allocator)
    : Frame(allocator), line1_(allocator), line2_(allocator), buttons_(allocator), gap_(allocator), gap2_(allocator), accept_(allocator), decline_(allocator),
      alternate_(allocator) {
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
  // decline button goes first and the default one ends up on the left (with the third button, when there is one, between them).
  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 28);
  gap_.set_style(PanelStyle::Transparent);
  decline_.set_dock(DockStyle::Right);
  decline_.set_size(104, 0);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(10, 0);
  alternate_.set_dock(DockStyle::Right);
  alternate_.set_size(104, 0);
  alternate_.set_visible(false);
  gap2_.set_style(PanelStyle::Transparent);
  gap2_.set_dock(DockStyle::Right);
  gap2_.set_size(10, 0);
  gap2_.set_visible(false);
  accept_.set_dock(DockStyle::Right);
  accept_.set_size(104, 0);
  accept_.set_default(true);
  decline_.on_click(Handler::immediate(&MessageDialog::on_decline, this));
  alternate_.on_click(Handler::immediate(&MessageDialog::on_alternate, this));
  accept_.on_click(Handler::immediate(&MessageDialog::on_accept, this));
  buttons_.add(&decline_);
  buttons_.add(&gap_);
  buttons_.add(&alternate_);
  buttons_.add(&gap2_);
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
  set_alternate(StringView());
  set_default_choice(Choice::Accept);
}

void MessageDialog::set_default_choice(Choice choice) {
  UiGuard lock(this);
  default_ = choice;
  accept_.set_default(choice == Choice::Accept);
  alternate_.set_default(choice == Choice::Alternate);
  decline_.set_default(choice == Choice::Decline);
}

void MessageDialog::set_alternate(StringView text) {
  UiGuard lock(this);
  alternate_.set_text(text);
  alternate_.set_visible(!text.empty());
  gap2_.set_visible(!text.empty());
}

void MessageDialog::set_save_changes(StringView document) {
  UiGuard lock(this);
  String line(allocator());
  line.append("Save the changes to \"");
  line.append(document.size() > 24 ? document.first(24) : document);   // (the line has room for 45 characters)
  if (document.size() > 24) line.append("...");
  line.append("\"?");
  set_texts(document, line.view(), "Changes that are not saved are lost.", "Save", "Cancel");
  set_alternate("Don't Save");
}

bool MessageDialog::show(WindowManager& manager) {
  UiGuard lock(&manager);
  if (showing()) {
    activate();
    return true;
  }
  __atomic_store_n(&choice_, static_cast<uint8_t>(Choice::Decline), __ATOMIC_RELEASE);
  const int32_t w = kWidth, h = kHeight;
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0), static_cast<uint32_t>(w),
              static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&default_button());
  }
  return true;
}

void MessageDialog::finish(Choice choice) {
  __atomic_store_n(&choice_, static_cast<uint8_t>(choice), __ATOMIC_RELEASE);
  close();
}

void MessageDialog::on_accept(void* self, Control&) { static_cast<MessageDialog*>(self)->finish(Choice::Accept); }
void MessageDialog::on_decline(void* self, Control&) { static_cast<MessageDialog*>(self)->finish(Choice::Decline); }
void MessageDialog::on_alternate(void* self, Control&) { static_cast<MessageDialog*>(self)->finish(Choice::Alternate); }

bool MessageDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {  // a focused button handles Enter itself; this is for anything else
    default_button().click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace gui
