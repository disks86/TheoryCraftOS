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
#include "Process/WindowsMisc.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsWait.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kAlerted = 0x101, kUserApc = 0xC0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u,
                   kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u, kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u,
                   kObjectTypeMismatch = 0xC0000024u, kInsufficientResources = 0xC000009Au, kInvalidCid = 0xC000000Bu, kPrivilegeNotHeld = 0xC0000061u,
                   kNotSupported = 0xC00000BBu, kBufferTooSmall = 0xC0000023u, kInvalidParameter1 = 0xC00000EFu, kInvalidParameter2 = 0xC00000F0u;
constexpr uint64_t kUnixToNt = 11644473600ull;      // 1601-01-01 to 1970-01-01, in seconds

uint64_t current_thread(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }

bool put32(Process& p, uint64_t address, uint32_t v) { return p.write_user_u32(address, v); }
bool put16(Process& p, uint64_t address, uint16_t v) {
  const uint8_t raw[2] = {uint8_t(v), uint8_t(v >> 8)};
  return p.write_user(address, ConstByteView(raw, 2));
}

// A relative (negative) or absolute (positive) NT timeout as nanoseconds; no pointer: forever.
uint64_t timeout_of(Process& p, uint64_t pointer, bool* fault) {
  *fault = false;
  if (pointer == 0) return ~uint64_t{0};
  uint64_t raw = 0;
  if (!p.read_user_u64(pointer, &raw)) { *fault = true; return 0; }
  if (int64_t(raw) < 0) {
    const uint64_t units = uint64_t(0) - raw;
    return units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100;
  }
  const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt * 10000000ull;
  return raw > now ? (raw - now) * 100 : 0;
}

// The object a handle names, if the descriptor is of the kind: the process's pseudo handles and handles of other kinds are not.
bool object_of(Process& p, uint64_t handle, DescriptorKind kind, uint32_t* id) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  const uint64_t slot = handle / 4 - 1;
  if (p.files().kind(slot) != kind) return false;
  *id = uint32_t(p.files().token(slot));
  return true;
}

}  // namespace

uint32_t nt_alert_thread_by_thread_id(Process& p, const SyscallRequest& r) {
  Process::ThreadRecord* t = p.find_thread(r.args[0]);
  if (!t) return kInvalidCid;
  if (t->finished) return kSuccess;      // (a thread that has ended can still be named)
  t->alert_pending = true;
  if (p.runtime()) p.runtime()->notify();
  if (ObjectManager* objects = system_objects()) objects->notify();
  return kSuccess;
}

uint32_t nt_wait_for_alert_by_thread_id(Process& p, const SyscallRequest& r) {
  bool fault = false;
  const uint64_t timeout = timeout_of(p, r.args[1], &fault);
  if (fault) return kAccessViolation;
  const uint64_t me = current_thread(p);
  return wait_until(p, timeout, [&]() -> uint64_t {
    Process::ThreadRecord* t = p.find_thread(me);
    if (!t) return kInvalidCid;
    if (t->alert_pending) { t->alert_pending = false; return kAlerted; }
    return kWaitPending;
  });
}

namespace {

// The threads that wait in NtWaitForKeyedEvent and NtReleaseKeyedEvent (guarded by the system lock): a release and a wait with the same object and key are a pair.
struct KeyedSlot {
  bool used = false, releasing = false, matched = false;
  uint32_t object = 0;
  uint64_t key = 0;
};
constexpr uint32_t kKeyedSlots = 128;
KeyedSlot g_keyed[kKeyedSlots];

uint32_t keyed_event(Process& p, const SyscallRequest& r, bool releasing) {
  if (r.args[1] & 1) return kInvalidParameter1;      // (the low bit of the key is reserved)
  uint32_t object = 0;      // (a null handle is the keyed event of the system: object 0)
  if (r.args[0] != 0) {
    if (!object_of(p, r.args[0], DescriptorKind::KeyedEvent, &object)) {
      const bool other = r.args[0] % 4 == 0 && r.args[0] / 4 <= Process::kMaxDescriptors && p.files().kind(r.args[0] / 4 - 1) != DescriptorKind::Closed;
      return r.args[0] == ~uint64_t{0} || other ? kObjectTypeMismatch : kInvalidHandle;
    }
    if (p.files().denied_access(r.args[0] / 4 - 1) & (releasing ? 2u : 1u)) return 0xC0000022u;      // KEYEDEVENT_WAKE / KEYEDEVENT_WAIT
  }
  uint64_t alertable = 0;
  if (!argument(p, r, 2, &alertable)) return kAccessViolation;
  bool fault = false;
  const uint64_t timeout = timeout_of(p, r.args[3], &fault);
  if (fault) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t key = r.args[1], me = current_thread(p);
  int slot = -1;
  uint32_t status = wait_until(p, timeout, [&]() -> uint64_t {
    if (alertable & 0xFF) {
      Process::ThreadRecord* t = p.find_thread(me);
      if (t && t->apc_count > 0) return kUserApc;
      if (t && t->alert_pending) { t->alert_pending = false; return kAlerted; }
    }
    if (slot < 0) {      // first look: is the other side here already?
      for (KeyedSlot& k : g_keyed) {
        if (k.used && !k.matched && k.releasing != releasing && k.object == object && k.key == key) {
          k.matched = true;
          objects->sync().notify_all();
          return kSuccess;
        }
      }
      for (uint32_t i = 0; i < kKeyedSlots; ++i) {
        if (g_keyed[i].used) continue;
        g_keyed[i] = KeyedSlot{true, releasing, false, object, key};
        slot = int(i);
        return kWaitPending;
      }
      return kInsufficientResources;
    }
    if (g_keyed[slot].matched) { g_keyed[slot] = KeyedSlot(); slot = -1; return kSuccess; }
    return kWaitPending;
  });
  if (slot >= 0) {      // it gave up (a timeout, an APC): the pair may have been made at the last moment
    objects->sync().lock();
    if (g_keyed[slot].matched) status = kSuccess;
    g_keyed[slot] = KeyedSlot();
    objects->sync().unlock();
  }
  return status;
}

}  // namespace

uint32_t nt_wait_for_keyed_event(Process& p, const SyscallRequest& r) { return keyed_event(p, r, false); }
uint32_t nt_release_keyed_event(Process& p, const SyscallRequest& r) { return keyed_event(p, r, true); }

uint32_t nt_clear_event(Process& p, const SyscallRequest& r) {
  uint32_t id = 0;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (!object_of(p, r.args[0], DescriptorKind::Event, &id)) return kInvalidHandle;
  return objects->clear_event(id);
}

namespace {
// Reads the state of an object of the kind; the output buffer is checked by the caller.
uint32_t object_info(Process& p, const SyscallRequest& r, DescriptorKind kind, ObjectManager::ObjectState* state) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t id = 0;
  if (!object_of(p, r.args[0], kind, &id)) return kInvalidHandle;
  ISystemSync& sync = objects->sync();
  sync.lock();
  const bool found = objects->object_state_locked(id, current_thread(p), state);
  sync.unlock();
  return found ? kSuccess : kInvalidHandle;
}
bool report_length(Process& p, const SyscallRequest& r, uint32_t value) {
  uint64_t pointer = 0;
  return argument(p, r, 4, &pointer) && (pointer == 0 || put32(p, pointer, value));
}
}  // namespace

// NtQueryEvent(HANDLE, EVENT_INFORMATION_CLASS (0: EventBasicInformation), PVOID, ULONG, PULONG): { EVENT_TYPE EventType (0 notification, 1 synchronization); LONG EventState; }
uint32_t nt_query_event(Process& p, const SyscallRequest& r) {
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
  if (uint32_t(r.args[3]) != 8) return kInfoLengthMismatch;
  ObjectManager::ObjectState state;
  if (const uint32_t status = object_info(p, r, DescriptorKind::Event, &state)) return status;
  if (!put32(p, r.args[2], state.manual_reset ? 0 : 1) || !put32(p, r.args[2] + 4, state.signaled ? 1 : 0) || !report_length(p, r, 8)) return kAccessViolation;
  return kSuccess;
}

// NtQueryMutant(HANDLE, 0: MutantBasicInformation, ...): { LONG CurrentCount (1 when free, 1 - depth when owned); BOOLEAN OwnedByCaller; BOOLEAN AbandonedState; }
uint32_t nt_query_mutant(Process& p, const SyscallRequest& r) {
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
  if (uint32_t(r.args[3]) != 8) return kInfoLengthMismatch;
  ObjectManager::ObjectState state;
  if (const uint32_t status = object_info(p, r, DescriptorKind::Mutex, &state)) return status;
  if (!put32(p, r.args[2], uint32_t(1 - state.recursion)) || !put16(p, r.args[2] + 4, uint16_t((state.owned_by_waiter ? 1 : 0) | (state.abandoned ? 0x100 : 0))) || !report_length(p, r, 8))
    return kAccessViolation;
  return kSuccess;
}

// NtQuerySemaphore(HANDLE, 0: SemaphoreBasicInformation, ...): { ULONG CurrentCount; ULONG MaximumCount; }
uint32_t nt_query_semaphore(Process& p, const SyscallRequest& r) {
  if (uint32_t(r.args[1]) != 0) return kInvalidInfoClass;
  if (uint32_t(r.args[3]) != 8) return kInfoLengthMismatch;
  ObjectManager::ObjectState state;
  if (const uint32_t status = object_info(p, r, DescriptorKind::Semaphore, &state)) return status;
  if (!put32(p, r.args[2], uint32_t(state.count)) || !put32(p, r.args[2] + 4, uint32_t(state.maximum)) || !report_length(p, r, 8)) return kAccessViolation;
  return kSuccess;
}

// NtSetEventBoostPriority(HANDLE): sets a synchronization event and boosts the thread that gets it; the priority boost is not kept here.
uint32_t nt_set_event_boost_priority(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint32_t id = 0;
  if (!object_of(p, r.args[0], DescriptorKind::Event, &id)) return kInvalidHandle;
  return objects->set_event(id, nullptr);
}

// NtGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER): { USHORT Group; UCHAR Number; UCHAR Reserved; }
uint32_t nt_get_current_processor_number_ex(Process& p, const SyscallRequest& r) {
  const uint32_t number = p.runtime() ? p.runtime()->current_processor() : 0;
  if (r.args[0] == 0) return kSuccess;
  return put32(p, r.args[0], (number & 0xFF) << 16) ? kSuccess : kAccessViolation;
}

namespace {
// The locale and the user interface language are the same for every program: English (United States).
constexpr uint32_t kLocaleEnUs = 0x0409;
uint32_t g_default_locale = kLocaleEnUs, g_default_ui_language = kLocaleEnUs;
}  // namespace

// NtQueryDefaultLocale(BOOLEAN UserProfile, PLCID): the user's locale or the system's.
uint32_t nt_query_default_locale(Process& p, const SyscallRequest& r) {
  if (r.args[1] == 0) return kAccessViolation;
  return put32(p, r.args[1], __atomic_load_n(&g_default_locale, __ATOMIC_RELAXED)) ? kSuccess : kAccessViolation;
}
uint32_t nt_set_default_locale(Process& p, const SyscallRequest& r) {
  (void)p;
  const uint32_t locale = uint32_t(r.args[1]);
  if (locale == 0 || locale > 0xFFFF) return kInvalidParameter2;
  __atomic_store_n(&g_default_locale, locale, __ATOMIC_RELAXED);
  return kSuccess;
}
uint32_t nt_query_default_ui_language(Process& p, const SyscallRequest& r) {
  if (r.args[0] == 0) return kAccessViolation;
  return put16(p, r.args[0], uint16_t(__atomic_load_n(&g_default_ui_language, __ATOMIC_RELAXED))) ? kSuccess : kAccessViolation;
}
uint32_t nt_set_default_ui_language(Process& p, const SyscallRequest& r) {
  (void)p;
  const uint32_t language = uint32_t(r.args[0]) & 0xFFFF;
  if (language == 0) return kInvalidParameter;
  __atomic_store_n(&g_default_ui_language, language, __ATOMIC_RELAXED);
  return kSuccess;
}
uint32_t nt_query_install_ui_language(Process& p, const SyscallRequest& r) {
  if (r.args[0] == 0) return kAccessViolation;
  return put16(p, r.args[0], uint16_t(kLocaleEnUs)) ? kSuccess : kAccessViolation;
}

namespace {
// UUID allocation (what UuidCreate asks for): time stamps in 100 ns units since 1582-10-15 that never repeat, a clock sequence and the 6-byte node.
constexpr uint64_t kUuidEpochOffset = 0x00146BF33E42C000ull;      // 1582-10-15 to 1601-01-01, in 100 ns units
constexpr uint32_t kUuidRange = 1000;
uint64_t g_uuid_next_time = 0;
uint32_t g_uuid_sequence = 0;
uint8_t g_uuid_seed[6] = {};
bool g_uuid_seeded = false;
uint8_t g_uuid_lock = 0;
}  // namespace

// NtAllocateUuids(PULARGE_INTEGER Time, PULONG Range, PULONG Sequence, PUCHAR Seed): hands out the time stamps Time .. Time + Range - 1.
uint32_t nt_allocate_uuids(Process& p, const SyscallRequest& r) {
  while (__atomic_exchange_n(&g_uuid_lock, 1, __ATOMIC_ACQUIRE)) {}
  const uint64_t now = (p.host().realtime_ns() / 100) + kUnixToNt * 10000000ull + kUuidEpochOffset;
  if (!g_uuid_seeded) {
    uint8_t node[6] = {};
    if (!p.host().random_bytes(ByteView(node, 6))) for (uint32_t i = 0; i < 6; ++i) node[i] = uint8_t(now >> (i * 5));
    node[0] |= 1;      // (a random node id has the multicast bit set, so it cannot be a real address)
    for (uint32_t i = 0; i < 6; ++i) g_uuid_seed[i] = node[i];
    uint8_t sequence[2] = {};
    p.host().random_bytes(ByteView(sequence, 2));
    g_uuid_sequence = uint32_t(sequence[0]) | uint32_t(sequence[1]) << 8;
    g_uuid_seeded = true;
  }
  uint64_t time = now > g_uuid_next_time ? now : g_uuid_next_time;
  g_uuid_next_time = time + kUuidRange;
  const uint32_t sequence = g_uuid_sequence;
  uint8_t seed[6];
  for (uint32_t i = 0; i < 6; ++i) seed[i] = g_uuid_seed[i];
  __atomic_store_n(&g_uuid_lock, 0, __ATOMIC_RELEASE);
  if (!p.write_user_u64(r.args[0], time) || !put32(p, r.args[1], kUuidRange) || !put32(p, r.args[2], sequence) || !p.write_user(r.args[3], ConstByteView(seed, 6))) return kAccessViolation;
  return kSuccess;
}

// NtSetUuidSeed(PUCHAR Seed): the node id of the UUIDs (6 bytes).
uint32_t nt_set_uuid_seed(Process& p, const SyscallRequest& r) {
  uint8_t seed[6];
  if (!p.read_user(r.args[0], ByteView(seed, 6))) return kAccessViolation;
  while (__atomic_exchange_n(&g_uuid_lock, 1, __ATOMIC_ACQUIRE)) {}
  for (uint32_t i = 0; i < 6; ++i) g_uuid_seed[i] = seed[i];
  g_uuid_seeded = true;
  __atomic_store_n(&g_uuid_lock, 0, __ATOMIC_RELEASE);
  return kSuccess;
}

namespace {
uint32_t g_execution_state = 0x80000000u;      // ES_CONTINUOUS
}

// NtSetThreadExecutionState(EXECUTION_STATE New, EXECUTION_STATE* Previous): remembered for the system (there is no sleep to keep from).
uint32_t nt_set_thread_execution_state(Process& p, const SyscallRequest& r) {
  const uint32_t added = uint32_t(r.args[0]);
  const uint32_t previous = __atomic_load_n(&g_execution_state, __ATOMIC_RELAXED);
  if (added & 0x7FFFFFF8u) return kInvalidParameter;
  if (added & 0x80000000u) __atomic_store_n(&g_execution_state, added, __ATOMIC_RELAXED);
  if (r.args[1] != 0 && !put32(p, r.args[1], previous)) return kAccessViolation;
  return kSuccess;
}

// NtPowerInformation(POWER_INFORMATION_LEVEL, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength): the levels programs ask for on a machine that never sleeps.
uint32_t nt_power_information(Process& p, const SyscallRequest& r) {
  uint64_t out_length = 0;
  if (!argument(p, r, 4, &out_length)) return kAccessViolation;
  const uint32_t level = uint32_t(r.args[0]);
  const uint64_t out = r.args[3];
  const uint32_t capacity = uint32_t(out_length);
  auto zeros = [&](uint32_t size) -> uint32_t {
    if (capacity < size) return kBufferTooSmall;
    uint8_t zero[96] = {};
    return size <= sizeof(zero) && p.write_user(out, ConstByteView(zero, size)) ? kSuccess : kAccessViolation;
  };
  switch (level) {
    case 5: return zeros(32);         // SystemBatteryState: AcOnLine is set below
    case 4: return zeros(76);         // SystemPowerCapabilities
    case 9: return zeros(4);          // SystemReserveHiberFile / LastWakeTime-ish counters
    case 14: case 15: return zeros(8);      // LastWakeTime, LastSleepTime
    case 16: if (capacity < 4) return kBufferTooSmall; return put32(p, out, __atomic_load_n(&g_execution_state, __ATOMIC_RELAXED)) ? kSuccess : kAccessViolation;
    case 11: {      // ProcessorInformation: one PROCESSOR_POWER_INFORMATION (Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState) per logical processor
      const uint32_t cpus = p.runtime() ? p.runtime()->processor_count() : 1;
      if (capacity < cpus * 24) return kBufferTooSmall;
      for (uint32_t i = 0; i < cpus; ++i) {
        const uint32_t record[6] = {i, 2000, 2000, 2000, 0, 0};
        uint8_t raw[24];
        for (uint32_t k = 0; k < 6; ++k) for (uint32_t b = 0; b < 4; ++b) raw[k * 4 + b] = uint8_t(record[k] >> (b * 8));
        if (!p.write_user(out + uint64_t(i) * 24, ConstByteView(raw, 24))) return kAccessViolation;
      }
      return kSuccess;
    }
    case 12: case 13: return zeros(4);      // SystemPowerPolicyAc / Dc are not kept: a block of zeros
    default: return kNotImplemented;
  }
}

// NtRaiseHardError(NTSTATUS, ULONG NumberOfParameters, ULONG UnicodeStringParameterMask, PULONG_PTR Parameters, ULONG ValidResponseOptions, PULONG Response): there is nobody to
// show the box to: the error is not handled (ResponseNotHandled).
uint32_t nt_raise_hard_error(Process& p, const SyscallRequest& r) {
  uint64_t options = 0, response = 0;
  if (!argument(p, r, 4, &options) || !argument(p, r, 5, &response)) return kAccessViolation;
  if (uint32_t(r.args[1]) > 32) return kInvalidParameter2;
  if (response != 0 && !put32(p, response, 1 /* ResponseNotHandled */)) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_query_interval_profile(Process& p, const SyscallRequest& r) {
  (void)p; (void)r;
  return kSuccess;
}

uint32_t nt_succeed(Process& p, const SyscallRequest& r) { (void)p; (void)r; return kSuccess; }
uint32_t nt_privilege_not_held(Process& p, const SyscallRequest& r) { (void)p; (void)r; return kPrivilegeNotHeld; }
uint32_t nt_not_supported(Process& p, const SyscallRequest& r) { (void)p; (void)r; return kNotSupported; }

// NtMakeTemporaryObject(HANDLE): the name goes when the last handle does. Objects here already go with the last handle unless they are permanent, and none is.
uint32_t nt_make_object_temporary(Process& p, const SyscallRequest& r) {
  if (r.args[0] == 0 || r.args[0] % 4 != 0 || r.args[0] / 4 > Process::kMaxDescriptors || p.files().kind(r.args[0] / 4 - 1) == DescriptorKind::Closed) return kInvalidHandle;
  return kSuccess;
}
// NtMakePermanentObject needs SeCreatePermanentPrivilege, which no program here has.
uint32_t nt_make_object_permanent(Process& p, const SyscallRequest& r) {
  if (r.args[0] == 0 || r.args[0] % 4 != 0 || r.args[0] / 4 > Process::kMaxDescriptors || p.files().kind(r.args[0] / 4 - 1) == DescriptorKind::Closed) return kInvalidHandle;
  return kPrivilegeNotHeld;
}

}  // namespace process::windowsabi
