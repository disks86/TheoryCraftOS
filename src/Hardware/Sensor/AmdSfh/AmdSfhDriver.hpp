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

// The AMD Sensor Fusion Hub (see AmdSfhRegisters.hpp for the interface and how sure its numbers are): the PCI binding (memory decoding, bus
// mastering, legacy interrupt off - the driver polls), the start-up of either generation, one ISensor per sensor the MP2 reports and the driver
// could enable, added to a SensorHub next to the I2C, ISH and ACPI ones. The ambient light sensor gives lux, the motion sensors the units of
// ISensor.hpp, a presence sensor 1 or 0.
//
// Values come from memory the firmware writes (a DMA buffer per sensor on 1.0, the shared memory on 1.1) and are not trusted: each read takes the
// values twice and drops a sample that changed in between (half-written), and drops values that are not finite or out of any real sensor's range.
// A read (ISensor::read) takes the latest values itself (unless set_refresh_on_read(false)); poll() does the same for every sensor, for a thread
// that keeps them fresh. Both may run on any thread at once; taking values is serialized, so a sample never goes back to an older one.
//
// A command the firmware never answers (1.0 v2, 1.1) times out after 1.6 s, and start-up stops at the first one. Memory the firmware may still
// write to (a buffer of a sensor whose disable was not confirmed) is never freed.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/Sensor/AmdSfh/AmdSfhRegisters.hpp"
#include "Hardware/Sensor/SensorHub.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class AmdSfhDriver;

class AmdSfhSensor final : public ISensor {
 public:
  AmdSfhSensor(AmdSfhDriver& driver, const amd_sfh_layout::SensorEntry& entry) : driver_(driver), entry_(entry) {}
  const char* name() const override;
  bool initialize() override { return true; }
  void shutdown() override {}
  SensorKind kind() const override;
  bool read(SensorReading* out) override;
  uint8_t sensor_id() const { return entry_.id; }
  AmdSfhSensorType type() const { return entry_.type; }

 private:
  friend class AmdSfhDriver;
  void publish(const float values[3], bool new_sample);
  void forget();

  AmdSfhDriver& driver_;
  const amd_sfh_layout::SensorEntry& entry_;
  // The driver's (under its mutex): the 1.0 DMA buffer, and what the last accepted read saw.
  ArrayView<uint32_t> dma_;
  bool enabled_ = false;
  bool seen_ = false;
  uint32_t last_raw_[4] = {};
  uint32_t zero_reads_ = 0;
  // The reading, under its own lock so a reader never waits for a command.
  kernel::Mutex mutex_;
  SensorReading latest_;
  bool have_ = false;
};

class AmdSfhDriver {
 public:
  enum class Outcome : uint8_t { NotStarted, Started, UnknownDevice, BarUnmapped, OutOfMemory, NoResponse, SharedMemoryUnmapped, FirmwareNotReady, NoSensors };
  static constexpr uint32_t kMaxSensors = amd_sfh_layout::kSensorCount;
  // Reads of a sensor that saw all-zero values before they count as a sample (the firmware has not written yet, or it really is 0 lux).
  static constexpr uint32_t kZeroGraceReads = 5;

  explicit AmdSfhDriver(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~AmdSfhDriver() { stop(); }
  AmdSfhDriver(const AmdSfhDriver&) = delete;
  AmdSfhDriver& operator=(const AmdSfhDriver&) = delete;

  static bool supports(const PciDeviceInfo& device) {
    return device.vendor_id == amd_sfh_layout::kVendorAmd && (device.device_id == amd_sfh_layout::kDeviceSfh10 || device.device_id == amd_sfh_layout::kDeviceSfh11);
  }
  static const char* outcome_name(Outcome outcome);
  static const char* generation_name(AmdSfhGeneration generation);

  [[nodiscard]] Outcome start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, SensorHub* hub);
  // The part after the registers are reachable (tests bring their own). `mapper` maps the 1.1 shared memory.
  [[nodiscard]] Outcome start_registers(IAmdSfhRegisters& registers, uint16_t device_id, IRegisterMapper& mapper, SensorHub* hub);
  // Disables every sensor, takes them out of the hub, frees what the firmware can no longer write to.
  void stop();
  // Disables the sensors (before the machine sleeps); reads fail until resume(). False if the firmware did not confirm all of it.
  [[nodiscard]] bool suspend();
  // Enables them again. False if one could not be enabled.
  [[nodiscard]] bool resume();
  // Takes the latest values of every sensor.
  void poll();
  // Off: a read only gives what the last poll() took (for a system with a poller thread: readers then never touch the firmware's memory).
  void set_refresh_on_read(bool on) { __atomic_store_n(&refresh_on_read_, on, __ATOMIC_RELAXED); }

  Outcome outcome() const { return outcome_; }
  AmdSfhGeneration generation() const { return generation_; }
  bool suspended() const { return __atomic_load_n(&suspended_, __ATOMIC_RELAXED); }
  uint32_t firmware_version() const { return firmware_version_; }
  uint32_t sensor_count() const { return count_; }
  AmdSfhSensor* sensor(uint32_t index) { return index < count_ ? sensors_[index].get() : nullptr; }
  AmdSfhSensor* find(SensorKind kind);
  // Samples dropped because they were out of range, not finite, or changed while being read.
  uint32_t rejected_samples() const { return __atomic_load_n(&rejected_, __ATOMIC_RELAXED); }

 private:
  friend class AmdSfhSensor;
  // (all under mutex_)
  [[nodiscard]] Outcome start_v10(uint32_t active_status);
  [[nodiscard]] Outcome start_v11(IRegisterMapper& mapper);
  [[nodiscard]] bool enable(AmdSfhSensor& sensor);
  [[nodiscard]] bool disable(AmdSfhSensor& sensor);
  [[nodiscard]] bool wait_v2(uint8_t sensor_id, uint32_t code);
  [[nodiscard]] bool wait_v11(uint8_t sensor_id, uint32_t command);
  [[nodiscard]] bool snapshot(AmdSfhSensor& sensor, uint32_t raw[4]);
  void refresh(AmdSfhSensor& sensor);
  // From a sensor's read(): refresh unless a command is in progress (the reader then gets the last sample).
  void try_refresh(AmdSfhSensor& sensor);
  void reject() { __atomic_add_fetch(&rejected_, 1, __ATOMIC_RELAXED); }
  void release_buffers(bool firmware_quiet);

  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  UniquePointer<MmioAmdSfhRegisters> mmio_;
  IAmdSfhRegisters* registers_ = nullptr;
  MmioRegion shared_;
  SensorHub* hub_ = nullptr;
  UniquePointer<AmdSfhSensor> sensors_[kMaxSensors];
  uint32_t count_ = 0;
  AmdSfhGeneration generation_ = AmdSfhGeneration::None;
  Outcome outcome_ = Outcome::NotStarted;
  bool running_ = false;
  bool suspended_ = false;
  uint32_t firmware_version_ = 0;
  uint32_t rejected_ = 0;
  bool refresh_on_read_ = true;
};

}  // namespace hardware
