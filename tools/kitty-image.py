#!/usr/bin/env python3
"""Display a PNG using direct Kitty graphics transport, without Kitty installed."""
import argparse
import base64
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--columns', type=int, default=0)
    parser.add_argument('--rows', type=int, default=0)
    parser.add_argument('--id', type=int, default=1)
    args = parser.parse_args()
    if args.columns < 0 or args.rows < 0 or not 0 < args.id <= 0xffffffff:
        parser.error('dimensions must be nonnegative; id must be 1..4294967295')
    try:
        with args.image.open('rb') as image:
            data = image.read(64 * 1024 * 1024 + 1)
    except OSError as error:
        parser.error(str(error))
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        parser.error('input must be a PNG image')
    if len(data) > 64 * 1024 * 1024:
        parser.error('input exceeds the 64 MiB upload limit')
    encoded = base64.b64encode(data)
    output = sys.stdout.buffer
    for offset in range(0, len(encoded), 4096):
        payload = encoded[offset:offset + 4096]
        more = int(offset + len(payload) < len(encoded))
        controls = f'm={more}'
        if offset == 0:
            controls = (f'a=T,f=100,t=d,i={args.id},q=2,'
                        f'c={args.columns},r={args.rows},' + controls)
        output.write(b'\x1b_G' + controls.encode('ascii') + b';' + payload + b'\x1b\\')
    output.flush()


if __name__ == '__main__':
    main()
