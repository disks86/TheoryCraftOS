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

#include "Desktop/PrintDialog.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;
using hardware::Fit;
using hardware::Halftone;
using hardware::PaperSize;
using hardware::PrinterState;

PrintDialog::PrintDialog(allocators::IAllocator& allocator)
    : Frame(allocator),
      options_panel_(allocator),
      preview_panel_(allocator),
      page_row_(allocator),
      printer_row_(allocator),
      paper_row_(allocator),
      orientation_row_(allocator),
      range_row_(allocator),
      quality_row_(allocator),
      copies_row_(allocator),
      toner_row_(allocator),
      picture_row_(allocator),
      stretch_row_(allocator),
      button_row_(allocator),
      gap_(allocator),
      printer_label_(allocator),
      paper_label_(allocator),
      orientation_label_(allocator),
      range_label_(allocator),
      to_label_(allocator),
      quality_label_(allocator),
      copies_label_(allocator),
      picture_label_(allocator),
      page_label_(allocator),
      status_(allocator),
      message_(allocator),
      printer_(allocator),
      paper_(allocator),
      quality_(allocator),
      picture_type_(allocator),
      portrait_(allocator),
      landscape_(allocator),
      toner_(allocator),
      stretch_(allocator),
      all_pages_(allocator),
      page_range_(allocator),
      from_page_(allocator),
      to_page_(allocator),
      copies_(allocator),
      previous_(allocator),
      next_(allocator),
      preview_(allocator),
      layout_(allocator),
      text_(allocator),
      print_(allocator),
      cancel_(allocator) {
  set_title("Print");
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(8));

  // Buttons along the bottom, with the message beside them.
  button_row_.set_style(PanelStyle::Transparent);
  button_row_.set_dock(DockStyle::Bottom);
  button_row_.set_size(0, 32);
  button_row_.set_padding(Insets{0, 4, 0, 0});
  cancel_.set_text("Cancel");
  cancel_.set_dock(DockStyle::Right);
  cancel_.set_size(92, 0);
  cancel_.on_click({&PrintDialog::on_cancel, this});
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(8, 0);
  print_.set_text("Print");
  print_.set_dock(DockStyle::Right);
  print_.set_size(92, 0);
  print_.on_click({&PrintDialog::on_print, this});
  message_.set_dock(DockStyle::Fill);
  button_row_.add(&cancel_);
  button_row_.add(&gap_);
  button_row_.add(&print_);
  button_row_.add(&message_);

  // The rows, from the top: a caption on the left and the control after it.
  struct Row {
    Panel& row;
    Label* label;
    const char* text;
    int32_t height;
  };
  const Row rows[] = {{printer_row_, &printer_label_, "Printer:", 30},      {paper_row_, &paper_label_, "Paper size:", 30},
                      {orientation_row_, &orientation_label_, "Orientation:", 30}, {range_row_, &range_label_, "Pages:", 30},
                      {quality_row_, &quality_label_, "Quality:", 30},
                      {copies_row_, &copies_label_, "Copies:", 30},        {toner_row_, nullptr, nullptr, 26},
                      {picture_row_, &picture_label_, "Picture as:", 30},  {stretch_row_, nullptr, nullptr, 26}};
  // The status line sits under the printer row, so the printer row goes first and the others after the status label.
  status_.set_dock(DockStyle::Top);
  status_.set_size(0, 22);
  for (const Row& r : rows) {
    r.row.set_style(PanelStyle::Transparent);
    r.row.set_dock(DockStyle::Top);
    r.row.set_size(0, r.height);
    r.row.set_padding(Insets{0, 2, 0, 2});
    if (r.label) {
      r.label->set_text(r.text);
      r.label->set_dock(DockStyle::Left);
      r.label->set_size(112, 0);
      r.row.add(r.label);
    } else {
      r.row.set_padding(Insets{112, 2, 0, 2});   // a check box lines up with the controls
    }
  }
  printer_.set_dock(DockStyle::Fill);
  printer_.on_selection_changed({&PrintDialog::on_printer, this});
  printer_row_.add(&printer_);
  paper_.set_dock(DockStyle::Left);
  paper_.set_size(180, 0);
  paper_row_.add(&paper_);
  portrait_.set_text("Portrait");
  portrait_.set_dock(DockStyle::Left);
  portrait_.set_size(100, 0);
  portrait_.set_group(&orientation_group_);
  landscape_.set_text("Landscape");
  landscape_.set_dock(DockStyle::Left);
  landscape_.set_size(120, 0);
  landscape_.set_group(&orientation_group_);
  orientation_row_.add(&portrait_);
  orientation_row_.add(&landscape_);
  quality_.set_dock(DockStyle::Left);
  quality_.set_size(180, 0);
  quality_row_.add(&quality_);
  copies_.set_range(1, 99);
  copies_.set_dock(DockStyle::Left);
  copies_.set_size(84, 0);
  copies_row_.add(&copies_);
  toner_.set_text("Save toner");
  toner_.set_dock(DockStyle::Fill);
  toner_row_.add(&toner_);
  picture_type_.set_dock(DockStyle::Left);
  picture_type_.set_size(180, 0);
  picture_type_.add_item("Photograph (smooth tones)");
  picture_type_.add_item("Drawing (sharp lines)");
  picture_type_.add_item("Pattern (regular dots)");
  picture_row_.add(&picture_type_);
  stretch_.set_text("Stretch to fill the page");
  stretch_.set_dock(DockStyle::Fill);
  stretch_row_.add(&stretch_);

  // The pages to print: all, or from one to another.
  all_pages_.set_text("All");
  all_pages_.set_dock(DockStyle::Left);
  all_pages_.set_size(56, 0);
  all_pages_.set_group(&range_group_);
  all_pages_.set_checked(true);
  all_pages_.on_changed({&PrintDialog::on_range_mode, this});
  page_range_.set_text("From");
  page_range_.set_dock(DockStyle::Left);
  page_range_.set_size(72, 0);
  page_range_.set_group(&range_group_);
  from_page_.set_dock(DockStyle::Left);
  from_page_.set_size(70, 0);
  from_page_.set_range(1, 1);
  from_page_.on_value_changed({&PrintDialog::on_from, this});
  to_label_.set_text(" to ");
  to_label_.set_dock(DockStyle::Left);
  to_label_.set_size(36, 0);
  to_page_.set_dock(DockStyle::Left);
  to_page_.set_size(70, 0);
  to_page_.set_range(1, 1);
  to_page_.on_value_changed({&PrintDialog::on_to, this});
  from_page_.set_enabled(false);
  to_page_.set_enabled(false);
  range_row_.add(&all_pages_);
  range_row_.add(&page_range_);
  range_row_.add(&from_page_);
  range_row_.add(&to_label_);
  range_row_.add(&to_page_);

  // Changing what the sheet looks like changes the preview and the pages.
  portrait_.on_changed({&PrintDialog::on_layout_changed, this});
  landscape_.on_changed({&PrintDialog::on_layout_changed, this});
  paper_.on_selection_changed({&PrintDialog::on_layout_changed, this});
  picture_type_.on_selection_changed({&PrintDialog::on_picture_changed, this});
  stretch_.on_changed({&PrintDialog::on_picture_changed, this});

  // The preview on the right, with the page buttons under it.
  preview_panel_.set_style(PanelStyle::Transparent);
  preview_panel_.set_dock(DockStyle::Fill);
  preview_panel_.set_padding(Insets{8, 0, 0, 0});
  page_row_.set_style(PanelStyle::Transparent);
  page_row_.set_dock(DockStyle::Bottom);
  page_row_.set_size(0, 30);
  page_row_.set_padding(Insets{0, 4, 0, 0});
  previous_.set_text("<");
  previous_.set_dock(DockStyle::Left);
  previous_.set_size(32, 0);
  previous_.on_click({&PrintDialog::on_previous, this});
  next_.set_text(">");
  next_.set_dock(DockStyle::Right);
  next_.set_size(32, 0);
  next_.on_click({&PrintDialog::on_next, this});
  page_label_.set_dock(DockStyle::Fill);
  page_row_.add(&previous_);
  page_row_.add(&next_);
  page_row_.add(&page_label_);
  preview_.set_dock(DockStyle::Fill);
  preview_panel_.add(&page_row_);
  preview_panel_.add(&preview_);

  options_panel_.set_style(PanelStyle::Transparent);
  options_panel_.set_dock(DockStyle::Left);
  options_panel_.set_size(410, 0);
  options_panel_.add(&printer_row_);
  options_panel_.add(&status_);
  options_panel_.add(&paper_row_);
  options_panel_.add(&orientation_row_);
  options_panel_.add(&range_row_);
  options_panel_.add(&quality_row_);
  options_panel_.add(&copies_row_);
  options_panel_.add(&toner_row_);
  options_panel_.add(&picture_row_);
  options_panel_.add(&stretch_row_);

  add(&button_row_);
  add(&options_panel_);
  add(&preview_panel_);  // Fill goes last
}

bool PrintDialog::show(WindowManager& manager, Kind kind, StringView title, Handler result) {
  UiGuard lock(&manager);
  if (showing()) return false;
  kind_ = kind;
  accepted_ = false;
  set_title(title);
  picture_row_.set_visible(kind == Kind::Picture);
  stretch_row_.set_visible(kind == Kind::Picture);
  range_row_.set_visible(kind == Kind::Text);
  message_.set_text(StringView());
  loading_ = true;   // the choices go back to their starting values without the preview following each one
  copies_.set_value(1);
  toner_.set_checked(false);
  stretch_.set_checked(false);
  picture_type_.set_selected_index(0);
  portrait_.set_checked(true);
  all_pages_.set_checked(true);
  from_page_.set_value(1);
  to_page_.set_value(1);
  loading_ = false;
  preview_.set_page(0);
  load_printers();
  on_closed(result);
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  const int32_t w = kWidth < area_w ? kWidth : area_w, h = kHeight < area_h ? kHeight : area_h;
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0), static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&print_);
  }
  return true;
}

void PrintDialog::load_printers() {
  loading_ = true;
  printer_.clear_items();
  const uint32_t count = service_ ? service_->printer_count() : 0;
  for (uint32_t i = 0; i < count; ++i) {
    PrinterListing p;
    service_->printer(i, &p);
    printer_.add_item(p.name);
  }
  loading_ = false;
  if (count == 0) {
    status_.set_text("No printers found.");
    paper_.clear_items();
    quality_.clear_items();
    paper_count_ = dpi_count_ = 0;
    print_.set_enabled(false);
    relayout();
    return;
  }
  uint32_t chosen = service_->default_printer();
  if (chosen >= count) chosen = 0;
  printer_.set_selected_index(static_cast<int32_t>(chosen));
  load_printer(chosen);
}

// What the chosen printer offers: its status line, its paper sizes and its resolutions.
void PrintDialog::load_printer(uint32_t index) {
  PrinterListing p;
  if (!service_ || !service_->printer(index, &p)) return;
  loading_ = true;
  printer_index_ = index;
  margin_mm10_ = p.unprintable_mm10;
  String line(allocator());
  line.append(!p.present ? "Not connected" : p.state == PrinterState::Busy ? "Busy" : p.state == PrinterState::Error ? "Error" : "Ready");
  line.append(p.color ? " - color" : " - black and white");
  if (p.duplex) line.append(", duplex");
  line.append(", up to ");
  line.append_uint(p.max_dpi);
  line.append(" dpi");
  if (p.pages_per_minute) {
    line.append(", ");
    line.append_uint(p.pages_per_minute);
    line.append(" pages/min");
  }
  status_.set_text(line.view());

  paper_.clear_items();
  paper_count_ = 0;
  int32_t preferred = 0;
  for (uint8_t s = 0; s < static_cast<uint8_t>(PaperSize::Count); ++s) {
    if (p.paper_sizes && !((p.paper_sizes >> s) & 1)) continue;
    papers_[paper_count_] = static_cast<PaperSize>(s);
    if (static_cast<PaperSize>(s) == PaperSize::A4) preferred = static_cast<int32_t>(paper_count_);
    paper_.add_item(hardware::paper_info(static_cast<PaperSize>(s)).name);
    ++paper_count_;
  }
  paper_.set_selected_index(preferred);

  quality_.clear_items();
  dpi_count_ = 0;
  static const struct { uint16_t dpi; const char* name; } kQualities[] = {{300, "300 dpi (draft)"}, {600, "600 dpi (normal)"}, {1200, "1200 dpi (best)"}};
  int32_t normal = 0;
  for (const auto& q : kQualities) {
    if (q.dpi > p.max_dpi && dpi_count_ > 0) break;
    dpis_[dpi_count_] = q.dpi;
    if (q.dpi <= 600) normal = static_cast<int32_t>(dpi_count_);
    quality_.add_item(q.name);
    ++dpi_count_;
  }
  quality_.set_selected_index(normal);
  print_.set_enabled(p.present);
  loading_ = false;
  relayout();
}

void PrintDialog::on_printer(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (d->loading_ || d->printer_.selected_index() < 0) return;
  d->load_printer(static_cast<uint32_t>(d->printer_.selected_index()));
}

void PrintDialog::gather() {
  options_ = PrintOptions();
  printer_index_ = printer_.selected_index() >= 0 ? static_cast<uint32_t>(printer_.selected_index()) : 0;
  if (paper_.selected_index() >= 0 && static_cast<uint32_t>(paper_.selected_index()) < paper_count_) options_.paper = papers_[paper_.selected_index()];
  options_.landscape = landscape_.checked();
  if (quality_.selected_index() >= 0 && static_cast<uint32_t>(quality_.selected_index()) < dpi_count_) options_.dpi = dpis_[quality_.selected_index()];
  options_.copies = static_cast<uint16_t>(copies_.value());
  options_.toner_save = toner_.checked();
  if (kind_ == Kind::Text && page_range_.checked()) {
    from_page_.normalize();
    to_page_.normalize();
    options_.first_page = static_cast<uint32_t>(from_page_.value());
    options_.last_page = static_cast<uint32_t>(to_page_.value());
  }
  if (kind_ == Kind::Picture) {
    const int32_t type = picture_type_.selected_index();
    options_.halftone = type == 1 ? Halftone::Threshold : type == 2 ? Halftone::Ordered : Halftone::ErrorDiffusion;
    options_.fit = stretch_.checked() ? Fit::Stretch : Fit::Contain;
  }
}

// The sheet, the text's pages (or the picture) and the page buttons follow the choices made.
void PrintDialog::relayout() {
  const PaperSize paper = paper_.selected_index() >= 0 && static_cast<uint32_t>(paper_.selected_index()) < paper_count_ ? papers_[paper_.selected_index()] : PaperSize::A4;
  const bool landscape = landscape_.checked();
  preview_.set_sheet(paper, landscape, margin_mm10_);
  const hardware::TextGeometry geometry = hardware::text_geometry(paper, landscape, margin_mm10_);
  if (kind_ == Kind::Text && !text_.empty() && geometry.valid() && layout_.layout(text_.view(), geometry.columns, geometry.lines)) {
    preview_.show_text(&layout_);
  } else if (kind_ == Kind::Picture && picture_ && picture_->valid()) {
    preview_.show_picture(picture_, stretch_.checked() ? Fit::Stretch : Fit::Contain,
                          picture_type_.selected_index() == 1 ? Halftone::Threshold : picture_type_.selected_index() == 2 ? Halftone::Ordered : Halftone::ErrorDiffusion);
  } else {
    preview_.show_nothing();
  }
  update_pages();
}

void PrintDialog::update_pages() {
  const uint32_t pages = preview_.page_count();
  const int32_t last = pages ? static_cast<int32_t>(pages) : 1;
  const bool was_loading = loading_;
  loading_ = true;
  from_page_.set_range(1, last);
  to_page_.set_range(1, last);
  if (!page_range_.checked()) {
    from_page_.set_value(1);
    to_page_.set_value(last);
  }
  loading_ = was_loading;
  const bool ranged = page_range_.checked() && kind_ == Kind::Text;
  from_page_.set_enabled(ranged);
  to_page_.set_enabled(ranged);
  String text(allocator());
  if (pages == 0) {
    text.assign("Nothing to preview");
  } else {
    text.assign(kind_ == Kind::Picture ? "The page" : "Page ");
    if (kind_ == Kind::Text) {
      text.append_uint(preview_.page() + 1);
      text.append(" of ");
      text.append_uint(pages);
    }
  }
  page_label_.set_text(text.view());
  previous_.set_enabled(preview_.page() > 0);
  next_.set_enabled(preview_.page() + 1 < pages);
}

void PrintDialog::on_layout_changed(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (!d->loading_) d->relayout();
}
void PrintDialog::on_picture_changed(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (!d->loading_) d->relayout();
}
void PrintDialog::on_range_mode(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (!d->loading_) d->update_pages();
}
void PrintDialog::on_from(void* self, Control&) {   // the range stays in order, and the preview shows its first page
  auto* d = static_cast<PrintDialog*>(self);
  if (d->loading_) return;
  if (d->to_page_.value() < d->from_page_.value()) d->to_page_.set_value(d->from_page_.value());
  if (d->page_range_.checked()) {
    d->preview_.set_page(static_cast<uint32_t>(d->from_page_.value() - 1));
    d->update_pages();
  }
}
void PrintDialog::on_to(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (d->loading_) return;
  if (d->from_page_.value() > d->to_page_.value()) d->from_page_.set_value(d->to_page_.value());
}
void PrintDialog::on_previous(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  d->preview_.set_page(d->preview_.page() > 0 ? d->preview_.page() - 1 : 0);
  d->update_pages();
}
void PrintDialog::on_next(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  d->preview_.set_page(d->preview_.page() + 1);
  d->update_pages();
}

void PrintDialog::on_print(void* self, Control&) {
  auto* d = static_cast<PrintDialog*>(self);
  if (!d->print_.enabled()) return;
  d->copies_.normalize();
  d->gather();
  d->accepted_ = true;
  d->close();
}

void PrintDialog::on_cancel(void* self, Control&) { static_cast<PrintDialog*>(self)->close(); }

bool PrintDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    print_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
