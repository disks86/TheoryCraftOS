# A UDP forwarder that loses, duplicates and delays (so reorders) datagrams, for trying QUIC over a bad network.
#   python3 udp_chaos_proxy.py LISTEN_PORT TARGET_PORT LOSS DUPLICATE MAX_DELAY_MS [SEED]
# LOSS and DUPLICATE are probabilities (0.05 = one in twenty); delays are random up to MAX_DELAY_MS for each datagram.
import asyncio, random, sys

listen, target, loss, dup, delay = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]) / 1000
rng = random.Random(int(sys.argv[6]) if len(sys.argv) > 6 else 1)

class Upstream(asyncio.DatagramProtocol):   # towards the server, one per client
    def __init__(self, front, client): self.front, self.client = front, client
    def connection_made(self, transport): self.transport = transport
    def datagram_received(self, data, addr): chaos(lambda: self.front.sendto(data, self.client), data)

def chaos(send, data):
    if rng.random() < loss: return
    for _ in range(2 if rng.random() < dup else 1):
        d = rng.random() * delay
        if d > 0.0005: asyncio.get_running_loop().call_later(d, send)
        else: send()

class Front(asyncio.DatagramProtocol):
    def __init__(self): self.clients = {}
    def connection_made(self, transport): self.transport = transport
    def sendto(self, data, addr): self.transport.sendto(data, addr)
    def datagram_received(self, data, addr):
        if addr not in self.clients:
            self.clients[addr] = asyncio.ensure_future(asyncio.get_running_loop().create_datagram_endpoint(lambda: Upstream(self, addr), remote_addr=("127.0.0.1", target)))
        async def go():
            transport, _ = await self.clients[addr]
            chaos(lambda: transport.sendto(data), data)
        asyncio.ensure_future(go())

async def main():
    await asyncio.get_running_loop().create_datagram_endpoint(Front, local_addr=("127.0.0.1", listen))
    await asyncio.Event().wait()
asyncio.run(main())
