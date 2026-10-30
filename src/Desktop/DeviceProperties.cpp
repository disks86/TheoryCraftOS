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

#include "Desktop/DeviceProperties.hpp"

namespace desktop {

using namespace gui;

namespace {
void copy_text(char* out, uint32_t capacity, StringView text) {
  uint32_t n = 0;
  for (; n < text.size() && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}
}  // namespace

bool DevicePropertyList::add(StringView name, StringView value) {
  DeviceProperty p;
  copy_text(p.name, sizeof p.name, name);
  copy_text(p.value, sizeof p.value, value);
  return lines_.push_back(p);
}

DeviceProperties::DeviceProperties(allocators::IAllocator& allocator)
    : Frame(allocator),
      opened_name_(allocator),
      grid_(allocator),
      buttons_(allocator),
      copy_button_(allocator),
      close_button_(allocator),
      status_(allocator),
      lines_(allocator) {
  set_title("Device Properties");
  set_bounds({150, 90, 440, 400});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(90, 0);
  close_button_.on_click(Handler::immediate(&DeviceProperties::on_close, this));
  copy_button_.set_text("Copy Row");
  copy_button_.set_dock(DockStyle::Left);
  copy_button_.set_size(100, 0);
  copy_button_.on_click({&DeviceProperties::on_copy, this});
  buttons_.add(&close_button_);
  buttons_.add(&copy_button_);

  status_.set_dock(DockStyle::Bottom);
  status_.set_size(0, 22);

  grid_.add_column("Property", 150);
  grid_.add_column("Value", 240);
  grid_.set_dock(DockStyle::Fill);
  add(&buttons_);
  add(&status_);
  add(&grid_);   // (Fill goes last)
}

StringView DeviceProperties::cell(uint32_t row, uint32_t column) const {
  if (row >= lines_.count()) return StringView();
  return column == 0 ? StringView(lines_[row].name) : StringView(lines_[row].value);
}

StringView DeviceProperties::value(StringView property) const {
  for (const DeviceProperty& line : lines_)
    if (property.equals(StringView(line.name))) return StringView(line.value);
  return StringView();
}

void DeviceProperties::show_device(HardwareGroup group, uint32_t id) {
  group_ = group;
  id_ = id;
  have_name_ = false;
  update();
}

void DeviceProperties::on_copy(void* self, Control&) { static_cast<DeviceProperties*>(self)->grid_.copy_selected_row(); }
void DeviceProperties::on_close(void* self, Control&) { static_cast<DeviceProperties*>(self)->close(); }

// Makes the table the list: the values of the lines that stay are changed in place (so the selection and the scroll position stay); a different set
// of lines replaces the table.
void DeviceProperties::fill(const DevicePropertyList& list) {
  bool same = lines_.count() == list.count();
  for (uint32_t i = 0; same && i < list.count(); ++i) same = StringView(lines_[i].name).equals(StringView(list.at(i).name));
  const int32_t selected = grid_.selected_row();
  if (!same) {
    grid_.clear_rows();
    lines_.clear();
    for (uint32_t i = 0; i < list.count(); ++i) {
      lines_.push_back(list.at(i));
      const StringView cells[2] = {list.at(i).name, list.at(i).value};
      grid_.add_row(cells, 2);
    }
    if (selected >= 0 && static_cast<uint32_t>(selected) < list.count()) grid_.select_row(selected);
    return;
  }
  for (uint32_t i = 0; i < list.count(); ++i) {
    if (StringView(lines_[i].value).equals(StringView(list.at(i).value))) continue;
    lines_[i] = list.at(i);
    grid_.set_cell(i, 1, list.at(i).value);
  }
}

void DeviceProperties::update() {
  UiGuard lock(this);
  DevicePropertyList list(allocator());
  const bool found = details_ && details_->describe(group_, id_, &list);
  // Another device may have taken the number (a USB device unplugged, the next one moved up): that is not the device this window was opened for.
  const bool gone = found && have_name_ && !opened_name_.view().equals(StringView(list.title));
  if (!found || gone) {
    DevicePropertyList none(allocator());
    fill(none);
    status_.set_text(details_ ? "This device is not there any more." : "No device information is available.");
    return;
  }
  if (!have_name_) {
    opened_name_.assign(list.title);
    have_name_ = true;
  }
  status_.set_text(StringView());
  String title(allocator());
  title.assign(list.title);
  title.append(" - Properties");
  set_title(title.view());
  fill(list);
}

}  // namespace desktop
