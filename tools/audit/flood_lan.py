"""Bounded malformed UDP flood against loopback LDN endpoints only."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import socket
import time
from build_runtime import ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ports', default='24872,24873')
    ap.add_argument('--packets', type=int, default=3000, help='packets per endpoint, at most 10000')
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    assert args.out.resolve().is_relative_to(ROOT / 'build/run') and not args.out.exists()
    ports = [int(v) for v in args.ports.split(',')]
    assert 0 < args.packets <= 10000 and len(ports) <= 16 and all(1024 <= p <= 65535 for p in ports)
    rng = random.Random(15)
    bodies = [b'', b'S', b'SLDN' + bytes(60), b'SLDN\xff\xff' + rng.randbytes(58), rng.randbytes(4161), rng.randbytes(8192)]
    start = time.perf_counter(); stamp = datetime.now(timezone.utc).isoformat()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for i in range(args.packets):
            for port in ports:
                s.sendto(bodies[i % len(bodies)], ('127.0.0.1', port))
    result = {'started_utc': stamp, 'address': '127.0.0.1', 'ports': ports,
              'datagrams_sent': args.packets * len(ports), 'elapsed_seconds': time.perf_counter() - start,
              'payloads': 'empty, short, invalid size/version, over transport limit'}
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
