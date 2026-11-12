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

// The real libpipewire against the kernel's PipeWire (tests/pipewire_emulated.sh, built with the development machine's libpipewire and run on the
// kernel's CPU emulator through the OS's Linux system calls). One mode a run:
//   video        a camera (pw_stream Video/Source, PW_STREAM_FLAG_DRIVER: its own timer runs its cycles, SPA_META_Header on every buffer) and a watcher
//                (pw_stream video input, target.object the camera) in one program: the watcher must see the camera's frames whole and in order;
//   video-alloc  the same with the camera allocating its own buffers (PW_STREAM_FLAG_ALLOC_BUFFERS: memfds it makes, given to the server with
//                port_buffers);
//   midi         a pw_filter MIDI output (8 bit raw midi) playing 16 notes and a pw_stream MIDI input: the input must hear them through the server's MIDI
//                bus (the emulator counts what reached its synthesizer);
//   adapter      a virtual sink made with the adapter factory (support.null-audio-sink), a stream (PW_STREAM_FLAG_DRIVER: an audio driver, which the
//                server makes follow its clock) playing 500 Hz into it and a stream recording from it: the recording must hold the tone.
// Prints "pwmedia: <mode> ok ..." or "pwmedia: <mode> FAILED ...", and ends with 0 when it passed.
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>

#define W 64
#define H 48
#define CAMERA_FRAMES 60
#define WANT_FRAMES 15
#define NOTES 16

struct data {
  const char* mode;
  struct pw_main_loop* loop;
  struct pw_context* context;
  struct pw_core* core;
  struct pw_stream* camera;
  struct pw_stream* watcher;
  struct spa_hook camera_listener, watcher_listener;
  struct spa_source* timer;
  struct spa_source* deadline;
  int alloc;
  uint32_t produced;
  uint32_t seen, bad, last_frame;
  int camera_stride;
  // midi
  struct pw_filter* filter;
  void* midi_port;
  struct pw_stream* midi_in;
  struct spa_hook midi_in_listener;
  uint32_t notes_sent, notes_heard;
  uint32_t heard_mask;
  // adapter
  struct pw_proxy* sink;
  struct spa_hook sink_listener;
  uint32_t sink_id;
  struct pw_stream* player;
  struct pw_stream* recorder;
  struct spa_hook player_listener, recorder_listener;
  uint64_t played, recorded;
  float* capture;
  uint32_t captured;
  int result;
};

static void finish(struct data* d, int ok, const char* what) {
  if (d->result == -1) {
    d->result = ok ? 0 : 1;
    printf("pwmedia: %s %s %s\n", d->mode, ok ? "ok" : "FAILED", what);
    fflush(stdout);
  }
  pw_main_loop_quit(d->loop);
}

static void on_deadline(void* userdata, uint64_t expirations) {
  struct data* d = userdata;
  (void)expirations;
  char what[160];
  snprintf(what, sizeof what, "(timed out: produced %u, seen %u, bad %u, notes sent %u heard %u, played %llu recorded %llu captured %u)", d->produced, d->seen,
           d->bad, d->notes_sent, d->notes_heard, (unsigned long long)d->played, (unsigned long long)d->recorded, d->captured);
  finish(d, 0, what);
}

// ---- video -----------------------------------------------------------------------------------------------------------------------------------------

static void camera_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
  struct data* d = userdata;
  if (param == NULL || id != SPA_PARAM_Format) return;
  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) < 0) return;
  d->camera_stride = SPA_ROUND_UP_N(info.size.width * 4, 4);
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[2];
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int(d->camera_stride * (int)info.size.height),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(d->camera_stride),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(d->alloc ? (1 << SPA_DATA_MemFd) : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
  params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
  pw_stream_update_params(d->camera, params, 2);
}

// (video-alloc) the camera's own buffers: a memfd each, mapped here
static void camera_add_buffer(void* userdata, struct pw_buffer* buffer) {
  struct data* d = userdata;
  if (!d->alloc) return;
  struct spa_data* sd = &buffer->buffer->datas[0];
  if ((sd->type & (1 << SPA_DATA_MemFd)) == 0) {
    fprintf(stderr, "pwmedia: the server did not let the camera use memfds (types %x)\n", sd->type);
    return;
  }
  const size_t size = (size_t)d->camera_stride * H;
  int fd = memfd_create("pwmedia-camera", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
    fprintf(stderr, "pwmedia: memfd: %m\n");
    return;
  }
  void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "pwmedia: mmap: %m\n");
    close(fd);
    return;
  }
  sd->type = SPA_DATA_MemFd;
  sd->flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
  sd->fd = fd;
  sd->mapoffset = 0;
  sd->maxsize = (uint32_t)size;
  sd->data = mem;
}

static void camera_remove_buffer(void* userdata, struct pw_buffer* buffer) {
  struct data* d = userdata;
  if (!d->alloc) return;
  struct spa_data* sd = &buffer->buffer->datas[0];
  if (sd->data) munmap(sd->data, sd->maxsize);
  if (sd->fd >= 0) close((int)sd->fd);
}

// a frame: blue = the frame's number, green = the row, red = the column
static void camera_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b = pw_stream_dequeue_buffer(d->camera);
  if (b == NULL) return;
  struct spa_buffer* buf = b->buffer;
  uint8_t* p = buf->datas[0].data;
  if (p != NULL && d->produced < CAMERA_FRAMES) {
    const uint32_t frame = ++d->produced;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t* px = p + y * d->camera_stride + x * 4;
        px[0] = (uint8_t)frame;
        px[1] = (uint8_t)y;
        px[2] = (uint8_t)x;
        px[3] = 0xFF;
      }
    struct spa_meta_header* h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof *h);
    if (h) {
      h->pts = (int64_t)frame * 40000000;
      h->flags = 0;
      h->seq = frame;
      h->dts_offset = 0;
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = (uint32_t)(d->camera_stride * H);
    buf->datas[0].chunk->stride = d->camera_stride;
  }
  pw_stream_queue_buffer(d->camera, b);
}

static void camera_state(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
  struct data* d = userdata;
  (void)old;
  if (state == PW_STREAM_STATE_ERROR) {
    char what[200];
    snprintf(what, sizeof what, "(the camera: %s)", error ? error : "error");
    finish(d, 0, what);
  }
}

static const struct pw_stream_events camera_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = camera_state,
    .param_changed = camera_param_changed,
    .add_buffer = camera_add_buffer,
    .remove_buffer = camera_remove_buffer,
    .process = camera_process,
};

// the camera's beat: it drives itself (25 frames a second)
static void on_timer(void* userdata, uint64_t expirations) {
  struct data* d = userdata;
  (void)expirations;
  if (d->camera && pw_stream_is_driving(d->camera)) pw_stream_trigger_process(d->camera);
}

static void watcher_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
  struct data* d = userdata;
  if (param == NULL || id != SPA_PARAM_Format) return;
  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) < 0) return;
  if (info.format != SPA_VIDEO_FORMAT_BGRA || info.size.width != W || info.size.height != H) {
    char what[128];
    snprintf(what, sizeof what, "(the watcher was offered format %u %ux%u)", info.format, info.size.width, info.size.height);
    finish(d, 0, what);
    return;
  }
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[2];
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 8),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int(W * 4 * H),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(W * 4));
  params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
  pw_stream_update_params(d->watcher, params, 2);
}

static void watcher_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b;
  while ((b = pw_stream_dequeue_buffer(d->watcher)) != NULL) {
    struct spa_buffer* buf = b->buffer;
    const uint8_t* p = buf->datas[0].data;
    const struct spa_chunk* chunk = buf->datas[0].chunk;
    if (p && chunk->size >= (uint32_t)(W * 4 * H)) {
      const int stride = chunk->stride > 0 ? chunk->stride : W * 4;
      const uint8_t frame = p[chunk->offset];
      int whole = 1;
      for (int y = 0; y < H && whole; y += 7)
        for (int x = 0; x < W; x += 5) {
          const uint8_t* px = p + chunk->offset + y * stride + x * 4;
          if (px[0] != frame || px[1] != (uint8_t)y || px[2] != (uint8_t)x || px[3] != 0xFF) { whole = 0; break; }
        }
      if (!whole || (d->seen && frame <= (uint8_t)d->last_frame)) ++d->bad;
      else {
        ++d->seen;
        d->last_frame = frame;
      }
      struct spa_meta_header* h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof *h);
      (void)h;
    }
    pw_stream_queue_buffer(d->watcher, b);
  }
  if (d->seen >= WANT_FRAMES) {
    char what[160];
    snprintf(what, sizeof what, "(%u frames watched whole and in order, %u produced, %u bad; %s buffers)", d->seen, d->produced, d->bad,
             d->alloc ? "the camera's own" : "the server's");
    finish(d, d->bad == 0, what);
  }
}

static const struct pw_stream_events watcher_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = watcher_param_changed,
    .process = watcher_process,
};

static int run_video(struct data* d) {
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[1];
  d->camera = pw_stream_new(d->core, "tcos-camera",
                            pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_NAME, "tcos-camera", PW_KEY_NODE_SUPPORTS_REQUEST, "1", NULL));
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(W, H), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(25, 1)));
  pw_stream_add_listener(d->camera, &d->camera_listener, &camera_events, d);
  if (pw_stream_connect(d->camera, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS | (d->alloc ? PW_STREAM_FLAG_ALLOC_BUFFERS : 0), params, 1) < 0)
    return -1;
  // the watcher: what the camera shows, in any of a few formats and sizes (the server offers the camera's)
  spa_pod_builder_init(&b, buffer, sizeof buffer);
  d->watcher = pw_stream_new(d->core, "tcos-watcher",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_TARGET_OBJECT, "tcos-camera", NULL));
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(320, 240), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(25, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
  pw_stream_add_listener(d->watcher, &d->watcher_listener, &watcher_events, d);
  if (pw_stream_connect(d->watcher, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) return -1;
  d->timer = pw_loop_add_timer(pw_main_loop_get_loop(d->loop), on_timer, d);
  struct timespec start = {0, 40000000}, every = {0, 40000000};
  pw_loop_update_timer(pw_main_loop_get_loop(d->loop), d->timer, &start, &every, false);
  return 0;
}

// ---- MIDI ------------------------------------------------------------------------------------------------------------------------------------------

static void midi_out_process(void* userdata, struct spa_io_position* position) {
  struct data* d = userdata;
  (void)position;
  struct pw_buffer* b = pw_filter_dequeue_buffer(d->midi_port);
  if (b == NULL) return;
  struct spa_data* sd = &b->buffer->datas[0];
  if (sd->data == NULL) {
    pw_filter_queue_buffer(d->midi_port, b);
    return;
  }
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(sd->data, sd->maxsize);
  struct spa_pod_frame f;
  spa_pod_builder_push_sequence(&builder, &f, 0);
  // a note a cycle: on, and the one before off
  if (d->notes_sent < NOTES) {
    uint8_t on[3] = {0x90, (uint8_t)(60 + d->notes_sent), 100};
    spa_pod_builder_control(&builder, 0, SPA_CONTROL_Midi);
    spa_pod_builder_bytes(&builder, on, 3);
    if (d->notes_sent > 0) {
      uint8_t off[3] = {0x80, (uint8_t)(59 + d->notes_sent), 0};
      spa_pod_builder_control(&builder, 10, SPA_CONTROL_Midi);
      spa_pod_builder_bytes(&builder, off, 3);
    }
    ++d->notes_sent;
  }
  spa_pod_builder_pop(&builder, &f);
  sd->chunk->offset = 0;
  sd->chunk->size = builder.state.offset;
  sd->chunk->stride = 1;
  pw_filter_queue_buffer(d->midi_port, b);
}

static const struct pw_filter_events midi_out_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = midi_out_process,
};

static void midi_heard(struct data* d, uint8_t status, uint8_t key) {
  if ((status & 0xF0) == 0x90 && key >= 60 && key < 60 + NOTES && !(d->heard_mask & (1u << (key - 60)))) {
    d->heard_mask |= 1u << (key - 60);
    ++d->notes_heard;
  }
}

static void midi_in_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b;
  while ((b = pw_stream_dequeue_buffer(d->midi_in)) != NULL) {
    struct spa_data* sd = &b->buffer->datas[0];
    if (sd->data && sd->chunk->size >= sizeof(struct spa_pod_sequence)) {
      struct spa_pod* pod = spa_pod_from_data(sd->data, sd->maxsize, sd->chunk->offset, sd->chunk->size);
      if (pod && spa_pod_is_sequence(pod)) {
        struct spa_pod_control* c;
        SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
          const uint8_t* bytes = SPA_POD_BODY(&c->value);
          const uint32_t size = SPA_POD_BODY_SIZE(&c->value);
          if (c->type == SPA_CONTROL_Midi && size >= 2) midi_heard(d, bytes[0], bytes[1]);
          if (c->type == SPA_CONTROL_UMP && size >= 4) {
            uint32_t w;
            memcpy(&w, bytes, 4);
            if ((w >> 28) == 2 || (w >> 28) == 4) midi_heard(d, (uint8_t)(w >> 16), (uint8_t)(w >> 8) & 0x7F);
          }
        }
      }
    }
    pw_stream_queue_buffer(d->midi_in, b);
  }
  if (d->notes_heard == NOTES) {
    char what[96];
    snprintf(what, sizeof what, "(%u notes played by a pw_filter, all %u heard by a pw_stream)", d->notes_sent, d->notes_heard);
    finish(d, 1, what);
  }
}

static const struct pw_stream_events midi_in_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = midi_in_process,
};

static int run_midi(struct data* d) {
  uint8_t buffer[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[1];
  // the listener first, so it is there when the notes come
  d->midi_in = pw_stream_new(d->core, "tcos-midi-in", pw_properties_new(PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Capture", NULL));
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
  pw_stream_add_listener(d->midi_in, &d->midi_in_listener, &midi_in_events, d);
  if (pw_stream_connect(d->midi_in, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) return -1;
  d->filter = pw_filter_new_simple(pw_main_loop_get_loop(d->loop), "tcos-midi-out",
                                   pw_properties_new(PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_CLASS, "Midi/Source", NULL),
                                   &midi_out_events, d);
  d->midi_port = pw_filter_add_port(d->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                    pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "output", NULL), NULL, 0);
  if (d->midi_port == NULL || pw_filter_connect(d->filter, 0, NULL, 0) < 0) return -1;
  return 0;
}

// ---- the adapter: a virtual sink -------------------------------------------------------------------------------------------------------------------

static void player_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b = pw_stream_dequeue_buffer(d->player);
  if (b == NULL) return;
  struct spa_data* sd = &b->buffer->datas[0];
  float* out = sd->data;
  if (out) {
    uint32_t frames = sd->maxsize / 8;
    if (b->requested && b->requested < frames) frames = (uint32_t)b->requested;
    for (uint32_t i = 0; i < frames; ++i) {
      const float v = 0.4f * sinf(2.0f * (float)M_PI * 500.0f * (float)(d->played + i) / 48000.0f);
      out[2 * i] = v;
      out[2 * i + 1] = -v;
    }
    d->played += frames;
    sd->chunk->offset = 0;
    sd->chunk->size = frames * 8;
    sd->chunk->stride = 8;
  }
  pw_stream_queue_buffer(d->player, b);
}

static const struct pw_stream_events player_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = player_process,
};

static double goertzel(const float* s, uint32_t n, uint32_t step, double f) {
  double s1 = 0, s2 = 0;
  const double c = 2 * cos(2 * M_PI * f / 48000.0);
  for (uint32_t i = 0; i < n; ++i) {
    const double s0 = s[i * step] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return sqrt(p > 0 ? p : 0) * 2 / n;
}

#define CAPTURE (48000 / 2)
static void recorder_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b;
  while ((b = pw_stream_dequeue_buffer(d->recorder)) != NULL) {
    struct spa_data* sd = &b->buffer->datas[0];
    const float* in = sd->data ? (const float*)((const uint8_t*)sd->data + sd->chunk->offset) : NULL;
    const uint32_t frames = sd->chunk->size / 8;
    d->recorded += frames;
    // (after the first half second: the sink's start and the player's)
    for (uint32_t i = 0; in && i < frames && d->recorded > 24000 && d->captured < CAPTURE; ++i) {
      d->capture[2 * d->captured] = in[2 * i];
      d->capture[2 * d->captured + 1] = in[2 * i + 1];
      ++d->captured;
    }
    pw_stream_queue_buffer(d->recorder, b);
  }
  if (d->captured == CAPTURE) {
    const double left = goertzel(d->capture, 4800, 2, 500), right = goertzel(d->capture + 1, 4800, 2, 500), other = goertzel(d->capture, 4800, 2, 1300);
    char what[200];
    snprintf(what, sizeof what, "(a stream played 500 Hz into the virtual sink %u, another recorded it: left %.3f, right %.3f, 1300 Hz %.3f; driving %d)",
             d->sink_id, left, right, other, pw_stream_is_driving(d->player));
    finish(d, left > 0.3 && left < 0.5 && right > 0.3 && right < 0.5 && other < 0.05, what);
  }
}

static const struct pw_stream_events recorder_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = recorder_process,
};

static void sink_bound(void* userdata, uint32_t global_id) {
  struct data* d = userdata;
  d->sink_id = global_id;
  // the sink is there: a stream plays into it (an audio driver, which follows the server's clock), another records from it
  uint8_t buffer[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[1];
  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2);
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  d->player = pw_stream_new(d->core, "tcos-into-null", pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_TARGET_OBJECT,
                                                                          "tcos-null", NULL));
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  pw_stream_add_listener(d->player, &d->player_listener, &player_events, d);
  pw_stream_connect(d->player, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DRIVER, params, 1);
  spa_pod_builder_init(&b, buffer, sizeof buffer);
  d->recorder = pw_stream_new(d->core, "tcos-from-null", pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_TARGET_OBJECT,
                                                                            "tcos-null", PW_KEY_STREAM_CAPTURE_SINK, "true", NULL));
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  pw_stream_add_listener(d->recorder, &d->recorder_listener, &recorder_events, d);
  pw_stream_connect(d->recorder, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
}

static const struct pw_proxy_events sink_events = {
    PW_VERSION_PROXY_EVENTS,
    .bound = sink_bound,
};

static int run_adapter(struct data* d) {
  d->capture = calloc(CAPTURE * 2, sizeof(float));
  struct pw_properties* props = pw_properties_new("factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, "tcos-null", PW_KEY_MEDIA_CLASS, "Audio/Sink",
                                                  "audio.position", "FL,FR", "audio.rate", "48000", "object.linger", "false", NULL);
  d->sink = pw_core_create_object(d->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
  pw_properties_free(props);
  if (d->sink == NULL) return -1;
  pw_proxy_add_listener(d->sink, &d->sink_listener, &sink_events, d);
  return 0;
}

int main(int argc, char** argv) {
  struct data d;
  memset(&d, 0, sizeof d);
  d.result = -1;
  d.mode = argc > 1 ? argv[1] : "video";
  d.alloc = strcmp(d.mode, "video-alloc") == 0;
  pw_init(&argc, &argv);
  d.loop = pw_main_loop_new(NULL);
  d.context = pw_context_new(pw_main_loop_get_loop(d.loop), NULL, 0);
  d.core = pw_context_connect(d.context, NULL, 0);
  if (d.core == NULL) {
    printf("pwmedia: %s FAILED (no connection: %m)\n", d.mode);
    return 1;
  }
  int r = -1;
  if (strncmp(d.mode, "video", 5) == 0) r = run_video(&d);
  else if (strcmp(d.mode, "midi") == 0) r = run_midi(&d);
  else if (strcmp(d.mode, "adapter") == 0) r = run_adapter(&d);
  if (r < 0) {
    printf("pwmedia: %s FAILED (could not set up)\n", d.mode);
    return 1;
  }
  d.deadline = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), on_deadline, &d);
  struct timespec limit = {60, 0}, none = {0, 0};
  pw_loop_update_timer(pw_main_loop_get_loop(d.loop), d.deadline, &limit, &none, false);
  pw_main_loop_run(d.loop);
  if (d.camera) pw_stream_destroy(d.camera);
  if (d.watcher) pw_stream_destroy(d.watcher);
  if (d.midi_in) pw_stream_destroy(d.midi_in);
  if (d.filter) pw_filter_destroy(d.filter);
  if (d.player) pw_stream_destroy(d.player);
  if (d.recorder) pw_stream_destroy(d.recorder);
  if (d.sink) pw_proxy_destroy(d.sink);
  pw_core_disconnect(d.core);
  pw_context_destroy(d.context);
  pw_main_loop_destroy(d.loop);
  free(d.capture);
  pw_deinit();
  return d.result == 0 ? 0 : 1;
}
