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

#include "Desktop/DisplaySettings.hpp"

namespace desktop {

using namespace gui;
using hardware::Monitor;

DisplaySettings::DisplaySettings(allocators::IAllocator& allocator)
    : Frame(allocator),
      list_(allocator),
      details_(allocator),
      buttons_(allocator),
      info_{Label(allocator), Label(allocator), Label(allocator), Label(allocator), Label(allocator)},
      primary_button_(allocator),
      left_button_(allocator),
      right_button_(allocator),
      close_button_(allocator) {
  set_title("Display Settings");
  set_bounds({110, 70, 580, 360});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(90, 0);
  close_button_.on_click(Handler::immediate(&DisplaySettings::on_close, this));
  primary_button_.set_text("Make Primary");
  primary_button_.set_dock(DockStyle::Left);
  primary_button_.set_size(130, 0);
  primary_button_.on_click({&DisplaySettings::on_primary, this});
  left_button_.set_text("Move Left");
  left_button_.set_dock(DockStyle::Left);
  left_button_.set_size(110, 0);
  left_button_.on_click({&DisplaySettings::on_left, this});
  right_button_.set_text("Move Right");
  right_button_.set_dock(DockStyle::Left);
  right_button_.set_size(110, 0);
  right_button_.on_click({&DisplaySettings::on_right, this});
  buttons_.add(&close_button_);
  buttons_.add(&primary_button_);
  buttons_.add(&left_button_);
  buttons_.add(&right_button_);

  details_.set_style(PanelStyle::Sunken);
  details_.set_dock(DockStyle::Bottom);
  details_.set_size(0, 4 + kInfoLines * 20);
  details_.set_padding(Insets::all(4));
  for (Label& line : info_) {
    line.set_dock(DockStyle::Top);
    line.set_size(0, 20);
    details_.add(&line);
  }

  list_.set_view_mode(ViewMode::Details);
  list_.add_column("Monitor", 190);
  list_.add_column("Connector", 90);
  list_.add_column("Size", 100);
  list_.add_column("Place", 60, Align::Right);
  list_.set_dock(DockStyle::Fill);
  list_.on_selection_changed({&DisplaySettings::on_selection, this});

  add(&buttons_);
  add(&details_);
  add(&list_);   // (Fill goes last)
  refresh();
}

const Monitor* DisplaySettings::selected_monitor() const { return monitors_ ? monitors_->find(selected_id_) : nullptr; }

void DisplaySettings::refresh() {
  UiGuard lock(this);
  const uint32_t keep = selected_id_;   // (clearing the list unselects)
  list_.clear_items();
  if (!monitors_) {
    update_details();
    return;
  }
  int32_t select = -1;
  for (uint32_t i = 0; i < monitors_->count(); ++i) {
    const Monitor& m = monitors_->monitor(i);
    String size(allocator()), place(allocator()), name(allocator());
    const hardware::MonitorMode mode = m.current_mode();
    const int32_t slot = monitors_->layout().find(m.id());
    if (m.connected()) {
      size.append_uint(mode.width);
      size.push_back('x');
      size.append_uint(mode.height);
    } else {
      size.assign("-");
    }
    if (slot >= 0 && monitors_->layout().at(static_cast<uint32_t>(slot)).enabled) place.append_uint(static_cast<uint64_t>(slot) + 1);
    else place.assign("-");
    name.assign(m.name());
    if (slot >= 0 && monitors_->layout().at(static_cast<uint32_t>(slot)).primary) name.append("  (primary)");
    const StringView cells[4] = {name.view(), StringView(m.connector_name()), size.view(), place.view()};
    const int32_t item = list_.add_item(cells, 4, ItemIcon::Drive);
    if (m.id() == keep) select = item;
  }
  if (select < 0 && list_.item_count()) select = 0;
  selected_id_ = select >= 0 ? monitors_->monitor(static_cast<uint32_t>(select)).id() : ~0u;
  if (select >= 0) list_.select_item(select);
  update_details();
}

void DisplaySettings::update_details() {
  for (Label& line : info_) line.set_text(StringView());
  update_buttons();
  const Monitor* m = selected_monitor();
  if (!m) {
    info_[0].set_text(monitors_ ? "Select a monitor." : "No display information is available.");
    return;
  }
  String text(allocator());
  info_[0].set_text(m->name());
  const hardware::EdidInfo* edid = m->edid();
  if (edid) {
    text.assign("Made by ");
    text.append(hardware::edid_vendor_name(edid->manufacturer));
    text.append(", product ");
    text.append_uint(edid->product_code);
    if (edid->serial_text[0]) {
      text.append(", serial ");
      text.append(edid->serial_text);
    }
    if (edid->year) {
      text.append(", ");
      text.append_uint(edid->year);
    }
    info_[1].set_text(text.view());
    if (edid->width_mm) {
      text.assign("Picture ");
      text.append_uint(edid->width_mm);
      text.append(" x ");
      text.append_uint(edid->height_mm);
      text.append(" mm");
      const uint32_t diagonal = edid->diagonal_inches_x10();
      if (diagonal) {
        text.append(", ");
        text.append_uint(diagonal / 10);
        text.push_back('.');
        text.append_uint(diagonal % 10);
        text.append(" inch");
      }
      if (edid->dpi()) {
        text.append(", ");
        text.append_uint(edid->dpi());
        text.append(" dpi");
      }
      info_[2].set_text(text.view());
    } else {
      info_[2].set_text("Physical size not reported.");
    }
    hardware::EdidTiming native;
    if (edid->best_timing(&native)) {
      text.assign("Native ");
      text.append_uint(native.width);
      text.push_back('x');
      text.append_uint(native.height);
      if (native.refresh_millihz) {
        text.append(" at ");
        text.append_uint(native.refresh_millihz / 1000);
        text.push_back('.');
        text.append_uint(native.refresh_millihz % 1000 / 10 / 10);
        text.append_uint(native.refresh_millihz % 1000 / 10 % 10);
        text.append(" Hz");
      }
      text.append(", ");
      text.append_uint(edid->timing_count);
      text.append(edid->timing_count == 1 ? " mode" : " modes");
      info_[3].set_text(text.view());
    }
  } else {
    info_[1].set_text("The monitor did not report anything about itself (no EDID).");
  }
  const int32_t slot = monitors_->layout().find(m->id());
  if (!m->connected()) info_[4].set_text("Not connected.");
  else if (slot < 0) info_[4].set_text("Plugged in after start-up: restart to use it.");
  else if (!monitors_->layout().at(static_cast<uint32_t>(slot)).enabled) info_[4].set_text("Not connected.");
  else if (monitors_->layout().at(static_cast<uint32_t>(slot)).primary) info_[4].set_text("This is the primary monitor.");
  else info_[4].set_text("A secondary monitor.");
}

void DisplaySettings::update_buttons() {
  const Monitor* m = selected_monitor();
  const int32_t slot = m && monitors_ ? monitors_->layout().find(m->id()) : -1;
  const bool usable = slot >= 0 && monitors_->layout().at(static_cast<uint32_t>(slot)).enabled;
  primary_button_.set_enabled(usable && !monitors_->layout().at(static_cast<uint32_t>(slot)).primary);
  left_button_.set_enabled(slot > 0);
  right_button_.set_enabled(slot >= 0 && static_cast<uint32_t>(slot) + 1 < monitors_->layout().count());
}

bool DisplaySettings::make_primary() {
  const Monitor* m = selected_monitor();
  if (!m || !monitors_ || !monitors_->layout().set_primary(m->id())) return false;
  monitors_->reapply();
  refresh();
  fire(changed_);
  return true;
}

bool DisplaySettings::move_selected(int32_t direction) {
  const Monitor* m = selected_monitor();
  if (!m || !monitors_) return false;
  const int32_t slot = monitors_->layout().find(m->id());
  const int32_t target = slot + direction;
  if (slot < 0 || target < 0 || target >= static_cast<int32_t>(monitors_->layout().count())) return false;
  if (!monitors_->layout().move_to_position(m->id(), static_cast<uint32_t>(target))) return false;
  monitors_->reapply();
  refresh();
  fire(changed_);
  return true;
}

void DisplaySettings::on_selection(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  const int32_t item = d->list_.selected_item();
  d->selected_id_ = item >= 0 && d->monitors_ && static_cast<uint32_t>(item) < d->monitors_->count() ? d->monitors_->monitor(static_cast<uint32_t>(item)).id() : ~0u;
  d->update_details();
}

void DisplaySettings::on_primary(void* self, Control&) { static_cast<DisplaySettings*>(self)->make_primary(); }
void DisplaySettings::on_left(void* self, Control&) { static_cast<DisplaySettings*>(self)->move_selected(-1); }
void DisplaySettings::on_right(void* self, Control&) { static_cast<DisplaySettings*>(self)->move_selected(1); }
void DisplaySettings::on_close(void* self, Control&) { static_cast<DisplaySettings*>(self)->close(); }

}  // namespace desktop
