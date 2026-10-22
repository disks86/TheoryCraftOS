# An HTTP/3 server (aioquic) and, on the same port number over TCP, an HTTPS server that announces it with Alt-Svc, for tests/http3_live.sh.
#   python3 http3_test_server.py PORT TCP_ONLY_PORT CERT KEY
import asyncio, gzip, http.server, json, socketserver, ssl, sys, threading
from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated

port, tcp_only_port, certfile, keyfile = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
BIG = b"".join(b"line %d of a big body\n" % i for i in range(150000))

class Server(QuicConnectionProtocol):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.h3 = None
        self.requests = {}

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            self.h3 = H3Connection(self._quic)
        if self.h3 is None:
            return
        for e in self.h3.handle_event(event):
            if isinstance(e, HeadersReceived):
                self.requests[e.stream_id] = {"headers": e.headers, "body": b""}
                if e.stream_ended:
                    self.respond(e.stream_id)
            elif isinstance(e, DataReceived):
                self.requests[e.stream_id]["body"] += e.data
                if e.stream_ended:
                    self.respond(e.stream_id)

    def reply(self, sid, status, body, headers=(), end=True):
        self.h3.send_headers(sid, [(b":status", str(status).encode()), *headers, (b"content-length", str(len(body)).encode())])
        self.h3.send_data(sid, body, end_stream=True)
        self.transmit()

    def respond(self, sid):
        req = self.requests.pop(sid)
        h = {k.decode(): v.decode() for k, v in req["headers"]}
        path = h[":path"]
        if path == "/plain": self.reply(sid, 200, b"hello over h3\n", [(b"content-type", b"text/plain")])
        elif path == "/big": self.reply(sid, 200, BIG)
        elif path == "/gzip": self.reply(sid, 200, gzip.compress(BIG[:200000]), [(b"content-encoding", b"gzip")])
        elif path == "/redirect": self.reply(sid, 302, b"", [(b"location", b"/plain")])
        elif path == "/missing": self.reply(sid, 404, b"no such page\n")
        elif path == "/nolength":
            self.h3.send_headers(sid, [(b":status", b"200")])
            self.h3.send_data(sid, b"no length here\n", end_stream=True); self.transmit()
        elif path == "/empty": self.h3.send_headers(sid, [(b":status", b"204")], end_stream=True); self.transmit()
        elif path == "/echo":
            self.reply(sid, 200, h[":method"].encode() + b" " + req["body"] + b" as " + h.get("content-type", "").encode())
        elif path == "/headers":
            self.reply(sid, 200, json.dumps({k: v for k, v in h.items()}, sort_keys=True).encode(), [(b"x-many", b"a" * 3000), (b"set-cookie", b"a=1"), (b"set-cookie", b"b=2")])
        elif path == "/goodbye":   # the answer, and the connection closed at once (H3_NO_ERROR)
            self.reply(sid, 200, b"closing\n")
            self._quic.close(error_code=0x100)
            self.transmit()
        elif path == "/reset": self._quic.reset_stream(sid, 0x10b); self.transmit()   # H3_REQUEST_REJECTED
        else: self.reply(sid, 404, b"unknown\n")

class TcpHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        body = b"over tcp\n"
        self.send_response(200)
        if self.server.server_address[1] == port:
            self.send_header("Alt-Svc", 'h3=":%d"; ma=60' % port)
        self.send_header("Content-Length", str(len(body))); self.send_header("Connection", "close"); self.end_headers()
        self.wfile.write(body); self.close_connection = True

class Tcp(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True

def tcp_server(p):
    s = Tcp(("127.0.0.1", p), TcpHandler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(certfile, keyfile)
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
    threading.Thread(target=s.serve_forever, daemon=True).start()

async def main():
    tcp_server(port); tcp_server(tcp_only_port)
    cfg = QuicConfiguration(is_client=False, alpn_protocols=["h3"])
    cfg.load_cert_chain(certfile, keyfile)
    tickets = {}
    await serve("127.0.0.1", port, configuration=cfg, create_protocol=Server,
                session_ticket_fetcher=lambda label: tickets.pop(label, None), session_ticket_handler=lambda t: tickets.__setitem__(t.ticket, t))
    await asyncio.Event().wait()

asyncio.run(main())
