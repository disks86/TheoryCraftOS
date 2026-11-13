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
// The native protocol's client-node (pw-play, pw-record) gets real memfds and eventfds from this host (HostMaker), passed with SCM_RIGHTS.
//
// Usage: PipeWireHostServer <native socket path> <pulse socket path> <out.wav> [seconds]   (runs until SIGTERM/SIGINT, or for that many seconds)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
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

// A microphone that hears, in real time, 880 Hz on the left at 8000 and 1320 Hz on the right at 4000 (what parecord must record).
struct RealtimeMicrophone final : hardware::IAudioCapture {
  std::mutex m;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  double owed = 0;
  uint64_t phase = 0;
  uint8_t level = hardware::kAudioMaxVolume;
  bool mute = false;
  void advance() {
    const auto now = std::chrono::steady_clock::now();
    owed += std::chrono::duration<double>(now - last).count() * 48000.0;
    if (owed > 16384) owed = 16384;
    last = now;
  }
  const char* name() const override { return "Host test microphone"; }
  uint32_t read(ByteView out) override {
    std::lock_guard<std::mutex> g(m);
    advance();
    uint32_t frames = uint32_t(out.size() / 4);
    if (frames > uint32_t(owed)) frames = uint32_t(owed);
    for (uint32_t i = 0; i < frames; ++i, ++phase) {
      const double t = double(phase) / 48000.0;
      const int16_t l = mute ? 0 : int16_t(8000.0 * sin(2 * M_PI * 880.0 * t) * level / hardware::kAudioMaxVolume);
      const int16_t r = mute ? 0 : int16_t(4000.0 * sin(2 * M_PI * 1320.0 * t) * level / hardware::kAudioMaxVolume);
      out[4 * i] = uint8_t(l); out[4 * i + 1] = uint8_t(uint16_t(l) >> 8);
      out[4 * i + 2] = uint8_t(r); out[4 * i + 3] = uint8_t(uint16_t(r) >> 8);
    }
    owed -= frames;
    return frames * 4;
  }
  uint32_t available_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return uint32_t(owed) * 4; }
  void flush() override { std::lock_guard<std::mutex> g(m); advance(); phase += uint64_t(owed); owed = 0; }
  void set_gain(uint8_t x) override { level = x; }
  uint8_t gain() const override { return level; }
  void set_muted(bool x) override { mute = x; }
  bool muted() const override { return mute; }
  hardware::AudioCaptureStatistics statistics() override { return {}; }
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

// A descriptor of this host: a memfd mapped here, or an eventfd.
class HostFd final : public Descriptor {
 public:
  HostFd(int fd, uint8_t* map, uint64_t size) : fd_(fd), map_(map), size_(size) {}
  ~HostFd() {
    if (map_) munmap(map_, size_);
    if (fd_ >= 0) close(fd_);
  }
  ByteView memory() override { return map_ ? ByteView(map_, size_) : ByteView(); }
  bool signal() override {
    const uint64_t one = 1;
    return write(fd_, &one, sizeof one) == sizeof one;
  }
  uint64_t take_signals() override {
    uint64_t n = 0;
    return read(fd_, &n, sizeof n) == sizeof n ? n : 0;
  }
  int fd() const { return fd_; }

 private:
  int fd_;
  uint8_t* map_;
  uint64_t size_;
};

class HostMaker final : public IDescriptorMaker {
 public:
  Fd make_memory(uint64_t size, StringView name) override {
    char label[64];
    snprintf(label, sizeof label, "%.*s", int(name.size() < 60 ? name.size() : 60), name.data());
    const int fd = memfd_create(label, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return Fd();
    if (ftruncate(fd, off_t(size)) != 0) { close(fd); return Fd(); }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return Fd(); }
    return Fd::make_as<HostFd>(A, fd, static_cast<uint8_t*>(map), size);
  }
  Fd make_event() override {
    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return fd < 0 ? Fd() : Fd::make_as<HostFd>(A, fd, nullptr, 0);
  }
};

// Output owed to a connection: bytes, and the descriptors that go with the first of them.
struct Segment {
  std::vector<uint8_t> bytes;
  std::vector<Fd> fds;
  size_t sent = 0;
};

struct Conn {
  int fd;
  uint32_t client;
  IProtocolServer* protocol;
  std::deque<Segment> pending;
  bool closing = false;
};

// Sends what it can of the first segment (its descriptors with its first bytes); returns the bytes sent, -1 on an error, 0 when the socket is full.
ssize_t send_segment(int fd, Segment& s) {
  iovec v{s.bytes.data() + s.sent, s.bytes.size() - s.sent};
  msghdr m{};
  m.msg_iov = &v;
  m.msg_iovlen = 1;
  std::vector<uint8_t> control;
  if (s.sent == 0 && !s.fds.empty()) {
    control.resize(CMSG_SPACE(sizeof(int) * s.fds.size()));
    m.msg_control = control.data();
    m.msg_controllen = control.size();
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * s.fds.size());
    int* out = reinterpret_cast<int*>(CMSG_DATA(c));      // (the cmsg area is aligned for ints by CMSG_SPACE)
    for (size_t i = 0; i < s.fds.size(); ++i) out[i] = static_cast<HostFd&>(*s.fds[i]).fd();
  }
  const ssize_t n = sendmsg(fd, &m, MSG_NOSIGNAL);
  if (n > 0) {
    s.sent += size_t(n);
    s.fds.clear();      // (the receiver has its copies now)
    return n;
  }
  return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}

}  // namespace

// The RTP modules' network on the host: real UDP sockets (non-blocking), multicast joined on any interface.
struct HostUdpSocket final : network::IUdpSocket {
  int fd = -1;
  uint16_t port = 0;
  ~HostUdpSocket() { if (fd >= 0) ::close(fd); }
  uint16_t local_port() const override { return port; }
  bool send_to(network::Ipv4Address address, uint16_t to, ConstByteView data) override {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(to);
    memcpy(&a.sin_addr, address.octets, 4);
    return fd >= 0 && ::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == ssize_t(data.size());
  }
  bool receive_from(ByteView buffer, network::Ipv4Address* from, uint16_t* from_port, uint32_t* length) override {
    if (fd < 0) return false;
    sockaddr_in a{};
    socklen_t n = sizeof a;
    const ssize_t got = ::recvfrom(fd, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC, reinterpret_cast<sockaddr*>(&a), &n);
    if (got < 0) return false;
    memcpy(from->octets, &a.sin_addr, 4);
    *from_port = ntohs(a.sin_port);
    *length = uint32_t(got);
    return true;
  }
  uint32_t pending() const override { return 0; }
  void close() override {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
};
struct HostRtpNetwork final : IRtpNetwork {
  std::vector<SharedPointer<HostUdpSocket>> sockets;
  SharedPointer<network::IUdpSocket> open(uint16_t port) override {
    SharedPointer<HostUdpSocket> s = SharedPointer<HostUdpSocket>::make(A);
    s->fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s->fd < 0 || ::bind(s->fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return SharedPointer<network::IUdpSocket>();
    socklen_t n = sizeof a;
    getsockname(s->fd, reinterpret_cast<sockaddr*>(&a), &n);
    s->port = ntohs(a.sin_port);
    sockets.push_back(s);
    return s;
  }
  bool join(network::Ipv4Address group) override {
    // (on the SAP socket and the others already open: the host joins per socket)
    ip_mreq m{};
    memcpy(&m.imr_multiaddr, group.octets, 4);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    bool any = false;
    for (auto& s : sockets) any |= s->fd >= 0 && setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
    return any;
  }
  void leave(network::Ipv4Address) override {}
  network::Ipv4Address local_address() override { return network::Ipv4Address::make(127, 0, 0, 1); }
};

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
  RealtimeMicrophone microphone;
  audio::AudioCaptureHub capture(microphone, A);
  if (!capture.initialize()) return 1;
  Graph graph(A, &mixer, &capture);
  if (!graph.initialize()) return 1;
  NativeServer native(graph, A, [] { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); });
  // what programs play on MIDI: counted, and the notes listed at the end
  struct CountingSynth final : midi::IMidiSink {
    std::mutex m;
    std::vector<uint32_t> notes;
    uint32_t sysex = 0;
    void send(uint8_t status, uint8_t d1, uint8_t d2) override { std::lock_guard<std::mutex> g(m); notes.push_back(uint32_t(status) << 16 | uint32_t(d1) << 8 | d2); }
    void send_sysex(ConstByteView message) override {
      std::lock_guard<std::mutex> g(m);
      if (message.size() == 22 && message[0] == 0xF0 && message[1] == 0x7D) ++sysex;      // (pwmedia's: 20 bytes between F0 and F7)
    }
    void reset() override {}
  } synth;
  graph.midi_bus().set_output(&synth);
  HostMaker maker;
  native.set_descriptor_maker(&maker);
  PulseServer pulse(graph, A);
  HostRtpNetwork rtp_network;
  pulse.set_rtp_network(&rtp_network);
  // (module-stream-restore is not loaded here as the kernel loads it at start-up: it would carry one check's volumes into the next; the interop
  // script loads it for its own check)
  const int listeners[2] = {listen_on(argv[1]), listen_on(argv[2])};
  IProtocolServer* protocols[2] = {&native, &pulse};
  std::thread mix([&] {
    while (!g_stop) {
      mixer.pump();
      capture.pump();
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
    poll(fds.data(), fds.size(), native.running_nodes() ? 2 : 5);
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
        // (with the descriptors that came: a client's memfd is mapped here, anything else is just held)
        iovec v{chunk, sizeof chunk};
        alignas(cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int) * 64)];
        msghdr m{};
        m.msg_iov = &v;
        m.msg_iovlen = 1;
        m.msg_control = control;
        m.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(c.fd, &m, MSG_CMSG_CLOEXEC);
        List<Fd> in(A);
        if (n >= 0)
          for (cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
            const size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t k = 0; k < count; ++k) {
              int fd = -1;
              memcpy(&fd, CMSG_DATA(h) + k * sizeof(int), sizeof fd);
              struct stat st{};
              void* map = MAP_FAILED;
              if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                map = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
              in.push_back(Fd::make_as<HostFd>(A, fd, map == MAP_FAILED ? nullptr : static_cast<uint8_t*>(map), map == MAP_FAILED ? 0 : uint64_t(st.st_size)));
            }
          }
        if (n > 0 || in.count()) { c.protocol->receive_with_fds(c.client, ConstByteView(chunk, n > 0 ? uint64_t(n) : 0), in); if (n > 0) continue; }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) c.closing = true;
        break;
      }
    }
    native.tick();
    pulse.tick();
    for (Conn& c : conns) {
      ByteBuffer out(A);
      List<Fd> fds(A);
      if (c.protocol->take_output_with_fds(c.client, &out, &fds)) c.closing = true;
      if (out.size() || fds.count()) {
        Segment seg;
        seg.bytes.assign(out.view().data(), out.view().data() + out.size());
        for (Fd& f : fds) seg.fds.push_back(f);
        c.pending.push_back(std::move(seg));
      }
      while (!c.pending.empty()) {
        Segment& s = c.pending.front();
        if (s.sent == s.bytes.size()) { c.pending.pop_front(); continue; }
        const ssize_t n = send_segment(c.fd, s);
        if (n > 0) continue;
        if (n < 0) { c.closing = true; c.pending.clear(); }
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
  {
    std::lock_guard<std::mutex> g(synth.m);
    uint32_t on = 0;
    for (uint32_t n : synth.notes) on += (n >> 16 & 0xF0) == 0x90;
    fprintf(stderr, "pipewire-host: midi: %zu messages to the synthesizer, %u note-ons, %u system exclusive\n", synth.notes.size(), on, synth.sysex);
  }
  graph.midi_bus().set_output(nullptr);
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
