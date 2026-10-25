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

#include "Desktop/WebNotifications.hpp"
#include "Utility/Move.hpp"

namespace desktop {

WebNotifications::WebNotifications(allocators::IAllocator& allocator, gui::ToastCenter& toasts, gui::MessageDialog& dialog, gui::WindowManager& windows,
                                   browser::ServiceWorkerHub& hub)
    : allocator_(allocator), toasts_(toasts), dialog_(dialog), windows_(windows), hub_(hub), entries_(allocator), questions_(allocator), asking_(allocator), mic_allowed_(allocator), mic_blocked_(allocator) {}

namespace {

bool listed(const List<String>& list, StringView text) {
  for (const String& s : list)
    if (s.view().equals(text)) return true;
  return false;
}

}  // namespace

// Puts a question to the user unless it is open already; -1 (the answer comes later), or 0 if there is no room for it.
static int enqueue(List<String>& questions, String& asking, allocators::IAllocator& allocator, uint32_t limit, StringView prefix, StringView origin) {
  String key(allocator);
  key.assign(prefix);
  key.append(origin);
  if (asking.view().equals(key.view()) || listed(questions, key.view())) return -1;
  return questions.count() >= limit || !questions.push_back(util::move(key)) ? 0 : -1;
}

int WebNotifications::prompt(StringView origin) {
  mutex_.lock();
  const int answer = enqueue(questions_, asking_, allocator_, kMaxQuestions, "n:", origin);
  mutex_.unlock();
  return answer;
}

int WebNotifications::microphone_state(StringView origin) const {
  mutex_.lock();
  const int state = listed(mic_allowed_, origin) ? 1 : listed(mic_blocked_, origin) ? 0 : -1;
  mutex_.unlock();
  return state;
}

int WebNotifications::ask_microphone(StringView origin) {
  const int state = microphone_state(origin);
  if (state >= 0) return state;
  mutex_.lock();
  const int answer = enqueue(questions_, asking_, allocator_, kMaxQuestions, "m:", origin);
  mutex_.unlock();
  return answer;
}

void WebNotifications::on_click(void* context, gui::Control&) { __atomic_store_n(&static_cast<Entry*>(context)->clicked, true, __ATOMIC_RELEASE); }

// (the handler runs inside the click, so the center still knows which button it was)
void WebNotifications::on_action(void* context, gui::Control& sender) {
  Entry* entry = static_cast<Entry*>(context);
  __atomic_store_n(&entry->action, static_cast<gui::ToastCenter&>(sender).last_action(), __ATOMIC_RELAXED);
  __atomic_store_n(&entry->clicked, true, __ATOMIC_RELEASE);
}

uint32_t WebNotifications::show(StringView origin, StringView title, StringView body, bool sticky, StringView actions) {
  UniquePointer<Entry> entry = UniquePointer<Entry>::make(allocator_);
  if (!entry) return 0;
  // (the text of a toast is what the page said; the site is named when the page said nothing else)
  StringView labels[gui::ToastCenter::kMaxActions];   // (one button for each line of `actions`)
  uint32_t label_count = 0;
  StringView rest = actions, line;
  while (label_count < gui::ToastCenter::kMaxActions && rest.next_token('\n', &line)) labels[label_count++] = line;
  entry->toast = toasts_.post(title, body.empty() ? origin : body, gui::NoticeKind::Information, sticky ? 0 : kDurationMs, gui::Handler::immediate(&WebNotifications::on_click, entry.get()),
                              ArrayView<const StringView>(labels, label_count), gui::Handler::immediate(&WebNotifications::on_action, entry.get()));
  if (entry->toast == 0) return 0;
  kernel::RecursiveMutex& m = mutex_;
  m.lock();
  entry->number = next_number_++;
  if (next_number_ == 0) next_number_ = 1;
  const uint32_t number = entry->number, toast = entry->toast;
  const bool kept = entries_.push_back(util::move(entry));
  m.unlock();
  if (!kept) {
    toasts_.dismiss(toast);
    return 0;
  }
  return number;
}

void WebNotifications::close(uint32_t number) {
  mutex_.lock();
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    if (entries_[i]->number != number) continue;
    toasts_.dismiss(entries_[i]->toast);   // (the toast is gone before the entry its handler points at)
    entries_.remove_at(i);
    break;
  }
  mutex_.unlock();
}

uint32_t WebNotifications::showing() const {
  mutex_.lock();
  const uint32_t n = static_cast<uint32_t>(entries_.count());
  mutex_.unlock();
  return n;
}

void WebNotifications::pump() {
  struct Event {
    uint32_t number;
    bool clicked;
    int32_t action;
  };
  Event events[16];
  uint32_t event_count = 0;
  String answered(allocator_), next(allocator_);
  bool granted = false, have_answer = false;
  mutex_.lock();
  for (uint64_t i = 0; i < entries_.count() && event_count < 16;) {
    const bool clicked = __atomic_load_n(&entries_[i]->clicked, __ATOMIC_ACQUIRE);
    if (!clicked && toasts_.showing(entries_[i]->toast)) {
      ++i;
      continue;
    }
    events[event_count++] = {entries_[i]->number, clicked, __atomic_load_n(&entries_[i]->action, __ATOMIC_RELAXED)};
    entries_.remove_at(i);
  }
  if (!asking_.empty() && !dialog_.showing()) {   // the user answered
    granted = dialog_.accepted();
    answered.assign(asking_.view());
    asking_.clear();
    have_answer = true;
  }
  if (asking_.empty() && questions_.count() > 0) {
    next.assign(questions_[0].view());
    questions_.remove_at(0);
    asking_.assign(next.view());
  }
  mutex_.unlock();
  if (!next.empty()) {
    const bool microphone = next.view().starts_with("m:");
    String line(allocator_);
    line.append(next.view().substr(2));
    line.append(microphone ? " wants to use your microphone." : " wants to show notifications.");
    if (microphone) dialog_.set_texts("Microphone", line.view(), "Allow this site to use the microphone?", "Allow", "Block");
    else dialog_.set_texts("Notifications", line.view(), "Allow this site to show notifications?", "Allow", "Block");
    if (!dialog_.show(windows_)) {   // (it cannot be asked: the answer is no)
      mutex_.lock();
      asking_.clear();
      if (microphone) {
        String copy(allocator_);
        copy.assign(next.view().substr(2));
        mic_blocked_.push_back(util::move(copy));
      }
      mutex_.unlock();
      if (!microphone) hub_.set_notification_permission(next.view().substr(2), false);
    }
  }
  if (have_answer) {
    const StringView origin = answered.view().substr(2);
    if (answered.view().starts_with("m:")) {
      String copy(allocator_);
      copy.assign(origin);
      mutex_.lock();
      (granted ? mic_allowed_ : mic_blocked_).push_back(util::move(copy));
      mutex_.unlock();
    } else {
      hub_.set_notification_permission(origin, granted);
    }
  }
  for (uint32_t i = 0; i < event_count; ++i) {   // (outside the lock: the hub runs the worker's handlers, which may show or close notifications)
    if (events[i].clicked) hub_.notification_activated(events[i].number, events[i].action);
    else hub_.notification_closed(events[i].number);
  }
}

}  // namespace desktop
