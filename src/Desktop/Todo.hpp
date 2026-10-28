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

// The to-do application: a list of the things still to do, each assigned to Today, Tomorrow, This week, Next week or Someday. A line of text and the
// bucket to put it in are added with "Add"; "Done" (or Enter or a double click on the item) marks the chosen item as completed, and it leaves the list.
// "Show" narrows the list to one bucket, or shows the completed items instead (where "Restore" puts one back and "Clear completed" deletes them for
// good). The items are kept in the TodoStore (the application's storage under /AppStore/Todo), so they are still there after a restart. Like every program
// window it can be a tab of a tabbed window, dragged out into its own window and back.
#pragma once
#include "Desktop/TodoStore.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Todo : public gui::Frame {
 public:
  // What "Show" offers: every open item, the open items of one bucket (Bucket + 1), or the completed ones.
  enum Filter : int32_t { kShowOpen, kShowToday, kShowTomorrow, kShowThisWeek, kShowNextWeek, kShowSomeday, kShowCompleted };

  explicit Todo(allocators::IAllocator& allocator);

  // The items (nullptr: none; the window then shows nothing). The store must outlive the window.
  void set_store(TodoStore* store);

  // The commands behind the buttons. add_item() and update_item() take the form as it is; they return false (and say why in status()) when the text is empty,
  // (update) no item is chosen or the store refuses. complete_item() marks the chosen item done, or open again while the completed ones are shown.
  bool add_item();
  bool update_item();
  bool complete_item();
  bool delete_item();
  uint32_t clear_completed();
  void set_filter(Filter filter);
  Filter filter() const { return static_cast<Filter>(show_.selected_index()); }
  // Chooses the item at `index` in the list (-1: none) and puts it in the form.
  void select_item(int32_t index);
  // Shows the items again (after the store changed).
  void refresh();

  uint32_t item_count() const { return static_cast<uint32_t>(items_.count()); }
  // The number of the item chosen in the list, 0 if none.
  uint32_t selected_id() const;
  StringView status() const { return status_.text(); }
  gui::ListView& list() { return list_; }
  gui::TextBox& text_box() { return text_; }
  gui::DropDown& bucket_box() { return bucket_; }
  gui::DropDown& show_box() { return show_; }
  gui::Button& add_button() { return add_; }
  gui::Button& save_button() { return save_; }
  gui::Button& done_button() { return done_; }
  gui::Button& delete_button() { return delete_; }
  gui::Button& clear_button() { return clear_; }

  void layout() override;

 private:
  static void on_show(void* self, gui::Control&);
  static void on_pick(void* self, gui::Control&);
  static void on_add(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_done(void* self, gui::Control&);
  static void on_delete(void* self, gui::Control&);
  static void on_clear(void* self, gui::Control&);
  void say(StringView text) { status_.set_text(text); }
  bool showing_completed() const { return show_.selected_index() == kShowCompleted; }
  void update_buttons();   // enables what applies to the chosen item, and names the Done button (Restore for a completed item)

  gui::Label show_label_, text_label_, bucket_label_, status_;
  gui::ListView list_;
  gui::TextBox text_;
  gui::DropDown show_, bucket_;
  gui::Button add_, save_, done_, delete_, clear_;
  TodoStore* store_ = nullptr;
  List<TodoStore::Item> items_;   // the items on show, in list order
};

}  // namespace desktop
