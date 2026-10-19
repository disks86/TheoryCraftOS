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
#include "Gui/Window.hpp"

namespace desktop {

using namespace gui;
using hardware::Monitor;
using hardware::MonitorMode;

namespace {

void append_rate(String* out, uint32_t millihz) {
  if (millihz == 0) {
    out->append("rate not reported");
    return;
  }
  out->append_uint(millihz / 1000);
  out->push_back('.');
  out->append_uint(millihz % 1000 / 100);
  out->append_uint(millihz % 100 / 10);
  out->append(" Hz");
}

void append_scale(String* out, uint32_t percent) {
  out->append_uint(percent);
  out->append("% (");
  out->append_uint(96u * percent / 100);
  out->append(" dpi)");
}

void make_row(Panel& row, Label& label, DropDown& list, const char* text) {
  row.set_style(PanelStyle::Transparent);
  row.set_dock(DockStyle::Top);
  row.set_size(0, 28);
  label.set_text(text);
  label.set_dock(DockStyle::Left);
  label.set_size(140, 0);
  list.set_dock(DockStyle::Fill);
  row.add(&label);
  row.add(&list);
}

}  // namespace

DisplaySettings::DisplaySettings(allocators::IAllocator& allocator)
    : Frame(allocator),
      map_(allocator),
      details_(allocator),
      settings_(allocator),
      resolution_row_(allocator),
      refresh_row_(allocator),
      scale_row_(allocator),
      buttons_(allocator),
      info_{Label(allocator), Label(allocator), Label(allocator), Label(allocator), Label(allocator)},
      resolution_label_(allocator),
      refresh_label_(allocator),
      scale_label_(allocator),
      resolution_(allocator),
      refresh_(allocator),
      scale_(allocator),
      primary_button_(allocator),
      apply_button_(allocator),
      close_button_(allocator) {
  set_title("Display Settings");
  set_bounds({110, 40, 600, 500});
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
  apply_button_.set_text("Apply");
  apply_button_.set_dock(DockStyle::Left);
  apply_button_.set_size(100, 0);
  apply_button_.on_click({&DisplaySettings::on_apply, this});
  buttons_.add(&close_button_);
  buttons_.add(&primary_button_);
  buttons_.add(&apply_button_);

  map_.set_dock(DockStyle::Top);
  map_.set_size(0, 190);
  map_.on_selection_changed({&DisplaySettings::on_map_selected, this});
  map_.on_moved({&DisplaySettings::on_map_moved, this});

  details_.set_style(PanelStyle::Transparent);
  details_.set_dock(DockStyle::Top);
  details_.set_size(0, 6 + kInfoLines * 20);
  details_.set_padding({0, 4, 0, 0});
  for (Label& line : info_) {
    line.set_dock(DockStyle::Top);
    line.set_size(0, 20);
    details_.add(&line);
  }

  settings_.set_style(PanelStyle::Transparent);
  settings_.set_dock(DockStyle::Top);
  settings_.set_size(0, 3 * 28 + 4);
  settings_.set_padding({0, 4, 0, 0});
  make_row(resolution_row_, resolution_label_, resolution_, "Resolution");
  make_row(refresh_row_, refresh_label_, refresh_, "Refresh rate");
  make_row(scale_row_, scale_label_, scale_, "Scale (DPI)");
  resolution_.on_selection_changed({&DisplaySettings::on_resolution_chosen, this});
  settings_.add(&resolution_row_);
  settings_.add(&refresh_row_);
  settings_.add(&scale_row_);

  add(&buttons_);
  add(&map_);
  add(&details_);
  add(&settings_);
  refresh();
}

const Monitor* DisplaySettings::selected_monitor() const { return monitors_ && selected_id_ != MonitorMap::kNone ? monitors_->find(selected_id_) : nullptr; }

bool DisplaySettings::make_real() { return applier_ ? applier_(applier_context_) : true; }

// --- Showing ------------------------------------------------------------------------------------------

void DisplaySettings::refresh() {
  UiGuard lock(this);
  const uint32_t keep = selected_id_;
  filling_ = true;
  map_.clear();
  if (monitors_) {
    const hardware::MonitorLayout& layout = monitors_->layout();
    for (uint32_t i = 0; i < layout.count(); ++i) {
      const hardware::MonitorSlot& s = layout.at(i);
      const Monitor* m = monitors_->find(s.id);
      if (!s.enabled || !m) continue;   // (the monitors that are plugged in)
      map_.add_box(s.id, s.rect, m->name(), s.primary);
    }
  }
  selected_id_ = MonitorMap::kNone;
  for (uint32_t i = 0; i < map_.box_count(); ++i)
    if (map_.box_id(i) == keep) selected_id_ = keep;
  if (selected_id_ == MonitorMap::kNone && map_.box_count()) selected_id_ = map_.box_id(0);
  map_.select(selected_id_);
  filling_ = false;
  fill_lists();
  update_details();
}

void DisplaySettings::fill_lists() {
  filling_ = true;
  resolution_.clear_items();
  resolution_count_ = 0;
  const Monitor* m = selected_monitor();
  if (m) {
    resolution_count_ = monitors_->compatible_resolutions(m->id(), resolutions_, kMaxResolutions);
    const MonitorMode now = m->current_mode();
    int32_t current = 0;
    for (uint32_t i = 0; i < resolution_count_; ++i) {
      String text(allocator());
      text.append_uint(resolutions_[i].width);
      text.append(" x ");
      text.append_uint(resolutions_[i].height);
      if (resolutions_[i].preferred) text.append("  (native)");
      resolution_.add_item(text.view());
      if (resolutions_[i].width == now.width && resolutions_[i].height == now.height) current = static_cast<int32_t>(i);
    }
    resolution_.set_selected_index(resolution_count_ ? current : -1);
  }
  filling_ = false;
  fill_dependent_lists();
}

void DisplaySettings::fill_dependent_lists() {
  filling_ = true;
  refresh_.clear_items();
  scale_.clear_items();
  rate_count_ = scale_count_ = 0;
  const Monitor* m = selected_monitor();
  const int32_t chosen = resolution_.selected_index();
  if (m && chosen >= 0 && static_cast<uint32_t>(chosen) < resolution_count_) {
    const MonitorMode& mode = resolutions_[chosen];
    const MonitorMode now = m->current_mode();
    rate_count_ = monitors_->compatible_refresh_rates(m->id(), mode.width, mode.height, rates_, kMaxRates);
    int32_t current_rate = 0;
    for (uint32_t i = 0; i < rate_count_; ++i) {
      String text(allocator());
      append_rate(&text, rates_[i]);
      refresh_.add_item(text.view());
      if (rates_[i] == now.refresh_millihz) current_rate = static_cast<int32_t>(i);
    }
    refresh_.set_selected_index(rate_count_ ? current_rate : -1);
    refresh_.set_enabled(monitors_->can_set_refresh(m->id()) && rate_count_ > 1);   // (a device that cannot set the rate, or one rate: nothing to choose)
    scale_count_ = monitors_->compatible_scales(m->id(), scales_, kMaxScales, mode.width, mode.height);
    const uint32_t scale_now = monitors_->scale_of(m->id());
    int32_t current_scale = 0;
    for (uint32_t i = 0; i < scale_count_; ++i) {
      String text(allocator());
      append_scale(&text, scales_[i]);
      scale_.add_item(text.view());
      if (scales_[i] <= scale_now) current_scale = static_cast<int32_t>(i);   // (the largest that is not more than the scale in use)
    }
    scale_.set_selected_index(scale_count_ ? current_scale : -1);
  }
  filling_ = false;
}

void DisplaySettings::update_details() {
  for (Label& line : info_) line.set_text(StringView());
  update_buttons();
  const Monitor* m = selected_monitor();
  if (!m) {
    info_[0].set_text(monitors_ ? "No monitor is connected." : "No display information is available.");
    return;
  }
  const int32_t slot = monitors_->layout().find(m->id());
  String text(allocator());
  text.assign(m->name());
  text.append(" on ");
  text.append(m->connector_name()[0] ? StringView(m->connector_name()) : StringView("the display"));
  if (slot >= 0 && monitors_->layout().at(static_cast<uint32_t>(slot)).primary) text.append(" - the primary monitor");
  info_[0].set_text(text.view());
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
      const uint32_t dpi = edid->dpi();
      if (dpi) {
        text.append(", ");
        text.append_uint(dpi);
        text.append(" dpi");
      }
      info_[2].set_text(text.view());
    } else {
      info_[2].set_text("Physical size not reported.");
    }
  } else {
    info_[1].set_text("The monitor did not report anything about itself (no EDID).");
  }
  const MonitorMode now = m->current_mode();
  text.assign("Showing ");
  text.append_uint(now.width);
  text.append(" x ");
  text.append_uint(now.height);
  text.append(", ");
  append_rate(&text, now.refresh_millihz);
  text.append(", scale ");
  append_scale(&text, monitors_->scale_of(m->id()));
  info_[3].set_text(text.view());
  if (asking_) info_[4].set_text("Keep the new settings? They go back soon.");
  else if (!monitors_->can_set_refresh(m->id())) info_[4].set_text("This display device cannot change the refresh rate.");
  else info_[4].set_text(StringView());
}

void DisplaySettings::update_buttons() {
  const Monitor* m = selected_monitor();
  const int32_t slot = m && monitors_ ? monitors_->layout().find(m->id()) : -1;
  primary_button_.set_enabled(slot >= 0 && !monitors_->layout().at(static_cast<uint32_t>(slot)).primary);
  apply_button_.set_enabled(m != nullptr && !asking_);
}

// --- Events --------------------------------------------------------------------------------------------

void DisplaySettings::on_map_selected(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  if (d->filling_) return;
  d->selected_id_ = d->map_.selected();
  d->fill_lists();
  d->update_details();
}

void DisplaySettings::on_map_moved(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  d->move_monitor(d->map_.moved_id(), d->map_.moved_x(), d->map_.moved_y());
}

void DisplaySettings::on_resolution_chosen(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  if (!d->filling_) d->fill_dependent_lists();
}

void DisplaySettings::on_primary(void* self, Control&) { static_cast<DisplaySettings*>(self)->make_primary(); }
void DisplaySettings::on_apply(void* self, Control&) { static_cast<DisplaySettings*>(self)->apply_settings(); }
void DisplaySettings::on_close(void* self, Control&) { static_cast<DisplaySettings*>(self)->close(); }

// --- Commands ----------------------------------------------------------------------------------------

bool DisplaySettings::make_primary() {
  const Monitor* m = selected_monitor();
  if (!m || !monitors_ || !monitors_->layout().set_primary(m->id())) return false;
  make_real();
  refresh();
  return true;
}

bool DisplaySettings::move_monitor(uint32_t id, int32_t x, int32_t y) {
  if (!monitors_) return false;
  const bool moved = monitors_->move_monitor(id, x, y);
  if (moved) make_real();
  refresh();   // (a monitor that could go nowhere is drawn back where it was)
  return moved;
}

bool DisplaySettings::apply_settings() {
  UiGuard lock(this);
  const Monitor* m = selected_monitor();
  if (!m || !monitors_ || asking_) return false;
  const uint32_t id = m->id();
  const int32_t res = resolution_.selected_index(), rate = refresh_.selected_index(), scale = scale_.selected_index();
  if (res < 0 || static_cast<uint32_t>(res) >= resolution_count_) return false;
  const MonitorMode now = m->current_mode();
  const MonitorMode& wanted = resolutions_[res];
  const bool new_size = wanted.width != now.width || wanted.height != now.height;
  const bool new_rate = rate >= 0 && static_cast<uint32_t>(rate) < rate_count_ && refresh_.enabled() && rates_[rate] != now.refresh_millihz && !new_size;
  const bool new_scale = scale >= 0 && static_cast<uint32_t>(scale) < scale_count_ && scales_[scale] != monitors_->scale_of(id);
  if (!new_size && !new_rate && !new_scale) return true;   // (nothing to change)
  monitors_->save(&before_);
  bool ok = true;
  if (new_size) ok = monitors_->set_resolution(id, wanted.width, wanted.height);
  if (ok && (new_rate || (new_size && rate >= 0 && static_cast<uint32_t>(rate) < rate_count_ && refresh_.enabled() && rates_[rate] != wanted.refresh_millihz))) ok = monitors_->set_refresh(id, rates_[rate]);
  if (ok && new_scale) ok = monitors_->set_scale(id, scales_[scale]);
  if (!ok || !make_real()) {   // it did not work: everything goes back
    monitors_->restore(before_);
    make_real();
    before_ = hardware::SystemMonitors::State();
    refresh();
    return false;
  }
  refresh();
  if (!confirm_ || !manager()) {   // nobody to ask: the settings are kept
    before_ = hardware::SystemMonitors::State();
    return true;
  }
  asking_ = true;
  seconds_left_ = kConfirmSeconds;
  confirm_->set_texts("Display Settings", "Keep these display settings?", "They go back in 15 seconds.", "Keep", "Revert");
  confirm_->on_result({&DisplaySettings::on_confirm_result, this});
  if (!confirm_->show(*manager())) {
    asking_ = false;
    before_ = hardware::SystemMonitors::State();
  }
  update_buttons();
  return true;
}

void DisplaySettings::on_confirm_result(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  if (!d->asking_) return;
  if (d->confirm_ && d->confirm_->accepted()) d->keep_changes();
  else d->revert_changes();
}

void DisplaySettings::keep_changes() {
  UiGuard lock(this);
  if (!asking_) return;
  asking_ = false;
  before_ = hardware::SystemMonitors::State();
  if (confirm_ && confirm_->showing()) confirm_->close();
  update_details();
}

void DisplaySettings::revert_changes() {
  UiGuard lock(this);
  const bool was_asking = asking_;
  asking_ = false;
  if (confirm_ && confirm_->showing()) confirm_->close();
  if (!was_asking || !before_.valid() || !monitors_) return;
  monitors_->restore(before_);
  before_ = hardware::SystemMonitors::State();
  make_real();
  refresh();
}

// Once a second while the question is open.
void DisplaySettings::tick() {
  UiGuard lock(this);
  if (!asking_) return;
  if (seconds_left_ > 0) --seconds_left_;
  if (seconds_left_ == 0) {
    revert_changes();   // (the dialog closes with it)
    return;
  }
  if (confirm_ && confirm_->showing()) {
    String text(allocator());
    text.assign("They go back in ");
    text.append_uint(seconds_left_);
    text.append(seconds_left_ == 1 ? " second." : " seconds.");
    confirm_->set_texts("Display Settings", "Keep these display settings?", text.view(), "Keep", "Revert");
  }
}

}  // namespace desktop
