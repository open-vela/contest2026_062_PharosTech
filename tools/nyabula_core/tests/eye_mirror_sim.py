#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Eye mirror end to end on the simulator: real Eye service, real display
pipeline (software TE) into a probe LCD, real web service, a viewer over TCP.
The mirrored pages are compared with what the probe LCD was given."""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import tempfile
import threading
import time
from product_records_sim import Simulator

PORT = 18091
TOKEN = 'a5' * 32


def http(request, read_all=True):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=5)
    s.sendall(request.encode())
    data = b''
    try:
        while read_all:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    if read_all:
        s.close()
        return data
    return s


def get(path, token=TOKEN, method='GET'):
    auth = f'Authorization: Bearer {token}\r\n' if token else ''
    return f'{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n{auth}\r\n'


class Viewer(threading.Thread):
    """Reads the stream and keeps every decoded page with its arrival time."""

    def __init__(self, query='', path='/eyes/stream', token=TOKEN):
        super().__init__(daemon=True)
        self.sock = http(get(path + query, token=token), read_all=False)
        self.sock.settimeout(3)
        self.pages = {}          # eye -> list of 16-bit pixel values
        self.size = {}
        self.history = []        # (time, eye, tuple(pixels), key, payload size)
        self.keepalives = 0
        self.bytes = 0
        self.status = None
        self.error = None
        self.stop = False

    def run(self):
        buf = b''
        try:
            while b'\r\n\r\n' not in buf:
                buf += self.sock.recv(65536)
            head, buf = buf.split(b'\r\n\r\n', 1)
            self.status = int(head.split()[1])
            self.head = head.decode()
            while not self.stop:
                while len(buf) >= 16:
                    assert buf[:4] == b'NEM1', buf[:16]
                    kind, eye, flags, scale, w, h, size = struct.unpack('<BBBBHHI', buf[4:16])
                    if len(buf) < 16 + size:
                        break
                    payload, buf = buf[16:16 + size], buf[16 + size:]
                    self.bytes += 16 + size
                    if kind == 1:
                        self.keepalives += 1
                        continue
                    self.apply(eye, flags & 1, w, h, payload)
                    self.history.append((time.monotonic(), eye, tuple(self.pages[eye]), flags & 1, size))
                try:
                    chunk = self.sock.recv(262144)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
        except Exception as error:  # noqa: BLE001 - reported by the caller
            self.error = repr(error)

    def apply(self, eye, key, w, h, data):
        pixels = w * h
        if key:
            self.pages[eye] = [0] * pixels
            self.size[eye] = (w, h)
        page = self.pages[eye]
        assert self.size[eye] == (w, h)
        at = pixel = 0
        while at < len(data):
            op = data[at]
            at += 1
            count = (op & 63) + 1
            if op & 63 == 63:
                count = data[at] | data[at + 1] << 8
                at += 2
            assert count and pixel + count <= pixels
            if op >> 6 == 1:
                page[pixel:pixel + count] = [data[at] | data[at + 1] << 8] * count
                at += 2
            elif op >> 6 == 2:
                page[pixel:pixel + count] = struct.unpack(f'<{count}H', data[at:at + 2 * count])
                at += 2 * count
            else:
                assert op >> 6 == 0
            pixel += count
        assert pixel == pixels, (pixel, pixels)

    def close(self):
        self.stop = True
        self.sock.close()
        self.join(5)


def used(sim):
    out = sim.command('free')
    match = re.search(r'(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+Umem', out)
    assert match, out
    return int(match[2])


def far(truth, page):
    """Pixels where some RGB565 channel differs by more than one step."""
    count = 0
    for a, b in zip(truth, page):
        if a != b and (abs((a >> 11) - (b >> 11)) > 1 or abs(((a >> 5) & 63) - ((b >> 5) & 63)) > 1
                       or abs((a & 31) - (b & 31)) > 1):
            count += 1
    return count


def ppm(path, page, size):
    w, h = size
    body = bytearray()
    for v in page:
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        body += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    path.write_bytes(f'P6 {w} {h} 255\n'.encode() + bytes(body))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix='eye-mirror-') as directory:
        (Path(directory) / 'web.token').write_text(TOKEN)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.command('eyeprobe init')
            output = sim.command('nyabula_eye &') + sim.drain(2)
            assert 'Eye Engine attached' in output, output
            sim.command(f'nyabula_web {PORT} /host/web.token http://127.0.0.1:{PORT} 127.0.0.1 &')
            sim.drain(1.5)

            # Credentials and method are checked before anything is allocated.
            assert b' 401 ' in http(get('/eyes/stream', token=None))
            assert b' 401 ' in http(get('/eyes/stream', token='0' * 64))
            assert b' 405 ' in http(get('/eyes/stream', method='POST'))
            result['refusals'] = 'ok'

            # Built for a board without panels (NYABULA_CORE_WEB_EYEPROBE): the
            # page and the pages, to anyone.  Otherwise neither is there.
            page = http(get('/eyeprobe', token=None))
            result['eyeprobe'] = b' 200 ' in page.splitlines()[0]
            if result['eyeprobe']:
                assert b'text/html' in page and b"/eyeprobe/stream" in page and page.rstrip().endswith(b'</html>')
                open_viewer = Viewer(path='/eyeprobe/stream', token=None)
                open_viewer.start()
                sim.drain(2)
                assert open_viewer.status == 200 and set(open_viewer.pages) == {0, 1}, (open_viewer.status, open_viewer.error)
                open_viewer.close()
                sim.drain(1.5)
            else:
                assert b' 401 ' not in http(get('/eyeprobe/stream', token=None)).splitlines()[0]

            sim.drain(1)
            idle = used(sim)
            viewer = Viewer('?fps=20')
            viewer.start()
            sim.drain(2)
            assert viewer.status == 200, (viewer.status, viewer.error)
            assert 'application/octet-stream' in viewer.head
            watching = used(sim)
            assert set(viewer.pages) == {0, 1}, (viewer.pages.keys(), viewer.error)
            assert viewer.size[0] == (360, 360) and viewer.size[1] == (360, 360)
            firsts = [h for h in viewer.history if h[3]]
            assert len(firsts) == 2, 'one key page per eye'

            # A change of expression has to show up in the mirror.
            before = len(viewer.history)
            sim.request('eyes.expression', {'expression': 'happy'})
            sim.drain(2.5)
            assert len(viewer.history) > before, 'no page after the expression changed'

            # What the probe LCD holds against what the viewer was given.
            matches = []
            for attempt in range(6):
                sim.drain(.7)
                stamp = time.monotonic()
                frame = sim.command('eyeprobe save /host')
                assert frame.count('EYE_FRAME') == 2, frame
                sim.drain(.4)
                for eye, name in ((0, 'left'), (1, 'right')):
                    raw = (Path(directory) / f'{name}.rgb').read_bytes()
                    # The display layer hands the panel big-endian RGB565 with
                    # ordered (Bayer 4x4) dithering, the mirror is not dithered:
                    # a channel may differ by one step, and by no more.
                    truth = struct.unpack(f'>{360 * 360}H', raw)
                    best = min(
                        (far(truth, page), round(when - stamp, 3), index)
                        for index, (when, e, page, _, _) in enumerate(viewer.history)
                        if e == eye and abs(when - stamp) < 1.5)
                    page = viewer.history[best[2]][2]
                    matches.append({'attempt': attempt, 'eye': name, 'dt': best[1],
                                    'beyondOneStep': best[0],
                                    'identical': sum(1 for a, b in zip(truth, page) if a == b),
                                    'lit': sum(1 for a in truth if a)})
                    ppm(args.output / f'lcd-{eye}.ppm', truth, (360, 360))
                    ppm(args.output / f'mirror-at-lcd-{eye}.ppm', page, (360, 360))
            result['probe'] = matches
            for eye in (0, 1):
                ppm(args.output / f'mirror-{eye}.ppm', viewer.pages[eye], viewer.size[eye])

            # A third viewer is refused, a second one is served at half size.
            half = Viewer('?scale=2')
            half.start()
            sim.drain(1.5)
            assert half.status == 200 and half.size.get(0) == (180, 180), (half.status, half.size, half.error)
            assert b' 503 ' in http(get('/eyes/stream'))
            ppm(args.output / 'mirror-half-0.ppm', half.pages[0], half.size[0])
            half.close()

            elapsed = viewer.history[-1][0] - viewer.history[0][0]
            sizes = [h[4] for h in viewer.history if not h[3]]
            result['stream'] = {
                'pages': len(viewer.history), 'seconds': round(elapsed, 2),
                'pagesPerSecondPerEye': round(len(viewer.history) / 2 / elapsed, 2),
                'bytesPerSecond': round(viewer.bytes / elapsed),
                'keyPageBytes': [h[4] for h in firsts],
                'differencePageBytes': {'min': min(sizes), 'median': sorted(sizes)[len(sizes) // 2], 'max': max(sizes)},
                'rawPageBytes': 360 * 360 * 2, 'keepalives': viewer.keepalives, 'error': viewer.error}
            viewer.close()
            sim.drain(2)
            released = used(sim)
            result['memory'] = {'idle': idle, 'watching': watching, 'released': released,
                                'costWhileWatching': watching - idle, 'leak': released - idle}

            # Attaching again after everything was freed works the same.
            again = Viewer()
            again.start()
            sim.drain(1.5)
            assert again.status == 200 and set(again.pages) == {0, 1}, (again.status, again.error)
            again.close()
            sim.drain(1.5)
            result['memory']['releasedAgain'] = used(sim) - idle
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
    # A page with an eye on it (not a blink) that is the LCD's page, for each
    # eye: the viewer is paced, so not every probe lands on a mirrored page.
    good = {m['eye'] for m in result['probe'] if m['beyondOneStep'] == 0 and m['lit'] > 10000}
    result['eyesConfirmedAgainstLcd'] = sorted(good)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    assert good == {'left', 'right'}, 'no mirrored page equals what the LCD was given'
    assert abs(result['memory']['leak']) < 4096, result['memory']
    print('EYE_MIRROR_SIM_PASS')


if __name__ == '__main__':
    main()
