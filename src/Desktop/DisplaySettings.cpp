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
#include "Desktop/Format.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
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
  label.set_size(220, 0);
  list.set_dock(DockStyle::Fill);
  row.add(&label);
  row.add(&list);
}

}  // namespace

DisplaySettings::DisplaySettings(allocators::IAllocator& allocator)
    : Frame(allocator),
      map_(allocator),
      body_(allocator),
      buttons_(allocator),
      details_(allocator),
      settings_(allocator),
      resolution_row_(allocator),
      refresh_row_(allocator),
      scale_row_(allocator),
      filler_(allocator),
      spacer_(allocator),
      gap_a_(allocator),
      gap_b_(allocator),
      gap_c_(allocator),
      map_menu_(allocator),
      info_{Label(allocator), Label(allocator), Label(allocator), Label(allocator), Label(allocator)},
      resolution_label_(allocator),
      refresh_label_(allocator),
      scale_label_(allocator),
      resolution_(allocator),
      refresh_(allocator),
      scale_(allocator),
      primary_button_(allocator),
      restore_button_(allocator),
      ok_button_(allocator),
      cancel_button_(allocator),
      apply_button_(allocator) {
  set_title("Display Settings");
  set_bounds({110, 40, 600, 500});
  set_padding(Insets::all(8));
  set_minimize_enabled(false);     // a secondary frame: no Minimize or Maximize
  set_maximize_enabled(false);

  // The buttons: what acts on the monitors at the left, then OK (the default button), Cancel and Apply at the right.
  buttons_.set_size(0, 30);
  primary_button_.set_text("Make Primary");
  primary_button_.set_size(118, 0);
  primary_button_.on_click({&DisplaySettings::on_primary, this});
  restore_button_.set_text("Restore Previous");
  restore_button_.set_size(142, 0);
  restore_button_.on_click({&DisplaySettings::on_restore, this});
  ok_button_.set_text("OK");
  ok_button_.set_size(80, 0);
  ok_button_.set_default(true);
  ok_button_.on_click({&DisplaySettings::on_ok, this});
  cancel_button_.set_text("Cancel");
  cancel_button_.set_size(80, 0);
  cancel_button_.on_click({&DisplaySettings::on_cancel, this});
  apply_button_.set_text("Apply");
  apply_button_.set_size(80, 0);
  apply_button_.on_click({&DisplaySettings::on_apply, this});
  Panel* const gaps[] = {&gap_a_, &gap_b_, &gap_c_};
  for (Panel* gap : gaps) {
    gap->set_style(PanelStyle::Transparent);
    gap->set_size(6, 0);
  }
  spacer_.set_style(PanelStyle::Transparent);
  spacer_.set_dock(DockStyle::Fill);
  buttons_.add(&primary_button_);
  buttons_.add(&gap_a_);
  buttons_.add(&restore_button_);
  buttons_.add(&spacer_);
  buttons_.add(&ok_button_);
  buttons_.add(&gap_b_);
  buttons_.add(&cancel_button_);
  buttons_.add(&gap_c_);
  buttons_.add(&apply_button_);

  // A monitor's commands, for the keyboard and for those who do not drag.
  primary_item_ = map_menu_.add_item("Make Primary", {&DisplaySettings::on_primary, this});
  map_menu_.add_separator();
  move_items_[0] = map_menu_.add_item("Move Left", {&DisplaySettings::on_move_left, this});
  move_items_[1] = map_menu_.add_item("Move Right", {&DisplaySettings::on_move_right, this});
  move_items_[2] = map_menu_.add_item("Move Up", {&DisplaySettings::on_move_up, this});
  move_items_[3] = map_menu_.add_item("Move Down", {&DisplaySettings::on_move_down, this});
  map_menu_.on_opening(Handler::immediate(&DisplaySettings::on_map_menu_opening, this));
  map_.set_context_menu(&map_menu_);

  map_.set_size(0, 190);
  map_.on_selection_changed({&DisplaySettings::on_map_selected, this});
  map_.on_moved({&DisplaySettings::on_map_moved, this});

  details_.set_style(PanelStyle::Transparent);
  details_.set_size(0, 6 + kInfoLines * 20);
  details_.set_padding({0, 4, 0, 0});
  for (Label& line : info_) {
    line.set_dock(DockStyle::Top);
    line.set_size(0, 20);
    details_.add(&line);
  }

  settings_.set_style(PanelStyle::Transparent);
  settings_.set_size(0, 3 * 28 + 4);
  settings_.set_padding({0, 4, 0, 0});
  make_row(resolution_row_, resolution_label_, resolution_, "Resolution:");
  make_row(refresh_row_, refresh_label_, refresh_, "Refresh rate:");
  make_row(scale_row_, scale_label_, scale_, "Size of text and pictures:");
  resolution_.on_selection_changed({&DisplaySettings::on_resolution_chosen, this});
  refresh_.on_selection_changed({&DisplaySettings::on_list_chosen, this});
  scale_.on_selection_changed({&DisplaySettings::on_list_chosen, this});
  settings_.add(&resolution_row_);
  settings_.add(&refresh_row_);
  settings_.add(&scale_row_);

  filler_.set_style(PanelStyle::Transparent);
  filler_.set_dock(DockStyle::Fill);
  body_.set_dock(DockStyle::Fill);
  body_.add(&map_);
  body_.add(&details_);
  body_.add(&settings_);
  body_.add(&filler_);
  body_.add(&buttons_);
  add(&body_);
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
  update_buttons();
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
  if (monitors_->gpu_count() > 1) {   // (the desktop spans several GPUs: which one drives this monitor)
    text.append(", driven by GPU ");
    text.append_uint(m->gpu());
    text.append(" (");
    text.append(monitors_->device_name(m->id()));
    text.push_back(')');
  }
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
  if (asking_) info_[4].set_text("The new settings are waiting for your answer: Keep or Revert.");
  else if (!monitors_->can_set_refresh(m->id())) info_[4].set_text("This display device cannot change the refresh rate.");
  else info_[4].set_text(StringView());
}

void DisplaySettings::update_buttons() {
  const Monitor* m = selected_monitor();
  const int32_t slot = m && monitors_ ? monitors_->layout().find(m->id()) : -1;
  primary_button_.set_enabled(slot >= 0 && !monitors_->layout().at(static_cast<uint32_t>(slot)).primary);
  apply_button_.set_enabled(m != nullptr && !asking_ && pending());   // (nothing to apply: unavailable)
  restore_button_.set_enabled(previous_.valid() && !asking_);
}

bool DisplaySettings::pending() const {
  const Monitor* m = selected_monitor();
  if (!m || !monitors_) return false;
  const int32_t res = resolution_.selected_index(), rate = refresh_.selected_index(), scale = scale_.selected_index();
  if (res < 0 || static_cast<uint32_t>(res) >= resolution_count_) return false;
  const MonitorMode now = m->current_mode();
  if (resolutions_[res].width != now.width || resolutions_[res].height != now.height) return true;
  if (rate >= 0 && static_cast<uint32_t>(rate) < rate_count_ && refresh_.enabled() && rates_[rate] != now.refresh_millihz) return true;
  // (the scale list starts at the largest scale that is not more than the one in use; another choice than that is a change)
  if (scale >= 0 && static_cast<uint32_t>(scale) < scale_count_) {
    const uint32_t in_use = monitors_->scale_of(m->id());
    uint32_t shown = scales_[0];
    for (uint32_t i = 0; i < scale_count_; ++i)
      if (scales_[i] <= in_use) shown = scales_[i];
    if (scales_[scale] != shown) return true;
  }
  return false;
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

void DisplaySettings::on_list_chosen(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  if (!d->filling_) d->update_buttons();
}

void DisplaySettings::on_primary(void* self, Control&) { static_cast<DisplaySettings*>(self)->make_primary(); }
void DisplaySettings::on_apply(void* self, Control&) { static_cast<DisplaySettings*>(self)->apply_settings(); }
void DisplaySettings::on_ok(void* self, Control&) { static_cast<DisplaySettings*>(self)->accept(); }
void DisplaySettings::on_cancel(void* self, Control&) { static_cast<DisplaySettings*>(self)->cancel(); }
void DisplaySettings::on_restore(void* self, Control&) { static_cast<DisplaySettings*>(self)->restore_previous(); }
void DisplaySettings::on_move_left(void* self, Control&) { static_cast<DisplaySettings*>(self)->map_.move_selected(-1, 0); }
void DisplaySettings::on_move_right(void* self, Control&) { static_cast<DisplaySettings*>(self)->map_.move_selected(1, 0); }
void DisplaySettings::on_move_up(void* self, Control&) { static_cast<DisplaySettings*>(self)->map_.move_selected(0, -1); }
void DisplaySettings::on_move_down(void* self, Control&) { static_cast<DisplaySettings*>(self)->map_.move_selected(0, 1); }

void DisplaySettings::on_map_menu_opening(void* self, Control&) {   // what does not apply is unavailable, not missing
  auto* d = static_cast<DisplaySettings*>(self);
  if (d->primary_item_) d->primary_item_->set_enabled(d->primary_button_.enabled());
  for (MenuItem* item : d->move_items_)
    if (item) item->set_enabled(d->map_.box_count() > 1 && d->map_.selected() != MonitorMap::kNone);
}

// Enter is OK and Escape is Cancel (a focused button or list takes Enter first); Ctrl+Z is Restore Previous.
bool DisplaySettings::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    ok_button_.click();
    return true;
  }
  if (args.usage == hardware::key::kA + 25 && (args.modifiers & hardware::modifier::kControl)) {   // Ctrl+Z
    if (restore_button_.enabled()) restore_button_.click();
    return true;
  }
  if (args.usage == hardware::key::kEscape) {
    cancel_button_.click();
    return true;
  }
  return Frame::on_key(args);
}

// OK: applies what is pending (the window then closes once the new settings are kept), or just closes.
void DisplaySettings::accept() {
  if (asking_) return;
  if (!pending()) {
    close();
    return;
  }
  close_when_kept_ = true;
  if (!apply_settings() || !asking_) {      // it did not work (the window stays), or nobody had to be asked
    const bool done = !pending();
    close_when_kept_ = false;
    if (done) close();
  }
}

// Cancel: the choices that were not applied are dropped (what Apply already did stays).
void DisplaySettings::cancel() {
  if (asking_) return;
  fill_lists();
  close();
}

// The close button is not Cancel: with choices that were not applied it asks what to do with them.
bool DisplaySettings::close_requested() {
  if (asking_) return false;      // (the keep question is open: it is answered first)
  if (!pending() || !confirm_ || !manager()) return true;
  question_ = Question::ApplyOnClose;
  confirm_->set_texts("Display Settings", "Apply the changes to the display settings?", "They have not been applied yet.", "Apply", "Cancel");
  confirm_->set_alternate("Don't Apply");
  confirm_->on_result({&DisplaySettings::on_confirm_result, this});
  if (!confirm_->show(*manager())) {
    question_ = Question::None;
    return true;
  }
  return false;
}

bool DisplaySettings::restore_previous() {
  if (!previous_.valid() || !monitors_ || asking_) return false;
  hardware::SystemMonitors::State now;
  monitors_->save(&now);
  const bool ok = monitors_->restore(previous_) && make_real();
  if (!ok) {              // it could not be put back: as it was
    monitors_->restore(now);
    make_real();
  } else {
    previous_ = now;      // (Restore Previous again goes forward again)
  }
  refresh();
  return ok;
}

// --- Commands ----------------------------------------------------------------------------------------

bool DisplaySettings::make_primary() {
  const Monitor* m = selected_monitor();
  if (!m || !monitors_) return false;
  hardware::SystemMonitors::State was;
  monitors_->save(&was);
  if (!monitors_->layout().set_primary(m->id())) return false;
  previous_ = was;
  make_real();
  refresh();
  return true;
}

bool DisplaySettings::move_monitor(uint32_t id, int32_t x, int32_t y) {
  if (!monitors_) return false;
  hardware::SystemMonitors::State was;
  monitors_->save(&was);
  const bool moved = monitors_->move_monitor(id, x, y);
  if (moved) {
    previous_ = was;
    make_real();
  }
  refresh();   // (a monitor that could go nowhere is drawn back where it was)
  return moved;
}

bool DisplaySettings::apply_settings() {
  // (no UI lock here: making the settings real stops the render thread, which needs it)
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
    previous_ = before_;
    before_ = hardware::SystemMonitors::State();
    update_buttons();
    return true;
  }
  asking_ = true;
  seconds_left_ = kConfirmSeconds;
  show_keep_question();
  if (!confirm_->showing()) {      // it could not be asked: kept
    asking_ = false;
    question_ = Question::None;
    previous_ = before_;
    before_ = hardware::SystemMonitors::State();
  }
  update_buttons();
  return true;
}

// "Keep these display settings?" with Keep, More Time and Revert. Revert, the answer that brings back a picture that worked, is the default
// button and has the focus, so Enter and Escape both go back; the line under the question says how long the automatic way back still waits.
void DisplaySettings::show_keep_question() {
  if (!confirm_ || !manager()) return;
  String seconds(allocator()), line(allocator());
  seconds.append_uint(seconds_left_);
  format_message(&line, seconds_left_ == 1 ? "The old ones come back in %1 second." : "The old ones come back in %1 seconds.", seconds.view());
  confirm_->set_texts("Display Settings", "Keep these display settings?", line.view(), "Keep", "Revert");
  confirm_->set_alternate("More Time");
  confirm_->set_default_choice(MessageDialog::Choice::Decline);
  if (confirm_->showing()) return;      // (only the seconds changed)
  question_ = Question::Keep;
  confirm_->on_result({&DisplaySettings::on_confirm_result, this});
  confirm_->show(*manager());
}

void DisplaySettings::more_time() {
  if (!asking_) return;
  seconds_left_ = kMoreSeconds;
  show_keep_question();
}

void DisplaySettings::on_confirm_result(void* self, Control&) {
  auto* d = static_cast<DisplaySettings*>(self);
  const MessageDialog::Choice choice = d->confirm_ ? d->confirm_->choice() : MessageDialog::Choice::Decline;
  const Question question = d->question_;
  if (question == Question::ApplyOnClose) {
    d->question_ = Question::None;
    if (choice == MessageDialog::Choice::Accept) d->accept();           // apply, then close
    else if (choice == MessageDialog::Choice::Alternate) d->cancel();   // close without applying
    return;                                                             // (Cancel: the window stays, the choices too)
  }
  if (!d->asking_) return;
  if (choice == MessageDialog::Choice::Accept) d->keep_changes();
  else if (choice == MessageDialog::Choice::Alternate) d->more_time();
  else d->revert_changes();
}

void DisplaySettings::keep_changes() {
  if (!asking_) return;
  asking_ = false;
  question_ = Question::None;
  previous_ = before_;      // (Restore Previous can still take them back)
  before_ = hardware::SystemMonitors::State();
  if (confirm_ && confirm_->showing()) confirm_->close();
  update_details();
  if (close_when_kept_) {
    close_when_kept_ = false;
    close();
  }
}

void DisplaySettings::revert_changes() {
  const bool was_asking = asking_;
  asking_ = false;
  question_ = Question::None;
  close_when_kept_ = false;      // (OK did not get its settings: the window stays)
  if (confirm_ && confirm_->showing()) confirm_->close();
  if (!was_asking || !before_.valid() || !monitors_) return;
  monitors_->restore(before_);
  before_ = hardware::SystemMonitors::State();
  make_real();
  refresh();
}

// Once a second while the question is open.
void DisplaySettings::tick() {
  if (!asking_) return;
  if (seconds_left_ > 0) --seconds_left_;
  if (seconds_left_ == 0) {
    revert_changes();   // (the dialog closes with it)
    return;
  }
  if (confirm_ && confirm_->showing()) show_keep_question();   // (the seconds in its second line)
}

}  // namespace desktop
