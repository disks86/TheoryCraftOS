#pragma once
#include "hardware/NetworkInterface/INetworkInterface.hpp"
#include "uefi.hpp"

// Sends an ARP "who has 10.0.2.2?" request out of the interface (the gateway
// address of QEMU's built-in user-mode network) and waits up to two seconds for
// the answer, logging what it sees. Returns true if the gateway answered, which
// shows that both sending and receiving work. On a network that is not
// 10.0.2.0/24 there is simply no answer.
bool run_network_probe(hardware::INetworkInterface& nic, efi::SystemTable* st);
