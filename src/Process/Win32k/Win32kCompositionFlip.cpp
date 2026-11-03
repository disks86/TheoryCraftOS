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
#include "Process/Win32k/Win32kCompositionInternal.hpp"

// NtFlipObject*: flip objects, the present queue between a producer (an application's swap chain) and a consumer (the compositor) with its pool buffers, contents, presents,
// back-channel messages and events (docs/win32k/Composition.md).
namespace process::win32k::composition {

namespace {

constexpr uint32_t kMaxEndpointsPerProcess = 1024, kMaxEventsPerFlip = 16;
constexpr uint32_t kPresentQueued = 0, kPresentAcquired = 1, kPresentProcessing = 2;
constexpr uint32_t kMessageStatistics = 0x100;

FlipEndpoint* endpoint_of(Ctx& x, uint64_t handle, uint32_t right, bool want_consumer, bool either, uint32_t* status) {
  FlipEndpoint* e = lookup<FlipEndpoint>(x, handle, Kind::FlipEndpoint, right, status);
  if (!e) return nullptr;
  if (!either && e->consumer != want_consumer) { *status = kStatusObjectTypeMismatch; return nullptr; }
  if (e->disconnected) { *status = kStatusPortDisconnected; return nullptr; }
  return e;
}
FlipEndpoint* producer_of(Ctx& x, uint64_t handle, uint32_t right, uint32_t* status) { return endpoint_of(x, handle, right, false, false, status); }
FlipEndpoint* consumer_of(Ctx& x, uint64_t handle, uint32_t right, uint32_t* status) { return endpoint_of(x, handle, right, true, false, status); }

FlipBuffer* find_buffer(FlipObject& f, uint32_t id) {
  for (FlipBuffer& b : f.buffers.view()) if (b.id == id) return &b;
  return nullptr;
}
FlipContent* find_content(FlipObject& f, uint32_t id) {
  for (FlipContent& c : f.contents.view()) if (c.id == id) return &c;
  return nullptr;
}
FlipPresent* find_present(FlipObject& f, uint64_t id) {
  for (FlipPresent& p : f.presents.view()) if (p.id == id) return &p;
  return nullptr;
}
bool buffer_in_use(const FlipObject& f, uint32_t buffer) {
  for (const FlipPresent& p : f.presents.view()) if (p.buffer == buffer) return true;
  return false;
}
uint32_t usage_of(const FlipObject& f, uint32_t buffer) {
  for (uint64_t i = 0; i + 1 < f.usage_refs.count(); i += 2) if (f.usage_refs[i] == buffer) return f.usage_refs[i + 1];
  return 0;
}

// A pool buffer is available when no present refers to it. The events the producer asked for follow that: signalled while some buffer is, reset while none is.
void refresh_availability(Ctx& x, FlipObject& f) {
  bool any = false;
  for (const FlipBuffer& b : f.buffers.view()) if (!buffer_in_use(f, b.id)) any = true;
  if (any == f.available) return;
  f.available = any;
  for (uint32_t id : f.events.view()) x.st.defer_event(id, any, false, !any);
}

uint64_t make_endpoint(Ctx& x, SharedPointer<FlipObject> flip, bool consumer, uint32_t access, uint64_t out) {
  Call& c = x.call;
  if (out == 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::FlipEndpoint) >= kMaxEndpointsPerProcess) return kStatusQuotaExceeded;
  SharedPointer<FlipEndpoint> e = make_object<FlipEndpoint>(x, flip, consumer);
  if (!e) return kStatusNoMemory;
  uint32_t status;
  SharedPointer<Object> base = e;
  const uint64_t h = new_handle(x, base, access, &status);
  if (!h) return status;
  if (!write_handle(c, out, h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}

// NtFlipObjectCreate(PHANDLE producer, ACCESS_MASK access, DWORD flags (0)): a new flip object; the handle is its producer end
uint64_t svc_create(Call& c, Ctx& x) {
  uint32_t access;
  if (!map_access(c.arg32(1), &access)) return kStatusAccessDenied;
  if (c.arg32(2) != 0) return kStatusInvalidParameter;
  SharedPointer<FlipObject> f = SharedPointer<FlipObject>::make(x.st.allocator, x.st.allocator);
  if (!f) return kStatusNoMemory;
  f->owner = c.pid();
  return make_endpoint(x, f, false, access, c.arg(0));
}

// NtFlipObjectOpen(HANDLE token, ACCESS_MASK access, PHANDLE consumer): the consumer end of the flip object a token names (a return token gives the producer end back)
uint64_t svc_open(Call& c, Ctx& x) {
  uint32_t status, access;
  if (!map_access(c.arg32(1), &access)) return kStatusAccessDenied;
  Token* t = lookup<Token>(x, c.arg(0), Kind::Token, kRightQuery, &status);
  if (!t) return status;
  if (t->type == Token::Composition || !t->flip) return kStatusObjectTypeMismatch;
  FlipObject& f = *t->flip;
  const bool consumer = t->type == Token::Flip;
  if (consumer ? !f.producer_open : !f.consumer_open) return kStatusPortDisconnected;
  if (consumer ? f.consumer_open : f.producer_open) return kStatusDeviceBusy;
  SharedPointer<FlipObject> flip = t->flip;
  const uint64_t result = make_endpoint(x, flip, consumer, access, c.arg(2));
  if (result == kStatusSuccess) {
    if (consumer) { f.consumer_open = true; f.consumer_ever = true; } else f.producer_open = true;
    // the lost events handed out for the end that was missing are not lost any more
    for (uint32_t id : (consumer ? f.lost_for_producer : f.lost_for_consumer).view()) x.st.defer_event(id, false, false, true);
  }
  return result;
}

// NtFlipObjectDisconnectEndpoint(HANDLE endpoint)
uint64_t svc_disconnect(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = lookup<FlipEndpoint>(x, c.arg(0), Kind::FlipEndpoint, 0, &status);
  if (!e) return status;
  if (e->disconnected) return kStatusPortDisconnected;
  flip_disconnect(x.st, *e);
  return kStatusSuccess;
}

// NtFlipObjectQueryEndpointConnected(HANDLE endpoint, BOOLEAN* connected): is the other end there
uint64_t svc_query_connected(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = lookup<FlipEndpoint>(x, c.arg(0), Kind::FlipEndpoint, kRightQuery, &status);
  if (!e) return status;
  const uint8_t connected = !e->disconnected && (e->consumer ? e->flip->producer_open : e->flip->consumer_open) ? 1 : 0;
  return c.write(c.arg(1), ConstByteView(&connected, 1)) ? kStatusSuccess : kStatusAccessViolation;
}

// Hands the caller an event the flip object keeps: a new one each call, all signalled together.
uint64_t give_flip_event(Call& c, Ctx& x, SharedPointer<Object> pin, bool lost, uint64_t out) {
  FlipEndpoint* e = static_cast<FlipEndpoint*>(pin.get());
  SharedPointer<FlipObject> flip = e->flip;
  const bool consumer = e->consumer;
  List<uint32_t>& list = lost ? (consumer ? flip->lost_for_consumer : flip->lost_for_producer) : flip->events;
  if (list.count() >= kMaxEventsPerFlip) return kStatusQuotaExceeded;
  const bool signaled = lost ? (consumer ? !flip->producer_open : (flip->consumer_ever && !flip->consumer_open)) : flip->available;
  const uint32_t id = make_event(x, true, signaled, 1);      // our reference stays with the flip object, the second goes to the caller's handle
  if (!id) return kStatusNoMemory;
  List<uint32_t>& again = lost ? (consumer ? flip->lost_for_consumer : flip->lost_for_producer) : flip->events;
  if (!again.push_back(id)) { x.st.defer_event(id, false, true); x.st.defer_event(id, false, true); return kStatusNoMemory; }
  uint64_t handle = 0;
  if (!give_descriptor(c, DescriptorKind::Event, id, &handle)) { x.st.defer_event(id, false, true); return kStatusNoMemory; }
  if (!write_handle(c, out, handle)) { drop_descriptor(x, handle); return kStatusAccessViolation; }
  return kStatusSuccess;
}
uint64_t flip_event_service(Call& c, Ctx& x, bool lost) {
  uint32_t status;
  if (c.arg(1) == 0) return kStatusInvalidParameter;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::FlipEndpoint, kRightQuery, &status);
  if (!pin) return status;
  FlipEndpoint* e = static_cast<FlipEndpoint*>(pin.get());
  if (e->disconnected) return kStatusPortDisconnected;
  if (!lost && e->consumer) return kStatusObjectTypeMismatch;
  return give_flip_event(c, x, pin, lost, c.arg(1));
}
// NtFlipObjectQueryBufferAvailableEvent(HANDLE producer, PHANDLE event): signalled while a pool buffer is free
uint64_t svc_buffer_available_event(Call& c, Ctx& x) { return flip_event_service(c, x, false); }
// NtFlipObjectQueryLostEvent(HANDLE endpoint, PHANDLE event): signalled when the other end is gone
uint64_t svc_lost_event(Call& c, Ctx& x) { return flip_event_service(c, x, true); }

// NtFlipObjectAddPoolBuffer(HANDLE producer, HANDLE section (optional), DWORD size, DWORD* buffer_id)
uint64_t svc_add_pool_buffer(Call& c, Ctx& x) {
  uint32_t status = kStatusSuccess, section = 0;
  const uint32_t size = c.arg32(2);
  if (size == 0 || size > (256u << 20) || c.arg(3) == 0) return kStatusInvalidParameter;
  if (c.arg(1) != 0) { section = take_object(x, c.arg(1), DescriptorKind::Section, 0, &status); if (!section) return status; }
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e || e->flip->buffers.count() >= kFlipMaxBuffers) {
    if (section) x.st.defer_event(section, false, true);
    return e ? kStatusQuotaExceeded : status;
  }
  FlipObject& f = *e->flip;
  FlipBuffer b;
  b.id = f.next_buffer++;
  b.size = size;
  b.section = section;
  if (!f.buffers.push_back(b)) { if (section) x.st.defer_event(section, false, true); return kStatusNoMemory; }
  if (!c.write_u32(c.arg(3), b.id)) {
    f.buffers.remove_at(f.buffers.count() - 1);
    if (section) x.st.defer_event(section, false, true);
    return kStatusAccessViolation;
  }
  refresh_availability(x, f);
  return kStatusSuccess;
}
// NtFlipObjectRemovePoolBuffer(HANDLE producer, DWORD buffer_id): not while a present or the consumer uses it
uint64_t svc_remove_pool_buffer(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  for (uint64_t i = 0; i < f.buffers.count(); ++i) {
    if (f.buffers[i].id != c.arg32(1)) continue;
    if (buffer_in_use(f, f.buffers[i].id) || usage_of(f, f.buffers[i].id) != 0) return kStatusDeviceBusy;
    for (const FlipContent& ct : f.contents.view()) if (ct.buffer == f.buffers[i].id) return kStatusDeviceBusy;
    if (f.buffers[i].section) x.st.defer_event(f.buffers[i].section, false, true);
    f.buffers.remove_at(i);
    refresh_availability(x, f);
    return kStatusSuccess;
  }
  return kStatusNotFound;
}
// NtFlipObjectAddContent(HANDLE producer, DWORD buffer_id, DWORD flags, DWORD* content_id): content to present: a view of a pool buffer
uint64_t svc_add_content(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  if (!find_buffer(f, c.arg32(1))) return kStatusNotFound;
  if (c.arg(3) == 0 || c.arg32(2) > 0xFFFF) return kStatusInvalidParameter;
  if (f.contents.count() >= kFlipMaxContents) return kStatusQuotaExceeded;
  FlipContent ct;
  ct.id = f.next_content++;
  ct.buffer = c.arg32(1);
  ct.flags = c.arg32(2);
  if (!c.write_u32(c.arg(3), ct.id)) return kStatusAccessViolation;
  return f.contents.push_back(ct) ? kStatusSuccess : kStatusNoMemory;
}
// NtFlipObjectRemoveContent(HANDLE producer, DWORD content_id)
uint64_t svc_remove_content(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  for (const FlipPresent& p : f.presents.view()) if (p.content == c.arg32(1)) return kStatusDeviceBusy;
  for (uint64_t i = 0; i < f.contents.count(); ++i) if (f.contents[i].id == c.arg32(1)) { f.contents.remove_at(i); return kStatusSuccess; }
  return kStatusNotFound;
}
// NtFlipObjectSetContent(HANDLE producer, DWORD content_id, UINT64 present_id, DWORD flags): queues a present of the content
uint64_t svc_set_content(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  FlipContent* ct = find_content(f, c.arg32(1));
  if (!ct) return kStatusNotFound;
  if (find_present(f, c.arg(2))) return kStatusInvalidParameter;
  if (f.presents.count() >= kFlipMaxPresents) return kStatusQuotaExceeded;
  FlipPresent p;
  p.id = c.arg(2); p.content = ct->id; p.buffer = ct->buffer; p.flags = c.arg32(3); p.state = kPresentQueued;
  if (!f.presents.push_back(p)) return kStatusNoMemory;
  refresh_availability(x, f);
  return kStatusSuccess;
}
// NtFlipObjectPresentCancel(HANDLE producer, UINT64 present_id): withdraws a present the consumer has not taken
uint64_t svc_present_cancel(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  for (uint64_t i = 0; i < f.presents.count(); ++i) {
    if (f.presents[i].id != c.arg(1)) continue;
    if (f.presents[i].state != kPresentQueued) return kStatusUnsuccessful;
    f.presents.remove_at(i);
    refresh_availability(x, f);
    return kStatusSuccess;
  }
  return kStatusNotFound;
}
// NtFlipObjectEnablePresentStatisticsType(HANDLE producer, DWORD type (0..7)): the consumer's finished presents are reported back as messages
uint64_t svc_enable_statistics(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  if (c.arg32(1) > 7) return kStatusInvalidParameter;
  e->flip->stats_types |= 1u << c.arg32(1);
  return kStatusSuccess;
}
// NtFlipObjectSetMaximumBackchannelQueueDepth(HANDLE producer, DWORD depth (1..256))
uint64_t svc_set_backchannel_depth(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  if (c.arg32(1) == 0 || c.arg32(1) > 256) return kStatusInvalidParameter;
  e->flip->max_backchannel = c.arg32(1);
  return kStatusSuccess;
}
// NtFlipObjectQueryNextMessageToProducer(HANDLE producer, DWORD* size, DWORD* type): looks at the next back-channel message
uint64_t svc_query_next_message(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightQuery, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  if (f.messages.count() == 0) return kStatusNoMoreEntries;
  if (!c.write_u32(c.arg(1), f.messages[0].size) || (c.arg(2) != 0 && !c.write_u32(c.arg(2), f.messages[0].type))) return kStatusAccessViolation;
  return kStatusSuccess;
}
// NtFlipObjectReadNextMessageToProducer(HANDLE producer, void* buffer, DWORD size, DWORD* read): the message as {DWORD type, data}; STATUS_BUFFER_TOO_SMALL leaves it queued
uint64_t svc_read_next_message(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = producer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  if (f.messages.count() == 0) return kStatusNoMoreEntries;
  const FlipMessage m = f.messages[0];
  const uint32_t need = 4 + m.size;
  if (c.arg32(2) < need) {
    if (c.arg(3) != 0) c.write_u32(c.arg(3), need);
    return kStatusBufferTooSmall;
  }
  uint8_t out[4 + kFlipMaxMessage];
  for (uint32_t i = 0; i < 4; ++i) out[i] = uint8_t(m.type >> (8 * i));
  for (uint32_t i = 0; i < m.size; ++i) out[4 + i] = m.data[i];
  if (!c.write(c.arg(1), ConstByteView(out, need)) || (c.arg(3) != 0 && !c.write_u32(c.arg(3), need))) return kStatusAccessViolation;
  f.messages.remove_at(0);
  return kStatusSuccess;
}

// --- the consumer -----------------------------------------------------------------------------------------------------------------------------------------------------

struct PresentInfo { uint64_t present_id; uint32_t content, buffer, flags, reserved; };

// NtFlipObjectConsumerAcquirePresent(HANDLE consumer, PresentInfo* out): takes the oldest queued present; STATUS_NO_MORE_ENTRIES if there is none
uint64_t svc_acquire_present(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  for (FlipPresent& p : f.presents.view()) {
    if (p.state != kPresentQueued) continue;
    const PresentInfo info = {p.id, p.content, p.buffer, p.flags, 0};
    if (!c.write_struct(c.arg(1), info)) return kStatusAccessViolation;
    p.state = kPresentAcquired;
    return kStatusSuccess;
  }
  return kStatusNoMoreEntries;
}
// NtFlipObjectConsumerBeginProcessPresent(HANDLE consumer, UINT64 present_id)
uint64_t svc_begin_process(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipPresent* p = find_present(*e->flip, c.arg(1));
  if (!p) return kStatusNotFound;
  if (p->state != kPresentAcquired) return kStatusInvalidDeviceState;
  p->state = kPresentProcessing;
  return kStatusSuccess;
}
// NtFlipObjectConsumerEndProcessPresent(HANDLE consumer, UINT64 present_id, DWORD status): the present is done and its buffer goes back to the producer
uint64_t svc_end_process(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  for (uint64_t i = 0; i < f.presents.count(); ++i) {
    if (f.presents[i].id != c.arg(1)) continue;
    if (f.presents[i].state != kPresentProcessing) return kStatusInvalidDeviceState;
    f.presents.remove_at(i);
    ++f.completed;
    if ((f.stats_types & 1) && f.messages.count() < f.max_backchannel) {
      FlipMessage m;
      m.type = kMessageStatistics;
      m.size = 16;
      for (uint32_t b = 0; b < 8; ++b) m.data[b] = uint8_t(c.arg(1) >> (8 * b));
      for (uint32_t b = 0; b < 4; ++b) m.data[8 + b] = uint8_t(c.arg32(2) >> (8 * b));
      for (uint32_t b = 0; b < 4; ++b) m.data[12 + b] = uint8_t(f.completed >> (8 * b));
      f.messages.push_back(m);
    }
    refresh_availability(x, f);
    return kStatusSuccess;
  }
  return kStatusNotFound;
}
// NtFlipObjectConsumerPostMessage(HANDLE consumer, DWORD type, void* data, DWORD size (0..56)): a message for the producer; STATUS_QUOTA_EXCEEDED when its queue is full
uint64_t svc_post_message(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  const uint32_t size = c.arg32(3);
  if (size > kFlipMaxMessage || (size != 0 && c.arg(2) == 0)) return kStatusInvalidParameter;
  if (f.messages.count() >= f.max_backchannel) return kStatusQuotaExceeded;
  FlipMessage m;
  m.type = c.arg32(1);
  m.size = size;
  if (size != 0 && !c.read(c.arg(2), ByteView(m.data, size))) return kStatusAccessViolation;
  return f.messages.push_back(m) ? kStatusSuccess : kStatusNoMemory;
}
struct BufferInfo { uint32_t id, size, flags, usage; };
// NtFlipObjectConsumerQueryBufferInfo(HANDLE consumer, DWORD buffer_id, BufferInfo* out)
uint64_t svc_query_buffer_info(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightQuery, &status);
  if (!e) return status;
  FlipBuffer* b = find_buffer(*e->flip, c.arg32(1));
  if (!b) return kStatusNotFound;
  const BufferInfo info = {b->id, b->size, b->flags, usage_of(*e->flip, b->id)};
  return c.write_struct(c.arg(2), info) ? kStatusSuccess : kStatusAccessViolation;
}
// NtFlipObjectConsumerAdjustUsageReference(HANDLE consumer, DWORD buffer_id, LONG delta): the consumer keeps a buffer alive while it uses it
uint64_t svc_adjust_usage(Call& c, Ctx& x) {
  uint32_t status;
  FlipEndpoint* e = consumer_of(x, c.arg(0), kRightModify, &status);
  if (!e) return status;
  FlipObject& f = *e->flip;
  if (!find_buffer(f, c.arg32(1))) return kStatusNotFound;
  const int32_t delta = c.argi(2);
  for (uint64_t i = 0; i + 1 < f.usage_refs.count(); i += 2) {
    if (f.usage_refs[i] != c.arg32(1)) continue;
    const int64_t next = int64_t(f.usage_refs[i + 1]) + delta;
    if (next < 0 || next > 0xFFFF) return kStatusInvalidParameter;
    f.usage_refs[i + 1] = uint32_t(next);
    return kStatusSuccess;
  }
  if (delta < 0 || delta > 0xFFFF) return kStatusInvalidParameter;
  return f.usage_refs.push_back(c.arg32(1)) && f.usage_refs.push_back(uint32_t(delta)) ? kStatusSuccess : kStatusNoMemory;
}

}  // namespace

void install_flip() {
  COMPOSITION_REGISTER(NtFlipObjectCreate, svc_create);
  COMPOSITION_REGISTER(NtFlipObjectOpen, svc_open);
  COMPOSITION_REGISTER(NtFlipObjectDisconnectEndpoint, svc_disconnect);
  COMPOSITION_REGISTER(NtFlipObjectQueryEndpointConnected, svc_query_connected);
  COMPOSITION_REGISTER(NtFlipObjectQueryBufferAvailableEvent, svc_buffer_available_event);
  COMPOSITION_REGISTER(NtFlipObjectQueryLostEvent, svc_lost_event);
  COMPOSITION_REGISTER(NtFlipObjectAddPoolBuffer, svc_add_pool_buffer);
  COMPOSITION_REGISTER(NtFlipObjectRemovePoolBuffer, svc_remove_pool_buffer);
  COMPOSITION_REGISTER(NtFlipObjectAddContent, svc_add_content);
  COMPOSITION_REGISTER(NtFlipObjectRemoveContent, svc_remove_content);
  COMPOSITION_REGISTER(NtFlipObjectSetContent, svc_set_content);
  COMPOSITION_REGISTER(NtFlipObjectPresentCancel, svc_present_cancel);
  COMPOSITION_REGISTER(NtFlipObjectEnablePresentStatisticsType, svc_enable_statistics);
  COMPOSITION_REGISTER(NtFlipObjectSetMaximumBackchannelQueueDepth, svc_set_backchannel_depth);
  COMPOSITION_REGISTER(NtFlipObjectQueryNextMessageToProducer, svc_query_next_message);
  COMPOSITION_REGISTER(NtFlipObjectReadNextMessageToProducer, svc_read_next_message);
  COMPOSITION_REGISTER(NtFlipObjectConsumerAcquirePresent, svc_acquire_present);
  COMPOSITION_REGISTER(NtFlipObjectConsumerBeginProcessPresent, svc_begin_process);
  COMPOSITION_REGISTER(NtFlipObjectConsumerEndProcessPresent, svc_end_process);
  COMPOSITION_REGISTER(NtFlipObjectConsumerPostMessage, svc_post_message);
  COMPOSITION_REGISTER(NtFlipObjectConsumerQueryBufferInfo, svc_query_buffer_info);
  COMPOSITION_REGISTER(NtFlipObjectConsumerAdjustUsageReference, svc_adjust_usage);
}

}  // namespace process::win32k::composition
