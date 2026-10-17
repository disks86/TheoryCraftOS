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

#include "Desktop/TaskManager.hpp"
#include "Desktop/Format.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using namespace gui;

bool TaskManager::Model::cell_text(uint32_t row, uint32_t column, String* out) const {
  out->clear();
  if (row >= rows.count()) return true;
  const Row& r = rows[row];
  switch (column) {
    case 0: return r.idle ? out->assign("-") : out->append_uint(r.id);
    case 1: {
      uint64_t length = 0;
      while (length < sizeof r.name && r.name[length]) ++length;
      return out->assign(StringView(r.name, length));
    }
    case 2: return out->assign(r.state == TaskState::Running ? "Running" : r.state == TaskState::Blocked ? "Blocked" : "Ready");
    case 3: return r.core < 0 || out->append_uint(static_cast<uint64_t>(r.core));
    case 4: return append_tenths(out, r.tenths);
    case 5: return append_tenths(out, r.ticks * 10 / (ticks_per_second ? ticks_per_second : 1));
    default: return true;
  }
}

TaskManager::TaskManager(allocators::IAllocator& allocator)
    : Frame(allocator),
      perf_(allocator),
      cpu_row_(allocator),
      memory_row_(allocator),
      cpu_title_(allocator),
      cpu_value_(allocator),
      memory_title_(allocator),
      memory_value_(allocator),
      info_(allocator),
      cores_(allocator),
      cpu_meter_(allocator),
      memory_meter_(allocator),
      grid_(allocator),
      model_(allocator),
      previous_cores_(allocator),
      previous_threads_(allocator) {
  buffer_ = allocate_view<TaskThread>(allocator, kMaxThreads);
  set_title("Task Manager");
  set_bounds({90, 50, 480, 380});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  perf_.set_dock(DockStyle::Top);
  perf_.set_size(0, 92);
  perf_.set_padding(Insets::all(2));

  struct RowParts {
    Panel& row;
    Label& title;
    Label& value;
    LevelMeter& meter;
    const char* text;
  };
  const RowParts rows[2] = {{cpu_row_, cpu_title_, cpu_value_, cpu_meter_, "CPU"}, {memory_row_, memory_title_, memory_value_, memory_meter_, "Memory"}};
  for (const RowParts& r : rows) {
    r.row.set_style(PanelStyle::Transparent);
    r.row.set_dock(DockStyle::Top);
    r.row.set_size(0, 24);
    r.title.set_text(r.text);
    r.title.set_dock(DockStyle::Left);
    r.title.set_size(64, 0);
    r.value.set_dock(DockStyle::Right);
    r.value.set_size(170, 0);
    r.value.set_text("-");
    r.meter.set_peak_hold(false);
    r.meter.set_dock(DockStyle::Fill);
    r.row.add(&r.title);
    r.row.add(&r.value);
    r.row.add(&r.meter);
    perf_.add(&r.row);
  }
  info_.set_dock(DockStyle::Top);
  info_.set_size(0, 20);
  cores_.set_dock(DockStyle::Top);
  cores_.set_size(0, 20);
  perf_.add(&info_);
  perf_.add(&cores_);

  grid_.add_column("ID", 44, Align::Right);
  grid_.add_column("Thread", 150);
  grid_.add_column("State", 70);
  grid_.add_column("Core", 44, Align::Right);
  grid_.add_column("CPU %", 56, Align::Right);
  grid_.add_column("CPU s", 70, Align::Right);
  grid_.set_model(&model_);
  grid_.set_dock(DockStyle::Fill);

  add(&perf_);
  add(&grid_);  // Fill goes last
}

TaskManager::~TaskManager() { release_view(allocator(), buffer_); }

void TaskManager::set_monitor(ISystemMonitor* monitor) {
  monitor_ = monitor;
  have_previous_ = false;
  previous_cores_.clear();
  previous_threads_.clear();
  model_.ticks_per_second = monitor ? monitor->ticks_per_second() : 1000;
}

int32_t TaskManager::thread_load_tenths(uint64_t id) const {
  for (const Row& r : model_.rows)
    if (r.id == id && !r.idle) return static_cast<int32_t>(r.tenths);
  return -1;
}

void TaskManager::update(uint64_t now_us) {
  UiGuard lock(this);
  if (!monitor_ || buffer_.empty()) {
    info_.set_text("No system monitor available");
    return;
  }
  const uint64_t hz = monitor_->ticks_per_second() ? monitor_->ticks_per_second() : 1;
  const uint64_t elapsed_us = have_previous_ && now_us > last_us_ ? now_us - last_us_ : 0;

  // --- Processor: busy share per core since the last call, and over all online cores.
  const uint32_t cores = monitor_->core_count();
  List<TaskCore> current(allocator());
  uint64_t all_busy = 0, all_total = 0;
  String text(allocator());
  uint32_t online = 0, shown = 0;
  for (uint32_t i = 0; i < cores; ++i) {
    TaskCore core;
    if (!monitor_->core(i, &core)) core = TaskCore();
    current.push_back(core);
    if (!core.online) continue;
    ++online;
    uint64_t busy = 0, idle = 0;
    if (have_previous_ && i < previous_cores_.count()) {
      busy = core.busy_ticks - previous_cores_[i].busy_ticks;
      idle = core.idle_ticks - previous_cores_[i].idle_ticks;
    }
    all_busy += busy;
    all_total += busy + idle;
    if (shown < kMaxCoresShown) {
      if (shown == 0) text.assign("Cores: ");
      else text.append("  ");
      text.append_uint(i);
      text.push_back(core.efficiency ? 'e' : ' ');
      text.push_back(':');
      text.append_uint(busy + idle ? busy * 100 / (busy + idle) : 0);
      text.push_back('%');
      ++shown;
    }
  }
  if (online > shown) text.append(" ...");
  cores_.set_text(text.view());
  const uint64_t cpu_percent = all_total ? all_busy * 100 / all_total : 0;
  cpu_meter_.set_level(static_cast<int32_t>(cpu_percent));
  text.clear();
  text.append_uint(cpu_percent);
  text.append("%");
  cpu_value_.set_text(text.view());

  // --- Memory.
  const TaskMemory memory = monitor_->memory();
  const uint64_t used = memory.total_bytes > memory.free_bytes ? memory.total_bytes - memory.free_bytes : 0;
  memory_meter_.set_level(memory.total_bytes ? static_cast<int32_t>(used * 100 / memory.total_bytes) : 0);
  String used_text(allocator()), total_text(allocator());
  format_size(used, &used_text);
  format_size(memory.total_bytes, &total_text);
  text.assign(used_text.view());
  text.append(" / ");
  text.append(total_text.view());
  memory_value_.set_text(text.view());

  // --- Threads: load since the last call, in tenths of a percent of one core.
  const uint32_t count = monitor_->threads(buffer_.data(), kMaxThreads);
  const uint64_t selected_id = grid_.selected_row() >= 0 && static_cast<uint32_t>(grid_.selected_row()) < model_.rows.count()
                                   ? model_.rows[static_cast<uint32_t>(grid_.selected_row())].id
                                   : ~0ull;
  const uint64_t period_ticks = elapsed_us * hz / 1000000;  // ticks one core counts meanwhile
  model_.rows.clear();
  List<Previous> next_previous(allocator());
  int32_t reselect = -1;
  uint32_t shown_threads = 0;
  for (uint32_t i = 0; i < count && i < kMaxThreads; ++i) {
    const TaskThread& t = buffer_[i];
    Row row;
    row.id = t.id;
    __builtin_memcpy(row.name, t.name, sizeof row.name);
    row.name[sizeof row.name - 1] = 0;
    row.state = t.state;
    row.idle = t.idle;
    row.core = t.core;
    row.ticks = t.cpu_ticks;
    uint64_t before = t.cpu_ticks;
    bool known = false;
    for (const Previous& p : previous_threads_)
      if (p.id == t.id && !t.idle) {
        before = p.ticks;
        known = true;
        break;
      }
    if (have_previous_ && period_ticks && (known || t.idle)) {
      const uint64_t delta = t.idle ? 0 : t.cpu_ticks - before;
      row.tenths = static_cast<uint32_t>(delta * 1000 / period_ticks);
    }
    if (!t.idle) next_previous.push_back(Previous{t.id, t.cpu_ticks});
    if (!t.idle && t.id == selected_id) reselect = static_cast<int32_t>(model_.rows.count());
    model_.rows.push_back(row);
    ++shown_threads;
  }
  previous_threads_.clear();
  for (const Previous& p : next_previous) previous_threads_.push_back(p);
  previous_cores_.clear();
  for (const TaskCore& c : current) previous_cores_.push_back(c);
  last_us_ = now_us;
  have_previous_ = true;
  grid_.model_changed();
  grid_.select_row(reselect);

  // --- Summary line.
  text.assign("Threads: ");
  text.append_uint(shown_threads);
  text.append("   Cores: ");
  text.append_uint(online);
  text.append("   Uptime: ");
  const uint64_t seconds = now_us / 1000000;
  auto two = [&](uint64_t v) {
    text.push_back(static_cast<char>('0' + (v / 10) % 10));
    text.push_back(static_cast<char>('0' + v % 10));
  };
  two(seconds / 3600);
  text.push_back(':');
  two(seconds / 60 % 60);
  text.push_back(':');
  two(seconds % 60);
  info_.set_text(text.view());
}

}  // namespace desktop
