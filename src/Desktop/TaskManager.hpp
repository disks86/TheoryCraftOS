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

// The task manager: a window with the processor load (overall and per core), memory use and a table of the
// running kernel threads with their state, core and share of the processor. It reads the machine through
// an ISystemMonitor and refreshes itself when update() is called (the desktop does so once a second while the
// window is open). Click a column title to sort the table.
#pragma once
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/LevelMeter.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

enum class TaskState : uint8_t { Ready, Running, Blocked };

struct TaskThread {
  uint64_t id = 0;
  char name[24] = {};
  TaskState state = TaskState::Ready;
  bool idle = false;       // a core's idle thread
  int32_t core = -1;       // the core it runs on right now, or -1
  uint64_t cpu_ticks = 0;  // cumulative
};

struct TaskCore {
  bool online = false;
  bool efficiency = false;
  uint64_t busy_ticks = 0, idle_ticks = 0;  // cumulative
};

struct TaskMemory {
  uint64_t total_bytes = 0, free_bytes = 0;
};

// Where the task manager gets its numbers (the kernel's, in the real system). All counters only grow.
class ISystemMonitor {
 public:
  // How many ticks a core counts per second (the unit of cpu_ticks, busy_ticks and idle_ticks).
  virtual uint32_t ticks_per_second() const = 0;
  virtual uint32_t core_count() const = 0;
  virtual bool core(uint32_t index, TaskCore* out) = 0;
  // Writes up to `capacity` threads; returns how many.
  virtual uint32_t threads(TaskThread* out, uint32_t capacity) = 0;
  virtual TaskMemory memory() = 0;

 protected:
  constexpr ISystemMonitor() = default;
  ~ISystemMonitor() = default;
};

class TaskManager : public gui::Frame {
 public:
  static constexpr uint32_t kMaxThreads = 256;
  static constexpr uint32_t kMaxCoresShown = 16;

  explicit TaskManager(allocators::IAllocator& allocator);
  ~TaskManager();

  // The source of the numbers (nullptr: none); it must outlive the task manager.
  void set_monitor(ISystemMonitor* monitor);
  // Reads the monitor and refreshes everything. `now_us` is the uptime; load figures are over the time since
  // the previous call.
  void update(uint64_t now_us);

  gui::DataGrid& grid() { return grid_; }
  gui::LevelMeter& cpu_meter() { return cpu_meter_; }
  gui::LevelMeter& memory_meter() { return memory_meter_; }
  gui::Label& cpu_label() { return cpu_value_; }
  gui::Label& memory_label() { return memory_value_; }
  gui::Label& info_label() { return info_; }
  gui::Label& cores_label() { return cores_; }
  // The CPU share (tenths of a percent of one core) of the thread with this id, or -1 if it is not listed.
  int32_t thread_load_tenths(uint64_t id) const;

 private:
  struct Row {
    uint64_t id = 0;
    char name[24] = {};
    TaskState state = TaskState::Ready;
    bool idle = false;
    int32_t core = -1;
    uint32_t tenths = 0;  // share of one core, in tenths of a percent
    uint64_t ticks = 0;
  };
  struct Previous {
    uint64_t id, ticks;
  };

  class Model final : public gui::IGridModel {
   public:
    explicit Model(allocators::IAllocator& allocator) : rows(allocator) {}
    uint32_t row_count() const override { return static_cast<uint32_t>(rows.count()); }
    bool cell_text(uint32_t row, uint32_t column, String* out) const override;
    List<Row> rows;
    uint32_t ticks_per_second = 1000;
  };

  gui::Panel perf_, cpu_row_, memory_row_;
  gui::Label cpu_title_, cpu_value_, memory_title_, memory_value_, info_, cores_;
  gui::LevelMeter cpu_meter_, memory_meter_;
  gui::DataGrid grid_;
  Model model_;
  ISystemMonitor* monitor_ = nullptr;
  List<TaskCore> previous_cores_;
  List<Previous> previous_threads_;
  ArrayView<TaskThread> buffer_;
  uint64_t last_us_ = 0;
  bool have_previous_ = false;
};

}  // namespace desktop
