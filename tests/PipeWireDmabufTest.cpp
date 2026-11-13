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

// The PipeWire transport with a real DMA-BUF: an export of the kernel's Direct3D layer (on its software adapter), passed over AF_UNIX with
// SCM_RIGHTS as a client's camera would pass it in port_buffers. The descriptor the transport makes of it must be a GPU image whose copy, after
// sync_from_gpu, holds the image's pixels, and follows the image when it is drawn again. (PipeWireTest checks the server's side with a fake one.)
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "PipeWire/DxgGpuImages.hpp"
#include "PipeWire/SocketServer.hpp"
#include "Process/DxgBufferFile.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/UnixSockets.hpp"

using namespace pipewire;
namespace dst = dxgk::status;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size ? size : 1) != 0) { --live; return nullptr; }
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;
static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct SystemSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

// What the server is given (the bytes and the descriptors), and descriptors it sends on (`to_send`, to connection `send_to`, with one byte).
struct CaptureProtocol final : IProtocolServer {
  std::vector<uint8_t> bytes;
  std::vector<Fd> fds;
  std::vector<Fd> to_send;
  uint32_t send_to = 0, clients = 0;
  uint32_t open_client(uint64_t) override { return ++clients; }
  bool take_output_with_fds(uint32_t client, ByteBuffer* out, List<Fd>* out_fds) override {
    if (client != send_to || to_send.empty()) return false;
    out->put8(0x42);
    for (Fd& f : to_send) out_fds->push_back(f);
    to_send.clear();
    return false;
  }
  void receive(uint32_t, ConstByteView b) override { bytes.insert(bytes.end(), b.data(), b.data() + b.size()); }
  void receive_with_fds(uint32_t client, ConstByteView b, List<Fd>& in) override {
    receive(client, b);
    for (Fd& f : in) fds.push_back(f);
    in.clear();
  }
  bool take_output(uint32_t, ByteBuffer*) override { return false; }
  void close_client(uint32_t) override {}
  void tick() override {}
  uint64_t tick_interval_us() const override { return 10000; }
};

static uint32_t pattern(uint32_t x, uint32_t y, uint32_t frame) { return 0xFF000000u | (x * 4 + frame) << 16 | (y * 5) << 8 | ((x + y + frame) & 0xFF); }

int main() {
  graphics::SoftwareDevice device{A, nullptr};
  dxgk::Kernel k(A);
  CHECK(k.add_adapter(device, dxgk::AdapterConfig()) == 0);
  process::set_dxg_buffer_copier(&dxgk::copy_export);
  {
    // a camera's frame: an image of 40 x 30 drawn by its process, exported (what /dev/dxg's export makes a dmabuf of)
    const dxgk::ProcessId pid = 7;
    dxgk::Handle adapter = 0, dev = 0, image = 0;
    CHECK(dst::ok(k.open_process(pid)) && dst::ok(k.open_adapter_from_index(pid, 0, &adapter)) && dst::ok(k.create_device(pid, adapter, &dev)));
    dxgk::AllocationDesc d;
    d.flags = dxgk::kAllocTexture | dxgk::kAllocRenderTarget | dxgk::kAllocShared;
    d.format = dxgk::Format::A8R8G8B8;
    d.width = 40, d.height = 30;
    CHECK(dst::ok(k.create_allocation(pid, dev, d, &image)));
    auto paint = [&](uint32_t frame) {
      dxgk::LockResult v;
      if (!dst::ok(k.lock(pid, image, dxgk::kLockDiscard, 0, &v))) return false;
      for (uint32_t y = 0; y < 30; ++y)
        for (uint32_t x = 0; x < 40; ++x) {
          const uint32_t p = pattern(x, y, frame);
          memcpy(v.data.data() + uint64_t(y) * v.pitch + uint64_t(x) * 4, &p, 4);
        }
      return dst::ok(k.unlock(pid, image));
    };
    CHECK(paint(0));
    SharedPointer<dxgk::ExportedAllocation> exported;
    CHECK(dst::ok(k.export_allocation(pid, image, &exported)) && exported);
    const dxgk::ExportInfo& info = dxgk::export_info(*exported);
    SharedPointer<process::OpenObject> object = SharedPointer<process::OpenObject>::make(A, A);
    UniquePointer<process::DxgBufferFile> file = UniquePointer<process::DxgBufferFile>::make(A, A, exported, info.bytes);
    object->gpu_buffer = file.get();
    object->file = util::move(file);
    object->readable = true;

    // through the transport, as the camera's port_buffers sends it
    SystemSync sync;
    process::UnixSocketManager sockets(sync, A);
    CaptureProtocol proto;
    {
      SocketServer server(sockets, A);
      CHECK(server.listen(proto, "/run/user/0/dmabuf-test", StringView(), nullptr));
      uint32_t sock = 0;
      CHECK(sockets.create(process::UnixType::Stream, 9, &sock) == process::UnixStatus::Ok);
      sync.lock();
      CHECK(sockets.connect_locked(sock, process::UnixName::from(process::UnixName::Kind::Abstract, StringView("/run/user/0/dmabuf-test").bytes()), StringView(), 9) ==
            process::UnixStatus::Ok);
      sync.unlock();
      server.pump();
      Queue<process::UnixInFlight> rights(A);
      process::UnixInFlight entry;
      entry.kind = process::DescriptorKind::File;
      entry.object = object;
      rights.enqueue(util::move(entry));
      sync.lock();
      uint32_t sent = 0;
      const uint8_t hello[4] = {1, 2, 3, 4};
      CHECK(sockets.send_locked(sock, ConstByteView(hello, 4), nullptr, 9, &rights, &sent) == process::UnixStatus::Ok && sent == 4);
      sync.unlock();
      for (int i = 0; i < 3; ++i) server.pump();
      CHECK(proto.fds.size() == 1);
      if (proto.fds.size() == 1) {
        Fd& fd = proto.fds[0];
        CHECK(fd->gpu() && fd->memory().size() == info.bytes);
        // the image's pixels in the copy, rows `pitch` apart
        auto holds = [&](uint32_t frame) {
          const ByteView m = fd->memory();
          for (uint32_t y = 0; y < 30; ++y)
            for (uint32_t x = 0; x < 40; ++x) {
              uint32_t p = 0;
              memcpy(&p, m.data() + uint64_t(y) * info.pitch + uint64_t(x) * 4, 4);
              if (p != pattern(x, y, frame)) return false;
            }
          return true;
        };
        CHECK(!holds(0));      // (not yet read)
        CHECK(fd->sync_from_gpu() && holds(0));
        CHECK(paint(3));       // the camera draws its next frame: the copy follows at the next sync
        CHECK(holds(0) && fd->sync_from_gpu() && holds(3));

        // zero-copy: the same DMA-BUF sent on to another client (a watcher given the camera's own buffer) is the very export, not a copy; and an image of
        // the server's own (a watcher that takes DMA-BUFs only) holds what the server wrote into it, for the client that is given it
        DxgGpuImages images(k, A);
        CHECK(!server.gpu_images());
        uint32_t none_stride = 7;
        CHECK(!server.make_gpu_image(16, 8, &none_stride) && none_stride == 0);
        server.set_gpu_images(&images);
        uint32_t stride = 0;
        Fd made = server.make_gpu_image(16, 8, &stride);
        CHECK(made && made->gpu() && stride >= 64 && made->memory().size() >= uint64_t(stride) * 8);
        if (made) {
          for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 16; ++x) {
              const uint32_t p = pattern(x, y, 9);
              memcpy(made->memory().data() + uint64_t(y) * stride + uint64_t(x) * 4, &p, 4);
            }
          CHECK(made->sync_to_gpu());
        }
        uint32_t sock2 = 0;
        CHECK(sockets.create(process::UnixType::Stream, 9, &sock2) == process::UnixStatus::Ok);
        sync.lock();
        CHECK(sockets.connect_locked(sock2, process::UnixName::from(process::UnixName::Kind::Abstract, StringView("/run/user/0/dmabuf-test").bytes()), StringView(), 9) ==
              process::UnixStatus::Ok);
        sync.unlock();
        for (int i = 0; i < 2; ++i) server.pump();
        proto.send_to = proto.clients;
        proto.to_send = {fd, made};
        for (int i = 0; i < 3; ++i) server.pump();
        Queue<process::UnixInFlight> got(A);
        uint8_t byte[8] = {};
        process::UnixSocketManager::Received rec;
        sync.lock();
        CHECK(sockets.receive_locked(sock2, ByteView(byte, sizeof byte), false, &rec, &got) == process::UnixStatus::Ok && rec.got == 1 && byte[0] == 0x42);
        sync.unlock();
        process::UnixInFlight first, second;
        CHECK(got.dequeue(&first) && got.dequeue(&second));
        CHECK(first.kind == process::DescriptorKind::File && first.object && first.object->gpu_buffer == object->gpu_buffer);      // (the camera's own)
        CHECK(second.kind == process::DescriptorKind::File && second.object && second.object->gpu_buffer);
        if (second.object && second.object->gpu_buffer) {
          std::vector<uint8_t> back(size_t(stride) * 8, 0);
          CHECK(second.object->gpu_buffer->copy_image(ByteView(back.data(), back.size()), false));
          bool image = true;
          for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 16; ++x) {
              uint32_t p = 0;
              memcpy(&p, back.data() + uint64_t(y) * stride + uint64_t(x) * 4, 4);
              image &= p == pattern(x, y, 9);
            }
          CHECK(image);
        }
        first = process::UnixInFlight();
        second = process::UnixInFlight();
        made = Fd();
        server.set_gpu_images(nullptr);      // (before the images' maker goes)
        sockets.release(sock2);
        for (int i = 0; i < 3; ++i) server.pump();
      }
      proto.fds.clear();
      sockets.release(sock);
      for (int i = 0; i < 3; ++i) server.pump();
    }
    object = SharedPointer<process::OpenObject>();
    exported = SharedPointer<dxgk::ExportedAllocation>();
    CHECK(dst::ok(k.destroy_allocation(pid, image)));
    k.close_process(pid);
  }
  process::set_dxg_buffer_copier(nullptr);
  printf("PipeWireDmabufTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
