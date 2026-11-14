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

#include "Desktop/Todo.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

namespace {
constexpr int32_t kGap = 6, kRowHeight = 24, kLabelHeight = 22, kButtonHeight = 26, kLabelWidth = 88;
// The menu's "Move to" commands, one whole text per bucket (title capitalization, as menu items have it).
const char* const kMoveCommands[kBucketCount] = {"Move to Today", "Move to Tomorrow", "Move to This Week", "Move to Next Week", "Move to Someday"};
// The most of an item's text a question quotes (a line of the question holds about 45 characters).
constexpr uint64_t kQuoted = 28;

Rect2D rect(int32_t x, int32_t y, int32_t width, int32_t height) {
  return {x, y, static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(height > 0 ? height : 0)};
}
}  // namespace

Todo::Todo(allocators::IAllocator& allocator)
    : Frame(allocator), show_label_(allocator), text_label_(allocator), bucket_label_(allocator), status_(allocator), list_(allocator), text_(allocator), show_(allocator),
      bucket_(allocator), add_(allocator), save_(allocator), done_(allocator), delete_(allocator), clear_(allocator), item_menu_(allocator), ask_(allocator), items_(allocator) {
  set_title("To-Do");
  set_bounds({140, 40, 520, 500});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  // (each label ends in a colon and comes directly before its field: it carries the field's access key)
  show_label_.set_text("Show:");
  text_label_.set_text("To do:");
  bucket_label_.set_text("Assign to:");
  show_.add_item("All open items");
  for (uint32_t b = 0; b < kBucketCount; ++b) {
    show_.add_item(bucket_name(static_cast<Bucket>(b)));
    bucket_.add_item(bucket_name(static_cast<Bucket>(b)));
  }
  show_.add_item("Completed");
  show_.set_selected_index(kShowOpen);
  bucket_.set_selected_index(static_cast<int32_t>(Bucket::Today));
  add_.set_text("Add");
  save_.set_text("Save");
  done_.set_text("Mark Complete");
  delete_.set_text("Delete");
  clear_.set_text("Clear Completed");
  text_.set_max_length(TodoStore::kMaxTitleBytes);
  list_.add_column("To do", 330);
  list_.add_column("When", 90);
  list_.set_view_mode(ViewMode::Details);
  list_.set_copy_enabled(false);

  show_.on_selection_changed({&Todo::on_show, this});
  list_.on_selection_changed({&Todo::on_pick, this});
  list_.on_item_activated({&Todo::on_done, this});
  text_.on_changed({&Todo::on_text_changed, this});
  add_.on_click({&Todo::on_add, this});
  save_.on_click({&Todo::on_save, this});
  done_.on_click({&Todo::on_done, this});
  delete_.on_click({&Todo::on_delete, this});
  clear_.on_click({&Todo::on_clear, this});
  ask_.on_result({&Todo::on_answer, this});

  // The menu of an item: complete it (or put it back), move it to another bucket, delete it, and last the command that edits it.
  menu_done_ = item_menu_.add_item("Mark Complete", {&Todo::on_done, this});
  item_menu_.add_separator();
  for (uint32_t b = 0; b < kBucketCount; ++b) {
    move_targets_[b] = {this, static_cast<Bucket>(b)};
    menu_move_[b] = item_menu_.add_item(kMoveCommands[b], {&Todo::on_menu_move, &move_targets_[b]});
  }
  item_menu_.add_separator();
  menu_delete_ = item_menu_.add_item("Delete", {&Todo::on_delete, this});
  item_menu_.add_separator();
  menu_edit_ = item_menu_.add_item("Edit", {&Todo::on_edit, this});
  item_menu_.on_opening(Handler::immediate(&Todo::on_menu_opening, this));
  list_.set_context_menu(&item_menu_);

  Control* const parts[] = {&show_label_, &show_, &list_, &text_label_, &text_, &bucket_label_, &bucket_, &add_, &save_, &done_, &delete_, &clear_, &status_};
  for (Control* part : parts) add(part);
  refresh();
}

void Todo::set_store(TodoStore* store) {
  UiGuard lock(this);
  store_ = store;
  refresh();
}

uint32_t Todo::selected_id() const {
  const int32_t index = list_.selected_item();
  return index >= 0 && static_cast<uint64_t>(index) < items_.count() ? items_[static_cast<uint64_t>(index)].id : 0;
}

// A command that cannot work now is disabled, not left to answer with a message: nothing to add or save while the text field is empty, nothing to
// complete, delete or save while no item is chosen. Enter in the form presses Save while an open item is chosen and Add otherwise; that button has
// the heavier outline.
void Todo::update_buttons() {
  const bool chosen = selected_id() != 0, completed = showing_completed(), written = !text_.text().empty();
  const bool can_save = chosen && !completed && written, can_add = store_ != nullptr && written;
  done_.set_text(completed ? "Restore" : "Mark Complete");
  done_.set_enabled(chosen);
  delete_.set_enabled(chosen);
  save_.set_enabled(can_save);
  add_.set_enabled(can_add);
  save_.set_default(can_save);
  add_.set_default(can_add && !can_save);
  clear_.set_enabled(store_ && store_->done_count() > 0);
}

void Todo::refresh() {
  UiGuard lock(this);
  const uint32_t chosen = selected_id();
  items_.clear();
  list_.clear_items();
  if (store_) {
    const int32_t shown = show_.selected_index();
    if (shown == kShowOpen) store_->open_items(&items_);
    else if (shown == kShowCompleted) store_->done_items(&items_);
    else store_->open_items(static_cast<Bucket>(shown - kShowToday), &items_);
    for (const TodoStore::Item& item : items_) {
      const StringView cells[2] = {item.title.view(), StringView(bucket_name(item.bucket))};
      list_.add_item(cells, 2, ItemIcon::File);
    }
  }
  int32_t again = -1;
  for (uint64_t i = 0; i < items_.count(); ++i)
    if (items_[i].id == chosen) again = static_cast<int32_t>(i);
  list_.select_item(again);
  update_buttons();
}

void Todo::set_filter(Filter filter) {
  UiGuard lock(this);
  show_.set_selected_index(filter);
  if (filter >= kShowToday && filter <= kShowSomeday) bucket_.set_selected_index(filter - kShowToday);   // (what is added next belongs to the list on show)
  refresh();
}

void Todo::select_item(int32_t index) {
  UiGuard lock(this);
  list_.select_item(index);
  if (const uint32_t id = selected_id()) {
    TodoStore::Item item(allocator());
    if (store_ && store_->get(id, &item)) {
      text_.set_text(item.title.view());
      bucket_.set_selected_index(static_cast<int32_t>(item.bucket));
    }
  }
  update_buttons();
}

bool Todo::add_item() {
  UiGuard lock(this);
  if (!store_ || text_.text().empty()) {
    say(store_ ? "Write what there is to do, then choose Add." : "The list cannot be used because no disk is ready.");
    return false;
  }
  const Bucket bucket = static_cast<Bucket>(bucket_.selected_index() < 0 ? 0 : bucket_.selected_index());
  if (!store_->add(bucket, text_.text())) {
    say("The item could not be saved. The disk may be full.");
    return false;
  }
  text_.set_text("");
  if (showing_completed()) show_.set_selected_index(kShowOpen);   // (the new item is not among the completed ones)
  refresh();
  say(store_->persistent() ? "Added." : "Added, but only until shutdown: the disk cannot be written.");
  return true;
}

bool Todo::update_item() {
  UiGuard lock(this);
  const uint32_t id = selected_id();
  if (!store_ || !id || showing_completed()) {
    say("Choose an item to change.");
    return false;
  }
  if (text_.text().empty()) {
    say("Write what there is to do, then choose Save.");
    return false;
  }
  if (!store_->update(id, static_cast<Bucket>(bucket_.selected_index() < 0 ? 0 : bucket_.selected_index()), text_.text())) {
    say("The item could not be saved. The disk may be full.");
    return false;
  }
  refresh();
  say("Saved.");
  return true;
}

bool Todo::complete_item() {
  UiGuard lock(this);
  const uint32_t id = selected_id();
  if (!store_ || !id) {
    say("Choose an item first.");
    return false;
  }
  const bool restoring = showing_completed();
  if (!store_->set_done(id, !restoring)) {
    say("The change could not be saved. The disk may be full.");
    return false;
  }
  text_.set_text("");
  refresh();
  say(restoring ? "Put back on the list." : "Completed.");
  return true;
}

bool Todo::move_item(Bucket bucket) {
  UiGuard lock(this);
  const uint32_t id = selected_id();
  if (!store_ || !id || showing_completed()) {
    say("Choose an item to move.");
    return false;
  }
  if (!store_->set_bucket(id, bucket)) {
    say("The change could not be saved. The disk may be full.");
    return false;
  }
  refresh();
  if (list_.selected_item() >= 0) select_item(list_.selected_item());   // (the form follows the item to its new bucket)
  String text(allocator());
  if (format_message(&text, "Moved to \"%1\".", bucket_name(bucket))) say(text.view());
  return true;
}

bool Todo::remove_item(uint32_t id) {
  if (!store_ || !id || !store_->remove(id)) {
    say(id ? "The item could not be deleted. The disk may be full." : "Choose an item first.");
    return false;
  }
  text_.set_text("");
  refresh();
  say("Deleted.");
  return true;
}

bool Todo::delete_item() {
  UiGuard lock(this);
  return remove_item(selected_id());
}

uint32_t Todo::clear_completed() {
  UiGuard lock(this);
  const uint32_t n = store_ ? store_->remove_done() : 0;
  refresh();
  say(n ? "Completed items deleted." : "There are no completed items.");
  return n;
}

// A deleted item cannot be brought back, so the question comes first, and the button that keeps the item is the default: Enter and Escape both
// leave the list as it was.
bool Todo::ask_delete() {
  UiGuard lock(this);
  const uint32_t id = selected_id();
  TodoStore::Item item(allocator());
  WindowManager* m = manager();
  if (!m || !store_ || !id || !store_->get(id, &item)) return false;
  String name(allocator()), line(allocator());
  const StringView title = item.title.view();
  if (title.size() > kQuoted) {
    uint64_t cut = kQuoted;
    while (cut > 0 && (static_cast<uint8_t>(title[cut]) & 0xC0) == 0x80) --cut;   // (not in the middle of a character)
    if (!name.assign(title.first(cut)) || !name.append("...")) return false;
  } else if (!name.assign(title)) {
    return false;
  }
  if (!format_message(&line, "Delete \"%1\"?", name.view())) return false;
  pending_ = Pending::DeleteItem;
  pending_id_ = id;
  ask_.set_texts(this->title(), line.view(), "A deleted item cannot be brought back.", "Delete", "Cancel");
  ask_.set_default_choice(MessageDialog::Choice::Decline);
  return ask_.show(*m);
}

bool Todo::ask_clear_completed() {
  UiGuard lock(this);
  WindowManager* m = manager();
  if (!m || !store_ || store_->done_count() == 0) return false;
  pending_ = Pending::ClearCompleted;
  ask_.set_texts(title(), "Delete all completed items?", "Deleted items cannot be brought back.", "Delete", "Cancel");
  ask_.set_default_choice(MessageDialog::Choice::Decline);
  return ask_.show(*m);
}

void Todo::on_answer(void* self, Control&) {
  auto* t = static_cast<Todo*>(self);
  UiGuard lock(t);
  const Pending what = t->pending_;
  t->pending_ = Pending::None;
  if (!t->ask_.accepted()) return;
  if (what == Pending::DeleteItem) t->remove_item(t->pending_id_);
  else if (what == Pending::ClearCompleted) t->clear_completed();
}

bool Todo::edit_item() {
  UiGuard lock(this);
  if (!selected_id() || showing_completed()) return false;
  if (Window* w = window()) w->set_focus(&text_);
  text_.select_all();
  return true;
}

void Todo::on_show(void* self, Control&) {
  auto* t = static_cast<Todo*>(self);
  const int32_t shown = t->show_.selected_index();
  if (shown >= kShowToday && shown <= kShowSomeday) t->bucket_.set_selected_index(shown - kShowToday);
  t->refresh();
}
void Todo::on_pick(void* self, Control&) {
  auto* t = static_cast<Todo*>(self);
  t->select_item(t->list_.selected_item());
}
void Todo::on_add(void* self, Control&) { static_cast<Todo*>(self)->add_item(); }
void Todo::on_save(void* self, Control&) { static_cast<Todo*>(self)->update_item(); }
void Todo::on_done(void* self, Control&) { static_cast<Todo*>(self)->complete_item(); }
void Todo::on_delete(void* self, Control&) { static_cast<Todo*>(self)->ask_delete(); }
void Todo::on_clear(void* self, Control&) { static_cast<Todo*>(self)->ask_clear_completed(); }
void Todo::on_edit(void* self, Control&) { static_cast<Todo*>(self)->edit_item(); }
void Todo::on_text_changed(void* self, Control&) {
  auto* t = static_cast<Todo*>(self);
  UiGuard lock(t);
  t->update_buttons();
}

// Enables what applies to the chosen item: a completed one can be put back or deleted, an open one moved to any bucket but its own, or edited.
void Todo::on_menu_opening(void* self, Control&) {
  auto* t = static_cast<Todo*>(self);
  const bool chosen = t->selected_id() != 0, completed = t->showing_completed();
  TodoStore::Item item(t->allocator());
  const bool known = chosen && t->store_ && t->store_->get(t->selected_id(), &item);
  t->menu_done_->text.assign(completed ? "Restore" : "Mark Complete");
  t->menu_done_->set_enabled(chosen);
  t->menu_delete_->set_enabled(chosen);
  t->menu_edit_->set_enabled(chosen && !completed);
  for (uint32_t b = 0; b < kBucketCount; ++b) {
    const bool current = known && static_cast<uint32_t>(item.bucket) == b;
    t->menu_move_[b]->set_checked(current);
    t->menu_move_[b]->set_enabled(known && !completed && !current);
  }
}
void Todo::on_menu_move(void* target, Control&) {
  auto* move = static_cast<MoveTarget*>(target);
  move->todo->move_item(move->bucket);
}

// Delete in the list deletes the chosen item (after the question). Enter that no control used (the text field, the bucket list) presses the default
// button: Save while an open item is chosen, Add otherwise.
bool Todo::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool plain = !(args.modifiers & (hardware::modifier::kControl | hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt));
  if (plain && args.usage == key::kDelete && list_.focused() && selected_id()) {
    ask_delete();
    return true;
  }
  if (plain && (args.usage == key::kEnter || args.usage == key::kKeypadEnter)) {
    if (save_.is_default()) save_.click();
    else if (add_.is_default()) add_.click();
    return true;
  }
  return Frame::on_key(args);
}

void Todo::layout() {
  Frame::layout();
  const Rect2D area = client_rect();
  const int32_t left = area.x, top = area.y, width = static_cast<int32_t>(area.width), height = static_cast<int32_t>(area.height);
  int32_t y = top;
  place(show_label_, rect(left, y, kLabelWidth, kRowHeight));
  place(show_, rect(left + kLabelWidth, y, 180, kRowHeight));
  y += kRowHeight + kGap;
  const int32_t form_height = 2 * (kRowHeight + 4) + kButtonHeight + kGap + kLabelHeight;
  const int32_t list_height = height - (y - top) - form_height - kGap;
  place(list_, rect(left, y, width, list_height > 60 ? list_height : 60));
  y += (list_height > 60 ? list_height : 60) + kGap;
  place(text_label_, rect(left, y, kLabelWidth, kRowHeight));
  place(text_, rect(left + kLabelWidth, y, width - kLabelWidth, kRowHeight));
  y += kRowHeight + 4;
  place(bucket_label_, rect(left, y, kLabelWidth, kRowHeight));
  place(bucket_, rect(left + kLabelWidth, y, 140, kRowHeight));
  y += kRowHeight + 4;
  // The buttons are as wide as their texts need (the texts differ a lot in length), with the same gap between them.
  int32_t x = left;
  Button* const row[] = {&add_, &save_, &done_, &delete_, &clear_};
  for (Button* button : row) {
    const int32_t w = button == &done_ ? 128 : button == &clear_ ? 144 : 64;
    place(*button, rect(x, y, w, kButtonHeight));
    x += w + kGap;
  }
  y += kButtonHeight + kGap;
  place(status_, rect(left, y, width, kLabelHeight));
}

}  // namespace desktop
