"""Bounded localhost TLS/WebSocket/PRUDPLite probe for the observed auth path.

SYN/CONNECT here establish an anonymous diagnostic transport, never an account
or secure-server session. RMC requests receive an explicit unsupported-method
error. Authentication/tickets/matchmaking/Pia are NOT implemented by this probe.
"""
import base64
import hashlib
import ipaddress
import secrets
import socketserver
import ssl
import struct
import threading

from .wire import LitePacket, decode_lite, decode_request, response, ACK, NEED_ACK, DATA, SYN, CONNECT, PING, DISCONNECT, MAX_MESSAGE


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size-len(data))
        if not part:
            raise EOFError()
        data.extend(part)
    return bytes(data)


def frame(payload, opcode=2):
    n = len(payload)
    if n > MAX_MESSAGE:
        raise ValueError('WebSocket output limit')
    header = bytes((128 | opcode, n)) if n < 126 else bytes((128 | opcode, 126)) + struct.pack('!H', n) if n < 65536 else bytes((128 | opcode, 127)) + struct.pack('!Q', n)
    return header + payload


def read_frame(sock):
    first, second = read_exact(sock, 2)
    fin, opcode = bool(first & 128), first & 15
    if first & 112 or not second & 128 or opcode not in (0, 1, 2, 8, 9, 10):
        raise ValueError('invalid WebSocket client frame')
    size = second & 127
    if size == 126:
        size, = struct.unpack('!H', read_exact(sock, 2))
        if size < 126:
            raise ValueError('noncanonical WebSocket size')
    elif size == 127:
        size, = struct.unpack('!Q', read_exact(sock, 8))
        if size < 65536:
            raise ValueError('noncanonical WebSocket size')
    if size > MAX_MESSAGE or opcode >= 8 and (not fin or size > 125):
        raise ValueError('WebSocket frame limit')
    mask = read_exact(sock, 4)
    data = read_exact(sock, size)
    return fin, opcode, bytes(b ^ mask[i & 3] for i, b in enumerate(data))


class Probe:
    def __init__(self, cfg, emit):
        self.emit = emit
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.set_alpn_protocols(['http/1.1'])
        self.context.load_cert_chain(cfg['tls_certificate'], cfg['tls_key'])
        self.context.set_servername_callback(lambda _s, name, _c: emit('tls-sni', hostname=name))
        gate = threading.BoundedSemaphore(16)
        owner = self
        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
            allow_reuse_address = False
            block_on_close = False
            def process_request(self, request, client_address):
                if not gate.acquire(False):
                    self.shutdown_request(request)
                    return
                try:
                    super().process_request(request, client_address)
                except BaseException:
                    gate.release()
                    raise
            def process_request_thread(self, request, client_address):
                try:
                    super().process_request_thread(request, client_address)
                finally:
                    gate.release()
        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                if not ipaddress.ip_address(self.client_address[0]).is_loopback:
                    return
                self.request.settimeout(5)
                try:
                    with owner.context.wrap_socket(self.request, server_side=True) as sock:
                        owner.connection(sock, self.client_address)
                except (OSError, ValueError, EOFError, ssl.SSLError) as e:
                    emit('websocket-closed', endpoint=list(self.client_address), reason=type(e).__name__)
        self.server = Server((cfg['bind'], cfg['tls_port']), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .05}, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join(2)

    def connection(self, sock, endpoint):
        # Read a bounded header without consuming any following WebSocket bytes.
        data = bytearray()
        while not data.endswith(b'\r\n\r\n'):
            if len(data) == 16384:
                raise ValueError('HTTP header limit')
            data.extend(read_exact(sock, 1))
        lines = data.decode('ascii').split('\r\n')
        method, path, version = lines[0].split(' ')
        headers = {}
        for line in lines[1:]:
            if not line:
                continue
            key, value = line.split(':', 1);key = key.lower()
            if key in headers:
                raise ValueError('duplicate HTTP header')
            headers[key] = value.strip()
        self.emit('http-request', endpoint=list(endpoint), method=method, path=path.split('?')[0], header_names=sorted(headers))
        if method != 'GET' or version != 'HTTP/1.1' or headers.get('upgrade', '').lower() != 'websocket' or headers.get('sec-websocket-version') != '13':
            sock.sendall(b'HTTP/1.1 501 Not Implemented\r\nConnection: close\r\nContent-Length: 0\r\n\r\n')
            return
        if 'upgrade' not in [s.strip().lower() for s in headers.get('connection', '').split(',')]:
            raise ValueError('missing upgrade token')
        key = headers.get('sec-websocket-key', '')
        self.emit('websocket-key-shape', length=len(key), public_nonce_hex=key.encode().hex() if len(key)<=64 else None)
        # The supplied game's NEX client sends the literal "NEX" key, not
        # RFC6455's browser nonce. main+965c4 supplies the handshake object's
        # key to the format at +c47c92; pair10 records public_nonce_hex=4e4558.
        # Permit only that exact NEX variant or a canonical 16-byte RFC nonce.
        # Neither form grants account authentication; TLS is still mandatory.
        if key != 'NEX' and len(base64.b64decode(key, validate=True)) != 16:
            raise ValueError('invalid WebSocket key')
        accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        reply = b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n'
        protocols = headers.get('sec-websocket-protocol', '').split(',')
        if protocols[0].strip():
            protocol = protocols[0].strip()
            if len(protocol) > 128 or any(not (ch.isalnum() or ch in '-_.') for ch in protocol):
                raise ValueError('invalid subprotocol')
            reply += b'Sec-WebSocket-Protocol: '+protocol.encode()+b'\r\n'
        sock.sendall(reply+b'\r\n')
        pending, cookie, connected, outseq, received = bytearray(), secrets.token_bytes(16), False, 1, 0
        while received < 4096:
            fin, opcode, payload = read_frame(sock);received += 1
            if opcode == 8:
                sock.sendall(frame(payload, 8));return
            if opcode == 9:
                sock.sendall(frame(payload, 10));continue
            if opcode == 10:
                continue
            if opcode == 1 or opcode == 0 and not pending or opcode == 2 and pending:
                raise ValueError('expected binary PRUDPLite message')
            pending.extend(payload)
            if len(pending) > MAX_MESSAGE:
                raise ValueError('fragmented WebSocket message limit')
            if not fin:
                continue
            for p in decode_lite(bytes(pending)):
                self.emit('prudp-lite', endpoint=list(endpoint), kind=p.kind, flags=p.flags, session=p.session,
                          sequence=p.sequence, source=p.source, destination=p.destination,
                          options=[(k, v.hex()) for k, v in p.options], payload_bytes=len(p.payload))
                types = (p.stream_types >> 4) | ((p.stream_types & 15) << 4)
                def send(flags, seq, options=(), body=b''):
                    sock.sendall(frame(LitePacket(types, p.destination, p.source, p.session, flags, seq, options, body).encode()))
                if p.flags & ACK:
                    continue
                if p.kind == SYN:
                    version=dict(p.options).get(0,bytes(4))
                    if len(version)!=4:raise ValueError('invalid SYN version option')
                    send(SYN | ACK, p.sequence, ((0, version), (1, cookie)))
                elif p.kind == CONNECT:
                    connected = True
                    send(CONNECT | ACK, p.sequence)
                    self.emit('anonymous-transport', endpoint=list(endpoint), authenticated_account=False)
                elif p.kind in (PING, DISCONNECT):
                    send(p.kind | ACK, p.sequence)
                    if p.kind == DISCONNECT:
                        return
                elif p.kind == DATA and connected:
                    if p.flags & NEED_ACK:
                        send(DATA | ACK, p.sequence)
                    try:
                        request = decode_request(p.payload)
                    except ValueError:
                        self.emit('rmc-undecoded', payload_bytes=len(p.payload))
                        continue
                    self.emit('rmc-unsupported', protocol=request.protocol, method=request.method, call=request.call,
                              parameter_bytes=len(request.parameters), authenticated_account=False)
                    send(DATA | NEED_ACK | 0x20, outseq, ((2, b'\0'),), response(request, error=0x80010002))
                    outseq = (outseq+1) & 65535
            pending.clear()
