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

#include "Wayland/Dxg/WaylandDxgImage.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Process/DxgBufferFile.hpp"

namespace wayland {

namespace {

namespace status = dxgk::status;

// The words of a locked image, for graphics::Surface. Safe: the view is the allocation's own backing (page-aligned memory of the video memory manager, checked
// below), its rows are whole pixels (the pitch is width * 4 for the formats exported), and it stays valid until end_read_export, which the caller does after
// the draw.
ArrayView<uint32_t> pixel_words(ByteView bytes) {
  if (reinterpret_cast<uintptr_t>(bytes.data()) % alignof(uint32_t) != 0) return ArrayView<uint32_t>();
  return ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(bytes.data()), bytes.size() / 4);
}

class DxgImage final : public GpuImage {
 public:
  DxgImage(dxgk::Kernel& kernel, SharedPointer<process::OpenObject> object)
      : kernel_(kernel), object_(util::move(object)), exported_(object_->gpu_buffer->exported()), info_(dxgk::export_info(*exported_)) {}

  bool committed() override { return object_->gpu_buffer->flush_cpu_writes(); }

  uint32_t width() const override { return info_.desc.width; }
  uint32_t height() const override { return info_.desc.height; }
  uint32_t pitch() const override { return info_.pitch; }

  bool draw(graphics::IRenderer2D& renderer, const graphics::Rect2D& source, int32_t x, int32_t y) override {
    if (&renderer != refused_) {
      dxgk::AcquiredImage lent;
      if (kernel_.acquire_export_image(*exported_, &lent) == status::kSuccess) {
        const bool shown = renderer.draw_device_image(*lent.device, *lent.image, source, x, y);
        kernel_.release_export_image(*exported_);
        if (shown) return true;
        refused_ = &renderer;      // (another device, or a CPU renderer: it will not take it next time either)
      }
    }
    dxgk::LockResult view;
    if (kernel_.read_export(*exported_, &view) != status::kSuccess) return false;
    graphics::Surface picture;
    picture.pixels = pixel_words(view.data);
    picture.width = info_.desc.width;
    picture.height = info_.desc.height;
    picture.pitch = info_.pitch / 4;
    picture.format = dxgk::is_rgba_order(info_.desc.format) ? graphics::Format::R8G8B8A8Unorm : graphics::Format::B8G8R8A8Unorm;
    const bool ok = picture.pixels.size() >= uint64_t{picture.pitch} * picture.height;
    if (ok) renderer.draw_image(picture, source, x, y);
    kernel_.end_read_export(*exported_);
    return ok;
  }

  bool read(const graphics::Rect2D& source, ArrayView<uint32_t> out) override {
    if (source.x < 0 || source.y < 0 || uint64_t(source.x) + source.width > info_.desc.width || uint64_t(source.y) + source.height > info_.desc.height ||
        out.size() < uint64_t{source.width} * source.height)
      return false;
    dxgk::LockResult view;
    if (kernel_.read_export(*exported_, &view) != status::kSuccess) return false;
    const bool swap = dxgk::is_rgba_order(info_.desc.format);
    bool ok = true;
    uint64_t at = 0;
    for (uint32_t row = 0; row < source.height && ok; ++row) {
      ConstByteView line;
      ok = ConstByteView(view.data).try_subview((uint64_t(source.y) + row) * info_.pitch + uint64_t(source.x) * 4, uint64_t{source.width} * 4, &line);
      for (uint32_t i = 0; ok && i < source.width; ++i) {
        const uint32_t b0 = line[i * 4], b1 = line[i * 4 + 1], b2 = line[i * 4 + 2], b3 = line[i * 4 + 3];
        out[at++] = swap ? (b3 << 24 | b0 << 16 | b1 << 8 | b2) : (b3 << 24 | b2 << 16 | b1 << 8 | b0);
      }
    }
    kernel_.end_read_export(*exported_);
    return ok;
  }

 private:
  dxgk::Kernel& kernel_;
  SharedPointer<process::OpenObject> object_;      // the descriptor (its DxgBufferFile has the CPU's copy, if it was mapped)
  SharedPointer<dxgk::ExportedAllocation> exported_;
  const dxgk::ExportInfo info_;
  const graphics::IRenderer2D* refused_ = nullptr;
};

class DxgTimeline final : public GpuTimeline {
 public:
  explicit DxgTimeline(SharedPointer<process::OpenObject> object) : object_(util::move(object)) {}
  uint64_t value() override { return dxgk::exported_sync_value(*object_->gpu_sync->exported()); }
  void signal(uint64_t point) override { dxgk::signal_exported_sync(*object_->gpu_sync->exported(), point); }

 private:
  SharedPointer<process::OpenObject> object_;      // (its DxgSyncFile holds the fence's export)
};

}  // namespace

SharedPointer<GpuTimeline> make_dxg_timeline(void* kernel, allocators::IAllocator& allocator, const SharedPointer<process::OpenObject>& object) {
  if (!kernel || !object || !object->gpu_sync || !object->gpu_sync->exported()) return SharedPointer<GpuTimeline>();
  return SharedPointer<GpuTimeline>::make_as<DxgTimeline>(allocator, object);
}

SharedPointer<GpuImage> make_dxg_image(void* kernel, allocators::IAllocator& allocator, const SharedPointer<process::OpenObject>& object) {
  if (!kernel || !object || !object->gpu_buffer || !object->gpu_buffer->exported()) return SharedPointer<GpuImage>();
  return SharedPointer<GpuImage>::make_as<DxgImage>(allocator, *static_cast<dxgk::Kernel*>(kernel), object);
}

}  // namespace wayland
