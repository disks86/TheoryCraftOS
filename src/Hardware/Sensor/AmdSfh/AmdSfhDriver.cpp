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

#include "Hardware/Sensor/AmdSfh/AmdSfhDriver.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace L = amd_sfh_layout;

namespace {

constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kCommandMemory = 1u << 1, kCommandBusMaster = 1u << 2, kCommandIntxDisable = 1u << 10;
constexpr uint32_t kAllOnes = 0xFFFFFFFFu;     // what a read of a device that is gone returns
constexpr uint32_t kSnapshotAttempts = 3;

// The bus address of a DMA buffer: memory from allocate_dma is identity-mapped below 4 GiB, so it is the pointer value (the convention of every
// driver here, see nic_dma_address). The one place a pointer becomes an address.
uint64_t dma_address(ArrayView<uint32_t> buffer) { return reinterpret_cast<uint64_t>(buffer.data()); }

// A word the device writes behind the compiler's back: an atomic load, so it is read every time and never torn.
uint32_t device_word(ArrayView<uint32_t> buffer, uint32_t index) { return __atomic_load_n(&buffer[index], __ATOMIC_ACQUIRE); }

SensorKind kind_of(AmdSfhSensorType type) {
  switch (type) {
    case AmdSfhSensorType::Accelerometer: return SensorKind::Accelerometer;
    case AmdSfhSensorType::Gyroscope: return SensorKind::Gyroscope;
    case AmdSfhSensorType::Magnetometer: return SensorKind::Magnetometer;
    case AmdSfhSensorType::AmbientLight: return SensorKind::AmbientLight;
    case AmdSfhSensorType::HumanPresence: return SensorKind::HumanPresence;
  }
  return SensorKind::AmbientLight;
}

bool finite(float v) { return v == v && v - v == 0.0f; }   // (NaN fails the first, infinities the second)

// Turns raw words into the ISensor values; false if they are not something a real sensor says.
bool convert(const L::SensorEntry& entry, AmdSfhGeneration generation, const uint32_t raw[3], float out[3]) {
  const bool floats = generation == AmdSfhGeneration::Sfh11;
  const float scale = floats ? entry.v11_scale : entry.v10_scale;
  for (uint32_t a = 0; a < 3; ++a) {
    out[a] = 0;
    if (a >= entry.axes) continue;
    const float v = floats ? __builtin_bit_cast(float, raw[a]) : static_cast<float>(static_cast<int32_t>(raw[a]));
    if (!finite(v)) return false;
    out[a] = v * scale;
  }
  float limit = 0;
  switch (entry.type) {
    case AmdSfhSensorType::Accelerometer: limit = amd_sfh_limits::kMaxAccelerationG; break;
    case AmdSfhSensorType::Gyroscope: limit = amd_sfh_limits::kMaxAngularDps; break;
    case AmdSfhSensorType::Magnetometer: limit = amd_sfh_limits::kMaxFluxGauss; break;
    case AmdSfhSensorType::AmbientLight:
      return finite(out[0]) && out[0] >= 0.0f && out[0] <= amd_sfh_limits::kMaxLux;
    case AmdSfhSensorType::HumanPresence:
      out[0] = raw[0] != 0 ? 1.0f : 0.0f;
      return true;
  }
  for (uint32_t a = 0; a < entry.axes; ++a)
    if (!finite(out[a]) || out[a] > limit || out[a] < -limit) return false;
  return true;
}

}  // namespace

// ---- AmdSfhSensor ------------------------------------------------------------------------------------------------------------------------------

const char* AmdSfhSensor::name() const {
  switch (entry_.type) {
    case AmdSfhSensorType::Accelerometer: return "AMD SFH accelerometer";
    case AmdSfhSensorType::Gyroscope: return "AMD SFH gyroscope";
    case AmdSfhSensorType::Magnetometer: return "AMD SFH magnetometer";
    case AmdSfhSensorType::AmbientLight: return "AMD SFH ambient light sensor";
    case AmdSfhSensorType::HumanPresence: return "AMD SFH presence sensor";
  }
  return "AMD SFH sensor";
}

SensorKind AmdSfhSensor::kind() const { return kind_of(entry_.type); }

bool AmdSfhSensor::read(SensorReading* out) {
  driver_.try_refresh(*this);
  kernel::ScopedMutex guard(mutex_);
  if (!have_) return false;
  if (out) *out = latest_;
  return true;
}

void AmdSfhSensor::publish(const float values[3], bool new_sample) {
  kernel::ScopedMutex guard(mutex_);
  latest_.kind = kind();
  latest_.x = values[0];
  latest_.y = values[1];
  latest_.z = values[2];
  if (new_sample || !have_) ++latest_.sequence;
  have_ = true;
}

void AmdSfhSensor::forget() {
  kernel::ScopedMutex guard(mutex_);
  have_ = false;
}

// ---- AmdSfhDriver ------------------------------------------------------------------------------------------------------------------------------

const char* AmdSfhDriver::outcome_name(Outcome o) {
  switch (o) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Started: return "started";
    case Outcome::UnknownDevice: return "not a known sensor fusion hub";
    case Outcome::BarUnmapped: return "the registers could not be mapped";
    case Outcome::OutOfMemory: return "out of memory";
    case Outcome::NoResponse: return "the MP2 does not answer";
    case Outcome::SharedMemoryUnmapped: return "the shared memory could not be mapped";
    case Outcome::FirmwareNotReady: return "the firmware did not fill in its sensor list";
    case Outcome::NoSensors: return "no sensors";
  }
  return "";
}

const char* AmdSfhDriver::generation_name(AmdSfhGeneration g) {
  switch (g) {
    case AmdSfhGeneration::None: return "none";
    case AmdSfhGeneration::Sfh10V1: return "SFH 1.0 (interface v1)";
    case AmdSfhGeneration::Sfh10V2: return "SFH 1.0 (interface v2)";
    case AmdSfhGeneration::Sfh11: return "SFH 1.1";
  }
  return "";
}

AmdSfhDriver::Outcome AmdSfhDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, SensorHub* hub) {
  stop();
  if (!supports(device)) return outcome_ = Outcome::UnknownDevice;
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | kCommandMemory | kCommandBusMaster | kCommandIntxDisable);   // (polled: no interrupt wanted)
  const uint64_t base = pci.memory_bar(device.address, L::kRegisterBar);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, L::kRegisterBytes, &region)) return outcome_ = Outcome::BarUnmapped;
  mmio_ = UniquePointer<MmioAmdSfhRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  return start_registers(*mmio_, device.device_id, mapper, hub);
}

AmdSfhDriver::Outcome AmdSfhDriver::start_registers(IAmdSfhRegisters& registers, uint16_t device_id, IRegisterMapper& mapper, SensorHub* hub) {
  if (running_) stop();                                    // (start() has stopped already, so its mmio_ stays)
  kernel::ScopedMutex guard(mutex_);
  registers_ = &registers;
  hub_ = hub;
  suspended_ = false;
  Outcome result;
  if (device_id == L::kDeviceSfh11) {
    generation_ = AmdSfhGeneration::Sfh11;
    result = start_v11(mapper);
  } else if (device_id == L::kDeviceSfh10) {
    const uint32_t status = registers.read32(L::kActiveStatusRegister);
    if (status == kAllOnes) return outcome_ = Outcome::NoResponse;
    generation_ = (status & 0xF) == L::kV2InterfaceVersion ? AmdSfhGeneration::Sfh10V2 : AmdSfhGeneration::Sfh10V1;
    result = start_v10(status);
  } else {
    return outcome_ = Outcome::UnknownDevice;
  }
  if (result != Outcome::Started) {
    // Sensors enabled before the failure keep their buffers (the firmware writes them until told to stop; a v1 MP2 is told, the others may be dead).
    if (generation_ == AmdSfhGeneration::Sfh10V1 && count_) registers.write32(L::kCommandRegister, L::v1_command(L::kCmdStopAll, 0, 0));
    release_buffers(false);
    for (uint32_t i = 0; i < count_; ++i) sensors_[i].reset();
    count_ = 0;
    return outcome_ = result;
  }
  running_ = true;
  for (uint32_t i = 0; i < count_; ++i)
    if (hub_) (void)hub_->add(sensors_[i].get());     // (a full hub only means programs do not see it)
  return outcome_ = Outcome::Started;
}

AmdSfhDriver::Outcome AmdSfhDriver::start_v10(uint32_t active_status) {
  const uint32_t mask = active_status >> 4;
  for (const L::SensorEntry& entry : L::kSensors) {
    if (!(mask & entry.v10_mask)) continue;             // (bits of sensors this driver does not know are ignored)
    UniquePointer<AmdSfhSensor> sensor = UniquePointer<AmdSfhSensor>::make(allocator_, *this, entry);
    if (!sensor) return Outcome::OutOfMemory;
    sensor->dma_ = allocate_dma_view<uint32_t>(allocator_, L::kV10BufferWords, 64);
    if (sensor->dma_.empty()) return Outcome::OutOfMemory;
    for (uint32_t i = 0; i < L::kV10BufferWords; ++i) __atomic_store_n(&sensor->dma_[i], 0u, __ATOMIC_RELEASE);
    sensors_[count_++] = util::move(sensor);
    if (!enable(*sensors_[count_ - 1])) return Outcome::NoResponse;
  }
  return count_ ? Outcome::Started : Outcome::NoSensors;
}

AmdSfhDriver::Outcome AmdSfhDriver::start_v11(IRegisterMapper& mapper) {
  const uint32_t base = registers_->read32(L::kSharedMemoryRegister);
  if (base == kAllOnes) return Outcome::NoResponse;
  if (base == 0) return Outcome::SharedMemoryUnmapped;
  const uint64_t physical = uint64_t{base} << L::kSharedMemoryShift;
  if (!mapper.map_registers(physical, L::kSharedMemoryBytes, &shared_) || shared_.length() < L::kSharedMemoryBytes) {
    shared_ = MmioRegion();
    return Outcome::SharedMemoryUnmapped;
  }
  uint32_t list = 0;
  for (uint32_t attempt = 0; attempt < L::kFirmwareAttempts; ++attempt) {
    firmware_version_ = shared_.read32(L::kV11FirmwareVersion);
    list = shared_.read32(L::kV11SensorList);
    if (firmware_version_ != 0 && list != 0) break;
    delay_us(L::kFirmwarePollUs);
  }
  if (firmware_version_ == 0 || list == 0) return Outcome::FirmwareNotReady;
  for (const L::SensorEntry& entry : L::kSensors) {
    if (!(list & entry.v11_mask)) continue;
    UniquePointer<AmdSfhSensor> sensor = UniquePointer<AmdSfhSensor>::make(allocator_, *this, entry);
    if (!sensor) return Outcome::OutOfMemory;
    sensors_[count_++] = util::move(sensor);
    if (!enable(*sensors_[count_ - 1])) return Outcome::NoResponse;
  }
  return count_ ? Outcome::Started : Outcome::NoSensors;
}

bool AmdSfhDriver::wait_v2(uint8_t sensor_id, uint32_t code) {
  for (uint32_t attempt = 0; attempt < L::kResponseAttempts; ++attempt) {
    const uint32_t r = registers_->read32(L::kResponseRegister);
    if (r != kAllOnes && L::v2_response_code(r) == code && L::v2_response_status(r) == 0 && (sensor_id == 0xFF || L::v2_response_sensor(r) == sensor_id)) return true;
    delay_us(L::kResponsePollUs);
  }
  return false;
}

bool AmdSfhDriver::wait_v11(uint8_t sensor_id, uint32_t command) {
  for (uint32_t attempt = 0; attempt < L::kResponseAttempts; ++attempt) {
    const uint32_t r = registers_->read32(L::kResponseRegister);
    if (r != kAllOnes && L::v11_response_code(r) == 0 && L::v11_response_command(r) == command && (sensor_id == 0xFF || L::v11_response_sensor(r) == sensor_id))
      return true;
    delay_us(L::kResponsePollUs);
  }
  return false;
}

bool AmdSfhDriver::enable(AmdSfhSensor& sensor) {
  const uint8_t id = sensor.entry_.id;
  sensor.seen_ = false;
  sensor.zero_reads_ = 0;
  bool ok = true;
  switch (generation_) {
    case AmdSfhGeneration::Sfh10V1:
      registers_->write64(L::kV1AddressRegister, dma_address(sensor.dma_));
      registers_->write32(L::kV1ParameterRegister, L::v1_parameter(1, 16));
      registers_->write32(L::kCommandRegister, L::v1_command(L::kCmdEnable, id, L::kPeriodMs));
      break;                                                   // (v1 firmware does not answer: assumed enabled, as Linux does)
    case AmdSfhGeneration::Sfh10V2: {
      const bool c2p_memory = L::kV2AlsUsesC2pMemory && sensor.entry_.type == AmdSfhSensorType::AmbientLight;
      registers_->write64(L::kV2AddressRegister, dma_address(sensor.dma_));
      registers_->write32(L::kCommandRegister, L::v2_command(L::kCmdEnable, id, L::kV2BufferLength, c2p_memory, false));
      ok = wait_v2(id, L::kV2SensorEnabled);
      break;
    }
    case AmdSfhGeneration::Sfh11:
      registers_->write32(L::kCommandRegister, L::v11_command(L::kCmdEnable, id, 1, false));
      ok = wait_v11(id, L::kCmdEnable);
      break;
    case AmdSfhGeneration::None: return false;
  }
  // Not answered: the firmware may still act on it later, so the buffer counts as the firmware's either way.
  sensor.enabled_ = true;
  return ok;
}

bool AmdSfhDriver::disable(AmdSfhSensor& sensor) {
  const uint8_t id = sensor.entry_.id;
  bool ok = true;
  switch (generation_) {
    case AmdSfhGeneration::Sfh10V1:
      registers_->write32(L::kCommandRegister, L::v1_command(L::kCmdDisable, id, 0));
      break;
    case AmdSfhGeneration::Sfh10V2:
      registers_->write32(L::kCommandRegister, L::v2_command(L::kCmdDisable, id, 0, false, false));
      ok = wait_v2(id, L::kV2SensorDisabled);
      break;
    case AmdSfhGeneration::Sfh11:
      registers_->write32(L::kCommandRegister, L::v11_command(L::kCmdDisable, id, 0, false));
      ok = wait_v11(id, L::kCmdDisable);
      break;
    case AmdSfhGeneration::None: return false;
  }
  if (ok) sensor.enabled_ = false;
  sensor.forget();
  return ok;
}

// Frees the DMA buffers the firmware can no longer write: those of sensors it confirmed disabled, or all of them once `firmware_quiet` (stopped
// and given time for a write in flight to land).
void AmdSfhDriver::release_buffers(bool firmware_quiet) {
  for (uint32_t i = 0; i < count_; ++i) {
    AmdSfhSensor& s = *sensors_[i];
    if (s.dma_.empty()) continue;
    if (s.enabled_ && !firmware_quiet) continue;              // (left to the firmware: leaked)
    release_view(allocator_, s.dma_);
  }
}

void AmdSfhDriver::stop() {
  {
    kernel::ScopedMutex guard(mutex_);
    if (running_) {
      for (uint32_t i = 0; i < count_; ++i)
        if (hub_) (void)hub_->remove(sensors_[i].get());
      bool all_confirmed = true;
      if (generation_ == AmdSfhGeneration::Sfh10V1) {
        registers_->write32(L::kCommandRegister, L::v1_command(L::kCmdStopAll, 0, 0));
        delay_us(2 * L::kPeriodMs * 1000);                     // (v1 says nothing: two periods for a write in flight)
      } else {
        for (uint32_t i = 0; i < count_; ++i)
          if (sensors_[i]->enabled_ && !disable(*sensors_[i])) all_confirmed = false;
      }
      release_buffers(generation_ == AmdSfhGeneration::Sfh10V1 || all_confirmed);
    } else {
      release_buffers(false);
    }
    for (uint32_t i = 0; i < count_; ++i) sensors_[i].reset();
    count_ = 0;
    running_ = false;
    suspended_ = false;
    registers_ = nullptr;
    shared_ = MmioRegion();
    hub_ = nullptr;
    generation_ = AmdSfhGeneration::None;
    firmware_version_ = 0;
    outcome_ = Outcome::NotStarted;
  }
  mmio_.reset();
}

bool AmdSfhDriver::suspend() {
  kernel::ScopedMutex guard(mutex_);
  if (!running_ || suspended_) return running_;
  bool ok = true;
  for (uint32_t i = 0; i < count_; ++i)
    if (sensors_[i]->enabled_ && !disable(*sensors_[i])) ok = false;
  suspended_ = true;
  return ok;
}

bool AmdSfhDriver::resume() {
  kernel::ScopedMutex guard(mutex_);
  if (!running_) return false;
  if (!suspended_) return true;
  bool ok = true;
  for (uint32_t i = 0; i < count_ && ok; ++i) {
    if (generation_ != AmdSfhGeneration::Sfh11 && sensors_[i]->dma_.empty()) { ok = false; break; }
    for (uint32_t w = 0; w < sensors_[i]->dma_.size(); ++w) __atomic_store_n(&sensors_[i]->dma_[w], 0u, __ATOMIC_RELEASE);
    ok = enable(*sensors_[i]);                                  // (stops at the first that is not answered, like start-up)
  }
  suspended_ = false;
  return ok;
}

AmdSfhSensor* AmdSfhDriver::find(SensorKind kind) {
  for (uint32_t i = 0; i < count_; ++i)
    if (sensors_[i]->kind() == kind) return sensors_[i].get();
  return nullptr;
}

// The raw words of a sensor (values, and on 1.1 the interrupt count as word 3), read twice: false if they changed in between.
bool AmdSfhDriver::snapshot(AmdSfhSensor& sensor, uint32_t raw[4]) {
  const bool v11 = generation_ == AmdSfhGeneration::Sfh11;
  if (!v11 && sensor.dma_.empty()) return false;
  auto load = [&](uint32_t out[4]) {
    if (v11) {
      const uint32_t block = L::v11_block(sensor.entry_.id);
      for (uint32_t a = 0; a < 3; ++a) out[a] = a < sensor.entry_.axes ? shared_.read32(block + L::kV11Values + 4 * a) : 0;
      out[3] = shared_.read32(block + L::kV11InterruptCount);
    } else {
      for (uint32_t a = 0; a < 3; ++a) out[a] = a < sensor.entry_.axes ? device_word(sensor.dma_, a) : 0;
      out[3] = 0;
    }
  };
  for (uint32_t attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
    uint32_t first[4], second[4];
    load(first);
    load(second);
    bool same = true;
    for (uint32_t i = 0; i < 4; ++i) same = same && first[i] == second[i];
    if (same) {
      for (uint32_t i = 0; i < 4; ++i) raw[i] = first[i];
      return true;
    }
  }
  return false;
}

void AmdSfhDriver::refresh(AmdSfhSensor& sensor) {
  if (!running_ || suspended_ || !sensor.enabled_) return;
  uint32_t raw[4];
  if (!snapshot(sensor, raw)) { reject(); return; }
  bool changed = !sensor.seen_;
  for (uint32_t i = 0; i < 4; ++i) changed = changed || raw[i] != sensor.last_raw_[i];
  if (!sensor.seen_) {
    // Nothing written yet looks like all zeros (and a 1.1 interrupt count of 0); wait a few reads before believing a zero.
    const bool blank = raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0;
    if (blank && ++sensor.zero_reads_ < kZeroGraceReads) return;
  } else if (!changed) {
    return;
  }
  float values[3];
  if (!convert(sensor.entry_, generation_, raw, values)) {
    reject();
    for (uint32_t i = 0; i < 4; ++i) sensor.last_raw_[i] = raw[i];   // (counted once, not on every read of the same garbage)
    sensor.seen_ = true;
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) sensor.last_raw_[i] = raw[i];
  sensor.seen_ = true;
  sensor.publish(values, changed);
}

void AmdSfhDriver::try_refresh(AmdSfhSensor& sensor) {
  if (!__atomic_load_n(&refresh_on_read_, __ATOMIC_RELAXED) || !mutex_.try_lock()) return;
  refresh(sensor);
  mutex_.unlock();
}

void AmdSfhDriver::poll() {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) refresh(*sensors_[i]);
}

}  // namespace hardware
