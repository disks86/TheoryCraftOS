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

#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiNamespace.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

namespace {

bool same_name(const char* a, const char* b) {
  for (uint32_t i = 0; i < 4; ++i) {
    const char c = b[i] ? b[i] : '_';
    if (a[i] != c) return false;
    if (!b[i]) {   // a short name is padded with '_'
      for (uint32_t j = i + 1; j < 4; ++j)
        if (a[j] != '_') return false;
      return true;
    }
  }
  return true;
}

// Visits every node of the namespace, depth first, in namespace order, up to kMaxWalk nodes. `visit` returns false to stop.
template <class Visit>
void walk(acpi::AmlInterpreter& aml, Visit&& visit) {
  acpi::Node* stack_storage[GpuAcpiNamespace::kMaxDepth] = {};
  const ArrayView<acpi::Node*> stack(stack_storage);
  uint32_t depth = 0, visited = 0;
  acpi::Node* n = aml.first_child(aml.root());
  while (n && visited < GpuAcpiNamespace::kMaxWalk) {
    ++visited;
    if (!visit(n)) return;
    acpi::Node* down = aml.first_child(n);
    if (down && depth < stack.size()) {   // (deeper than kMaxDepth: not searched)
      stack[depth++] = n;
      n = down;
      continue;
    }
    acpi::Node* next = aml.next_sibling(n);
    while (!next && depth != 0) next = aml.next_sibling(stack[--depth]);
    n = next;
  }
}

bool looks_like_gpu(acpi::AmlInterpreter& aml, acpi::Node* device) { return GpuAcpiNamespace::child_method(aml, device, "_ROM") || GpuAcpiNamespace::child_method(aml, device, "ATRM"); }

uint32_t gpu_hints(acpi::AmlInterpreter& aml, acpi::Node* device) {
  uint32_t score = looks_like_gpu(aml, device) ? 4 : 0;
  if (GpuAcpiNamespace::child(aml, device, "_DSM") || GpuAcpiNamespace::child(aml, device, "ATPX")) score += 2;
  if (GpuAcpiNamespace::child(aml, device, "_PR3") || GpuAcpiNamespace::child(aml, device, "_PS3")) score += 1;
  acpi::Node* parent = aml.parent(device);
  if (parent && GpuAcpiNamespace::child(aml, parent, "_PR3")) score += 1;
  return score;
}

uint64_t bus_number_of(acpi::AmlInterpreter& aml, acpi::Node* host_bridge) {
  uint64_t bbn = 0;
  acpi::Node* n = GpuAcpiNamespace::child(aml, host_bridge, "_BBN");
  if (n && !aml.evaluate_integer(n, &bbn)) bbn = 0;
  return bbn & 0xFF;
}

// How well `candidate` (a Device with the right _ADR) fits; 0 = it is not the function.
uint32_t score(acpi::AmlInterpreter& aml, acpi::Node* candidate, PciAddress function, const Optional<PciAddress>& bridge) {
  acpi::Node* parent = aml.parent(candidate);
  if (!parent || !aml.is_device(parent)) return 0;
  if (GpuAcpiNamespace::is_host_bridge(aml, parent)) return bus_number_of(aml, parent) == function.bus ? 1000 : 0;
  if (function.bus == 0) return 0;   // (on bus 0 the parent is a host bridge)
  const Optional<uint64_t> parent_adr = GpuAcpiNamespace::address_of(aml, parent);
  if (!parent_adr) return 0;   // a Device under something that is no PCI bridge (an embedded controller's child, ...)
  if (bridge) {
    if (*parent_adr != GpuAcpiNamespace::pci_adr(*bridge)) return 0;
    acpi::Node* above = aml.parent(parent);
    if (above && aml.is_device(above) && GpuAcpiNamespace::is_host_bridge(aml, above)) return bus_number_of(aml, above) == bridge->bus ? 500 : 0;
    return 100 + gpu_hints(aml, candidate);   // a bridge below another bridge: matched by _ADR alone
  }
  return 10 + gpu_hints(aml, candidate);
}

acpi::Node* find_locked(acpi::AmlInterpreter& aml, PciAddress function, const Optional<PciAddress>& bridge) {
  const uint64_t adr = GpuAcpiNamespace::pci_adr(function);
  acpi::Node* best = nullptr;
  uint32_t best_score = 0;
  walk(aml, [&](acpi::Node* n) {
    if (!aml.is_device(n)) return true;
    const Optional<uint64_t> a = GpuAcpiNamespace::address_of(aml, n);
    if (!a || *a != adr) return true;
    const uint32_t s = score(aml, n, function, bridge);
    if (s > best_score) {
      best = n;
      best_score = s;
    }
    return true;
  });
  return best;
}

}  // namespace

acpi::Node* GpuAcpiNamespace::child(acpi::AmlInterpreter& aml, acpi::Node* node, const char* name) {
  if (!node) return nullptr;
  for (acpi::Node* n = aml.first_child(node); n; n = aml.next_sibling(n))
    if (same_name(aml.name_of(n), name)) return n;
  return nullptr;
}

acpi::Node* GpuAcpiNamespace::child_method(acpi::AmlInterpreter& aml, acpi::Node* node, const char* name) {
  acpi::Node* n = child(aml, node, name);
  return aml.is_method(n) ? n : nullptr;
}

bool GpuAcpiNamespace::is_host_bridge(acpi::AmlInterpreter& aml, acpi::Node* node) {
  return aml.is_device(node) && (aml.device_has_id(node, "PNP0A08") || aml.device_has_id(node, "PNP0A03"));
}

Optional<uint64_t> GpuAcpiNamespace::address_of(acpi::AmlInterpreter& aml, acpi::Node* device) {
  acpi::Node* n = child(aml, device, "_ADR");
  uint64_t value = 0;
  if (!n || !aml.evaluate_integer(n, &value)) return {};
  return value;
}

acpi::Node* GpuAcpiNamespace::find_pci_device(acpi::AmlInterpreter& aml, PciAddress function, const Optional<PciAddress>& parent_bridge) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  return find_locked(aml, function, parent_bridge);
}

GpuAcpiNodes GpuAcpiNamespace::find_gpu(acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& parent_bridge) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  GpuAcpiNodes nodes;
  nodes.device = find_locked(aml, gpu, parent_bridge);
  if (nodes.device) {
    acpi::Node* parent = aml.parent(nodes.device);
    if (parent && aml.is_device(parent) && !is_host_bridge(aml, parent)) nodes.bridge = parent;
  } else if (parent_bridge) {
    nodes.bridge = find_locked(aml, *parent_bridge, {});
    if (nodes.bridge && is_host_bridge(aml, nodes.bridge)) nodes.bridge = nullptr;
  }
  return nodes;
}

acpi::Node* GpuAcpiNamespace::find_device_with_method(acpi::AmlInterpreter& aml, const char* name) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  acpi::Node* found = nullptr;
  walk(aml, [&](acpi::Node* n) {
    if (aml.is_device(n) && child_method(aml, n, name)) {
      found = n;
      return false;
    }
    return true;
  });
  return found;
}

ConstByteView acpi_value_bytes(const acpi::Value& value) {
  if (value.kind != acpi::ValueKind::Buffer && value.kind != acpi::ValueKind::String) return ConstByteView();
  return ConstByteView(value.bytes, value.length);   // (the interpreter's own buffer: valid until its next evaluation)
}

// --- _DSM

bool AcpiDsm::call(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision, uint32_t function, DsmArgument kind,
                   uint32_t argument, acpi::Value* result) {
  if (!dsm || !aml.is_method(dsm)) return false;
  uint8_t uuid_copy[16];                         // (private copies: the arguments are the method's to read)
  (void)ByteView(uuid_copy).copy_from(ConstByteView(uuid.bytes));
  uint8_t argument_bytes[4];
  for (uint32_t i = 0; i < 4; ++i) argument_bytes[i] = static_cast<uint8_t>(argument >> (8 * i));
  acpi::Value args[4];
  args[0].kind = acpi::ValueKind::Buffer;
  args[0].length = sizeof(uuid_copy);
  args[0].bytes = uuid_copy;
  args[1].kind = acpi::ValueKind::Integer;
  args[1].integer = revision;
  args[2].kind = acpi::ValueKind::Integer;
  args[2].integer = function;
  switch (kind) {
    case DsmArgument::EmptyPackage:
      args[3].kind = acpi::ValueKind::Package;
      args[3].length = 0;
      args[3].elements = nullptr;
      break;
    case DsmArgument::Integer:
      args[3].kind = acpi::ValueKind::Integer;
      args[3].integer = argument;
      break;
    case DsmArgument::Buffer32:
      args[3].kind = acpi::ValueKind::Buffer;
      args[3].length = sizeof(argument_bytes);
      args[3].bytes = argument_bytes;
      break;
  }
  acpi::Value ignored;
  return aml.evaluate(dsm, args, 4, result ? result : &ignored);
}

Optional<uint32_t> AcpiDsm::call_u32(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision, uint32_t function,
                                     DsmArgument kind, uint32_t argument) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  acpi::Value r;
  if (!call(aml, dsm, uuid, revision, function, kind, argument, &r)) return {};
  if (r.kind == acpi::ValueKind::Integer) return static_cast<uint32_t>(r.integer);
  if (r.kind != acpi::ValueKind::Buffer) return {};
  const ConstByteView bytes = acpi_value_bytes(r);
  if (bytes.empty()) return {};
  uint32_t value = 0;
  const ConstByteView first = bytes.at_most(4);
  for (uint64_t i = 0; i < first.size(); ++i) value |= uint32_t{first[i]} << (8 * i);
  return value;
}

uint64_t AcpiDsm::supported(acpi::AmlInterpreter& aml, acpi::Node* dsm, const AcpiDsmUuid& uuid, uint32_t revision) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  acpi::Value r;
  if (!call(aml, dsm, uuid, revision, 0, DsmArgument::EmptyPackage, 0, &r)) return 0;
  uint64_t mask = 0;
  if (r.kind == acpi::ValueKind::Integer) {
    mask = r.integer;
  } else if (r.kind == acpi::ValueKind::Buffer) {
    const ConstByteView first = acpi_value_bytes(r).at_most(8);
    for (uint64_t i = 0; i < first.size(); ++i) mask |= uint64_t{first[i]} << (8 * i);
  } else {
    return 0;
  }
  return (mask & 1) ? mask : 0;
}

// --- The PCI firmware _DSM: readiness durations

const AcpiDsmUuid kPciFirmwareDsmUuid = {{0xD0, 0x37, 0xC9, 0xE5, 0x53, 0x35, 0x7A, 0x4D, 0x91, 0x17, 0xEA, 0x4D, 0x19, 0xC3, 0x43, 0x4D}};

Optional<PciDeviceReadiness> AcpiPciReadiness::read(acpi::AmlInterpreter& aml, acpi::Node* device) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  acpi::Node* dsm = GpuAcpiNamespace::child_method(aml, device, "_DSM");
  if (!dsm || !(AcpiDsm::supported(aml, dsm, kPciFirmwareDsmUuid, kRevision) & (1ull << kFunction))) return {};
  acpi::Value r;
  if (!AcpiDsm::call(aml, dsm, kPciFirmwareDsmUuid, kRevision, kFunction, DsmArgument::EmptyPackage, 0, &r)) return {};
  if (r.kind != acpi::ValueKind::Package || !r.elements || r.length < 5) return {};
  const ArrayView<const acpi::Value> elements(r.elements, r.length);   // (the interpreter's package: read before the next evaluation, under the lock)
  auto field = [&](uint64_t index) -> Optional<uint32_t> {
    const acpi::Value& e = elements[index];
    if (e.kind != acpi::ValueKind::Integer || e.integer > 0xFFFFFFFFull) return {};
    return static_cast<uint32_t>(e.integer);
  };
  PciDeviceReadiness readiness;
  readiness.reset_us = field(0);
  readiness.dl_up_us = field(1);
  readiness.flr_us = field(2);
  readiness.d3hot_to_d0_us = field(3);
  readiness.vf_enable_us = field(4);
  return readiness;
}

uint32_t AcpiPciReadiness::d3cold_delay_ms(const Optional<PciDeviceReadiness>& readiness, uint32_t default_ms) {
  if (!readiness || (!readiness->reset_us && !readiness->dl_up_us)) return default_ms;
  uint32_t us = 0;
  if (readiness->reset_us) us = *readiness->reset_us;
  if (readiness->dl_up_us && *readiness->dl_up_us > us) us = *readiness->dl_up_us;
  const uint32_t ms = us / 1000 + (us % 1000 != 0 ? 1 : 0);   // (rounded up; no overflow: us <= 2^32 - 1)
  return ms < default_ms ? ms : default_ms;
}

// --- The power resource registry

namespace {
constinit AcpiPowerResourceRegistry g_power_resources;   // (constant initialized: the kernel image runs no static constructors)
}  // namespace

AcpiPowerResourceRegistry& AcpiPowerResourceRegistry::global() { return g_power_resources; }

AcpiPowerResourceRegistry::Entry* AcpiPowerResourceRegistry::find_locked(acpi::Node* resource) {
  for (Entry& e : ArrayView<Entry>(entries_).first(count_))
    if (e.resource == resource) return &e;
  return nullptr;
}

const AcpiPowerResourceRegistry::Entry* AcpiPowerResourceRegistry::find_locked(acpi::Node* resource) const {
  for (const Entry& e : ArrayView<const Entry>(entries_).first(count_))
    if (e.resource == resource) return &e;
  return nullptr;
}

AcpiPowerResourceRegistry::Entry* AcpiPowerResourceRegistry::add_locked(acpi::Node* resource) {
  if (Entry* e = find_locked(resource)) return e;
  if (count_ >= kMaxResources) return nullptr;
  Entry& e = entries_[count_++];
  e = Entry();
  e.resource = resource;
  return &e;
}

AcpiPowerResourceRegistry::Holder* AcpiPowerResourceRegistry::holder_in(Entry& e, acpi::Node* holder, bool create) {
  for (Holder& h : ArrayView<Holder>(e.holders).first(e.count))
    if (h.node == holder) return &h;
  if (!create || e.count >= kMaxHolders) return nullptr;
  Holder& h = e.holders[e.count++];
  h.node = holder;
  h.needs = false;
  return &h;
}

uint32_t AcpiPowerResourceRegistry::needing(const Entry& e) {
  uint32_t n = 0;
  for (const Holder& h : ArrayView<const Holder>(e.holders).first(e.count))
    if (h.needs) ++n;
  return n;
}

void AcpiPowerResourceRegistry::introduce(acpi::Node* resource, acpi::Node* holder) {
  if (!resource) return;
  kernel::ScopedMutex guard(mutex_);
  Entry* e = add_locked(resource);
  if (!e) return;
  bool known = false;
  for (const Holder& h : ArrayView<const Holder>(e->holders).first(e->count))
    if (h.node == holder) known = true;
  if (known) return;
  if (Holder* h = holder_in(*e, holder, true)) h->needs = true;
}

uint32_t AcpiPowerResourceRegistry::need(acpi::Node* resource, acpi::Node* holder) {
  if (!resource) return 1;
  kernel::ScopedMutex guard(mutex_);
  Entry* e = add_locked(resource);
  Holder* h = e ? holder_in(*e, holder, true) : nullptr;
  if (!h) return 1;   // (not tracked)
  h->needs = true;
  return needing(*e);
}

uint32_t AcpiPowerResourceRegistry::drop(acpi::Node* resource, acpi::Node* holder) {
  if (!resource) return 0;
  kernel::ScopedMutex guard(mutex_);
  Entry* e = find_locked(resource);
  if (!e) return 0;
  Holder* h = holder_in(*e, holder, false);
  if (h) h->needs = false;   // (a holder the list had no room for was never counted: the others are all there is)
  return needing(*e);
}

uint32_t AcpiPowerResourceRegistry::holders_needing(acpi::Node* resource) const {
  kernel::ScopedMutex guard(mutex_);
  const Entry* e = find_locked(resource);
  return e ? needing(*e) : 0;
}

bool AcpiPowerResourceRegistry::needs(acpi::Node* resource, acpi::Node* holder) const {
  kernel::ScopedMutex guard(mutex_);
  const Entry* e = find_locked(resource);
  if (!e) return false;
  for (const Holder& h : ArrayView<const Holder>(e->holders).first(e->count))
    if (h.node == holder) return h.needs;
  return false;
}

void AcpiPowerResourceRegistry::clear() {
  kernel::ScopedMutex guard(mutex_);
  for (Entry& e : ArrayView<Entry>(entries_).first(count_)) e = Entry();
  count_ = 0;
}

// --- Power resources

namespace {

bool resource_is_on(acpi::AmlInterpreter& aml, acpi::Node* resource) {
  acpi::Node* sta = GpuAcpiNamespace::child(aml, resource, "_STA");
  if (!sta) return true;   // (no _STA, or one that faults: counted as on, so _OFF runs)
  uint64_t value = 0;
  if (!aml.evaluate_integer(sta, &value)) return true;
  return value & 1;
}

bool resource_known_on(acpi::AmlInterpreter& aml, acpi::Node* resource) {
  acpi::Node* sta = GpuAcpiNamespace::child(aml, resource, "_STA");
  uint64_t value = 0;
  return sta && aml.evaluate_integer(sta, &value) && (value & 1);
}

bool run(acpi::AmlInterpreter& aml, acpi::Node* node, const char* method) {
  acpi::Node* m = GpuAcpiNamespace::child_method(aml, node, method);
  return !m || aml.evaluate(m, nullptr, 0, nullptr);
}

bool contains(ArrayView<acpi::Node* const> list, acpi::Node* n) {
  for (acpi::Node* e : list)
    if (e == n) return true;
  return false;
}

}  // namespace

uint32_t GpuPowerResourceSet::read_list(acpi::AmlInterpreter& aml, acpi::Node* holder, const char* name, ArrayView<acpi::Node*> out) {
  acpi::Node* list = GpuAcpiNamespace::child(aml, holder, name);
  if (!list) return 0;
  acpi::Value v;
  if (!aml.evaluate(list, nullptr, 0, &v) || v.kind != acpi::ValueKind::Package) return 0;
  uint32_t count = 0;
  const ArrayView<const acpi::Value> elements(v.elements, v.length);   // (the interpreter's package: valid until its next evaluation, and the lock is held)
  for (const acpi::Value& e : elements) {
    if (count >= out.size()) break;
    // A reference to a PowerResource: an object with _ON and _OFF methods of its own (anything else in the list is skipped).
    if (e.kind != acpi::ValueKind::Object || !e.node) continue;
    acpi::Node* r = e.node;
    if (!GpuAcpiNamespace::child_method(aml, r, "_ON") || !GpuAcpiNamespace::child_method(aml, r, "_OFF")) continue;
    if (contains(ArrayView<acpi::Node* const>(out.data(), count), r)) continue;
    out[count++] = r;
  }
  return count;
}

void GpuPowerResourceSet::read(acpi::AmlInterpreter& aml, acpi::Node* device, acpi::Node* bridge) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  device_ = device;
  bridge_ = bridge;
  holder_ = nullptr;
  d3_count_ = d0_count_ = 0;
  acpi::Node* const candidates[] = {device, bridge};
  for (acpi::Node* candidate : candidates) {
    if (!candidate) continue;
    const uint32_t n = read_list(aml, candidate, "_PR3", ArrayView<acpi::Node*>(d3_));
    if (n == 0) continue;
    holder_ = candidate;
    d3_count_ = n;
    d0_count_ = read_list(aml, candidate, "_PR0", ArrayView<acpi::Node*>(d0_));
    break;
  }
  ps3_ = GpuAcpiNamespace::child_method(aml, device, "_PS3") || (on_bridge() && GpuAcpiNamespace::child_method(aml, bridge, "_PS3"));
  // The GPU was found in D0: it holds what D0 and D3hot need (a second read of the same GPU changes nothing).
  for (acpi::Node* r : d0_resources()) registry_->introduce(r, identity());
  for (acpi::Node* r : d3_resources()) registry_->introduce(r, identity());
  readiness_ = AcpiPciReadiness::read(aml, device);
  if (!readiness_ && bridge) readiness_ = AcpiPciReadiness::read(aml, bridge);
}

bool GpuPowerResourceSet::power_off(acpi::AmlInterpreter& aml) const {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  if (device_ && !run(aml, device_, "_PS3")) return false;
  if (on_bridge() && !run(aml, bridge_, "_PS3")) return false;
  // In D3cold the GPU needs none of its resources; those another holder still needs stay on.
  for (acpi::Node* r : d0_resources()) (void)registry_->drop(r, identity());
  for (acpi::Node* r : d3_resources()) (void)registry_->drop(r, identity());
  const ArrayView<acpi::Node* const> list = d3_resources();
  for (uint64_t i = list.size(); i-- > 0;) {
    if (registry_->holders_needing(list[i]) > 0) continue;   // shared, and in use
    if (!resource_is_on(aml, list[i])) continue;
    if (!run(aml, list[i], "_OFF")) {
      for (acpi::Node* r : d3_resources()) (void)registry_->need(r, identity());   // (still in D3hot, which needs them)
      return false;
    }
  }
  return true;
}

bool GpuPowerResourceSet::power_on(acpi::AmlInterpreter& aml) const {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  bool ok = true;
  auto turn_on = [&](acpi::Node* r) {
    if (resource_known_on(aml, r)) return;   // (no _STA, or one that faults: _ON is run anyway)
    if (!run(aml, r, "_ON")) ok = false;
  };
  for (acpi::Node* r : d0_resources()) (void)registry_->need(r, identity());
  for (acpi::Node* r : d3_resources()) (void)registry_->need(r, identity());
  for (acpi::Node* r : d0_resources()) turn_on(r);
  for (acpi::Node* r : d3_resources())
    if (!contains(d0_resources(), r)) turn_on(r);
  if (on_bridge() && !run(aml, bridge_, "_PS0")) ok = false;
  if (device_ && !run(aml, device_, "_PS0")) ok = false;
  return ok;
}

}  // namespace hardware
