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

#include "Desktop/Paint.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Media/ImageCodecs.hpp"

namespace desktop {

using namespace gui;
using filesystem::IFile;
using filesystem::OpenMode;
using filesystem::Status;

namespace {

// --- Tool icons, drawn in a 16 x 16 area ---

void icon_pencil(Painter& p, const Rect2D& a, uint32_t ink) {
  p.line(a.x + 3, a.y + 12, a.x + 12, a.y + 3, ink);
  p.line(a.x + 4, a.y + 13, a.x + 13, a.y + 4, ink);
  p.line(a.x + 2, a.y + 14, a.x + 3, a.y + 12, ink);
  p.line(a.x + 2, a.y + 14, a.x + 4, a.y + 13, ink);
  p.line(a.x + 12, a.y + 2, a.x + 14, a.y + 4, ink);
}

void icon_eraser(Painter& p, const Rect2D& a, uint32_t ink) {
  p.line(a.x + 1, a.y + 9, a.x + 8, a.y + 2, ink);
  p.line(a.x + 8, a.y + 2, a.x + 14, a.y + 8, ink);
  p.line(a.x + 14, a.y + 8, a.x + 7, a.y + 15, ink);
  p.line(a.x + 7, a.y + 15, a.x + 1, a.y + 9, ink);
  p.line(a.x + 4, a.y + 6, a.x + 10, a.y + 12, ink);
}

void icon_line(Painter& p, const Rect2D& a, uint32_t ink) {
  p.line(a.x + 2, a.y + 14, a.x + 14, a.y + 2, ink);
  p.line(a.x + 2, a.y + 13, a.x + 13, a.y + 2, ink);
}

void icon_rectangle(Painter& p, const Rect2D& a, uint32_t ink) { p.outline({a.x + 1, a.y + 3, 14, 10}, ink); }

void icon_ellipse(Painter& p, const Rect2D& a, uint32_t ink) {
  p.line(a.x + 5, a.y + 3, a.x + 10, a.y + 3, ink);
  p.line(a.x + 10, a.y + 3, a.x + 14, a.y + 6, ink);
  p.line(a.x + 14, a.y + 6, a.x + 14, a.y + 9, ink);
  p.line(a.x + 14, a.y + 9, a.x + 10, a.y + 12, ink);
  p.line(a.x + 10, a.y + 12, a.x + 5, a.y + 12, ink);
  p.line(a.x + 5, a.y + 12, a.x + 1, a.y + 9, ink);
  p.line(a.x + 1, a.y + 9, a.x + 1, a.y + 6, ink);
  p.line(a.x + 1, a.y + 6, a.x + 5, a.y + 3, ink);
}

void icon_fill(Painter& p, const Rect2D& a, uint32_t ink) {  // a tilted bucket with a drop
  p.line(a.x + 2, a.y + 8, a.x + 7, a.y + 3, ink);
  p.line(a.x + 7, a.y + 3, a.x + 12, a.y + 8, ink);
  p.line(a.x + 12, a.y + 8, a.x + 7, a.y + 13, ink);
  p.line(a.x + 7, a.y + 13, a.x + 2, a.y + 8, ink);
  p.line(a.x + 3, a.y + 8, a.x + 11, a.y + 8, ink);
  p.fill({a.x + 13, a.y + 10, 2, 4}, ink);
}

void icon_picker(Painter& p, const Rect2D& a, uint32_t ink) {  // an eyedropper
  p.line(a.x + 3, a.y + 13, a.x + 10, a.y + 6, ink);
  p.line(a.x + 4, a.y + 13, a.x + 11, a.y + 6, ink);
  p.fill({a.x + 10, a.y + 2, 4, 4}, ink);
  p.line(a.x + 2, a.y + 14, a.x + 3, a.y + 13, ink);
}

void icon_select(Painter& p, const Rect2D& a, uint32_t ink) {  // a dashed rectangle
  for (int32_t i = 1; i < 15; i += 3) {
    p.fill({a.x + i, a.y + 2, 2, 1}, ink);
    p.fill({a.x + i, a.y + 13, 2, 1}, ink);
  }
  for (int32_t i = 3; i < 13; i += 3) {
    p.fill({a.x + 1, a.y + i, 1, 2}, ink);
    p.fill({a.x + 14, a.y + i, 1, 2}, ink);
  }
}

struct ToolDef {
  const char* name;
  ToolIcon icon;
};
const ToolDef kTools[] = {{"Pencil", icon_pencil}, {"Eraser", icon_eraser}, {"Line", icon_line},      {"Rectangle", icon_rectangle},
                          {"Ellipse", icon_ellipse}, {"Fill", icon_fill},   {"Color Picker", icon_picker}, {"Select", icon_select}};

// --- Files ---

bool equals_ignore_case(StringView a, const char* b) {
  uint64_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
    if (x != b[i]) return false;
  }
  return i == a.size() && !b[i];
}

media::ImageFormat format_of_name(StringView name) {
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos) return media::ImageFormat::Unknown;
  const StringView ext = name.substr(dot + 1);
  struct Entry {
    const char* ext;
    media::ImageFormat format;
  };
  static const Entry kFormats[] = {{"png", media::ImageFormat::Png},   {"jpg", media::ImageFormat::Jpeg},  {"jpeg", media::ImageFormat::Jpeg},
                                   {"bmp", media::ImageFormat::Bmp},   {"gif", media::ImageFormat::Gif},   {"ppm", media::ImageFormat::Pnm},
                                   {"pnm", media::ImageFormat::Pnm},   {"qoi", media::ImageFormat::Qoi},   {"tga", media::ImageFormat::Tga},
                                   {"tif", media::ImageFormat::Tiff},  {"tiff", media::ImageFormat::Tiff}, {"webp", media::ImageFormat::Webp}};
  for (const Entry& e : kFormats)
    if (equals_ignore_case(ext, e.ext)) return e.format;
  return media::ImageFormat::Unknown;
}

bool normalize(StringView in, String* out) {
  uint64_t from = 0, to = in.size();
  while (from < to && in[from] == ' ') ++from;
  while (to > from && in[to - 1] == ' ') --to;
  out->clear();
  if (from == to) return true;
  if (in[from] != '/' && !out->push_back('/')) return false;
  return out->append(in.substr(from, to - from));
}

uint32_t g_pictures = 0;   // numbers the new pictures: "Picture 1", "Picture 2"...

}  // namespace

bool Paint::is_picture_name(StringView name) { return format_of_name(name) != media::ImageFormat::Unknown; }

Paint::Paint(allocators::IAllocator& allocator)
    : DocumentFrame(allocator, "Paint", "Picture", &g_pictures),
      menu_(allocator),
      canvas_menu_(allocator),
      options_bar_(allocator),
      palette_bar_(allocator),
      status_bar_(allocator),
      size_label_(allocator),
      size_value_(allocator),
      zoom_label_(allocator),
      status_(allocator),
      position_(allocator),
      size_(allocator),
      tools_(allocator),
      palette_(allocator),
      picker_(allocator),
      canvas_(allocator) {
  set_bounds({30, 20, 640, 470});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  // The menus.
  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  menu_.on_opening(Handler::immediate(&Paint::on_menu_opening, this));
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&Paint::on_new, this}, "Ctrl+N");
    open_item_ = file->add_item("Open...", {&Paint::on_open, this}, "Ctrl+O");
    save_item_ = file->add_item("Save", {&Paint::on_save, this}, "Ctrl+S");
    save_as_item_ = file->add_item("Save As...", {&Paint::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    print_item_ = file->add_item("Print...", {&Paint::on_print, this}, "Ctrl+P");
    file->add_separator();
    file->add_item("Exit", {&Paint::on_close_window, this});   // (the picture goes with its window, so this is Exit and not Close)
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    undo_item_ = edit->add_item("Undo", {&Paint::on_undo, this}, "Ctrl+Z");
    redo_item_ = edit->add_item("Redo", {&Paint::on_redo, this}, "Ctrl+Y");
    edit->add_separator();
    cut_item_ = edit->add_item("Cut", {&Paint::on_cut, this}, "Ctrl+X");
    edit->add_item("Copy", {&Paint::on_copy, this}, "Ctrl+C");
    paste_item_ = edit->add_item("Paste", {&Paint::on_paste, this}, "Ctrl+V");
    delete_item_ = edit->add_item("Delete", {&Paint::on_delete_selection, this}, "Del");
    edit->add_separator();
    edit->add_item("Select All", {&Paint::on_select_all, this}, "Ctrl+A");
  }
  if (Menu* view = menu_.add_menu("View")) {
    zoom_in_item_ = view->add_item("Zoom In", {&Paint::on_zoom_in, this}, "Ctrl++");
    zoom_out_item_ = view->add_item("Zoom Out", {&Paint::on_zoom_out, this}, "Ctrl+-");
    actual_item_ = view->add_item("Actual Size", {&Paint::on_actual_size, this}, "Ctrl+0");
    view->add_separator();
    grid_item_ = view->add_item("Pixel Grid", {&Paint::on_toggle_grid, this});
  }
  // Everything the tool strip offers is here as well, so that no tool is reachable with the pointer only; the tool in use has the check mark.
  if (Menu* tools = menu_.add_menu("Tools")) {
    static constexpr void (*kChoose[kToolCount])(void*, Control&) = {&Paint::on_tool_item<0>, &Paint::on_tool_item<1>, &Paint::on_tool_item<2>, &Paint::on_tool_item<3>,
                                                                     &Paint::on_tool_item<4>, &Paint::on_tool_item<5>, &Paint::on_tool_item<6>, &Paint::on_tool_item<7>};
    for (uint32_t i = 0; i < kToolCount; ++i) tool_items_[i] = tools->add_item(kTools[i].name, {kChoose[i], this});
    tools->add_separator();
    filled_item_ = tools->add_item("Filled Shapes", {&Paint::on_toggle_filled, this});
  }
  if (Menu* colors = menu_.add_menu("Colors")) {
    primary_item_ = colors->add_item("Primary Color...", {&Paint::on_primary_color, this});
    secondary_item_ = colors->add_item("Secondary Color...", {&Paint::on_secondary_color, this});
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Paint", {&Paint::on_about, this});

  // Options bar: brush size and the zoom shown.
  options_bar_.set_dock(DockStyle::Top);
  options_bar_.set_size(0, 28);
  options_bar_.set_padding(Insets{0, 2, 0, 2});
  size_label_.set_text("Size:");   // (the colon makes it the label of the slider: its access key moves the focus there)
  size_label_.set_dock(DockStyle::Left);
  size_label_.set_size(48, 0);
  size_.set_range(1, 32);
  size_.set_value(1);
  size_.set_step(1, 4);
  size_.set_dock(DockStyle::Left);
  size_.set_size(110, 0);
  size_.on_value_changed({&Paint::on_size, this});
  size_value_.set_text("1");
  size_value_.set_dock(DockStyle::Left);
  size_value_.set_size(36, 0);
  zoom_label_.set_text("100%");
  zoom_label_.set_dock(DockStyle::Right);
  zoom_label_.set_size(56, 0);
  options_bar_.add(&size_label_);
  options_bar_.add(&size_);
  options_bar_.add(&size_value_);
  options_bar_.add(&zoom_label_);

  // Status bar, then the palette above it.
  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  position_.set_dock(DockStyle::Right);
  position_.set_size(210, 0);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&position_);
  status_bar_.add(&status_);

  palette_bar_.set_dock(DockStyle::Bottom);
  palette_bar_.set_size(0, 46);
  palette_.set_dock(DockStyle::Left);
  palette_.set_size(palette_.preferred_size().width, 0);
  palette_.on_colors_changed({&Paint::on_palette, this});
  picker_.set_dock(DockStyle::Left);
  picker_.set_size(120, 0);
  picker_.set_color(palette_.primary());
  picker_.on_color_changed({&Paint::on_picker, this});
  palette_bar_.add(&palette_);
  palette_bar_.add(&picker_);

  tools_.set_columns(2);
  for (const ToolDef& t : kTools) tools_.add_tool(t.name, t.icon);
  tools_.set_dock(DockStyle::Left);
  tools_.set_size(tools_.content_size().width, 0);
  tools_.on_selection_changed({&Paint::on_tool, this});
  tools_.on_hover_changed({&Paint::on_tool_hover, this});

  ctx_undo_ = canvas_menu_.add_item("Undo", {&Paint::on_undo, this}, "Ctrl+Z");
  ctx_redo_ = canvas_menu_.add_item("Redo", {&Paint::on_redo, this}, "Ctrl+Y");
  canvas_menu_.add_separator();
  ctx_cut_ = canvas_menu_.add_item("Cut", {&Paint::on_cut, this}, "Ctrl+X");
  ctx_copy_ = canvas_menu_.add_item("Copy", {&Paint::on_copy, this}, "Ctrl+C");
  ctx_paste_ = canvas_menu_.add_item("Paste", {&Paint::on_paste, this}, "Ctrl+V");
  ctx_delete_ = canvas_menu_.add_item("Delete", {&Paint::on_delete_selection, this}, "Del");
  canvas_menu_.add_separator();
  canvas_menu_.add_item("Select All", {&Paint::on_select_all, this}, "Ctrl+A");
  canvas_menu_.on_opening(Handler::immediate(&Paint::on_canvas_menu_opening, this));
  canvas_.set_context_menu(&canvas_menu_);
  canvas_.set_dock(DockStyle::Fill);
  canvas_.on_changed({&Paint::on_canvas_changed, this});
  canvas_.on_pointer_moved({&Paint::on_pointer, this});
  canvas_.on_zoom_changed({&Paint::on_zoom_changed, this});
  canvas_.on_tool_changed({&Paint::on_tool_changed, this});
  canvas_.on_color_picked({&Paint::on_picked, this});
  canvas_.set_primary(palette_.primary());
  canvas_.set_secondary(palette_.secondary());
  canvas_.new_image(kNewWidth, kNewHeight);

  add(&menu_);
  add(&options_bar_);
  add(&status_bar_);
  add(&palette_bar_);
  add(&tools_);
  add(&canvas_);  // Fill goes last
  update_state();
  say("Draw with the left button; the right button draws in the secondary color.");
}

// --- Display ------------------------------------------------------------------------

bool Paint::can_paste() const {
  const kernel::Clipboard* clip = clipboard();
  return clip && clip->has_image();
}

// The items that cannot work right now are disabled instead of answering with a message; the tool in use and the switches have their check marks.
void Paint::update_menus() {
  if (open_item_) open_item_->set_enabled(can_open());
  if (save_item_) save_item_->set_enabled(can_save());
  if (save_as_item_) save_as_item_->set_enabled(can_open());
  if (print_item_) print_item_->set_enabled(print_dialog_ != nullptr);
  if (undo_item_) undo_item_->set_enabled(canvas_.can_undo());
  if (redo_item_) redo_item_->set_enabled(canvas_.can_redo());
  if (cut_item_) cut_item_->set_enabled(canvas_.has_selection());
  if (delete_item_) delete_item_->set_enabled(canvas_.has_selection());
  if (paste_item_) paste_item_->set_enabled(can_paste());
  if (zoom_in_item_) zoom_in_item_->set_enabled(canvas_.zoom() < 16);
  if (zoom_out_item_) zoom_out_item_->set_enabled(canvas_.zoom() > 1);
  if (actual_item_) actual_item_->set_enabled(canvas_.zoom() != 1);
  if (primary_item_) primary_item_->set_enabled(color_dialog_ != nullptr);
  if (secondary_item_) secondary_item_->set_enabled(color_dialog_ != nullptr);
  for (uint32_t i = 0; i < kToolCount; ++i)
    if (tool_items_[i]) tool_items_[i]->set_checked(static_cast<int32_t>(i) == tools_.selected_tool());
}

void Paint::on_menu_opening(void* self, Control&) { static_cast<Paint*>(self)->update_menus(); }
void Paint::on_about(void* self, Control&) {
  static_cast<Paint*>(self)->show_about("Paint draws and changes pictures. It is part of TheoryCraftOS.");
}

void Paint::update_state() {
  update_menus();
  update_position();
}

void Paint::update_position() {
  String text(allocator());
  int32_t x, y;
  if (canvas_.pointer_position(&x, &y)) {
    text.append_uint(static_cast<uint64_t>(x));
    text.append(", ");
    text.append_uint(static_cast<uint64_t>(y));
    text.append("   ");
  }
  text.append_uint(canvas_.image_width());
  text.append(" x ");
  text.append_uint(canvas_.image_height());
  position_.set_text(text.view());
}

void Paint::on_canvas_changed(void* self, Control&) {
  static_cast<Paint*>(self)->update_state();
}
void Paint::on_pointer(void* self, Control&) { static_cast<Paint*>(self)->update_position(); }
void Paint::on_zoom_changed(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  String text(p->allocator());
  text.append_uint(p->canvas_.zoom() * 100);
  text.append("%");
  p->zoom_label_.set_text(text.view());
  p->update_menus();
}

// --- Tools and colors ---------------------------------------------------------------------

void Paint::on_tool(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.set_tool(static_cast<PaintTool>(p->tools_.selected_tool()));
  p->say(p->tools_.tool_name(static_cast<uint32_t>(p->tools_.selected_tool())));
}

void Paint::on_tool_hover(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  const int32_t hovered = p->tools_.hovered_tool();
  if (hovered >= 0) p->say(p->tools_.tool_name(static_cast<uint32_t>(hovered)));
}

void Paint::on_tool_changed(void* self, Control&) {  // the canvas switched tools itself (a paste selects Select)
  auto* p = static_cast<Paint*>(self);
  p->tools_.select_tool(static_cast<int32_t>(p->canvas_.tool()));
}

// (Cut needs a selection and Paste a picture on the clipboard: their menu items are disabled without, and the keys then do nothing.)
void Paint::copy_to_clipboard(bool cut) {
  if (cut && !canvas_.has_selection()) return;
  const bool ok = cut ? canvas_.cut_selection() : canvas_.copy_selection();
  if (ok) say(cut ? "Cut to the clipboard." : canvas_.has_selection() ? "Copied the selection." : "Copied the whole picture.");
  update_menus();
}

void Paint::paste_from_clipboard() {
  if (can_paste() && canvas_.paste()) say("Pasted. Drag it into place; Enter sets it down, Esc takes it back.");
}

void Paint::on_cut(void* self, Control&) { static_cast<Paint*>(self)->copy_to_clipboard(true); }
void Paint::on_copy(void* self, Control&) { static_cast<Paint*>(self)->copy_to_clipboard(false); }
void Paint::on_paste(void* self, Control&) { static_cast<Paint*>(self)->paste_from_clipboard(); }

void Paint::on_palette(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.set_primary(p->palette_.primary());
  p->canvas_.set_secondary(p->palette_.secondary());
  p->picker_.set_color(p->palette_.primary());
}

void Paint::on_picker(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->palette_.set_primary(p->picker_.color());
}

void Paint::on_picked(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->palette_.set_primary(p->canvas_.primary());
  p->palette_.set_secondary(p->canvas_.secondary());
}

void Paint::on_size(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.set_brush_size(static_cast<uint32_t>(p->size_.value()));
  String text(p->allocator());
  text.append_uint(static_cast<uint64_t>(p->size_.value()));
  p->size_value_.set_text(text.view());
}

void Paint::on_toggle_filled(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.set_filled(!p->canvas_.filled());
  p->filled_item_->set_checked(p->canvas_.filled());
}

void Paint::on_toggle_grid(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.set_grid(!p->canvas_.grid());
  p->grid_item_->set_checked(p->canvas_.grid());
}

void Paint::on_delete_selection(void* self, Control&) { static_cast<Paint*>(self)->canvas_.delete_selection(); }

// What can be done with the selection and the clipboard right now.
void Paint::on_canvas_menu_opening(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  const bool selected = p->canvas_.has_selection();
  p->ctx_undo_->set_enabled(p->canvas_.can_undo());
  p->ctx_redo_->set_enabled(p->canvas_.can_redo());
  p->ctx_cut_->set_enabled(selected);
  p->ctx_copy_->set_enabled(true);   // nothing selected: the whole picture
  p->ctx_delete_->set_enabled(selected);
  p->ctx_paste_->set_enabled(p->can_paste());
}

void Paint::on_actual_size(void* self, Control&) { static_cast<Paint*>(self)->canvas_.set_zoom(1); }
void Paint::on_close_window(void* self, Control&) { static_cast<Paint*>(self)->exit_requested(); }
void Paint::on_select_all(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  p->canvas_.select_all();
  p->say("Selected the whole picture.");
}

void Paint::on_undo(void* self, Control&) { static_cast<Paint*>(self)->canvas_.undo(); }
void Paint::on_redo(void* self, Control&) { static_cast<Paint*>(self)->canvas_.redo(); }
void Paint::on_zoom_in(void* self, Control&) { static_cast<Paint*>(self)->canvas_.zoom_in(); }
void Paint::on_zoom_out(void* self, Control&) { static_cast<Paint*>(self)->canvas_.zoom_out(); }

// --- Files ------------------------------------------------------------------------------------

void Paint::on_new(void* self, Control&) { static_cast<Paint*>(self)->new_picture(); }
void Paint::on_save(void* self, Control&) { static_cast<Paint*>(self)->save_file(); }
void Paint::on_open(void* self, Control&) { static_cast<Paint*>(self)->ask_open(); }
void Paint::on_save_as(void* self, Control&) { static_cast<Paint*>(self)->ask_save_as(); }
void Paint::on_print(void* self, Control&) { static_cast<Paint*>(self)->ask_print(); }
void Paint::on_primary_color(void* self, Control&) { static_cast<Paint*>(self)->ask_color(false); }
void Paint::on_secondary_color(void* self, Control&) { static_cast<Paint*>(self)->ask_color(true); }

void Paint::ask_print() {
  if (!print_dialog_ || !manager()) return;   // (the menu item is disabled then)
  canvas_.commit_floating();  // a picture still being placed is part of what is printed
  print_surface_ = canvas_.surface();
  print_dialog_->set_picture(&print_surface_);   // for the preview
  if (!print_dialog_->show(*manager(), PrintDialog::Kind::Picture, "Print", {&Paint::on_print_picked, this})) say("The Print dialog is already open for another window.");
}

void Paint::on_print_picked(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  PrintDialog& d = *p->print_dialog_;
  if (!d.accepted() || !d.print_service()) return;
  String error(p->allocator());
  if (d.print_service()->print_picture(d.printer_index(), p->canvas_.surface(), p->document_name(), d.options(), &error)) {
    p->say("Sent to the printer.");
  } else {
    p->notify(NoticeKind::Error, "\"%1\" could not be printed. %2", p->document_name(), error.view());
  }
}

void Paint::ask_color(bool secondary) {
  if (!color_dialog_ || !manager()) return;   // (the menu items are disabled then)
  choosing_secondary_ = secondary;
  if (!color_dialog_->show(*manager(), secondary ? "Secondary Color" : "Primary Color", secondary ? palette_.secondary() : palette_.primary(),
                           {&Paint::on_color_picked_by_dialog, this}))
    say("The color dialog is already open for another window.");
}

void Paint::on_color_picked_by_dialog(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  if (!p->color_dialog_->accepted()) return;
  if (p->choosing_secondary_) p->palette_.set_secondary(p->color_dialog_->color());
  else p->palette_.set_primary(p->color_dialog_->color());
}

namespace {
const FileDialog::Filter kPictureFilters[] = {
    {"Pictures", "png;jpg;jpeg;bmp;gif;qoi;tga;tif;tiff;webp;ppm;pnm"}, {"PNG", "png"}, {"JPEG", "jpg;jpeg"}, {"BMP", "bmp"}, {"GIF", "gif"},
    {"QOI", "qoi"}, {"TGA", "tga"}, {"TIFF", "tif;tiff"}, {"WebP", "webp"}, {"PPM", "ppm;pnm"}, {"All Files", "*"}};
constexpr uint32_t kPictureFilterCount = sizeof kPictureFilters / sizeof kPictureFilters[0];
}  // namespace

void Paint::carry_out(Next next, StringView path) {
  if (next == Next::New) start_new(new_width_, new_height_);
  else if (next == Next::Open) show_open_dialog();
  else if (next == Next::OpenFile) read_picture(path);
}

void Paint::ask_open() {
  if (!can_open() || !manager()) return;   // (the menu item is disabled then)
  if (ready_for(Next::Open)) show_open_dialog();
}

void Paint::show_open_dialog() {
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kPictures, ".png", false, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kPictureFilters, kPictureFilterCount, start.view(), {&Paint::on_open_picked, this}))
    say("The Open dialog is already open for another window.");
}

void Paint::ask_save_as() {
  if (!can_open() || !manager()) {
    save_abandoned();
    return;
  }
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kPictures, ".png", true, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kPictureFilters, kPictureFilterCount, start.view(), {&Paint::on_save_picked, this})) {
    save_abandoned();
    say("The Save As dialog is already open for another window.");
  }
}

void Paint::on_open_picked(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  if (p->dialog_->accepted()) p->read_picture(p->dialog_->selected_path());   // (the unsaved changes were dealt with before the dialog opened)
}

void Paint::on_save_picked(void* self, Control&) {
  auto* p = static_cast<Paint*>(self);
  if (p->dialog_->accepted()) p->write_picture(p->dialog_->selected_path());
  else p->save_abandoned();
}

void Paint::start_new(uint32_t width, uint32_t height) {
  if (!canvas_.new_image(width, height)) {
    notify(NoticeKind::Error, "A picture of that size cannot be made. Pictures can be up to 4096 by 4096 dots.");
    return;
  }
  name_new_document();
  update_state();
  say("New picture.");
}

bool Paint::new_picture(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > DrawingCanvas::kMaxSide || height > DrawingCanvas::kMaxSide) {
    notify(NoticeKind::Error, "A picture of that size cannot be made. Pictures can be up to 4096 by 4096 dots.");
    return false;
  }
  new_width_ = width;
  new_height_ = height;
  if (!ready_for(Next::New)) return false;
  start_new(width, height);
  return true;
}

bool Paint::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) return false;
  if (!fs_) {
    say("There is no disk to open pictures from.");
    return false;
  }
  if (!ready_for(Next::OpenFile, path.view())) return false;
  return read_picture(path.view());
}

bool Paint::read_picture(StringView path) {
  if (!fs_) return false;
  const StringView name = name_of(path);
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::Read, &file);
  if (status != Status::Ok) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status, false));
    return false;
  }
  const uint64_t size = file->size();
  if (size == 0) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. The file is empty, so there is no picture in it.", name);
    return false;
  }
  if (size > kMaxFileBytes) {
    notify(NoticeKind::Error, "\"%1\" is too large for Paint, which opens files of up to 64 MB. Open it with another program.", name);
    return false;
  }
  ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator(), size);
  uint64_t got = 0;
  status = bytes.empty() ? Status::OutOfMemory : file->read(0, bytes, &got);
  file.reset();
  media::Image image(allocator());
  const bool decoded = status == Status::Ok && got == size && media::decode_image(ConstByteView(bytes.data(), got), &image);
  release_view(allocator(), bytes);
  if (!decoded) {
    if (status == Status::Ok) notify(NoticeKind::Error, "\"%1\" could not be opened. It is not a picture of a kind Paint can read, or the file is damaged.", name);
    else notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status, false));
    return false;
  }
  if (image.width() > DrawingCanvas::kMaxSide || image.height() > DrawingCanvas::kMaxSide ||
      !canvas_.set_pixels(image.frame(0).data(), image.width(), image.height())) {
    notify(NoticeKind::Error, "\"%1\" is too large for Paint, which works on pictures of up to 4096 by 4096 dots. Open it with another program.", name);
    return false;
  }
  name_document(path);
  update_state();
  String message(allocator());
  format_message(&message, "Opened \"%1\".", name);
  say(message.view());
  return true;
}

bool Paint::save_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested.empty() ? this->path() : requested, &path) || path.empty()) {
    ask_save_as();   // nowhere to save to yet: the same as Save As
    return false;
  }
  return write_picture(path.view());
}

bool Paint::write_picture(StringView requested) {
  String path(allocator());
  if (!fs_ || !path.assign(requested)) {
    save_abandoned();
    if (!fs_) say("There is no disk to save pictures on.");
    return false;
  }
  media::ImageFormat format = format_of_name(path.view());
  if (format == media::ImageFormat::Unknown) {  // no known extension: a PNG file with .png added
    if (!path.append(".png")) {
      save_abandoned();
      return false;
    }
    format = media::ImageFormat::Png;
  }
  const StringView name = name_of(path.view());
  canvas_.commit_floating();  // a picture still being placed is part of what is saved
  media::Image image(allocator());
  ByteBuffer encoded(allocator());
  const ArrayView<const uint32_t> pixels = canvas_.pixels();
  if (!image.create(canvas_.image_width(), canvas_.image_height())) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name, file_advice(Status::OutOfMemory, true));
    return false;
  }
  image.frame(0).copy_from(pixels);
  if (!media::encode_image(image, format, 90, &encoded)) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved as that kind of picture. Choose another type in Save As, such as PNG.", name);
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path.view(), OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok) status = file->write(0, encoded.view(), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  if (status != Status::Ok) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name, file_advice(status, true));
    return false;
  }
  fs_->flush();
  name_document(path.view());
  canvas_.mark_saved();
  update_state();
  String message(allocator());
  format_message(&message, "Saved \"%1\".", document_name());
  say(message.view());
  document_saved();   // (what waited for the save - New, Open, closing - goes ahead)
  return true;
}

bool Paint::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  if (args.modifiers & hardware::modifier::kControl) {
    switch (args.usage) {
      case key::kA + 13: new_picture(); return true;                 // N
      case key::kA + 14: ask_open(); return true;                    // O
      case key::kA + 15: ask_print(); return true;                   // P
      case key::kA + 18:                                             // S
        if (args.modifiers & hardware::modifier::kShift) ask_save_as();
        else if (can_save()) save_file();
        return true;
      case key::kA + 25:                                             // Z, with Shift the same as Y
        if (args.modifiers & hardware::modifier::kShift) canvas_.redo();
        else canvas_.undo();
        return true;
      case key::kA + 24: canvas_.redo(); return true;                // Y
      case key::kA: on_select_all(this, *this); return true;   // A
      case 0x2E: canvas_.zoom_in(); return true;                // + (the = key)
      case 0x57: canvas_.zoom_in(); return true;                // + on the numeric keypad
      case 0x2D: canvas_.zoom_out(); return true;               // -
      case 0x56: canvas_.zoom_out(); return true;               // - on the numeric keypad
      case key::k1 + 9: canvas_.set_zoom(1); return true;       // 0
      case key::kA + 2: copy_to_clipboard(false); return true;       // C
      case key::kA + 23: copy_to_clipboard(true); return true;       // X
      case key::kA + 21: paste_from_clipboard(); return true;        // V
      default: break;
    }
  }
  // Del removes the selection wherever the focus is in the window (the canvas does it itself when it has the focus).
  if (args.usage == key::kDelete && !args.modifiers && canvas_.has_selection()) {
    canvas_.delete_selection();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
