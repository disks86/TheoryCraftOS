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

// The kernel's PipeWire (src/PipeWire: the graph, the native and the PulseAudio protocol) served on the development machine's own AF_UNIX sockets, so
// that the real libpipewire and libpulse tools (pw-dump, pw-cli, pactl, paplay) can be pointed at it: tests/pipewire_interop.sh does that.
// The mixer plays into a device that drains in real time (48 kHz) and keeps everything it was given; at the end that is written as a WAV file.
//
// Usage: PipeWireHostServer <native socket path> <pulse socket path> <out.wav> [seconds]   (runs until SIGTERM/SIGINT, or for that many seconds)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "PipeWire/Graph.hpp"
#include "PipeWire/NativeServer.hpp"
#include "PipeWire/PulseServer.hpp"

using namespace pipewire;

namespace {

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t align) override {
    void* p = nullptr;
    return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size ? size : 1) == 0 ? p : nullptr;
  }
  void deallocate(void* p) override { free(p); }
};
HostAllocator A;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// Plays 48000 frames a second, keeps what it got.
struct RealtimeDevice final : hardware::IAudioDevice {
  std::mutex m;
  std::vector<int16_t> recorded;
  double queued_frames = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  uint8_t vol = 100;
  bool mute = false;
  static constexpr uint32_t kCapacityFrames = 9600;
  void advance() {
    const auto now = std::chrono::steady_clock::now();
    queued_frames -= std::chrono::duration<double>(now - last).count() * 48000.0;
    if (queued_frames < 0) queued_frames = 0;
    last = now;
  }
  const char* name() const override { return "Host test speakers"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    std::lock_guard<std::mutex> g(m);
    advance();
    uint32_t frames = uint32_t(pcm.size() / 4);
    const uint32_t room = kCapacityFrames - uint32_t(queued_frames);
    if (frames > room) frames = room;
    for (uint32_t i = 0; i < frames * 2; ++i) recorded.push_back(int16_t(pcm[2 * i] | pcm[2 * i + 1] << 8));
    queued_frames += frames;
    return frames * 4;
  }
  uint32_t queued_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return uint32_t(queued_frames) * 4; }
  uint32_t free_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return (kCapacityFrames - uint32_t(queued_frames)) * 4; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool v) override { mute = v; }
  bool muted() const override { return mute; }
  hardware::AudioStatistics statistics() override { return {}; }
};

void write_wav(const char* path, const std::vector<int16_t>& samples) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  const uint32_t data = uint32_t(samples.size() * 2);
  fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
  fwrite("data", 1, 4, f); u32(data);
  fwrite(samples.data(), 2, samples.size(), f);
  fclose(f);
}

int listen_on(const char* path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
  unlink(path);
  if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 16) != 0) {
    perror(path);
    exit(2);
  }
  return fd;
}

struct Conn {
  int fd;
  uint32_t client;
  IProtocolServer* protocol;
  std::vector<uint8_t> pending;
  bool closing = false;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <native socket> <pulse socket> <out.wav> [seconds]\n", argv[0]);
    return 2;
  }
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);
  const double seconds = argc > 4 ? atof(argv[4]) : 0;
  RealtimeDevice device;
  audio::AudioMixer mixer(device, A);
  if (!mixer.initialize()) return 1;
  Graph graph(A, &mixer);
  if (!graph.initialize()) return 1;
  NativeServer native(graph, A);
  PulseServer pulse(graph, A);
  const int listeners[2] = {listen_on(argv[1]), listen_on(argv[2])};
  IProtocolServer* protocols[2] = {&native, &pulse};
  std::thread mix([&] {
    while (!g_stop) {
      mixer.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  fprintf(stderr, "pipewire-host: listening on %s and %s\n", argv[1], argv[2]);
  std::vector<Conn> conns;
  const auto start = std::chrono::steady_clock::now();
  uint8_t chunk[65536];
  while (!g_stop) {
    if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) break;
    std::vector<pollfd> fds;
    for (int l : listeners) fds.push_back(pollfd{l, POLLIN, 0});
    for (Conn& c : conns) fds.push_back(pollfd{c.fd, short(POLLIN | (c.pending.empty() ? 0 : POLLOUT)), 0});
    poll(fds.data(), fds.size(), 5);
    for (int i = 0; i < 2; ++i)
      if (fds[i].revents & POLLIN)
        for (;;) {
          const int fd = accept4(listeners[i], nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd < 0) break;
          ucred cred{};
          socklen_t len = sizeof cred;
          getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
          const uint32_t client = protocols[i]->open_client(uint64_t(cred.pid));
          if (!client) { close(fd); continue; }
          conns.push_back(Conn{fd, client, protocols[i], {}, false});
          fprintf(stderr, "pipewire-host: %s client %u (pid %d)\n", i == 0 ? "native" : "pulse", client, cred.pid);
        }
    for (Conn& c : conns) {
      for (;;) {
        const ssize_t n = recv(c.fd, chunk, sizeof chunk, 0);
        if (n > 0) { c.protocol->receive(c.client, ConstByteView(chunk, uint64_t(n))); continue; }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) c.closing = true;
        break;
      }
    }
    native.tick();
    pulse.tick();
    for (Conn& c : conns) {
      ByteBuffer out(A);
      if (c.protocol->take_output(c.client, &out)) c.closing = true;
      c.pending.insert(c.pending.end(), out.view().data(), out.view().data() + out.size());
      while (!c.pending.empty()) {
        const ssize_t n = send(c.fd, c.pending.data(), c.pending.size(), MSG_NOSIGNAL);
        if (n > 0) { c.pending.erase(c.pending.begin(), c.pending.begin() + n); continue; }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { c.closing = true; c.pending.clear(); }
        break;
      }
    }
    for (size_t i = 0; i < conns.size();) {
      if (conns[i].closing && conns[i].pending.empty()) {
        conns[i].protocol->close_client(conns[i].client);
        close(conns[i].fd);
        conns.erase(conns.begin() + long(i));
      } else {
        ++i;
      }
    }
  }
  g_stop = true;
  mix.join();
  for (Conn& c : conns) { c.protocol->close_client(c.client); close(c.fd); }
  {
    std::lock_guard<std::mutex> g(device.m);
    write_wav(argv[3], device.recorded);
    fprintf(stderr, "pipewire-host: %zu frames played, written to %s\n", device.recorded.size() / 2, argv[3]);
  }
  unlink(argv[1]);
  unlink(argv[2]);
  return 0;
}
