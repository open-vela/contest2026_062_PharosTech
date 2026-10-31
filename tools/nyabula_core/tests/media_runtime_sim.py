#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove WAV -> NxPlayer -> NuttX paced virtual audio output and lifecycle."""
import argparse
import errno
import math
from pathlib import Path
import struct
import tempfile
import time
import wave
from product_records_sim import Simulator

# NuttX errno values are not the host Linux errno namespace.
NUTTX_ENOTSUP = 138


def write_wave(path, seconds, frequency=440):
    pcm = b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * frequency * i / 16000)))
                   for i in range(int(seconds * 16000)))
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(pcm)
    return pcm


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabula-media-') as directory:
        root = Path(directory)
        (root / 'media').mkdir()
        (root / 'audio-out').mkdir()
        long_pcm = write_wave(root / 'media/long.wav', 8)
        short_pcm = write_wave(root / 'media/short.wav', 1, 660)
        (root / 'media/unsupported.txt').write_text('not audio')
        sim = Simulator(args.binary.resolve(), directory)
        try:
            status = sim.request('music.status', {})
            assert status['state'] == 'idle'
            library = sim.request('music.library', {})
            valid = {item['name']: item for item in library['items'] if item['supported']}
            assert set(valid) == {'long.wav', 'short.wav'} and valid['long.wav']['durationMs'] == 8000, library
            devices = sim.request('device.status', {})['audioDevices']
            assert any(item['path'] == '/dev/audio/pcm_test' and item['output'] for item in devices), devices
            selected = sim.request('music.output', {'device': '/dev/audio/pcm_test'})
            assert selected['settingsSaved'] and not selected['volumeSupported'], selected
            sim.request('music.play', {'name': '../private.wav'}, error=-errno.EINVAL)
            sim.request('music.play', {'name': 'unsupported.txt'}, error=-NUTTX_ENOTSUP)
            playing = sim.request('music.play', {'name': 'long.wav'})
            assert playing['state'] == 'playing' and playing['durationMs'] == 8000, playing
            sim.drain(.35)
            target = root / 'audio-out/pcm_test_16000_1_16.pcm'
            assert target.exists() and target.stat().st_size > 0, list((root / 'audio-out').iterdir())
            paused = sim.request('music.pause', {})
            assert paused['state'] == 'paused', paused
            sim.drain(.3)
            size = target.stat().st_size
            sim.drain(.3)
            assert target.stat().st_size == size, (size, target.stat().st_size)
            assert target.read_bytes() == long_pcm[:size], 'WAV header leaked into PCM output'
            resumed = sim.request('music.resume', {})
            assert resumed['state'] == 'playing', resumed
            sim.drain(.3)
            assert target.stat().st_size > size
            stopped = sim.request('music.stop', {})
            assert stopped['state'] == 'idle', stopped
            size = target.stat().st_size
            sim.drain(.2)
            assert target.stat().st_size == size < len(long_pcm)
            sim.request('music.volume', {'volume': 35}, error=-NUTTX_ENOTSUP)
            sim.request('music.play', {'name': 'short.wav'})
            deadline = time.monotonic() + 8
            while sim.request('music.status', {})['state'] != 'idle':
                assert time.monotonic() < deadline
            assert target.read_bytes() == short_pcm
            sim.request('music.play', {'name': 'long.wav'})
            sim.request('music.pause', {})
            assert sim.request('music.stop', {})['state'] == 'idle'
            sim.request('music.output', {'device': '/dev/audio/pcm_aux'})
            sim.request('product.stop', {})
            assert sim.request('music.status', {})['state'] == 'idle'
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            restored = sim.request('music.status', {})
            assert restored['device'] == '/dev/audio/pcm_aux' and restored['state'] == 'idle', restored
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    print('MEDIA_RUNTIME_SIM_PASS nativeWav pcmBytes pauseFreeze resume stop endOfFile preferenceRestart unsupportedVolume')


if __name__ == '__main__':
    main()
