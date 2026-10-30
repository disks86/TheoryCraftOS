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

// The USB4 connection manager in software (tb.c, switch.c, path.c, tunnel.c and usb4.c of the Linux thunderbolt driver), for the host routers that have no firmware connection manager: Tiger Lake
// and everything after it. The host talks to the routers through the control channel's configuration space: a router has a header, capabilities and ports (adapters), each with its own space,
// and a tunnel is a pair of paths whose hops are entries in the hop tables of the adapters on the way.
//
// What it does: configures the host router, follows every lane adapter whose link is up to the router behind it (up to depth 5), reads their adapters, configures each link, and keeps the topology
// up to date from the hot plug events. A PCIe tunnel to a device router is made only by `approve_pcie()`, since it gives the device DMA access: nothing is tunnelled by itself, and tunnels the
// BIOS left active are switched off at start. Not done: USB 3 and DisplayPort tunnels (and so the bandwidth management), lane bonding, the DROM, the time management unit, CL states,
// retimers, other hosts on the cable (XDomain), the credit allocation of routers that have it. Everything is checked against a model of the routers only.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltXdomainHost.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltXdomainTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace usb4 {
// port types (the low 24 bits of dword 2 of a port's header)
constexpr uint32_t kTypeInactive = 0, kTypePort = 0x000001, kTypeNhi = 0x000002, kTypeDpIn = 0x0e0101, kTypeDpOut = 0x0e0102, kTypePcieDown = 0x100101, kTypePcieUp = 0x100102,
                   kTypeUsb3Down = 0x200101, kTypeUsb3Up = 0x200102;
// the lane adapter capability (LANE_ADP_CS_0 and 1, relative to the PHY capability) and the USB4 port capability
constexpr uint32_t kLaneCs0 = 0, kLaneCs0WidthShift = 20, kLaneCs0WidthMask = 0x3F, kLaneCs0WidthDual = 2, kLaneCs1 = 1, kLaneCs1TargetWidthMask = 3u << 4, kLaneCs1TargetSingle = 1u << 4,
                   kLaneCs1TargetDual = 3u << 4, kLaneCs1Lb = 1u << 15, kLaneCs1CurrentWidthShift = 20, kLaneCs1CurrentWidthMask = 0x3Fu << 20, kPortCs18 = 0x12, kPortCs18Be = 1u << 8;
// USB 3 adapters (ADP_USB3_CS_*, relative to the adapter capability)
constexpr uint32_t kUsb3Cs0Pe = 1u << 31, kUsb3Cs1 = 1, kUsb3Cs1Hca = 1u << 31, kUsb3Cs2 = 2, kUsb3Cs2Cmr = 1u << 31, kUsb3Cs3 = 3, kUsb3Cs4 = 4, kUsb3Cs4MslrShift = 12, kUsb3Cs4MslrMask = 0x7F, kUsb3Cs4Mslr20G = 1,
                   kUsb3Hop = 8, kUsb3Priority = 3, kUsb3Weight = 2;
// capabilities of a port (TB_PORT_CAP_*) and of a router
constexpr uint8_t kPortCapPhy = 0x01, kPortCapTime1 = 0x03, kPortCapAdapter = 0x04, kPortCapUsb4 = 0x06;
// the time management unit: the router capability (TB_SWITCH_CAP_TMU), its registers, the port registers
constexpr uint8_t kSwitchCapTmu = 0x03;
constexpr uint32_t kTmuRtrCs0 = 0, kTmuRtrCs0FreqWindShift = 16, kTmuRtrCs0FreqWindMask = 0x7FFu << 16, kTmuRtrCs0Td = 1u << 27, kTmuRtrCs0Ucap = 1u << 30, kTmuRtrCs1 = 1, kTmuRtrCs3 = 3, kTmuRtrCs3IntervalShift = 16,
                   kTmuRtrCs15 = 15, kTmuRtrCs18 = 18, kTmuRtrCs22 = 22, kTmuRtrCs24 = 24, kTmuRtrCs25 = 25, kTmuAdpCs3 = 3, kTmuAdpCs3Udm = 1u << 29, kTmuAdpCs6 = 6, kTmuAdpCs6Dts = 1u << 1,
                   kTmuLowResRate = 1000, kTmuLowResFreqWindow = 30, kTmuLowResAverage = 4;
// router configuration space
constexpr uint32_t kRouterCs1 = 1, kRouterCs5 = 5, kRouterCs6 = 6, kRouterCs7 = 7, kRouterCs9 = 9, kRouterCs25 = 25, kRouterCs26 = 26;
// router operations (ROUTER_CS_26): the opcode in the low half, OV (the operation is valid / still running), ONS (not supported) and the status
constexpr uint32_t kCs26Ov = 1u << 31, kCs26Ons = 1u << 30, kCs26StatusShift = 24, kCs26StatusMask = 0x3Fu << 24, kOpDromRead = 0x24, kOpBufferAlloc = 0x33, kOpDataDwords = 16;
constexpr uint32_t kCs5Pto = 1u << 25, kCs5Hco = 1u << 26, kCs5Slp = 1u << 0, kCs5Wop = 1u << 1, kCs5Wou = 1u << 2, kCs5Wod = 1u << 3, kCs5Cns = 1u << 23, kCs5Cv = 1u << 31, kCs6Slpr = 1u << 0, kCs6Hci = 1u << 18, kCs6Cr = 1u << 25;
// adapter registers (relative to the adapter capability, except ADP_CS_4 and ADP_CS_5 which are in the port header)
constexpr uint32_t kAdpCs4 = 4, kAdpCs4Lck = 1u << 31, kAdpCs5 = 5, kAdpCs5Dhp = 1u << 31, kPortCs19 = 0x13, kPortCs19Pc = 1u << 3, kPortCs19Pid = 1u << 4, kPortCs19Woc = 1u << 16, kPortCs19Wod = 1u << 17, kPortCs19Wou4 = 1u << 18, kAdpPcieCs0 = 0, kAdpPcieCs0Pe = 1u << 31;
// port states (the lane adapter's)
constexpr uint32_t kPortDisabled = 0, kPortConnecting = 1, kPortUp = 2, kPortTxCl0s = 3, kPortRxCl0s = 4, kPortCl1 = 5, kPortCl2 = 6, kPortUnplugged = 7;
// limits
constexpr uint32_t kMaxPorts = 64, kMaxRouters = 32, kMaxDepth = 5, kMaxHops = 8, kMaxTunnels = 16, kHopWords = 8, kMinPathHop = 8, kPcieHop = 8, kPcieCredits = 16, kPciePriority = 3, kPcieWeight = 1;
}  // namespace usb4

struct Usb4Port {
  uint8_t number = 0;
  uint32_t type = 0;
  uint8_t cap_phy = 0, cap_adapter = 0, cap_usb4 = 0, cap_tmu = 0;
  uint16_t max_in_hop = 0, max_out_hop = 0;
  bool disabled = false;
  uint8_t link_nr = 0;               // 0 for the first of two lane adapters, 1 for the second
  int8_t dual_link = -1;             // the other lane adapter of the link
  int16_t remote_router = -1;        // the router behind this lane adapter (index into the domain's list)
  uint64_t in_hops[usb4::kHopWords] = {}, out_hops[usb4::kHopWords] = {};   // HopIDs in use (up to 512 each)
  bool is_lane() const { return type == usb4::kTypePort; }
  bool bonded = false;               // both lanes of the link carry traffic as one
  bool xdomain = false;              // another host is on the cable behind this lane adapter (no router answers)
  uint16_t dma_credits = 0;          // credits reserved for DMA paths on this lane adapter
  uint16_t total_credits = 0, ctl_credits = 2;   // the buffers of the adapter, and what the control path uses of them
};

struct Usb4Router {
  bool present = false;
  uint64_t route = 0;
  uint8_t depth = 0, upstream_port = 0, max_port = 0;
  uint16_t vendor_id = 0, device_id = 0;
  uint8_t usb4_version = 0;          // major version (1, 2), 0 for a router of the Thunderbolt 3 kind
  uint64_t uid = 0;
  char vendor_name[48] = {}, device_name[48] = {};      // from the DROM
  bool drom = false;
  // The buffer allocation the router prefers (USB4_SWITCH_OP_BUFFER_ALLOC): the most credits a path of each kind may have on a lane adapter. Without it the fixed values are used.
  bool credit_allocation = false;
  uint16_t max_usb3_credits = 0, max_pcie_credits = 0, max_dma_credits = 0, min_dp_aux_credits = 0, min_dp_main_credits = 0;
  int16_t parent = -1;               // the router above it and the port of that one it hangs from
  uint8_t parent_port = 0;
  bool configured = false;
  uint8_t cap_tmu = 0;               // the router's time management unit capability (0: none)
  bool tmu_lowres = false;           // its time is kept in step with the host router's, uni-directionally at the low resolution
  bool pcie_tunnel = false;          // a PCIe tunnel made by this domain ends here
  bool usb3_tunnel = false;          // so does a USB 3 tunnel
  Usb4Port ports[usb4::kMaxPorts];
};

struct Usb4Hop {
  int16_t router = -1;
  uint8_t in_port = 0, out_port = 0;
  uint16_t in_hop = 0, out_hop = 0;  // out_hop is the HopID the next router's adapter gets as its input
  uint8_t credits = 0;
};

struct Usb4Path {
  uint8_t length = 0;
  uint8_t priority = 3, weight = 1;
  bool dma = false;                  // a DMA path: its credits are counted against the lane adapters
  uint8_t egress_fc_enable = 3, ingress_fc_enable = 7;     // which hops (source 1, internal 2, destination 4) have flow control out of / into them
  Usb4Hop hops[usb4::kMaxHops];
  bool activated = false;
};

enum class Usb4TunnelKind : uint8_t { Pcie, Usb3, Dma };

struct Usb4Tunnel {
  bool used = false;
  Usb4TunnelKind kind = Usb4TunnelKind::Pcie;
  int16_t device_router = -1;        // the router at the far end (its PCIe up adapter)
  int16_t host_router = -1;          // where the PCIe down adapter is
  uint8_t down_port = 0, up_port = 0;
  Usb4Path down, up;                 // down: from the down adapter to the up adapter; up: back (a DMA tunnel: down is the path we send on, up the one we receive on)
  uint64_t xdomain_route = 0;        // a DMA tunnel: the route of the other host
  bool active = false;
};

class Usb4Domain final : public IThunderboltEvents, public IXdomainHost, private IXdomainPaths {
 public:
  Usb4Domain(TbControl& control, TbNhi& nhi, allocators::IAllocator& allocator) : control_(control), nhi_(nhi), allocator_(allocator), routers_(allocator), xd_(control, allocator, this, *this) {}
  ~Usb4Domain() = default;
  Usb4Domain(const Usb4Domain&) = delete;
  Usb4Domain& operator=(const Usb4Domain&) = delete;

  // Configures the host router and scans everything below it. False, with `*error` saying why, if the host router cannot be reached or is not a router we know how to run.
  bool start(const char** error);
  // Handles the hot plug events that have arrived (and the control channel's own packets); call regularly.
  void poll();

  uint32_t router_count() const { return static_cast<uint32_t>(routers_.count()); }
  const Usb4Router* router(uint32_t index) const { return index < routers_.count() && routers_[index] && routers_[index]->present ? routers_[index].get() : nullptr; }
  // Routers that are present, the host router included.
  uint32_t present_count() const;
  // PCIe tunnels the BIOS left active, which start() switched off.
  uint32_t preboot_tunnels_closed() const { return preboot_closed_; }

  // The PCIe tunnel to a device router (not the host): the router is told it may tunnel PCIe (and its xHCI over it), the down adapter of the host side that matches the link is found, and the
  // two paths are written hop by hop from the far end back and then the adapters are enabled. This gives the device DMA access to memory: nothing but this call does it.
  bool approve_pcie(uint32_t router_index, const char** error);
  // Disables the router's PCIe tunnel (the adapters first, then the paths).
  bool disconnect_pcie(uint32_t router_index);
  bool pcie_tunnelled(uint32_t router_index) const;
  // The USB 3 tunnel to a device router: the same kind of paths (HopID 8, priority 3, weight 2) between its USB 3 up adapter and the matching down adapter of the parent, with the bandwidth
  // allocated for the host router's down adapter (90% of the slower adapter's link rate). A USB 3 host controller on the device becomes reachable: also only on request.
  bool approve_usb3(uint32_t router_index, const char** error);
  bool disconnect_usb3(uint32_t router_index);
  bool usb3_tunnelled(uint32_t router_index) const;
  // Is the link above this router running both lanes as one?
  bool link_bonded(uint32_t router_index) const;

  // Sleep (tb_suspend): every tunnel is taken down and each router told to sleep, the ones farthest from the host first (ROUTER_CS_5.SLP, waiting for SLPR), and the topology is forgotten.
  // resume() starts afresh - the host router is configured again and everything below it found again, the links having come up on their own - so no tunnel is restored: they are approved
  // again. False if the host router would not go to sleep (the topology is forgotten all the same, and resume() works).
  // `runtime`: a short sleep while the machine runs (only with nothing tunnelled and no other host on the cable): the routers are told to wake the host for a device that is plugged in or
  // pulled out, a link that comes up as USB4, and PCIe, USB 3 or DisplayPort traffic (usb4_switch_set_wake with all the flags).
  bool suspend(bool runtime = false);
  bool resume(const char** error);
  bool runtime_allowed() const;
  bool suspended() const { return suspended_; }

  // Other hosts on the cable (IXdomainHost): a lane adapter whose link is up and behind which no router answers is taken for one, its UUID and properties are asked for in poll(), the
  // requests it makes are answered, and the data path for ThunderboltIP is a DMA tunnel from our host interface to that adapter (two paths: the HopIDs the service chose on the host
  // interface and on the cable). Services (ThunderboltNet) run on top of this exactly as on the firmware connection manager.
  TbControl& control() override { return control_; }
  TbNhi& nhi() override { return nhi_; }
  IThunderboltEvents* event_sink() override { return this; }
  uint64_t xdomain_count() const override { return xd_.count(); }
  const TbXDomain& xdomain(uint64_t index) const override { return xd_.at(index); }
  void set_ip_handler(IThunderboltIpHandler* handler) override { xd_.set_ip_handler(handler); }
  bool offer_network(uint32_t flags) override { return xd_.offer_network(flags); }
  // Hosts on the cable now (xdomain_count() is the size of the list, which keeps the empty places of hosts that left).
  uint32_t xdomains_present() const { return xd_.live(); }
  bool set_identity(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name) { return xd_.set_identity(vendor_id, device_id, device_revision, vendor_name, device_name); }
  bool approve_xdomain_paths(uint64_t index, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) override {
    return xd_.approve(index, transmit_path, transmit_ring, receive_path, receive_ring, error);
  }
  bool disconnect_xdomain_paths(uint64_t index) override { return xd_.disconnect(index); }
  void ring_interrupts(bool transmit, uint32_t hop, bool on) override { if (interrupts_ || !on) nhi_.set_ring_interrupts(transmit, hop, on, data_vector_ >= 0 ? data_vector_ : msix_vector_); }
  // The control channel's interrupts (and the data rings', through ring_interrupts) as ThunderboltIcm::use_interrupts does it.
  bool use_interrupts(bool on, int msix_vector = -1, int data_vector = -1);

  // IThunderboltEvents: hot plug events are queued, XDomain packets go to the hosts' side, everything else ignored.
  bool on_event(TbPacketType type, ConstByteView body) override;

  // Numbers that tests and the log want.
  uint32_t events_seen() const { return events_seen_; }
  uint32_t unreachable_links() const { return unreachable_; }
  // Reads and parses a router's DROM (names, which ports are unused, which lane adapters belong together); a router without one keeps the defaults. Done for every router found.
  bool read_drom(Usb4Router& r);
  bool read_buffer_allocation(Usb4Router& r);
  // Time synchronisation (tmu.c, uni-directional low resolution - the mode the Linux driver starts in): the host router is asked for its rate, each router below it gets the parameters, the
  // adapters on both ends of its link are set to uni-directional with time sync on, and its local time is first set from the host router's so that it starts near it.
  bool tmu_enabled(uint32_t router_index) const { const Usb4Router* r = router(router_index); return r && r->tmu_lowres; }

 private:
  struct Plug {
    uint64_t route = 0;
    uint8_t port = 0;
    bool unplug = false;
  };
  static constexpr uint32_t kPlugQueue = 16, kTimeoutUs = 500000;

  bool read(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<uint32_t> words, uint8_t* response_port = nullptr);
  bool write(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<const uint32_t> words);
  bool port_read(const Usb4Router& r, uint32_t port, uint32_t offset, ArrayView<uint32_t> words) { return read(r.route, port, TbConfigSpace::Port, offset, words); }
  bool port_write(const Usb4Router& r, uint32_t port, uint32_t offset, ArrayView<const uint32_t> words) { return write(r.route, port, TbConfigSpace::Port, offset, words); }
  int find_port_cap(const Usb4Router& r, uint32_t port, uint8_t first, uint8_t id);

  int allocate_router();
  bool read_router(uint64_t route, int16_t parent, uint8_t parent_port, int* index, const char** error, bool* silent = nullptr);
  bool configure_router(Usb4Router& r);
  bool init_ports(Usb4Router& r);
  void link_lane_adapters(Usb4Router& r);
  bool port_up(const Usb4Router& r, uint32_t port);
  // A router operation (usb4_native_switch_op): metadata and data in, the opcode written, the answer waited for. `status` is the operation's own result; false if the router does not support it.
  bool router_op(const Usb4Router& r, uint32_t opcode, uint32_t* metadata, uint8_t* status, const uint32_t* tx, uint32_t tx_dwords, uint32_t* rx, uint32_t rx_dwords);
  bool drom_read(const Usb4Router& r, uint32_t address, ByteView out);
  int find_router_cap(const Usb4Router& r, uint8_t id);
  uint32_t available_credits(const Usb4Router& r, const Usb4Port& lane) const;
  bool lane_uses_allocation(const Usb4Router& r, const Usb4Port& p) const { return p.is_lane() && r.credit_allocation; }
  bool tmu_set_time_disruption(const Usb4Router& r, bool set);
  bool tmu_port_write(const Usb4Router& r, uint32_t port, uint32_t offset, uint32_t mask, uint32_t value);
  bool tmu_rate_write(const Usb4Router& r, uint32_t rate);
  bool tmu_post_time(const Usb4Router& child, const Usb4Router& host);
  bool tmu_enable_root(Usb4Router& root);
  bool tmu_enable(Usb4Router& child, Usb4Router& parent, const Usb4Router& root, uint32_t down_port);
  void scan_router(int index);
  void scan_port(int index, uint32_t port);
  void remove_router(int index);
  void remove_below(int index, uint32_t port);
  bool unlock_downstream(const Usb4Router& r, uint32_t port);
  bool configure_link(Usb4Router& child, Usb4Router& parent, uint32_t down_port);
  bool configuration_valid(Usb4Router& r);
  bool switch_setup(Usb4Router& child, const Usb4Router& parent);
  void close_preboot_tunnels();
  bool put_to_sleep(Usb4Router& r, bool runtime);
  bool set_wake(Usb4Router& r, bool runtime);
  void process_plug(const Plug& plug);
  void ack_plug(const Plug& plug);
  int find_router(uint64_t route) const;

  // paths
  int port_pcie_down_for(const Usb4Router& host, uint32_t lane_port, uint32_t type);
  int allocate_hop(Usb4Router& r, uint32_t port, bool in, int min_hop, int max_hop);
  void release_hop(Usb4Router& r, uint32_t port, bool in, uint32_t hop);
  bool build_path(Usb4Path* path, int start_router, uint32_t start_port, int end_router, uint32_t end_port, Usb4TunnelKind kind);
  void release_path(Usb4Path* path);
  bool activate_path(Usb4Path* path);
  void deactivate_path(Usb4Path* path);
  bool write_hop(const Usb4Router& r, uint32_t port, uint32_t hop_index, const Usb4Hop& hop, bool first, bool last, const Usb4Path& path);
  bool disable_hop(const Usb4Router& r, uint32_t port, uint32_t hop_index);
  bool set_pcie_enable(const Usb4Router& r, uint32_t port, bool enable);
  int find_tunnel(int router_index, Usb4TunnelKind kind = Usb4TunnelKind::Pcie) const;
  bool make_tunnel(uint32_t router_index, Usb4TunnelKind kind, const char** error);
  bool remove_tunnel(int slot);
  bool set_adapter_enable(const Usb4Router& r, uint32_t port, Usb4TunnelKind kind, bool enable);
  bool usb3_allocate_bandwidth(const Usb4Router& host, uint32_t down_port, const Usb4Router& device, uint32_t up_port);
  bool bond_link(Usb4Router& child, Usb4Router& parent, uint32_t down_port);
  int lane_width(const Usb4Router& r, uint32_t port);
  bool set_lane_width(const Usb4Router& r, uint32_t port, uint32_t target);

  // IXdomainPaths
  bool open_paths(const TbXDomain& xd, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) override;
  void close_paths(const TbXDomain& xd) override;
  void add_xdomain(int router_index, uint32_t port, uint64_t route);
  void remove_xdomain(int router_index, uint32_t port);
  bool take_hop(Usb4Router& r, uint32_t port, bool in, uint32_t hop);
  bool build_dma_path(Usb4Path* path, const int* chain, uint32_t chain_length, bool toward_host, uint32_t xdomain_port, uint32_t nhi_port, uint32_t ring, uint32_t cable_hop);

  TbControl& control_;
  TbNhi& nhi_;
  bool interrupts_ = false;
  int msix_vector_ = -1, data_vector_ = -1;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Usb4Router>> routers_;
  TbXdomainHost xd_;
  Usb4Tunnel tunnels_[usb4::kMaxTunnels];
  Plug plugs_[kPlugQueue];
  uint32_t plug_head_ = 0, plug_count_ = 0;
  uint32_t preboot_closed_ = 0, events_seen_ = 0, unreachable_ = 0;
  bool started_ = false, suspended_ = false;
};

}  // namespace hardware
