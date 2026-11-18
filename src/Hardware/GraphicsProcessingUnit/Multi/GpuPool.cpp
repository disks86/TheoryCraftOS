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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"

namespace hardware {


GpuPool::~GpuPool() {
  for (uint32_t i = 0; i < manager_.count(); ++i) {
    GpuNode& n = nodes_[i];
    if (n.destroy) n.destroy(n, allocator_);
    if (n.leased_for_driver) manager_.release(i);
    n.probe = nullptr;
    probes_[i] = UniquePointer<IGpuProbe>();
  }
}

void GpuPool::initialize() {
  manager_.initialize();
  for (uint32_t i = 0; i < GpuTopology::kMaxAdapters; ++i) {
    nodes_[i] = GpuNode{};
    probes_[i] = UniquePointer<IGpuProbe>();
    probe_wakes_[i] = 0;
  }
  for (uint32_t i = 0; i < manager_.count(); ++i) nodes_[i].adapter = manager_.topology().adapter(i);
}

bool GpuPool::adopt(uint32_t index, IGraphicsProcessingUnit* gpu, VirtioGpu* virtio, VmwareSvgaGpu* vmware) {
  if (index >= manager_.count() || !gpu || nodes_[index].gpu) return false;
  nodes_[index].gpu = gpu;
  nodes_[index].virtio = virtio;
  nodes_[index].vmware = vmware;
  return true;
}

uint32_t GpuPool::driver_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < manager_.count(); ++i)
    if (nodes_[i].gpu) ++n;
  return n;
}

uint32_t GpuPool::bind_probes(efi::SystemTable* st, const IPciBus& pci, IRegisterMapper* registers, GpuProbeFactory factory) {
  uint32_t probes = 0;
  for (uint32_t i = 0; i < manager_.count(); ++i) {
    GpuNode& n = nodes_[i];
    if (n.gpu) continue;
    if (!n.probe && factory && manager_.acquire(i)) {
      const GpuContext context{st, &pci, n.adapter.device, &allocator_, registers};
      UniquePointer<IGpuProbe> probe = factory(context, allocator_);
      if (probe && probe->start()) {
        probes_[i] = util::move(probe);
        n.probe = probes_[i].get();
        probe_wakes_[i] = manager_.wake_count(i);
      }
      manager_.release(i);
    }
    if (n.probe) ++probes;
    // Nothing on it keeps state that a power cut would lose (a probe reads it again), so its power may go.
    manager_.allow_cold(i, true);
  }
  return probes;
}

bool GpuPool::resume_probe(uint32_t index) {
  const uint32_t wakes = manager_.wake_count(index);
  if (wakes == probe_wakes_[index]) return true;
  probe_wakes_[index] = wakes;
  return probes_[index]->resume();
}

int32_t GpuPool::probe_temperature(uint32_t index) {
  if (index >= manager_.count() || !probes_[index] || manager_.asleep(index)) return -1;
  // A lease keeps it from sleeping while the sensor is read (the check above and the read would otherwise race a release on another core).
  if (manager_.asleep(index) || !manager_.acquire(index)) return -1;
  int32_t value = -1;
  if (manager_.wake_count(index) == probe_wakes_[index] || resume_probe(index)) value = probes_[index]->temperature_millicelsius();
  manager_.release(index);
  return value;
}

}  // namespace hardware
