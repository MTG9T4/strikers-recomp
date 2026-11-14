"""Bounded PRUDP v1 framing, RMC framing and transport primitives.

Wire layout is independently read from main+77474, +778c4, +77c44.
This module does not assert the game's packet signatures or ticket encryption
are verified. A packet's successful parse is NOT authenticated delivery.
"""
from dataclasses import dataclass
import struct
import zlib

MAGIC = b'\xea\xd0'
HEADER = struct.Struct('<2sBBHBBHBBH16s')
MAX_DATAGRAM = 65507
MAX_MESSAGE = 256 * 1024
SYN, CONNECT, DATA, DISCONNECT, PING = range(5)
ACK, RELIABLE, NEED_ACK = 0x10, 0x20, 0x40


@dataclass(frozen=True)
class Packet:
    source: int
    destination: int
    flags: int
    session: int
    channel: int
    sequence: int
    signature: bytes = bytes(16)
    options: tuple = ()
    payload: bytes = b''

    @property
    def kind(self):
        return self.flags & 15

    def encode(self):
        options = bytearray()
        for kind, value in self.options:
            if not 0 <= kind <= 255 or len(value) > 255:
                raise ValueError('invalid option')
            options.extend(bytes((kind, len(value))) + value)
        if len(options) > 255 or len(self.payload) > MAX_DATAGRAM - HEADER.size - len(options):
            raise ValueError('packet too large')
        return HEADER.pack(MAGIC, 1, len(options), len(self.payload), self.source,
                           self.destination, self.flags, self.session, self.channel,
                           self.sequence, self.signature) + options + self.payload


def decode_datagram(data):
    """All coalesced packets, or reject the entire datagram without side effects."""
    if not 0 < len(data) <= MAX_DATAGRAM:
        raise ValueError('invalid datagram length')
    packets = []
    offset = 0
    while offset < len(data):
        if len(packets) == 64 or len(data) - offset < HEADER.size:
            raise ValueError('too many or truncated packets')
        magic, version, option_size, size, src, dst, flags, session, channel, seq, sig = HEADER.unpack_from(data, offset)
        if magic != MAGIC or version != 1 or flags & 15 > 7:
            raise ValueError('invalid PRUDP v1 header')
        end_options = offset + HEADER.size + option_size
        end = end_options + size
        if end > len(data):
            raise ValueError('truncated options or payload')
        pos, options, seen = offset + HEADER.size, [], set()
        while pos < end_options:
            if pos + 2 > end_options:
                raise ValueError('truncated option header')
            kind, length = data[pos:pos+2]
            pos += 2
            if pos + length > end_options or kind in seen:
                raise ValueError('truncated or duplicate option')
            seen.add(kind)
            options.append((kind, bytes(data[pos:pos+length])))
            pos += length
        packets.append(Packet(src, dst, flags, session, channel, seq, sig, tuple(options), bytes(data[end_options:end])))
        offset = end
    return packets


class RC4:
    """Continuous stream state; retransmissions must reuse original ciphertext."""
    def __init__(self, key):
        if not 1 <= len(key) <= 256:
            raise ValueError('invalid RC4 key')
        self.s = list(range(256))
        j = 0
        for i in range(256):
            j = (j + self.s[i] + key[i % len(key)]) & 255
            self.s[i], self.s[j] = self.s[j], self.s[i]
        self.i = self.j = 0

    def apply(self, data):
        out = bytearray()
        for b in data:
            self.i = (self.i + 1) & 255
            self.j = (self.j + self.s[self.i]) & 255
            self.s[self.i], self.s[self.j] = self.s[self.j], self.s[self.i]
            out.append(b ^ self.s[(self.s[self.i] + self.s[self.j]) & 255])
        return bytes(out)


def inflate_bounded(data, limit=MAX_MESSAGE):
    if not 0 < limit <= MAX_MESSAGE:
        raise ValueError('invalid decompression limit')
    codec = zlib.decompressobj()
    out = codec.decompress(data, limit + 1)
    if len(out) > limit or codec.unconsumed_tail or not codec.eof or codec.unused_data:
        raise ValueError('oversize, truncated or trailing compressed stream')
    return out


class ReceiveWindow:
    """Bounded per-channel sequence ordering, including u16 wrap and duplicates.

    Authenticate before calling. Cipher/decompression state advances only when
    packets are returned in order, never when an out-of-order packet arrives.
    """
    def __init__(self, sequence=1, capacity=64):
        self.sequence, self.capacity, self.pending = sequence, capacity, {}

    def accept(self, packet):
        distance = (packet.sequence - self.sequence) & 65535
        if distance >= 32768:
            return []  # already delivered; caller can repeat ACK
        if distance >= self.capacity:
            raise ValueError('receive window exceeded')
        if packet.sequence in self.pending:
            if packet != self.pending[packet.sequence]:
                raise ValueError('conflicting duplicate')
            return []
        self.pending[packet.sequence] = packet
        result = []
        while self.sequence in self.pending:
            result.append(self.pending.pop(self.sequence))
            self.sequence = (self.sequence + 1) & 65535
        return result


class Fragments:
    """Fragment option 2: nonzero fragment indices, zero terminates a message.

    Used after sequence ordering. Timeout/peer teardown must call clear().
    """
    def __init__(self):
        self.clear()

    def clear(self):
        self.next_index, self.data = 1, bytearray()

    def append(self, index, payload):
        if not 0 <= index <= 255 or len(self.data) + len(payload) > MAX_MESSAGE:
            self.clear()
            raise ValueError('fragment limit')
        if index and index != self.next_index:
            self.clear()
            raise ValueError('fragment order')
        self.data.extend(payload)
        if not index:
            out = bytes(self.data)
            self.clear()
            return out
        self.next_index += 1
        return None


@dataclass(frozen=True)
class Request:
    protocol: int
    call: int
    method: int
    parameters: bytes


def decode_request(data):
    if len(data) < 13 or len(data) > MAX_MESSAGE:
        raise ValueError('invalid RMC size')
    size, = struct.unpack_from('<I', data)
    if size != len(data) - 4 or not data[4] & 0x80:
        raise ValueError('invalid RMC request')
    protocol, pos = data[4] & 127, 5
    if protocol == 127:
        if len(data) < 15:
            raise ValueError('truncated extended protocol')
        protocol, = struct.unpack_from('<H', data, pos)
        pos += 2
    call, method = struct.unpack_from('<II', data, pos)
    if method & 0x8000:
        raise ValueError('response passed as request')
    return Request(protocol, call, method, bytes(data[pos+8:]))


def response(request, parameters=b'', error=None):
    p = bytes((request.protocol,)) if request.protocol < 127 else b'\x7f' + struct.pack('<H', request.protocol)
    if error is None:
        body = p + b'\x01' + struct.pack('<II', request.call, request.method | 0x8000) + parameters
    else:
        body = p + b'\x00' + struct.pack('<II', error, request.call)
    if len(body) > MAX_MESSAGE - 4:
        raise ValueError('RMC response too large')
    return struct.pack('<I', len(body)) + body


LITE_HEADER = struct.Struct('<BBHBBBBHH')


@dataclass(frozen=True)
class LitePacket:
    """PRUDPLite v1 in TLS WebSocket (main+78c74, +79298)."""
    stream_types: int
    source: int
    destination: int
    session: int
    flags: int
    sequence: int
    options: tuple = ()
    payload: bytes = b''

    @property
    def kind(self):
        return self.flags & 15

    def encode(self):
        options = b''.join(bytes((k, len(v))) + v for k, v in self.options)
        if len(options) > 255 or len(self.payload) > 65535:
            raise ValueError('oversize PRUDPLite frame')
        return LITE_HEADER.pack(128, len(options), len(self.payload), self.stream_types,
                                self.source, self.destination, self.session,
                                self.flags, self.sequence) + options + self.payload


def decode_lite(data):
    if not LITE_HEADER.size <= len(data) <= MAX_MESSAGE:
        raise ValueError('invalid PRUDPLite length')
    packets, offset = [], 0
    while offset < len(data):
        if len(packets) == 64 or len(data) - offset < LITE_HEADER.size:
            raise ValueError('truncated or excess PRUDPLite packets')
        magic, options_size, size, types, src, dst, session, flags, seq = LITE_HEADER.unpack_from(data, offset)
        end_options = offset + LITE_HEADER.size + options_size
        end = end_options + size
        if magic != 128 or flags & 15 > 7 or end > len(data):
            raise ValueError('invalid PRUDPLite frame')
        pos, options, seen = offset + LITE_HEADER.size, [], set()
        while pos < end_options:
            if pos + 2 > end_options:
                raise ValueError('truncated PRUDPLite option')
            k, n = data[pos:pos+2];pos += 2
            if k in seen or pos+n > end_options:
                raise ValueError('duplicate or truncated PRUDPLite option')
            options.append((k, bytes(data[pos:pos+n])));seen.add(k);pos += n
        packets.append(LitePacket(types, src, dst, session, flags, seq, tuple(options), bytes(data[end_options:end])))
        offset = end
    return packets
