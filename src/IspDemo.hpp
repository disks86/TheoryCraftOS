#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "hardware/ImageSignalProcessor/IImageSignalProcessor.hpp"

// Exercises an ISP on a synthetic sensor frame: renders colour bars as a raw
// Bayer mosaic with a colour cast, then draws the ISP's output twice in the
// top-right corner of the screen - once with neutral settings and once with
// the white balance the ISP measured. Returns false if there is no room or
// memory for it.
bool run_isp_demo(hardware::IGraphicsProcessingUnit& gpu, hardware::IImageSignalProcessor& isp,
                  allocators::IAllocator& allocator);
