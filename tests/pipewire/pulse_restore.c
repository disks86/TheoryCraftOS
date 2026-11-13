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

/* The kernel's module-stream-restore through the real libpulse (pa_ext_stream_restore_*, as pavucontrol uses it), run by tests/pipewire_interop.sh
 * on the host against tests/pipewire/PipeWireHostServer.cpp: the extension answers TEST with version 1; an entry written for an application is read
 * back; a playback stream that application opens then starts at that volume; the stream's volume set through the introspection API is remembered
 * (READ shows it) and a subscriber hears the change; DELETE takes the entry away. Prints "pulse_restore: N checks, M failed". */
#include <pulse/pulseaudio.h>
#include <pulse/ext-stream-restore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

static pa_threaded_mainloop *loop;
static pa_context *ctx;

/* Waits for an operation (FALSE when it failed or took too long). */
static int wait_for(pa_operation *o) {
  if (!o) return 0;
  for (int k = 0; k < 500 && pa_operation_get_state(o) == PA_OPERATION_RUNNING; ++k) {
    pa_threaded_mainloop_unlock(loop);
    usleep(10000);
    pa_threaded_mainloop_lock(loop);
  }
  const int done = pa_operation_get_state(o) == PA_OPERATION_DONE;
  pa_operation_unref(o);
  return done;
}

static uint32_t g_version;
static void test_cb(pa_context *c, uint32_t version, void *u) { (void)c; (void)u; g_version = version; }
static int g_success = -1;
static void success_cb(pa_context *c, int success, void *u) { (void)c; (void)u; g_success = success; }

/* READ: the entry of this name, if listed */
static const char *g_want;
static int g_found, g_found_mute, g_entries;
static pa_volume_t g_found_volume;
static void read_cb(pa_context *c, const pa_ext_stream_restore_info *info, int eol, void *u) {
  (void)c; (void)u;
  if (eol || !info) return;
  ++g_entries;
  if (strcmp(info->name, g_want) == 0) {
    g_found = 1;
    g_found_mute = info->mute;
    g_found_volume = pa_cvolume_max(&info->volume);
  }
}

static int g_events;
static void event_cb(pa_context *c, void *u) { (void)c; (void)u; ++g_events; }

static pa_volume_t g_input_volume;
static int g_input_found;
static uint32_t g_input_index;
static void input_cb(pa_context *c, const pa_sink_input_info *info, int eol, void *u) {
  (void)c; (void)u;
  if (eol || !info) return;
  g_input_found = 1;
  g_input_volume = pa_cvolume_max(&info->volume);
}

static int find_entry(const char *name) {
  g_want = name;
  g_found = g_entries = 0;
  return wait_for(pa_ext_stream_restore_read(ctx, read_cb, NULL)) && g_found;
}

int main(void) {
  loop = pa_threaded_mainloop_new();
  ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "pulse_restore");
  pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
  pa_threaded_mainloop_start(loop);
  pa_threaded_mainloop_lock(loop);
  for (int k = 0; k < 500 && pa_context_get_state(ctx) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(pa_context_get_state(ctx)); ++k) {
    pa_threaded_mainloop_unlock(loop);
    usleep(10000);
    pa_threaded_mainloop_lock(loop);
  }
  CHECK(pa_context_get_state(ctx) == PA_CONTEXT_READY);
  if (pa_context_get_state(ctx) != PA_CONTEXT_READY) goto done;

  CHECK(wait_for(pa_ext_stream_restore_test(ctx, test_cb, NULL)) && g_version == 1);
  pa_ext_stream_restore_set_subscribe_cb(ctx, event_cb, NULL);
  CHECK(wait_for(pa_ext_stream_restore_subscribe(ctx, 1, success_cb, NULL)) && g_success == 1);

  /* an entry for "restore-player": 30%, not muted */
  {
    pa_ext_stream_restore_info info;
    memset(&info, 0, sizeof info);
    info.name = "sink-input-by-application-name:restore-player";
    pa_channel_map_init_stereo(&info.channel_map);
    pa_cvolume_set(&info.volume, 2, PA_VOLUME_NORM * 3 / 10);
    info.device = NULL;
    info.mute = 0;
    g_success = -1;
    const int events = g_events;
    CHECK(wait_for(pa_ext_stream_restore_write(ctx, PA_UPDATE_MERGE, &info, 1, 1, success_cb, NULL)) && g_success == 1);
    for (int k = 0; k < 100 && g_events == events; ++k) {
      pa_threaded_mainloop_unlock(loop);
      usleep(10000);
      pa_threaded_mainloop_lock(loop);
    }
    CHECK(g_events > events);
    CHECK(find_entry("sink-input-by-application-name:restore-player") && g_found_volume == PA_VOLUME_NORM * 3 / 10 && !g_found_mute);
  }
  /* a stream of that application starts at it */
  {
    pa_proplist *props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "restore-player");
    pa_sample_spec ss = {PA_SAMPLE_S16LE, 48000, 2};
    pa_stream *s = pa_stream_new_with_proplist(ctx, "restore stream", &ss, NULL, props);
    pa_proplist_free(props);
    CHECK(s && pa_stream_connect_playback(s, NULL, NULL, PA_STREAM_NOFLAGS, NULL, NULL) == 0);
    for (int k = 0; k < 300 && s && pa_stream_get_state(s) != PA_STREAM_READY; ++k) {
      pa_threaded_mainloop_unlock(loop);
      usleep(10000);
      pa_threaded_mainloop_lock(loop);
    }
    CHECK(s && pa_stream_get_state(s) == PA_STREAM_READY);
    if (s && pa_stream_get_state(s) == PA_STREAM_READY) {
      g_input_index = pa_stream_get_index(s);
      /* (the server gives it the volume on its next round) */
      pa_volume_t seen = 0;
      for (int k = 0; k < 100; ++k) {
        g_input_found = 0;
        wait_for(pa_context_get_sink_input_info(ctx, g_input_index, input_cb, NULL));
        seen = g_input_volume;
        if (g_input_found && seen < PA_VOLUME_NORM / 2) break;
        pa_threaded_mainloop_unlock(loop);
        usleep(10000);
        pa_threaded_mainloop_lock(loop);
      }
      printf("pulse_restore: the new stream's volume %u (remembered %u)\n", (unsigned)seen, (unsigned)(PA_VOLUME_NORM * 3 / 10));
      CHECK(g_input_found && seen + 2 >= PA_VOLUME_NORM * 3 / 10 && seen <= PA_VOLUME_NORM * 3 / 10 + 2);
      /* its volume changed (as pavucontrol's slider would): remembered */
      pa_cvolume v;
      pa_cvolume_set(&v, 2, PA_VOLUME_NORM * 7 / 10);
      g_success = -1;
      CHECK(wait_for(pa_context_set_sink_input_volume(ctx, g_input_index, &v, success_cb, NULL)) && g_success == 1);
      int remembered = 0;
      for (int k = 0; k < 100 && !remembered; ++k) {
        pa_threaded_mainloop_unlock(loop);
        usleep(20000);
        pa_threaded_mainloop_lock(loop);
        remembered = find_entry("sink-input-by-application-name:restore-player") && g_found_volume + 2 >= PA_VOLUME_NORM * 7 / 10 &&
                     g_found_volume <= PA_VOLUME_NORM * 7 / 10 + 2;
      }
      printf("pulse_restore: after the slider: %u remembered\n", (unsigned)g_found_volume);
      CHECK(remembered);
      pa_stream_disconnect(s);
    }
    if (s) pa_stream_unref(s);
  }
  /* DELETE */
  {
    const char *names[] = {"sink-input-by-application-name:restore-player", NULL};
    g_success = -1;
    CHECK(wait_for(pa_ext_stream_restore_delete(ctx, names, success_cb, NULL)) && g_success == 1);
    CHECK(!find_entry("sink-input-by-application-name:restore-player"));
  }
done:
  pa_threaded_mainloop_unlock(loop);
  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
  pa_threaded_mainloop_stop(loop);
  pa_threaded_mainloop_free(loop);
  printf("pulse_restore: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
