#pragma once
#include "hardware/IHardware.hpp"
#include "hardware/NetworkInterface/NetworkTypes.hpp"

namespace hardware {

// An Ethernet network interface. It moves raw frames - destination MAC, source
// MAC, EtherType, payload - and knows nothing about protocols above that.
// Polled: nothing blocks, and received frames queue up inside the interface
// until receive() collects them. Implementations live in subfolders by
// vendor/transport (Virtio/, Intel/, Realtek/) plus a UEFI fallback.
class INetworkInterface : public IHardware {
 public:
  // The interface's own hardware address.
  virtual MacAddress mac_address() const = 0;
  // True while a cable is connected / the link is up.
  virtual bool link_up() const = 0;

  // Queues one frame for sending, copying it before returning. Frames shorter
  // than kEthernetMinFrame are padded. False if the frame is empty or longer
  // than kEthernetMaxFrame, or there is no room in the transmit queue right now
  // (try again after receiving or a short wait).
  virtual bool transmit(const uint8_t* frame, uint32_t length) = 0;
  // Copies the oldest received frame into `buffer` (at most `capacity` bytes,
  // the rest is dropped) and stores its full length in *length. False when no
  // frame is waiting.
  virtual bool receive(uint8_t* buffer, uint32_t capacity, uint32_t* length) = 0;

  virtual NetworkStatistics statistics() const = 0;

 protected:
  constexpr INetworkInterface() = default;
  ~INetworkInterface() = default;
};

}  // namespace hardware
