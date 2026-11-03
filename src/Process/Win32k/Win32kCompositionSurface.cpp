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

// Composition surfaces (NtCreateCompositionSurfaceHandle ...), composition input sinks, and the token manager (docs/win32k/Composition.md).
namespace process::win32k::composition {

namespace {

constexpr uint32_t kMaxSurfacesPerProcess = 4096, kMaxSinksPerProcess = 1024, kMaxTokensPerProcess = 1024;
constexpr uint64_t kMaxSurfaceSection = 64ull << 20;

struct SurfaceStatistics { uint32_t size, flags, frame_num, frame_den; uint64_t present_count; };
static_assert(sizeof(SurfaceStatistics) == 24, "SurfaceStatistics layout");
struct RectI { int32_t left, top, right, bottom; };

Surface* surface_of(Ctx& x, uint64_t handle, uint32_t right, uint32_t* status) { return lookup<Surface>(x, handle, Kind::Surface, right, status); }

// NtCreateCompositionSurfaceHandle(PHANDLE surface, ACCESS_MASK access)
uint64_t svc_create_surface(Call& c, Ctx& x) {
  uint32_t access, status;
  if (!map_access(c.arg32(1), &access)) return kStatusAccessDenied;
  if (c.arg(0) == 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::Surface) >= kMaxSurfacesPerProcess) return kStatusQuotaExceeded;
  SharedPointer<Surface> s = make_object<Surface>(x);
  if (!s) return kStatusNoMemory;
  s->owner = c.pid();
  SharedPointer<Object> base = s;
  const uint64_t h = new_handle(x, base, access, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}

// NtValidateCompositionSurfaceHandle(HANDLE surface, DWORD flags (0))
uint64_t svc_validate_surface(Call& c, Ctx& x) {
  uint32_t status;
  if (c.arg32(1) != 0) return kStatusInvalidParameter;
  surface_of(x, c.arg(0), 0, &status);
  return status;
}

// NtBindCompositionSurface(HANDLE surface, HANDLE channel, DWORD resource_id): the surface becomes the content of that resource
uint64_t svc_bind_surface(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  Channel* ch = lookup<Channel>(x, c.arg(1), Kind::Channel, kRightModify, &status);
  if (!ch) return status;
  if (s->bound_channel != 0) return kStatusInvalidDeviceState;
  const uint32_t id = c.arg32(2);
  bool found = false;
  for (const Resource& r : ch->resources.view()) if (r.id == id && !r.released) found = true;
  if (!found) return kStatusInvalidParameter;
  s->bound_channel = ch->serial;
  s->bound_resource = id;
  return kStatusSuccess;
}
// NtUnBindCompositionSurface(HANDLE surface)
uint64_t svc_unbind_surface(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (s->bound_channel == 0) return kStatusInvalidDeviceState;
  s->bound_channel = 0;
  s->bound_resource = 0;
  return kStatusSuccess;
}
// NtQueryCompositionSurfaceBinding(HANDLE surface, UINT64* channel_serial, DWORD* resource_id)
uint64_t svc_query_binding(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery, &status);
  if (!s) return status;
  if (s->bound_channel == 0) return kStatusNotFound;
  if (!c.write_u64(c.arg(1), s->bound_channel) || !c.write_u32(c.arg(2), s->bound_resource)) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtSetCompositionSurfaceStatistics(HANDLE surface, SurfaceStatistics* in, DWORD size) / NtQueryCompositionSurfaceStatistics(HANDLE, SurfaceStatistics* out, DWORD size)
uint64_t svc_set_statistics(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(2) != sizeof(SurfaceStatistics)) return kStatusInfoLengthMismatch;
  SurfaceStatistics in;
  if (!c.read_struct(c.arg(1), &in)) return kStatusAccessViolation;
  if (in.size != sizeof(in) || (in.frame_den == 0) != (in.frame_num == 0)) return kStatusInvalidParameter;
  s->stats_flags = in.flags;
  s->frame_num = in.frame_num;
  s->frame_den = in.frame_den;
  s->stats_present_count = in.present_count;
  return kStatusSuccess;
}
uint64_t svc_query_statistics(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery, &status);
  if (!s) return status;
  if (c.arg32(2) != sizeof(SurfaceStatistics)) return kStatusInfoLengthMismatch;
  SurfaceStatistics out = {sizeof(SurfaceStatistics), s->stats_flags, s->frame_num, s->frame_den, s->stats_present_count + s->present_notifications};
  return c.write_struct(c.arg(1), out) ? kStatusSuccess : kStatusAccessViolation;
}
// NtQueryCompositionSurfaceFrameRate(HANDLE surface, DWORD* numerator, DWORD* denominator): 0/0 when none was set
uint64_t svc_query_frame_rate(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery, &status);
  if (!s) return status;
  return c.write_u32(c.arg(1), s->frame_num) && c.write_u32(c.arg(2), s->frame_den) ? kStatusSuccess : kStatusAccessViolation;
}
// NtQueryCompositionSurfaceRenderingRealization(HANDLE surface, DWORD* realization): 0 until the first present; then 2 (software: this system has no hardware path)
uint64_t svc_query_realization(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery, &status);
  if (!s) return status;
  return c.write_u32(c.arg(1), s->realization) ? kStatusSuccess : kStatusAccessViolation;
}

// NtNotifyPresentToCompositionSurface(HANDLE surface, UINT64 present_id, RECT* dirty (optional), DWORD flags (0))
uint64_t svc_notify_present(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(3) != 0) return kStatusInvalidParameter;
  RectI r = {};
  if (c.arg(2) != 0) {
    if (!c.read_struct(c.arg(2), &r)) return kStatusAccessViolation;
    if (r.right < r.left || r.bottom < r.top) return kStatusInvalidParameter;
    s->dirty[0] = uint32_t(r.left); s->dirty[1] = uint32_t(r.top); s->dirty[2] = uint32_t(r.right); s->dirty[3] = uint32_t(r.bottom);
    s->has_dirty = true;
  }
  ++s->present_notifications;
  s->realization = 2;
  x.signal(s->event_present);
  return kStatusSuccess;
}
// NtOpenCompositionSurfaceDirtyRegion(HANDLE surface, RECT* dirty): the area presented since the last call (taken)
uint64_t svc_open_dirty_region(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery | kRightModify, &status);
  if (!s) return status;
  if (!s->has_dirty) return kStatusNotFound;
  const RectI r = {int32_t(s->dirty[0]), int32_t(s->dirty[1]), int32_t(s->dirty[2]), int32_t(s->dirty[3])};
  if (!c.write_struct(c.arg(1), r)) return kStatusAccessViolation;
  s->has_dirty = false;
  return kStatusSuccess;
}
// NtOpenCompositionSurfaceRealizationInfo(HANDLE surface, DWORD* realization, UINT64* present_count)
uint64_t svc_open_realization_info(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightQuery, &status);
  if (!s) return status;
  if (!c.write_u32(c.arg(1), s->realization) || (c.arg(2) != 0 && !c.write_u64(c.arg(2), s->present_notifications))) return kStatusAccessViolation;
  return kStatusSuccess;
}

// NtSetCompositionSurfaceBufferUsage(HANDLE surface, DWORD usage, UINT64 section_size): how the surface is used and how big its backing section is (0: none)
uint64_t svc_set_buffer_usage(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(1) > 0xFF || c.arg(2) > kMaxSurfaceSection) return kStatusInvalidParameter;
  if (s->section_id != 0 && c.arg(2) != s->section_size) return kStatusInvalidDeviceState;      // (the section exists already)
  s->buffer_usage = c.arg32(1);
  s->section_size = c.arg(2);
  return kStatusSuccess;
}
// NtSetCompositionSurfaceAnalogExclusive(HANDLE surface, DWORD enable): analog (TV) output needs hardware this system has not
uint64_t svc_set_analog_exclusive(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(1) > 1) return kStatusInvalidParameter;
  if (c.arg32(1) == 1) return kStatusNotSupported;
  s->analog_exclusive = 0;
  return kStatusSuccess;
}
// NtSetCompositionSurfaceDirectFlipState(HANDLE surface, DWORD state (0 off, 1 requested, 2 active))
uint64_t svc_set_direct_flip(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(1) > 2) return kStatusInvalidParameter;
  s->direct_flip = c.arg32(1);
  return kStatusSuccess;
}
// NtSetCompositionSurfaceIndependentFlipInfo(HANDLE surface, DWORD* info (4 values)) / NtConfirmCompositionSurfaceIndependentFlipEntry(HANDLE surface, DWORD entry (0..3))
uint64_t svc_set_independent_flip(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  uint32_t info[4];
  if (!c.read_array(c.arg(1), info, 4)) return kStatusAccessViolation;
  for (uint32_t i = 0; i < 4; ++i) s->independent_flip[i] = info[i];
  s->stats_flags |= 0x80000000u;      // (the info was set)
  return kStatusSuccess;
}
uint64_t svc_confirm_independent_flip(Call& c, Ctx& x) {
  uint32_t status;
  Surface* s = surface_of(x, c.arg(0), kRightModify, &status);
  if (!s) return status;
  if (c.arg32(1) > 3 || !(s->stats_flags & 0x80000000u)) return kStatusInvalidDeviceState;
  s->independent_flip[c.arg32(1)] |= 0x80000000u;
  return kStatusSuccess;
}

// --- the surface's backing section and events (made when first asked for) --------------------------------------------------------------------------------------------------

// Makes the section and the two events of the surface behind `pin` if it has none yet. False (status set) on failure. The lock is given up meanwhile.
bool ensure_backing(Ctx& x, const SharedPointer<Object>& pin, uint32_t* status) {
  {
    Surface* s = static_cast<Surface*>(pin.get());
    if (s->section_size == 0) { *status = kStatusNotFound; return false; }
    if (s->section_id != 0) return true;
  }
  uint32_t section = 0;
  {
    Ctx::Unlocked u(x);
    section = x.st.objects->create_section(StringView(), static_cast<Surface*>(pin.get())->section_size, 4, 0x8000000u, nullptr, StringView()).id;
  }
  uint32_t ready = section ? make_event(x, true, false, 0) : 0, present = ready ? make_event(x, false, false, 0) : 0;
  Surface* s = static_cast<Surface*>(pin.get());
  if (!section || !ready || !present || s->section_id != 0) {
    const bool raced = s->section_id != 0 && section && ready && present;
    if (section) x.st.defer_event(section, false, true);
    if (ready) x.st.defer_event(ready, false, true);
    if (present) x.st.defer_event(present, false, true);
    if (raced) return true;
    *status = kStatusNoMemory;
    return false;
  }
  s->section_id = section;
  s->event_ready = ready;
  s->event_present = present;
  return true;
}

// Gives the caller a new handle to ObjectManager object `id` (the surface keeps its own reference), written to `out`.
bool give_object(Ctx& x, DescriptorKind kind, uint32_t id, uint64_t out, uint32_t* status) {
  { Ctx::Unlocked u(x); x.st.objects->add_ref(id); }
  uint64_t handle = 0;
  if (!give_descriptor(x.call, kind, id, &handle)) { x.st.defer_event(id, false, true); *status = kStatusNoMemory; return false; }
  if (!write_handle(x.call, out, handle)) { drop_descriptor(x, handle); *status = kStatusAccessViolation; return false; }
  return true;
}

// NtOpenCompositionSurfaceSectionInfo(HANDLE surface, PHANDLE section, UINT64* size)
uint64_t svc_open_section_info(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::Surface, kRightQuery, &status);
  if (!pin) return status;
  if (!ensure_backing(x, pin, &status)) return status;
  Surface* s = static_cast<Surface*>(pin.get());
  if (c.arg(2) != 0 && !c.write_u64(c.arg(2), s->section_size)) return kStatusAccessViolation;
  if (!give_object(x, DescriptorKind::Section, s->section_id, c.arg(1), &status)) return status;
  return kStatusSuccess;
}

// --- composition input sinks -----------------------------------------------------------------------------------------------------------------------------------------------

bool finite_bits(uint32_t bits) { return ((bits >> 23) & 0xFF) != 0xFF; }
constexpr uint32_t kIdentity[6] = {0x3F800000u, 0, 0, 0x3F800000u, 0, 0};      // 1 0 0 1 0 0 (float bits)

uint64_t make_sink(Call& c, Ctx& x, bool implicit) {
  uint32_t status;
  if (c.arg(0) == 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::InputSink) >= kMaxSinksPerProcess) return kStatusQuotaExceeded;
  SharedPointer<InputSinkObject> o = make_object<InputSinkObject>(x);
  if (!o) return kStatusNoMemory;
  o->owner = c.pid();
  o->implicit = implicit;
  o->luid = c.arg(1);
  o->view_id = c.arg32(2);
  o->queue = uint32_t(c.tid());
  for (uint32_t i = 0; i < 6; ++i) o->transform[i] = kIdentity[i];
  SharedPointer<Object> base = o;
  const uint64_t h = new_handle(x, base, kRightAll, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(0), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
// NtCreateCompositionInputSink(PHANDLE sink, UINT64 luid, DWORD view_id) / NtCreateImplicitCompositionInputSink(PHANDLE sink, UINT64 luid, DWORD view_id)
uint64_t svc_create_sink(Call& c, Ctx& x) { return make_sink(c, x, false); }
uint64_t svc_create_implicit_sink(Call& c, Ctx& x) { return make_sink(c, x, true); }
// NtCloseCompositionInputSink(HANDLE sink)
uint64_t svc_close_sink(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, 0, &status);
  if (!o) return status;
  x.st.handles.close(c.arg(0), c.pid());
  return kStatusSuccess;
}
// NtDuplicateCompositionInputSink(HANDLE sink, PHANDLE out): another handle to the same sink in the caller's table
uint64_t svc_duplicate_sink(Call& c, Ctx& x) {
  uint32_t status;
  if (!lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, 0, &status)) return status;
  const uint64_t h = x.st.handles.duplicate(c.arg(0), c.pid(), c.pid(), 0, true, &status);
  if (!h) return status;
  if (!write_handle(c, c.arg(1), h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}

struct SinkInfo { uint32_t size, view_id, implicit, queue; uint64_t luid; uint32_t drop_target, reserved; };
// NtQueryCompositionInputSink(HANDLE sink, SinkInfo* out, DWORD size)
uint64_t svc_query_sink(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightQuery, &status);
  if (!o) return status;
  if (c.arg32(2) != sizeof(SinkInfo)) return kStatusInfoLengthMismatch;
  const SinkInfo info = {sizeof(SinkInfo), o->view_id, o->implicit, o->queue, o->luid, o->drop_target, 0};
  return c.write_struct(c.arg(1), info) ? kStatusSuccess : kStatusAccessViolation;
}
// NtQueryCompositionInputIsImplicit(HANDLE sink, BOOLEAN* implicit)
uint64_t svc_query_implicit(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightQuery, &status);
  if (!o) return status;
  const uint8_t value = o->implicit ? 1 : 0;
  return c.write(c.arg(1), ConstByteView(&value, 1)) ? kStatusSuccess : kStatusAccessViolation;
}
// NtQueryCompositionInputSinkLuid(HANDLE sink, UINT64* luid) / NtQueryCompositionInputSinkViewId(HANDLE sink, DWORD* view_id)
uint64_t svc_query_luid(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightQuery, &status);
  if (!o) return status;
  return c.write_u64(c.arg(1), o->luid) ? kStatusSuccess : kStatusAccessViolation;
}
uint64_t svc_query_view_id(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightQuery, &status);
  if (!o) return status;
  return c.write_u32(c.arg(1), o->view_id) ? kStatusSuccess : kStatusAccessViolation;
}
// NtQueryCompositionInputQueueAndTransform(HANDLE sink, DWORD* queue, float* matrix (6 values))
uint64_t svc_query_queue_transform(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightQuery, &status);
  if (!o) return status;
  if (!c.write_u32(c.arg(1), o->queue) || !c.write_array(c.arg(2), o->transform, 6)) return kStatusAccessViolation;
  return kStatusSuccess;
}
// NtUpdateInputSinkTransforms(HANDLE sink, float* matrix (6 values))
uint64_t svc_update_transforms(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightModify, &status);
  if (!o) return status;
  uint32_t m[6];
  if (!c.read_array(c.arg(1), m, 6)) return kStatusAccessViolation;
  for (uint32_t v : m) if (!finite_bits(v)) return kStatusInvalidParameter;
  for (uint32_t i = 0; i < 6; ++i) o->transform[i] = m[i];
  return kStatusSuccess;
}
// NtCompositionSetDropTarget(HANDLE sink, DWORD hwnd (0: none))
uint64_t svc_set_drop_target(Call& c, Ctx& x) {
  uint32_t status;
  InputSinkObject* o = lookup<InputSinkObject>(x, c.arg(0), Kind::InputSink, kRightModify, &status);
  if (!o) return status;
  o->drop_target = c.arg32(1);
  return kStatusSuccess;
}

// --- token manager ---------------------------------------------------------------------------------------------------------------------------------------------------------

uint64_t give_token(Ctx& x, SharedPointer<Token> t, uint64_t out) {
  Call& c = x.call;
  if (out == 0) return kStatusInvalidParameter;
  if (x.st.handles.kind_count(c.pid(), Kind::Token) >= kMaxTokensPerProcess) return kStatusQuotaExceeded;
  t->owner = c.pid();
  uint32_t status;
  SharedPointer<Object> base = t;
  const uint64_t h = new_handle(x, base, kRightQuery | kStandardRead, &status);
  if (!h) return status;
  if (!write_handle(c, out, h)) { x.st.handles.close(h, c.pid()); return kStatusAccessViolation; }
  return kStatusSuccess;
}
uint64_t flip_token(Call& c, Ctx& x, Token::Type type) {
  uint32_t status;
  FlipEndpoint* e = lookup<FlipEndpoint>(x, c.arg(0), Kind::FlipEndpoint, kRightQuery, &status);
  if (!e) return status;
  if (e->disconnected) return kStatusInvalidDeviceState;
  SharedPointer<Token> t = make_object<Token>(x, type);
  if (!t) return kStatusNoMemory;
  t->flip = e->flip;
  return give_token(x, t, c.arg(1));
}
// NtTokenManagerCreateFlipObjectTokenHandle(HANDLE flip_endpoint, PHANDLE token): lets another process open the flip object
uint64_t svc_flip_token(Call& c, Ctx& x) { return flip_token(c, x, Token::Flip); }
// NtTokenManagerCreateFlipObjectReturnTokenHandle(HANDLE flip_endpoint, PHANDLE token)
uint64_t svc_flip_return_token(Call& c, Ctx& x) { return flip_token(c, x, Token::FlipReturn); }
// NtTokenManagerCreateCompositionTokenHandle(HANDLE surface, PHANDLE token)
uint64_t svc_composition_token(Call& c, Ctx& x) {
  uint32_t status;
  SharedPointer<Object> pin = x.st.handles.share(c.arg(0), c.pid(), Kind::Surface, kRightQuery, &status);
  if (!pin) return status;
  SharedPointer<Token> t = make_object<Token>(x, Token::Composition);
  if (!t) return kStatusNoMemory;
  t->surface = pin;
  return give_token(x, t, c.arg(1));
}
// NtTokenManagerOpenSectionAndEvents(HANDLE token, PHANDLE section, PHANDLE ready_event, PHANDLE present_event): handles to what backs the surface of a composition token
uint64_t svc_open_section_and_events(Call& c, Ctx& x) {
  uint32_t status;
  Token* t = lookup<Token>(x, c.arg(0), Kind::Token, kRightQuery, &status);
  if (!t) return status;
  if (t->type != Token::Composition || !t->surface) return kStatusObjectTypeMismatch;
  SharedPointer<Object> pin = t->surface;
  if (!ensure_backing(x, pin, &status)) return status;
  Surface* s = static_cast<Surface*>(pin.get());
  const uint32_t section = s->section_id, ready = s->event_ready, present = s->event_present;
  uint64_t handles[3] = {};
  const uint32_t ids[3] = {section, ready, present};
  const DescriptorKind kinds[3] = {DescriptorKind::Section, DescriptorKind::Event, DescriptorKind::Event};
  for (uint32_t i = 0; i < 3; ++i) {
    { Ctx::Unlocked u(x); x.st.objects->add_ref(ids[i]); }
    if (!give_descriptor(c, kinds[i], ids[i], &handles[i])) {
      x.st.defer_event(ids[i], false, true);
      for (uint32_t j = 0; j < i; ++j) drop_descriptor(x, handles[j]);
      return kStatusNoMemory;
    }
  }
  const uint64_t outs[3] = {c.arg(1), c.arg(2), c.arg(3)};
  bool ok = true;
  for (uint32_t i = 0; i < 3; ++i) ok = ok && write_handle(c, outs[i], handles[i]);
  if (!ok) {
    for (uint32_t j = 0; j < 3; ++j) drop_descriptor(x, handles[j]);
    return kStatusAccessViolation;
  }
  return kStatusSuccess;
}
// The analog-exclusive token path: this system has no analog output.
uint64_t svc_analog_unsupported(Call& c, Ctx& x) { return kStatusNotSupported; }

}  // namespace

void install_surface() {
  COMPOSITION_REGISTER(NtCreateCompositionSurfaceHandle, svc_create_surface);
  COMPOSITION_REGISTER(NtValidateCompositionSurfaceHandle, svc_validate_surface);
  COMPOSITION_REGISTER(NtBindCompositionSurface, svc_bind_surface);
  COMPOSITION_REGISTER(NtUnBindCompositionSurface, svc_unbind_surface);
  COMPOSITION_REGISTER(NtQueryCompositionSurfaceBinding, svc_query_binding);
  COMPOSITION_REGISTER(NtSetCompositionSurfaceStatistics, svc_set_statistics);
  COMPOSITION_REGISTER(NtQueryCompositionSurfaceStatistics, svc_query_statistics);
  COMPOSITION_REGISTER(NtQueryCompositionSurfaceFrameRate, svc_query_frame_rate);
  COMPOSITION_REGISTER(NtQueryCompositionSurfaceRenderingRealization, svc_query_realization);
  COMPOSITION_REGISTER(NtNotifyPresentToCompositionSurface, svc_notify_present);
  COMPOSITION_REGISTER(NtOpenCompositionSurfaceDirtyRegion, svc_open_dirty_region);
  COMPOSITION_REGISTER(NtOpenCompositionSurfaceRealizationInfo, svc_open_realization_info);
  COMPOSITION_REGISTER(NtOpenCompositionSurfaceSectionInfo, svc_open_section_info);
  COMPOSITION_REGISTER(NtSetCompositionSurfaceBufferUsage, svc_set_buffer_usage);
  COMPOSITION_REGISTER(NtSetCompositionSurfaceAnalogExclusive, svc_set_analog_exclusive);
  COMPOSITION_REGISTER(NtSetCompositionSurfaceDirectFlipState, svc_set_direct_flip);
  COMPOSITION_REGISTER(NtSetCompositionSurfaceIndependentFlipInfo, svc_set_independent_flip);
  COMPOSITION_REGISTER(NtConfirmCompositionSurfaceIndependentFlipEntry, svc_confirm_independent_flip);
  COMPOSITION_REGISTER(NtCreateCompositionInputSink, svc_create_sink);
  COMPOSITION_REGISTER(NtCreateImplicitCompositionInputSink, svc_create_implicit_sink);
  COMPOSITION_REGISTER(NtCloseCompositionInputSink, svc_close_sink);
  COMPOSITION_REGISTER(NtDuplicateCompositionInputSink, svc_duplicate_sink);
  COMPOSITION_REGISTER(NtQueryCompositionInputSink, svc_query_sink);
  COMPOSITION_REGISTER(NtQueryCompositionInputIsImplicit, svc_query_implicit);
  COMPOSITION_REGISTER(NtQueryCompositionInputSinkLuid, svc_query_luid);
  COMPOSITION_REGISTER(NtQueryCompositionInputSinkViewId, svc_query_view_id);
  COMPOSITION_REGISTER(NtQueryCompositionInputQueueAndTransform, svc_query_queue_transform);
  COMPOSITION_REGISTER(NtUpdateInputSinkTransforms, svc_update_transforms);
  COMPOSITION_REGISTER(NtCompositionSetDropTarget, svc_set_drop_target);
  COMPOSITION_REGISTER(NtTokenManagerCreateFlipObjectTokenHandle, svc_flip_token);
  COMPOSITION_REGISTER(NtTokenManagerCreateFlipObjectReturnTokenHandle, svc_flip_return_token);
  COMPOSITION_REGISTER(NtTokenManagerCreateCompositionTokenHandle, svc_composition_token);
  COMPOSITION_REGISTER(NtTokenManagerOpenSectionAndEvents, svc_open_section_and_events);
  COMPOSITION_REGISTER(NtTokenManagerGetAnalogExclusiveSurfaceUpdates, svc_analog_unsupported);
  COMPOSITION_REGISTER(NtTokenManagerGetAnalogExclusiveTokenEvent, svc_analog_unsupported);
  COMPOSITION_REGISTER(NtTokenManagerConfirmOutstandingAnalogToken, svc_analog_unsupported);
}

}  // namespace process::win32k::composition
