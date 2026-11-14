"""Loopback protocol probe. Framing is implemented; wire authentication is not.

Unverified PRUDP packets are logged but never acknowledged as authenticated.
No tickets, lobby or Pia relay are advertised by this executable yet.
"""
import argparse
import ipaddress
import json
from pathlib import Path
import selectors
import signal
import socket
import time
import threading
from contextlib import nullcontext
import sys

from .wire import decode_datagram


def load_config(path):
    cfg = json.loads(Path(path).read_text())
    if not isinstance(cfg.get('local_only'), bool):
        raise ValueError('local_only must be an explicit boolean')
    for key in ('bind', 'advertise'):
        address = ipaddress.ip_address(cfg[key])  # never a hostname / DNS call
        if address.version != 4 or address.is_multicast or address.is_unspecified and key == 'advertise':
            raise ValueError('invalid address')
        if cfg['local_only'] and str(address) != '127.0.0.1':
            raise ValueError('local_only requires 127.0.0.1')
    ports = [cfg[k] for k in ('auth_port', 'secure_port', 'nat_port', 'relay_port')]
    if len(set(ports)) != 4 or any(type(p) is not int or not 1 <= p <= 65535 for p in ports):
        raise ValueError('four distinct numeric ports required')
    if not 1 <= cfg['max_peers'] <= 256 or not 1 <= cfg['idle_seconds'] <= 60:
        raise ValueError('invalid resource bounds')
    if cfg.get('tls_port') and (type(cfg['tls_port']) is not int or not 1 <= cfg['tls_port'] <= 65535):
        raise ValueError('invalid TLS port')
    return cfg


def serve(cfg, log, seconds=0):
    # Until authentication and the Pia wire adapter are complete, public bind
    # is not a supported deployment mode, even with local_only=false.
    if cfg['bind'] != '127.0.0.1' or not cfg['local_only']:
        raise ValueError('wire authentication pending; only local probe mode is enabled')
    selector = selectors.DefaultSelector()
    counters = {'received': 0, 'malformed': 0, 'unverified': 0, 'external_refused': 0, 'rate_limited': 0}
    stop = False
    websocket = None
    log_lock = threading.Lock()
    def interrupt(*_):
        nonlocal stop
        stop = True
    if hasattr(signal, 'SIGINT'):
        signal.signal(signal.SIGINT, interrupt)
    if hasattr(signal, 'SIGTERM'):
        signal.signal(signal.SIGTERM, interrupt)
    start = last_summary = time.monotonic()
    budget_second, budget = 0, 0
    def emit(event, **fields):
        with log_lock:
            log.write(json.dumps({'event': event, 'elapsed': round(time.monotonic()-start, 6), **fields}, sort_keys=True) + '\n')
            log.flush()
    try:
        for role in ('auth', 'secure', 'nat', 'relay'):
            fd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                fd.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                fd.bind((cfg['bind'], cfg[role + '_port']))
                fd.setblocking(False)
                selector.register(fd, selectors.EVENT_READ, role)
            except BaseException:
                fd.close()
                raise
        if cfg.get('tls_port'):
            from .websocket_probe import Probe
            websocket = Probe(cfg, emit)
        emit('ready', bind=cfg['bind'], ports={r: cfg[r+'_port'] for r in ('auth', 'secure', 'nat', 'relay')},
             status='probe-only; game login unsupported')
        while not stop and (not seconds or time.monotonic()-start < seconds):
            for key, _ in selector.select(0.02):
                # Bound each socket iteration so flood traffic cannot starve
                # shutdown or the other socket roles. No replies/amplification.
                for _ in range(64):
                    try:
                        data, endpoint = key.fileobj.recvfrom(65535)
                    except BlockingIOError:
                        break
                    counters['received'] += 1
                    if not ipaddress.ip_address(endpoint[0]).is_loopback:
                        counters['external_refused'] += 1
                        continue
                    try:
                        packets = decode_datagram(data)
                    except ValueError:
                        counters['malformed'] += 1
                        continue
                    counters['unverified'] += len(packets)
                    second = int(time.monotonic())
                    if budget_second != second:
                        budget_second, budget = second, 0
                    if budget + len(packets) > 128:
                        counters['rate_limited'] += len(packets)
                        continue
                    budget += len(packets)
                    for packet in packets:
                        emit('prudp-unverified', role=key.data, endpoint=list(endpoint), kind=packet.kind,
                             flags=packet.flags, channel=packet.channel, session=packet.session,
                             sequence=packet.sequence, source=packet.source, destination=packet.destination,
                             options=[(k, v.hex()) for k, v in packet.options], payload_bytes=len(packet.payload))
            if time.monotonic()-last_summary >= 1:
                emit('counters', **counters)
                last_summary = time.monotonic()
        emit('stopped', **counters)
    finally:
        if websocket:
            websocket.close()
        for key in list(selector.get_map().values()):
            selector.unregister(key.fileobj)
            key.fileobj.close()
        selector.close()
    return counters


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--config', type=Path, default=Path(__file__).with_name('config.local.json'))
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=0)
    args = ap.parse_args()
    cfg = load_config(args.config)
    if args.seconds < 0:
        ap.error('seconds must be nonnegative')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    output = nullcontext(sys.stdout) if args.log == Path('-') else args.log.open('x', encoding='utf-8')
    with output as log:
        serve(cfg, log, args.seconds)


if __name__ == '__main__':
    main()
