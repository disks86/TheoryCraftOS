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
//   video-yuv    the camera in NV12 with a data block a plane, the watcher with the planes one after another in one block; once the watcher has seen
//                some frames the camera says it is I420 at another size now (pw_stream_update_params): both are negotiated again while streaming, and
//                the watcher must see the I420 frames too;
//   midi         a pw_filter MIDI output (8 bit raw midi) playing 16 notes (on at the cycle's start, the last one's off half way through) and a 22-byte
//                system exclusive message, and a pw_stream MIDI input: the input must hear the notes and the sysex whole through the server's MIDI bus,
//                its controls in order, some past the cycle's start (the emulator counts what reached its synthesizer);
//   filter       an effect: a pw_filter (Audio/Filter) with two float inputs and two float outputs, one node that records and plays at once: its
//                inputs must hear the microphone (880 Hz left, 1320 Hz right), its outputs play a fiftieth of what came in (quiet: the interop test's
//                check of the mixed output looks for loud parts);
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
  // video-yuv
  int yuv;
  uint32_t cam_format, cam_w, cam_h;      // the camera's Format now
  uint32_t watch_format, watch_w, watch_h;
  uint32_t nv12_seen, i420_seen;
  int switch_now, switched;
  // midi
  struct pw_filter* filter;
  void* midi_port;
  struct pw_stream* midi_in;
  struct spa_hook midi_in_listener;
  uint32_t notes_sent, notes_heard;
  uint32_t heard_mask;
  int sysex_sent, sysex_heard, offsets_bad, offsets_later;
  uint8_t ump_sysex[64];
  uint32_t ump_sysex_len;
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
  // filter
  struct pw_filter* effect;
  void* fx_in[2];
  void* fx_out[2];
  float* fx_capture;
  uint32_t fx_frames, fx_cycles;
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
  d->cam_format = info.format;
  d->cam_w = info.size.width;
  d->cam_h = info.size.height;
  d->camera_stride = SPA_ROUND_UP_N(info.size.width * 4, 4);
  int blocks = 1, size = d->camera_stride * (int)info.size.height;
  if (d->yuv) {
    // a block a plane, each the luma's size at most; the luma's stride
    d->camera_stride = (int)info.size.width;
    blocks = info.format == SPA_VIDEO_FORMAT_NV12 ? 2 : 3;
    size = d->camera_stride * (int)info.size.height;
  }
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const struct spa_pod* params[2];
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
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

// (video-yuv) a frame's planes: luma row y = frame + y, chroma row y = frame * 3 + y
static void camera_yuv(struct data* d, struct spa_buffer* buf, uint32_t frame) {
  const uint32_t w = d->cam_w, h = d->cam_h, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const int nv12 = d->cam_format == SPA_VIDEO_FORMAT_NV12;
  const uint32_t planes = nv12 ? 2 : 3;
  if (buf->n_datas < planes) return;
  for (uint32_t k = 0; k < planes; ++k) {
    uint8_t* p = buf->datas[k].data;
    if (p == NULL) return;
    const uint32_t row = k == 0 ? w : nv12 ? cw * 2 : cw, rows = k == 0 ? h : ch;
    if (buf->datas[k].maxsize < row * rows) return;
    for (uint32_t y = 0; y < rows; ++y) memset(p + y * row, (int)((k == 0 ? frame : frame * 3) + y) & 0xFF, row);
    buf->datas[k].chunk->offset = 0;
    buf->datas[k].chunk->size = row * rows;
    buf->datas[k].chunk->stride = (int32_t)row;
  }
}

// a frame: blue = the frame's number, green = the row, red = the column
static void camera_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b = pw_stream_dequeue_buffer(d->camera);
  if (b == NULL) return;
  struct spa_buffer* buf = b->buffer;
  if (d->yuv) {
    if (d->produced < 4 * CAMERA_FRAMES) camera_yuv(d, buf, ++d->produced);
    pw_stream_queue_buffer(d->camera, b);
    return;
  }
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
  if (d->switch_now && !d->switched) {
    // (video-yuv) the camera says another format: I420 at 40 x 30
    d->switched = 1;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_I420),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(40, 30)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(25, 1)));
    pw_stream_update_params(d->camera, params, 1);
  }
  if (d->camera && pw_stream_is_driving(d->camera)) pw_stream_trigger_process(d->camera);
}

static void watcher_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
  struct data* d = userdata;
  if (param == NULL || id != SPA_PARAM_Format) return;
  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) < 0) return;
  d->watch_format = info.format;
  d->watch_w = info.size.width;
  d->watch_h = info.size.height;
  if (d->yuv) {
    const int nv12 = info.format == SPA_VIDEO_FORMAT_NV12 && info.size.width == W && info.size.height == H;
    const int i420 = info.format == SPA_VIDEO_FORMAT_I420 && info.size.width == 40 && info.size.height == 30;
    if (!nv12 && !i420) {
      char what[128];
      snprintf(what, sizeof what, "(the watcher was offered format %u %ux%u)", info.format, info.size.width, info.size.height);
      finish(d, 0, what);
      return;
    }
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)(info.size.width * info.size.height * 2)));
    pw_stream_update_params(d->watcher, params, 1);
    return;
  }
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

// (video-yuv) a frame in one block: the luma, then the chroma (NV12: one plane at the luma's stride; I420: two at half of it)
static void watch_yuv(struct data* d, struct spa_buffer* buf) {
  const uint8_t* p = buf->datas[0].data;
  const struct spa_chunk* chunk = buf->datas[0].chunk;
  const uint32_t w = d->watch_w, h = d->watch_h, ch = (h + 1) / 2;
  const int nv12 = d->watch_format == SPA_VIDEO_FORMAT_NV12;
  const uint32_t stride = chunk->stride > 0 ? (uint32_t)chunk->stride : w;
  const uint32_t cstride = nv12 ? stride : (stride + 1) / 2, crow = nv12 ? ((w + 1) / 2) * 2 : (w + 1) / 2;
  const uint32_t total = stride * h + cstride * ch * (nv12 ? 1 : 2);
  if (p == NULL || chunk->size < stride * (h - 1) + w || chunk->offset > buf->datas[0].maxsize || buf->datas[0].maxsize - chunk->offset < total) {
    ++d->bad;
    return;
  }
  p += chunk->offset;
  const uint8_t frame = p[0];
  int whole = 1;
  for (uint32_t y = 0; y < h && whole; ++y)
    for (uint32_t x = 0; x < w; x += 3)
      if (p[y * stride + x] != (uint8_t)(frame + y)) { whole = 0; break; }
  for (uint32_t plane = 0; plane < (nv12 ? 1u : 2u) && whole; ++plane) {
    const uint8_t* c = p + stride * h + plane * cstride * ch;
    for (uint32_t y = 0; y < ch && whole; ++y)
      for (uint32_t x = 0; x < crow; x += 2)
        if (c[y * cstride + x] != (uint8_t)(frame * 3 + y)) { whole = 0; break; }
  }
  if (!whole) { ++d->bad; return; }
  if (nv12) ++d->nv12_seen;
  else ++d->i420_seen;
  ++d->seen;
  if (d->nv12_seen >= 6) d->switch_now = 1;
}

static void watcher_process(void* userdata) {
  struct data* d = userdata;
  struct pw_buffer* b;
  if (d->yuv) {
    while ((b = pw_stream_dequeue_buffer(d->watcher)) != NULL) {
      watch_yuv(d, b->buffer);
      pw_stream_queue_buffer(d->watcher, b);
    }
    if (d->i420_seen >= 6) {
      char what[200];
      snprintf(what, sizeof what, "(%u NV12 frames at %ux%u in two blocks, then %u I420 at 40x30 in three after the camera changed format; %u produced, %u bad)",
               d->nv12_seen, W, H, d->i420_seen, d->produced, d->bad);
      finish(d, d->bad == 0, what);
    }
    return;
  }
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
      SPA_FORMAT_VIDEO_format, SPA_POD_Id(d->yuv ? SPA_VIDEO_FORMAT_NV12 : SPA_VIDEO_FORMAT_BGRA),
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
  const uint32_t first = d->yuv ? SPA_VIDEO_FORMAT_NV12 : SPA_VIDEO_FORMAT_BGRA, second = d->yuv ? SPA_VIDEO_FORMAT_I420 : SPA_VIDEO_FORMAT_RGBA;
  params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, first, first, second),
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
    if (d->notes_sent == 3 && !d->sysex_sent) {
      uint8_t sx[22] = {0xF0, 0x7D};
      for (int k = 2; k < 21; ++k) sx[k] = (uint8_t)k;
      sx[21] = 0xF7;
      spa_pod_builder_control(&builder, 200, SPA_CONTROL_Midi);
      spa_pod_builder_bytes(&builder, sx, sizeof sx);
      d->sysex_sent = 1;
    }
    if (d->notes_sent > 0) {
      uint8_t off[3] = {0x80, (uint8_t)(59 + d->notes_sent), 0};
      spa_pod_builder_control(&builder, 512, SPA_CONTROL_Midi);
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
        uint32_t last = 0;
        SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
          const uint8_t* bytes = SPA_POD_BODY(&c->value);
          const uint32_t size = SPA_POD_BODY_SIZE(&c->value);
          if (c->offset < last || c->offset >= 8192) ++d->offsets_bad;
          if (c->offset > 0) ++d->offsets_later;
          last = c->offset;
          if (c->type == SPA_CONTROL_Midi && size == 22 && bytes[0] == 0xF0 && bytes[1] == 0x7D && bytes[20] == 20 && bytes[21] == 0xF7) ++d->sysex_heard;
          else if (c->type == SPA_CONTROL_Midi && size >= 2) midi_heard(d, bytes[0], bytes[1]);
          if (c->type == SPA_CONTROL_UMP) {
            for (uint32_t at = 0; at + 4 <= size;) {
              uint32_t w;
              memcpy(&w, bytes + at, 4);
              const uint32_t type = w >> 28;
              if (type == 2 || type == 4) midi_heard(d, (uint8_t)(w >> 16), (uint8_t)(w >> 8) & 0x7F);
              if (type == 3 && at + 8 <= size) {      // (a piece of system exclusive: put back together)
                uint32_t w1;
                memcpy(&w1, bytes + at + 4, 4);
                const uint32_t status = (w >> 20) & 0xF, count = (w >> 16) & 0xF;
                const uint8_t piece[6] = {(uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1};
                if (status <= 1) d->ump_sysex_len = 0;
                for (uint32_t k = 0; k < count && k < 6 && d->ump_sysex_len < sizeof d->ump_sysex; ++k) d->ump_sysex[d->ump_sysex_len++] = piece[k];
                if ((status == 0 || status == 3) && d->ump_sysex_len == 20 && d->ump_sysex[0] == 0x7D && d->ump_sysex[19] == 20) ++d->sysex_heard;
              }
              at += type >= 3 && type <= 4 ? 8 : 4;
            }
          }
        }
      }
    }
    pw_stream_queue_buffer(d->midi_in, b);
  }
  if (d->notes_heard == NOTES && d->sysex_heard) {
    char what[200];
    snprintf(what, sizeof what, "(%u notes and a system exclusive message played by a pw_filter, all %u and the sysex heard by a pw_stream; offsets %s, %d past "
             "the cycle's start)", d->notes_sent, d->notes_heard, d->offsets_bad ? "OUT OF ORDER" : "in order", d->offsets_later);
    finish(d, d->offsets_bad == 0 && d->offsets_later > 0 && d->sysex_heard == 1, what);
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

// ---- an effect: a filter with inputs and outputs -------------------------------------------------------------------------------------------------

static void effect_process(void* userdata, struct spa_io_position* position) {
  struct data* d = userdata;
  const uint32_t n = position ? position->clock.duration : 1024;
  ++d->fx_cycles;
  const float* in[2];
  float* out[2];
  for (int k = 0; k < 2; ++k) {
    in[k] = pw_filter_get_dsp_buffer(d->fx_in[k], n);
    out[k] = pw_filter_get_dsp_buffer(d->fx_out[k], n);
  }
  for (int k = 0; k < 2; ++k)
    if (out[k]) for (uint32_t i = 0; i < n; ++i) out[k][i] = in[k] ? in[k][i] * 0.02f : 0.f;
  if (!in[0] || !in[1]) return;
  for (uint32_t i = 0; i < n && d->fx_frames < CAPTURE; ++i, ++d->fx_frames) {
    d->fx_capture[2 * d->fx_frames] = in[0][i];
    d->fx_capture[2 * d->fx_frames + 1] = in[1][i];
  }
  if (d->fx_frames >= CAPTURE) {
    // (the first tenth of a second skipped: the microphone may start late)
    const float* s = d->fx_capture + 2 * 4800;
    const uint32_t frames = CAPTURE - 4800;
    const double l880 = goertzel(s, frames, 2, 880), r1320 = goertzel(s + 1, frames, 2, 1320), l1320 = goertzel(s, frames, 2, 1320);
    char what[200];
    snprintf(what, sizeof what, "(its inputs heard left 880 Hz %.3f, right 1320 Hz %.3f, left 1320 Hz %.3f over %u cycles; its outputs played it back)", l880, r1320,
             l1320, d->fx_cycles);
    finish(d, l880 > 0.2 && r1320 > 0.1 && l1320 < 0.02, what);      // (8000 and 4000 of 32768: 0.244 and 0.122)
  }
}

static const struct pw_filter_events effect_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = effect_process,
};

static int run_filter(struct data* d) {
  d->fx_capture = calloc(2 * CAPTURE, sizeof(float));
  if (!d->fx_capture) return -1;
  d->effect = pw_filter_new_simple(pw_main_loop_get_loop(d->loop), "tcos-effect",
                                   pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter", PW_KEY_MEDIA_ROLE, "DSP",
                                                     PW_KEY_MEDIA_CLASS, "Audio/Filter", NULL),
                                   &effect_events, d);
  const char* names[2] = {"FL", "FR"};
  for (int k = 0; k < 2; ++k) {
    char in_name[16], out_name[16];
    snprintf(in_name, sizeof in_name, "input_%s", names[k]);
    snprintf(out_name, sizeof out_name, "output_%s", names[k]);
    d->fx_in[k] = pw_filter_add_port(d->effect, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                     pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, in_name, PW_KEY_AUDIO_CHANNEL, names[k], NULL),
                                     NULL, 0);
    d->fx_out[k] = pw_filter_add_port(d->effect, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, out_name, PW_KEY_AUDIO_CHANNEL, names[k], NULL),
                                      NULL, 0);
    if (!d->fx_in[k] || !d->fx_out[k]) return -1;
  }
  return pw_filter_connect(d->effect, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0 ? -1 : 0;
}

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
  d.yuv = strcmp(d.mode, "video-yuv") == 0;
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
  else if (strcmp(d.mode, "filter") == 0) r = run_filter(&d);
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
