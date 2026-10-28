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

namespace desktop {

using namespace gui;

namespace {
constexpr int32_t kGap = 6, kRowHeight = 24, kLabelHeight = 22, kButtonHeight = 26;

Rect2D rect(int32_t x, int32_t y, int32_t width, int32_t height) {
  return {x, y, static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(height > 0 ? height : 0)};
}
}  // namespace

Todo::Todo(allocators::IAllocator& allocator)
    : Frame(allocator), show_label_(allocator), text_label_(allocator), bucket_label_(allocator), status_(allocator), list_(allocator), text_(allocator), show_(allocator),
      bucket_(allocator), add_(allocator), save_(allocator), done_(allocator), delete_(allocator), clear_(allocator), items_(allocator) {
  set_title("To-Do");
  set_bounds({140, 40, 520, 500});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  show_label_.set_text("Show");
  text_label_.set_text("To do");
  bucket_label_.set_text("Assign to");
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
  done_.set_text("Done");
  delete_.set_text("Delete");
  clear_.set_text("Clear completed");
  text_.set_max_length(TodoStore::kMaxTitleBytes);
  list_.add_column("To do", 330);
  list_.add_column("When", 90);
  list_.set_view_mode(ViewMode::Details);
  list_.set_copy_enabled(false);

  show_.on_selection_changed({&Todo::on_show, this});
  list_.on_selection_changed({&Todo::on_pick, this});
  list_.on_item_activated({&Todo::on_done, this});
  add_.on_click({&Todo::on_add, this});
  save_.on_click({&Todo::on_save, this});
  done_.on_click({&Todo::on_done, this});
  delete_.on_click({&Todo::on_delete, this});
  clear_.on_click({&Todo::on_clear, this});

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

void Todo::update_buttons() {
  const bool chosen = selected_id() != 0, completed = showing_completed();
  done_.set_text(completed ? "Restore" : "Done");
  done_.set_enabled(chosen);
  delete_.set_enabled(chosen);
  save_.set_enabled(chosen && !completed);
  add_.set_enabled(store_ != nullptr);
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
    say(store_ ? "Write what there is to do." : "There is no storage for the list.");
    return false;
  }
  const Bucket bucket = static_cast<Bucket>(bucket_.selected_index() < 0 ? 0 : bucket_.selected_index());
  if (!store_->add(bucket, text_.text())) {
    say("The item could not be saved.");
    return false;
  }
  text_.set_text("");
  if (showing_completed()) show_.set_selected_index(kShowOpen);   // (the new item is not among the completed ones)
  refresh();
  say(store_->persistent() ? "Added." : "Added (the storage cannot be written: it is kept until shutdown).");
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
    say("Write what there is to do.");
    return false;
  }
  if (!store_->update(id, static_cast<Bucket>(bucket_.selected_index() < 0 ? 0 : bucket_.selected_index()), text_.text())) {
    say("The item could not be saved.");
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
    say("The change could not be saved.");
    return false;
  }
  text_.set_text("");
  refresh();
  say(restoring ? "Put back on the list." : "Completed.");
  return true;
}

bool Todo::delete_item() {
  UiGuard lock(this);
  const uint32_t id = selected_id();
  if (!store_ || !id || !store_->remove(id)) {
    say(id ? "The item could not be deleted." : "Choose an item first.");
    return false;
  }
  text_.set_text("");
  refresh();
  say("Deleted.");
  return true;
}

uint32_t Todo::clear_completed() {
  UiGuard lock(this);
  const uint32_t n = store_ ? store_->remove_done() : 0;
  refresh();
  say(n ? "Completed items deleted." : "Nothing to clear.");
  return n;
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
void Todo::on_delete(void* self, Control&) { static_cast<Todo*>(self)->delete_item(); }
void Todo::on_clear(void* self, Control&) { static_cast<Todo*>(self)->clear_completed(); }

void Todo::layout() {
  Frame::layout();
  const Rect2D area = client_rect();
  const int32_t left = area.x, top = area.y, width = static_cast<int32_t>(area.width), height = static_cast<int32_t>(area.height);
  int32_t y = top;
  place(show_label_, rect(left, y, 48, kRowHeight));
  place(show_, rect(left + 48, y, 180, kRowHeight));
  y += kRowHeight + kGap;
  const int32_t form_height = 2 * (kRowHeight + 4) + kButtonHeight + kGap + kLabelHeight;
  const int32_t list_height = height - (y - top) - form_height - kGap;
  place(list_, rect(left, y, width, list_height > 60 ? list_height : 60));
  y += (list_height > 60 ? list_height : 60) + kGap;
  place(text_label_, rect(left, y, 72, kRowHeight));
  place(text_, rect(left + 72, y, width - 72, kRowHeight));
  y += kRowHeight + 4;
  place(bucket_label_, rect(left, y, 72, kRowHeight));
  place(bucket_, rect(left + 72, y, 140, kRowHeight));
  y += kRowHeight + 4;
  place(add_, rect(left, y, 64, kButtonHeight));
  place(save_, rect(left + 70, y, 64, kButtonHeight));
  place(done_, rect(left + 140, y, 72, kButtonHeight));
  place(delete_, rect(left + 218, y, 64, kButtonHeight));
  place(clear_, rect(left + 288, y, 130, kButtonHeight));
  y += kButtonHeight + kGap;
  place(status_, rect(left, y, width, kLabelHeight));
}

}  // namespace desktop
