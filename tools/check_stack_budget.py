#!/usr/bin/env python3
"""Check the Tesla worker's deepest static call chains against its task stack.

Reads GCC -fstack-usage (.su) output from an ESP-IDF build. Frames of a function
and its compiler-outlined parts are added, because the wrapper calls the part.
Transport, TLS, journal and parser callees are not compiled with stack usage, so
the remaining stack must keep RESERVE_BYTES for them. On-device confirmation is
the status field tesla_stack_min_bytes.
"""
import argparse
from pathlib import Path
import re
import sys

RESERVE_BYTES = 16384
WORKER = 'app::{anonymous}::worker(void*)@tesla.cpp'
CHAINS = [
    (WORKER, 'tsl::FleetClient::poll', 'tsl::FleetClient::get', 'tsl::FleetClient::fetch'),
    (WORKER, 'tsl::FleetClient::poll', 'tsl::FleetClient::get', 'tsl::FleetClient::refresh'),
    (WORKER, 'tsl::FleetClient::poll', 'tsl::FleetClient::refresh'),
    (WORKER, 'tsl::FleetClient::refresh'),
]
LINE = re.compile(r'^(?P<path>.*?):\d+:\d+:(?P<signature>.*)\t(?P<bytes>\d+)\t(?P<kind>\S+)$')
PART = re.compile(r'_ZN3tsl11FleetClient(\d+)([A-Za-z_]\w*?)E\S*?\$part\$\d+')


def name(path: str, signature: str) -> str:
    part = PART.search(signature)
    if part:
        return 'tsl::FleetClient::' + part.group(2)[:int(part.group(1))]
    base = signature.split('(', 1)[0].split()[-1]
    if base.endswith('worker'):
        return f'{base}(void*)@{Path(path).name}'
    return base


def frames(build: Path) -> dict:
    result = {}
    for su in build.rglob('*.su'):
        for line in su.read_text().splitlines():
            m = LINE.match(line)
            if not m:
                continue
            if m['kind'] != 'static':
                raise SystemExit(f'{su}: {m["signature"]} has {m["kind"]} stack usage')
            key = name(m['path'], m['signature'])
            result[key] = result.get(key, 0) + int(m['bytes'])
    return result


def stack_bytes(source: Path) -> int:
    m = re.search(r'xTaskCreate\(worker,"tesla_worker",(\d+),', source.read_text())
    if not m:
        raise SystemExit(f'{source}: tesla_worker stack size not found')
    return int(m.group(1))


def check(found: dict, stack: int, reserve: int = RESERVE_BYTES) -> list:
    report = []
    for chain in CHAINS:
        missing = [f for f in chain if f not in found]
        if missing:
            raise SystemExit('stack usage missing for ' + ', '.join(missing) +
                             '; build with -fstack-usage and keep fetch/refresh out of line')
        used = sum(found[f] for f in chain)
        report.append((chain, used, stack - used))
    return report


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'main/tesla.cpp')
    args = p.parse_args()
    stack = stack_bytes(args.source)
    ok = True
    for chain, used, left in check(frames(args.build), stack):
        status = 'ok' if left >= RESERVE_BYTES else 'FAIL'
        ok &= left >= RESERVE_BYTES
        print(f'{status} {used:6d} used, {left:6d} left of {stack}: ' + ' > '.join(c.split('::')[-1] for c in chain))
    print(f'Required reserve for uninstrumented callees: {RESERVE_BYTES} bytes')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
