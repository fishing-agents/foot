#!/usr/bin/env python3
"""Exercise Kitty graphics through a real Foot PTY on an existing Wayland display.

Run: python3 tests/kitty-graphics-smoke.py build/foot
Requires a running compositor (a headless Weston display is sufficient).
"""
import base64
import fcntl
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty
import zlib


def png():
    def chunk(kind, data):
        return (struct.pack('!I', len(data)) + kind + data +
                struct.pack('!I', zlib.crc32(kind + data)))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('!IIBBBBB', 2, 2, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\0' + b'\xff\0\0\xff' * 2 +
                                         b'\0' + b'\0\xff\0\x80' * 2)) +
            chunk(b'IEND', b''))


def worker(directory):
    results = []
    fd = os.open('/dev/tty', os.O_RDWR)
    old = termios.tcgetattr(fd)
    tty.setraw(fd)

    def send(control, payload=b'', expected=b'OK', split=False, separator=True):
        sequence = b'\x1b_G' + control.encode() + (b';' if separator else b'') + payload + b'\x1b\\'
        if split:
            os.write(fd, sequence[:-1])
            time.sleep(0.05)
            os.write(fd, sequence[-1:])
        else:
            os.write(fd, sequence)
        if expected is None:
            return
        reply = bytearray()
        deadline = time.monotonic() + 5
        while not reply.endswith(b'\x1b\\'):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                raise AssertionError(f'timeout for {control}: {reply!r}')
            reply.extend(os.read(fd, 4096))
        assert reply.startswith(b'\x1b_G'), repr(reply)
        response_header, status = bytes(reply)[3:-2].split(b';', 1)
        fields = dict(field.split('=', 1) for field in control.split(','))
        if 'i' in fields:
            response_fields = dict(field.split(b'=', 1) for field in response_header.split(b','))
            assert response_fields.get(b'i') == fields['i'].encode(), (control, reply)
        assert status.startswith(expected), (control, status, expected)
        results.append({'command': control, 'response': bytes(reply).decode('ascii')})
        if control.startswith(('a=T', 'a=p')) and expected == b'OK':
            time.sleep(0.1)  # Exercise a rendered Wayland frame with the placement alive.

    try:
        raw = b'\xff\0\0\xff' * 4
        send('a=q,i=101,f=32,s=2,v=2', base64.b64encode(raw), split=True)
        send('a=p,i=101', expected=b'ENOENT')  # Queries never store an image.
        send('a=T,i=102,f=32,s=2,v=2,c=4,r=2,C=1', base64.b64encode(raw))
        send('a=p,i=102,p=2,c=2,r=1,C=1', separator=False)
        send('a=T,i=103,f=100,c=4,r=2,C=1', base64.b64encode(png()))
        send('a=T,i=104,f=24,s=2,v=2,o=z,C=1',
             base64.b64encode(zlib.compress(b'\0\0\xff' * 4)))
        encoded = base64.b64encode(raw)
        send('a=T,i=105,f=32,s=2,v=2,m=1,C=1', encoded[:12], expected=None)
        send('m=0', encoded[12:])
        send('a=q,i=106,f=32,s=2,v=2', b'!!!!', expected=b'EBADMSG')
        # An aborted or non-graphics APC must not swallow the next valid request.
        os.write(fd, b'\x1b_Ga=T,f=32,s=2,v=2,i=107;AAAA\x18')
        os.write(fd, b'\x1b_not-graphics\x1b\\')
        send('a=q,i=108,f=32,s=2,v=2', base64.b64encode(raw))
        send('a=T,i=109,f=32,s=2,v=2,q=2,C=1', base64.b64encode(raw), expected=None)
        send('a=q,i=110,f=32,s=2,v=2', base64.b64encode(raw))
        send('a=q,i=111,t=f,f=32,s=2,v=2', b'', expected=b'ENOTSUP')
        # The real terminal, not the adapter unit-test stub, moves the cursor.
        os.write(fd, b'\x1b[H')
        send('a=T,i=112,f=32,s=2,v=2,c=3,r=2', base64.b64encode(raw))
        os.write(fd, b'\x1b[6n')
        cursor_reply = bytearray()
        while not cursor_reply.endswith(b'R'):
            assert select.select([fd], [], [], 5)[0], 'cursor response timeout'
            cursor_reply.extend(os.read(fd, 4096))
        assert cursor_reply == b'\x1b[3;4R', cursor_reply
        results.append({'command': 'CSI 6n', 'response': cursor_reply.decode('ascii')})
        # Deletion deliberately has no success reply in the Kitty protocol.
        send('a=d,d=I,i=102', expected=None)
        send('a=p,i=102', expected=b'ENOENT')
        send('a=d,d=A', expected=None)
        # Incremental-rendering integration: allow each cursor/text/scroll
        # operation to reach a Wayland frame, then check that the PTY remains
        # responsive. Exact pixels and work counts are tested in test-image-render.
        def frame_check(label):
            os.write(fd, b'\x1b[6n')
            reply = bytearray()
            deadline = time.monotonic() + 5
            while not reply.endswith(b'R'):
                remaining = deadline - time.monotonic()
                assert remaining > 0 and select.select([fd], [], [], remaining)[0], label
                reply.extend(os.read(fd, 4096))
            assert reply.startswith(b'\x1b['), (label, reply)
            results.append({'command': label, 'response': reply.decode('ascii')})
            time.sleep(0.1)

        os.write(fd, b'\x1b[2J\x1b[H')
        send('a=T,i=120,f=100,c=32,r=16,C=1', base64.b64encode(png()))
        for n in range(12):
            os.write(fd, f'\x1b[3;{3 + n}H'.encode() + (b'X' if n % 2 else b' '))
            time.sleep(0.025)
        frame_check('Kitty cursor/text damage')
        for _ in range(4):
            os.write(fd, b'\x1b[S')
            time.sleep(0.025)
        frame_check('Kitty forward pixel scroll')
        os.write(fd, b'\x1b[2T')
        frame_check('Kitty reverse pixel scroll')
        os.write(fd, b'\x1b[?1049h\x1b[Halternate\x1b[?1049l')
        frame_check('Kitty alternate-grid buffer reuse')
        send('a=d,d=A', expected=None)

        def sixel(transparent, col):
            os.write(fd, f'\x1b[2;{col}H'.encode())
            stripe = b'#1!32~!32?!32~!32?-' if transparent else b'#1!128~-'
            os.write(fd, b'\x1bP0;' + (b'1' if transparent else b'0') +
                     b';0q"1;1;128;96#1;2;100;0;0' + stripe * 16 + b'\x1b\\')
            time.sleep(0.1)

        os.write(fd, b'\x1b[2J\x1b[H')
        sixel(False, 2)
        for n in range(8):
            os.write(fd, f'\x1b[3;{3 + n}Hx'.encode())
            time.sleep(0.025)
        frame_check('opaque Sixel cell damage')
        os.write(fd, b'\x1b[2J\x1b[H')
        sixel(True, 2)
        sixel(True, 40)
        for n in range(8):
            os.write(fd, f'\x1b[3;{3 + n}Hx\x1b[3;{41 + n}Hy'.encode())
            time.sleep(0.025)
        frame_check('two transparent Sixels sharing dirty rows')
        os.write(fd, b'\x1b[2S\x1b[T')
        frame_check('Sixel forward/reverse pixel scroll')
        os.write(fd, b'\x1b[H')
        send('a=T,i=121,f=100,c=32,r=16,C=1', base64.b64encode(png()))
        os.write(fd, b'\x1b[3;12r\x1b[3;1H\x1b[2S\x1b[r')
        frame_check('mixed Kitty/Sixel partial-region scroll')
        send('a=d,d=A', expected=None)
        time.sleep(0.2)  # Allow a Wayland frame to composite before shutdown.
        # Regression: placement invalidation must precede freeing outgoing
        # grid rows, both for partial and full reverse scrolling.
        os.write(fd, b'\x1b[12;1H')
        send('a=T,i=122,f=32,s=2,v=2,c=1,r=1,C=1', encoded)
        os.write(fd, b'\x1b[3;12r\x1b[2T\x1b[r')
        frame_check('Kitty reverse partial scroll over outgoing placement')
        terminal_rows, _, _, _ = struct.unpack(
            'HHHH', fcntl.ioctl(fd, termios.TIOCGWINSZ, b'\0' * 8))
        os.write(fd, f'\x1b[{terminal_rows};1H'.encode())
        send('a=T,i=123,f=32,s=2,v=2,c=1,r=1,C=1', encoded)
        os.write(fd, b'\x1b[T')
        frame_check('Kitty reverse full scroll over outgoing placement')
        Path(directory, 'result.json').write_text(json.dumps(results, indent=2))
    except Exception as error:
        Path(directory, 'error.txt').write_text(repr(error))
        raise
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, old)
        os.close(fd)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--worker':
        worker(sys.argv[2])
        return
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    if not os.environ.get('WAYLAND_DISPLAY'):
        raise SystemExit('WAYLAND_DISPLAY must name a running compositor')
    foot = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='foot-kitty-') as directory:
        workers = os.environ.get('FOOT_SMOKE_WORKERS', '0')
        command = [foot, '--config=/dev/null', '-o', f'main.workers={workers}',
                   sys.executable, str(Path(__file__).resolve()), '--worker', directory]
        run = subprocess.run(command, capture_output=True, text=True, timeout=40)
        error = Path(directory, 'error.txt')
        result = Path(directory, 'result.json')
        if run.returncode or not result.exists():
            raise SystemExit(f'Foot exited {run.returncode}\n{run.stderr}\n'
                             + (error.read_text() if error.exists() else 'No worker result'))
        checks = json.loads(result.read_text())
        print(json.dumps({'checks': len(checks), 'results': checks}, indent=2))


if __name__ == '__main__':
    main()
