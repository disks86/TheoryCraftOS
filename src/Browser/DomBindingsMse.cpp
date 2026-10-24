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

// The natives under the Media Source Extensions: MediaSource, SourceBuffer and SourceBufferList are written in script (WebPrelude.cpp: the events, the
// updating flag, the errors the standard asks for); what they hold and do with the bytes is the engine in Media/Mse.hpp, which these functions reach by the
// numbers the script keeps (a MediaSource's and a SourceBuffer's). The element that plays it is Objects.hpp's.
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"
#include "Script/TypedArray.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

double nan_value() {
  const uint64_t bits = 0x7FF8000000000000ull;
  double d;
  __builtin_memcpy(&d, &bits, sizeof d);
  return d;
}

bool number_arg(Vm& vm, const Value* args, uint32_t argc, uint32_t i, double* out) { return vm.to_number(arg_at(args, argc, i), out); }

// Seconds as microseconds, kept inside what the engine counts in.
int64_t seconds_to_us(double seconds) {
  if (!(seconds == seconds)) return 0;
  if (seconds > 1e11) seconds = 1e11;
  if (seconds < -1e11) seconds = -1e11;
  return static_cast<int64_t>(seconds * 1e6 + (seconds < 0 ? -0.5 : 0.5));
}

uint32_t id_arg(Vm& vm, const Value* args, uint32_t argc, uint32_t i, bool* ok) {
  double d;
  if (!number_arg(vm, args, argc, i, &d)) {
    *ok = false;
    return 0;
  }
  return d >= 1 && d < 4294967295.0 ? static_cast<uint32_t>(d) : 0;
}

#define ID(i, name)                                   \
  bool name##_ok = true;                              \
  const uint32_t name = id_arg(vm, args, argc, i, &name##_ok); \
  if (!name##_ok) return false;

NATIVE(mse_supported) {
  (void)self;
  String type(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &type)) return false;
  *result = bool_value(media::mse_type_supported(type.view(), nullptr, nullptr));
  return true;
}

NATIVE(mse_create) {
  (void)self;
  (void)args;
  (void)argc;
  PageObjects::Lock lock(objs(vm));
  *result = num_value(objs(vm).mse().create_source());
  return true;
}

NATIVE(mse_drop) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  objs(vm).mse().drop_source(id);
  *result = Value();
  return true;
}

NATIVE(mse_register_url) {
  (void)self;
  String url(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  ID(1, id);
  PageObjects::Lock lock(objs(vm));
  *result = bool_value(id != 0 && objs(vm).mse().find(id) && objs(vm).mse().register_url(url.view(), id));
  return true;
}

NATIVE(mse_revoke_url) {
  (void)self;
  String url(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  PageObjects::Lock lock(objs(vm));
  objs(vm).mse().revoke_url(url.view());
  *result = Value();
  return true;
}

// 0 closed, 1 open, 2 ended; -1 for a MediaSource that is not there.
NATIVE(mse_state) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  media::MseSource* s = objs(vm).mse().find(id);
  *result = num_value(!s ? -1 : s->state == media::MseSource::State::Closed ? 0 : s->state == media::MseSource::State::Open ? 1 : 2);
  return true;
}

// A new SourceBuffer's number, or -1 (not open), -2 (type not supported), -3 (too many), -4 (out of memory).
NATIVE(mse_add_buffer) {
  (void)self;
  ID(0, id);
  String type(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &type)) return false;
  PageObjects::Lock lock(objs(vm));
  uint32_t buffer = 0;
  switch (objs(vm).mse().add_buffer(id, type.view(), &buffer)) {
    case media::MseHub::AddResult::Ok: *result = num_value(buffer); break;
    case media::MseHub::AddResult::InvalidState: *result = num_value(-1); break;
    case media::MseHub::AddResult::NotSupported: *result = num_value(-2); break;
    case media::MseHub::AddResult::Quota: *result = num_value(-3); break;
    default: *result = num_value(-4); break;
  }
  return true;
}

NATIVE(mse_remove_buffer) {
  (void)self;
  ID(0, id);
  ID(1, buffer);
  PageObjects::Lock lock(objs(vm));
  *result = bool_value(objs(vm).mse().remove_buffer(id, buffer));
  objs(vm).mark_dirty();
  return true;
}

// Appends bytes: 0 done, 1 no room (QuotaExceededError), 2 the bytes are not a valid stream (decode error), 3 out of memory, 4 the buffer is gone.
NATIVE(mse_append) {
  (void)self;
  ID(0, buffer);
  ArrayView<uint8_t> bytes;
  if (!typed_array_bytes(arg_at(args, argc, 1), &bytes)) return vm.throw_error(ErrorKind::TypeError, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
  PageObjects& o = objs(vm);
  PageObjects::Lock lock(o);
  media::MseSource* source = nullptr;
  media::MseBuffer* b = o.mse().find_buffer(buffer, &source);
  if (!b) {
    *result = num_value(4);
    return true;
  }
  if (o.mse().total_bytes() + bytes.size() > media::MseHub::kMaxTotalBytes) {
    *result = num_value(1);
    return true;
  }
  const media::MseAppend r = b->append(ConstByteView(bytes.data(), bytes.size()), o.mse_playhead(source->id));
  if (r == media::MseAppend::Ok) o.mse().appended(source);
  o.mark_dirty();
  *result = num_value(r == media::MseAppend::Ok ? 0 : r == media::MseAppend::Quota ? 1 : r == media::MseAppend::Decode ? 2 : 3);
  return true;
}

NATIVE(mse_abort) {
  (void)self;
  ID(0, buffer);
  PageObjects::Lock lock(objs(vm));
  if (media::MseBuffer* b = objs(vm).mse().find_buffer(buffer)) b->abort();
  *result = Value();
  return true;
}

NATIVE(mse_remove) {
  (void)self;
  ID(0, buffer);
  double start, end;
  if (!number_arg(vm, args, argc, 1, &start) || !number_arg(vm, args, argc, 2, &end)) return false;
  PageObjects::Lock lock(objs(vm));
  if (media::MseBuffer* b = objs(vm).mse().find_buffer(buffer)) b->remove(seconds_to_us(start), end > 1e11 ? INT64_MAX / 4 : seconds_to_us(end));
  objs(vm).mark_dirty();
  *result = Value();
  return true;
}

// The ranges of a buffer as a TimeRanges. (A source that has ended extends the last range of each buffer to the end of the longest.)
NATIVE(mse_buffered) {
  (void)self;
  ID(0, buffer);
  PageObjects& o = objs(vm);
  PageObjects::Lock lock(o);
  media::MseSource* source = nullptr;
  media::MseBuffer* b = o.mse().find_buffer(buffer, &source);
  List<media::MseBuffer::Range> ranges(vm.allocator());
  if (b) b->buffered(&ranges, source->state == media::MseSource::State::Ended ? static_cast<int64_t>(o.mse().highest_end(source->id) * 1e6 + 0.5) : -1);
  double pairs[PageObjects::VideoState::kMaxRanges * 2];
  uint32_t n = 0;
  for (const media::MseBuffer::Range& r : ranges.view()) {
    if (n >= PageObjects::VideoState::kMaxRanges) break;
    pairs[n * 2] = static_cast<double>(r.start) / 1e6;
    pairs[n * 2 + 1] = static_cast<double>(r.end) / 1e6;
    ++n;
  }
  *result = make_time_ranges(impl(vm), pairs, n);
  return true;
}

// Reads a setting: 0 mode (0 segments, 1 sequence), 1 timestampOffset, 2 appendWindowStart, 3 appendWindowEnd (seconds), 4 has an init segment arrived,
// 5 the number of tracks, 6 updating-relevant bytes held.
NATIVE(mse_get) {
  (void)self;
  ID(0, buffer);
  double what;
  if (!number_arg(vm, args, argc, 1, &what)) return false;
  PageObjects::Lock lock(objs(vm));
  media::MseBuffer* b = objs(vm).mse().find_buffer(buffer);
  if (!b) {
    *result = num_value(nan_value());
    return true;
  }
  switch (static_cast<int>(what)) {
    case 0: *result = num_value(b->mode() == media::MseBuffer::Mode::Sequence ? 1 : 0); break;
    case 1: *result = num_value(static_cast<double>(b->timestamp_offset()) / 1e6); break;
    case 2: *result = num_value(static_cast<double>(b->window_start()) / 1e6); break;
    case 3: *result = b->window_end() == INT64_MAX ? num_value(1e308 * 10) : num_value(static_cast<double>(b->window_end()) / 1e6); break;
    case 4: *result = bool_value(b->init_received()); break;
    case 5: *result = num_value(b->track_count()); break;
    case 6: *result = num_value(static_cast<double>(b->bytes())); break;
    default: *result = Value(); break;
  }
  return true;
}

NATIVE(mse_set) {
  (void)self;
  ID(0, buffer);
  double what, value;
  if (!number_arg(vm, args, argc, 1, &what) || !number_arg(vm, args, argc, 2, &value)) return false;
  PageObjects::Lock lock(objs(vm));
  media::MseBuffer* b = objs(vm).mse().find_buffer(buffer);
  bool ok = b != nullptr;
  if (b) {
    switch (static_cast<int>(what)) {
      case 0: ok = b->set_mode(value >= 1 ? media::MseBuffer::Mode::Sequence : media::MseBuffer::Mode::Segments); break;
      case 1: b->set_timestamp_offset(seconds_to_us(value)); break;
      case 2: b->set_window(seconds_to_us(value), b->window_end()); break;
      case 3: b->set_window(b->window_start(), value > 1e11 ? INT64_MAX : seconds_to_us(value)); break;
      default: ok = false; break;
    }
  }
  *result = bool_value(ok);
  return true;
}

// The tracks an initialization segment announced: kind (0 video, 1 audio) and id, one after the other.
NATIVE(mse_tracks) {
  (void)self;
  ID(0, buffer);
  PageObjects::Lock lock(objs(vm));
  JsObject* array = vm.new_array();
  if (!array) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  if (media::MseBuffer* b = objs(vm).mse().find_buffer(buffer)) {
    for (uint32_t i = 0; i < b->track_count(); ++i) {
      media::MseTrackKind kind;
      uint32_t id;
      if (!b->track_info(i, &kind, &id)) continue;
      vm.array_push(array, num_value(kind == media::MseTrackKind::Video ? 0 : 1));
      vm.array_push(array, num_value(id));
    }
  }
  *result = Value::make_object(array);
  return true;
}

// 0 done, -2 the type is not supported, -1 it is another kind of byte stream than the buffer reads.
NATIVE(mse_change_type) {
  (void)self;
  ID(0, buffer);
  String type(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &type)) return false;
  PageObjects::Lock lock(objs(vm));
  media::MseFormat format;
  if (!media::mse_type_supported(type.view(), &format, nullptr)) {
    *result = num_value(-2);
    return true;
  }
  media::MseBuffer* b = objs(vm).mse().find_buffer(buffer);
  *result = num_value(b && b->change_format(format) ? 0 : -1);
  return true;
}

NATIVE(mse_duration) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  media::MseSource* s = objs(vm).mse().find(id);
  *result = num_value(s && s->state != media::MseSource::State::Closed && s->duration_set ? s->duration : nan_value());
  return true;
}

NATIVE(mse_set_duration) {
  (void)self;
  ID(0, id);
  double seconds;
  if (!number_arg(vm, args, argc, 1, &seconds)) return false;
  PageObjects::Lock lock(objs(vm));
  const bool ok = objs(vm).mse().set_duration(id, seconds);
  *result = bool_value(ok);
  if (ok) {
    // a shorter duration takes what is past it out of the buffers
    if (media::MseSource* s = objs(vm).mse().find(id))
      for (uint64_t i = 0; i < s->buffers.count(); ++i) s->buffers[i]->remove(seconds_to_us(seconds), INT64_MAX / 4);
    objs(vm).mark_dirty();
  }
  return true;
}

NATIVE(mse_highest_end) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  *result = num_value(objs(vm).mse().highest_end(id));
  return true;
}

NATIVE(mse_end_of_stream) {
  (void)self;
  ID(0, id);
  double error = 0;
  if (argc > 1 && !number_arg(vm, args, argc, 1, &error)) return false;
  PageObjects::Lock lock(objs(vm));
  objs(vm).mse().end_of_stream(id, static_cast<uint8_t>(error));
  objs(vm).mark_dirty();
  *result = Value();
  return true;
}

NATIVE(mse_reopen) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  objs(vm).mse().reopen(id);
  *result = Value();
  return true;
}

// setLiveSeekableRange(start, end); clearLiveSeekableRange() is a NaN start.
NATIVE(mse_live_range) {
  (void)self;
  ID(0, id);
  double start, end = 0;
  if (!number_arg(vm, args, argc, 1, &start) || !number_arg(vm, args, argc, 2, &end)) return false;
  PageObjects::Lock lock(objs(vm));
  if (media::MseSource* s = objs(vm).mse().find(id)) {
    s->live_range = start == start;
    s->live_start = start;
    s->live_end = end;
  }
  *result = Value();
  return true;
}

NATIVE(mse_source_error) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  media::MseSource* s = objs(vm).mse().find(id);
  *result = num_value(s ? s->error : 0);
  return true;
}

// --- Encrypted Media Extensions: the keys, and the init data of "encrypted" events -------------------------------------------------------------

// A new key table for a MediaKeys object (its number, 0 if there are too many).
NATIVE(eme_create_keys) {
  (void)self;
  (void)args;
  (void)argc;
  PageObjects::Lock lock(objs(vm));
  *result = num_value(objs(vm).eme().create());
  return true;
}

// Gives a session's key to the table: (keys, session, key id, key); false if the identifier or the key is not 16 bytes.
NATIVE(eme_add_key) {
  (void)self;
  ID(0, keys);
  ID(1, session);
  ArrayView<uint8_t> kid, key;
  if (!typed_array_bytes(arg_at(args, argc, 2), &kid) || !typed_array_bytes(arg_at(args, argc, 3), &key)) return vm.throw_error(ErrorKind::TypeError, "The key and its identifier have to be byte arrays.");
  PageObjects::Lock lock(objs(vm));
  media::ClearKeyStore* store = objs(vm).eme().find(keys);
  const bool added = store && kid.size() == 16 && key.size() == 16 && store->add_key(session, kid.data(), key.data());
  *result = bool_value(added);
  if (added) objs(vm).mark_dirty();
  return true;
}

NATIVE(eme_remove_session) {
  (void)self;
  ID(0, keys);
  ID(1, session);
  PageObjects::Lock lock(objs(vm));
  if (media::ClearKeyStore* store = objs(vm).eme().find(keys)) store->remove_session(session);
  objs(vm).mark_dirty();
  *result = Value();
  return true;
}

// setMediaKeys(): the element decrypts with that table (0: with none).
NATIVE(eme_set_keys) {
  (void)self;
  dom::Node* element = node_of(arg_at(args, argc, 0));
  if (!element) return vm.throw_error(ErrorKind::TypeError, "The first argument is not an element.");
  double id = 0;
  if (!number_arg(vm, args, argc, 1, &id)) return false;
  PageObjects::Lock lock(objs(vm));
  const uint32_t keys = id >= 1 && id < 4294967295.0 ? static_cast<uint32_t>(id) : 0;
  const bool known = keys == 0 || objs(vm).eme().find(keys) != nullptr;
  *result = bool_value(known);
  if (known) objs(vm).video_set_keys(element->serial, keys);
  return true;
}

// The init data found in the stream of a SourceBuffer since the last call, for "encrypted" events: type (0 "cenc", 1 "keyids") and a byte array, one after the other.
NATIVE(mse_encrypted) {
  (void)self;
  ID(0, buffer);
  PageObjects::Lock lock(objs(vm));
  JsObject* array = vm.new_array();
  if (!array) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  if (media::MseBuffer* b = objs(vm).mse().find_buffer(buffer)) {
    UniquePointer<media::MseBuffer::EncryptedEvent> e;
    while (b->take_encrypted(&e)) {
      const ConstByteView data = e->data.view();
      JsObject* bytes = make_byte_array(impl(vm), StringView(reinterpret_cast<const char*>(data.data()), data.size()));
      if (!bytes) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
      vm.array_push(array, num_value(e->keyids ? 1 : 0));
      vm.array_push(array, Value::make_object(bytes));
    }
  }
  *result = Value::make_object(array);
  return true;
}

// The element a MediaSource plays in (null if it is attached to none).
NATIVE(mse_element) {
  (void)self;
  ID(0, id);
  PageObjects::Lock lock(objs(vm));
  media::MseSource* s = objs(vm).mse().find(id);
  dom::Node* node = s && s->element ? impl(vm).doc.by_serial(s->element) : nullptr;
  *result = node_value(impl(vm), node);
  return true;
}

}  // namespace

bool install_mse(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__mse_supported", mse_supported, 1);
  vm.define_native(g, "__mse_create", mse_create, 0);
  vm.define_native(g, "__mse_drop", mse_drop, 1);
  vm.define_native(g, "__mse_register_url", mse_register_url, 2);
  vm.define_native(g, "__mse_revoke_url", mse_revoke_url, 1);
  vm.define_native(g, "__mse_state", mse_state, 1);
  vm.define_native(g, "__mse_add_buffer", mse_add_buffer, 2);
  vm.define_native(g, "__mse_remove_buffer", mse_remove_buffer, 2);
  vm.define_native(g, "__mse_append", mse_append, 2);
  vm.define_native(g, "__mse_abort", mse_abort, 1);
  vm.define_native(g, "__mse_remove", mse_remove, 3);
  vm.define_native(g, "__mse_buffered", mse_buffered, 1);
  vm.define_native(g, "__mse_get", mse_get, 2);
  vm.define_native(g, "__mse_set", mse_set, 3);
  vm.define_native(g, "__mse_tracks", mse_tracks, 1);
  vm.define_native(g, "__mse_change_type", mse_change_type, 2);
  vm.define_native(g, "__mse_duration", mse_duration, 1);
  vm.define_native(g, "__mse_set_duration", mse_set_duration, 2);
  vm.define_native(g, "__mse_highest_end", mse_highest_end, 1);
  vm.define_native(g, "__mse_end_of_stream", mse_end_of_stream, 2);
  vm.define_native(g, "__mse_reopen", mse_reopen, 1);
  vm.define_native(g, "__mse_live_range", mse_live_range, 3);
  vm.define_native(g, "__mse_source_error", mse_source_error, 1);
  vm.define_native(g, "__mse_encrypted", mse_encrypted, 1);
  vm.define_native(g, "__mse_element", mse_element, 1);
  vm.define_native(g, "__eme_create_keys", eme_create_keys, 0);
  vm.define_native(g, "__eme_add_key", eme_add_key, 4);
  vm.define_native(g, "__eme_remove_session", eme_remove_session, 2);
  vm.define_native(g, "__eme_set_keys", eme_set_keys, 2);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
