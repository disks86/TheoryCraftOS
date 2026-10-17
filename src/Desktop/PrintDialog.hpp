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

// The print dialog: a modal window in which the user chooses a printer and how to print on it - paper size, portrait or landscape,
// quality (dots per inch), number of copies, toner saving and, for a picture, how it is turned into dots and whether it fills the
// page. It lists the printers the print service knows, shows what the chosen one is (state, color, duplex) and offers only the
// paper sizes and resolutions it has. For a text it also asks which pages to print (all, or from one page to another), and a preview
// beside the options shows the sheet with the text on it - page by page, laid out exactly as the printer will print it - or the
// picture placed and turned into dots; it follows the choices. Give the dialog what is to be printed (set_text / set_picture) before show(). Print (or Enter) accepts, Cancel, Escape or the close button leave accepted() false. The dialog
// only collects the choices; the program that opened it sends the job. One dialog serves every program: show() is given
// the handler to run once the dialog is gone.
#pragma once
#include "Desktop/PrintPreview.hpp"
#include "Desktop/PrintService.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/RadioButton.hpp"
#include "Gui/Controls/SpinBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace desktop {

class PrintDialog : public gui::Frame {
 public:
  enum class Kind : uint8_t { Text, Picture };
  static constexpr int32_t kWidth = 700, kHeight = 440;

  explicit PrintDialog(allocators::IAllocator& allocator);

  // The service whose printers are offered and that receives the job (nullptr: none, the dialog then says there are no printers). It must
  // outlive the dialog.
  void set_print_service(IPrintService* service) { service_ = service; }
  IPrintService* print_service() const { return service_; }

  // What is going to be printed, for the preview and the page count. A text is copied; a picture is borrowed and must stay valid while the
  // dialog is up. The next show() uses whatever was given last for its kind.
  void set_text(StringView text) { text_.assign(text); }
  void set_picture(const graphics::Surface* picture) { picture_ = picture; }

  // Opens the dialog modally in `manager`. `result` runs once it has closed, accepted or not. False if it is already showing.
  bool show(gui::WindowManager& manager, Kind kind, StringView title, gui::Handler result);
  bool showing() const { return manager() != nullptr; }

  // The outcome (valid once the result handler runs): whether Print was chosen, on which printer, and with what options.
  bool accepted() const { return accepted_; }
  uint32_t printer_index() const { return printer_index_; }
  const PrintOptions& options() const { return options_; }
  // How many pages the text comes to on the chosen paper (1 for a picture, 0 when there is nothing to print).
  uint32_t page_count() const { return preview_.page_count(); }
  Kind kind() const { return kind_; }

  gui::DropDown& printer_box() { return printer_; }
  gui::DropDown& paper_box() { return paper_; }
  gui::DropDown& quality_box() { return quality_; }
  gui::DropDown& picture_box() { return picture_type_; }
  gui::RadioButton& portrait_button() { return portrait_; }
  gui::RadioButton& landscape_button() { return landscape_; }
  gui::SpinBox& copies_box() { return copies_; }
  gui::CheckBox& toner_box() { return toner_; }
  gui::CheckBox& stretch_box() { return stretch_; }
  gui::Label& status_label() { return status_; }
  gui::Label& message_label() { return message_; }
  gui::RadioButton& all_pages_button() { return all_pages_; }
  gui::RadioButton& page_range_button() { return page_range_; }
  gui::SpinBox& from_page_box() { return from_page_; }
  gui::SpinBox& to_page_box() { return to_page_; }
  PrintPreview& preview() { return preview_; }
  gui::Button& previous_page_button() { return previous_; }
  gui::Button& next_page_button() { return next_; }
  gui::Label& page_label() { return page_label_; }
  gui::Button& print_button() { return print_; }
  gui::Button& cancel_button() { return cancel_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_printer(void* self, gui::Control&);
  static void on_print(void* self, gui::Control&);
  static void on_layout_changed(void* self, gui::Control&);
  static void on_picture_changed(void* self, gui::Control&);
  static void on_range_mode(void* self, gui::Control&);
  static void on_from(void* self, gui::Control&);
  static void on_to(void* self, gui::Control&);
  static void on_previous(void* self, gui::Control&);
  static void on_next(void* self, gui::Control&);
  void relayout();
  void update_pages();
  static void on_cancel(void* self, gui::Control&);
  void load_printers();
  void load_printer(uint32_t index);
  void gather();

  gui::Panel options_panel_, preview_panel_, page_row_, printer_row_, paper_row_, orientation_row_, range_row_, quality_row_, copies_row_, toner_row_,
      picture_row_, stretch_row_, button_row_, gap_;
  gui::Label printer_label_, paper_label_, orientation_label_, range_label_, to_label_, quality_label_, copies_label_, picture_label_, page_label_;
  gui::Label status_, message_;
  gui::DropDown printer_, paper_, quality_, picture_type_;
  gui::RadioGroup orientation_group_;
  gui::RadioButton portrait_, landscape_;
  gui::CheckBox toner_, stretch_;
  gui::RadioGroup range_group_;
  gui::RadioButton all_pages_, page_range_;
  gui::SpinBox from_page_, to_page_, copies_;
  gui::Button previous_, next_;
  PrintPreview preview_;
  hardware::TextLayout layout_;
  String text_;
  const graphics::Surface* picture_ = nullptr;
  uint32_t margin_mm10_ = 42;
  gui::Button print_, cancel_;
  IPrintService* service_ = nullptr;
  Kind kind_ = Kind::Text;
  PrintOptions options_;
  uint32_t printer_index_ = 0;
  hardware::PaperSize papers_[static_cast<uint8_t>(hardware::PaperSize::Count)] = {};
  uint32_t paper_count_ = 0;
  uint16_t dpis_[3] = {};
  uint32_t dpi_count_ = 0;
  bool accepted_ = false, loading_ = false;
};

}  // namespace desktop
