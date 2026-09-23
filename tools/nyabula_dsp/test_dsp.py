#!/usr/bin/env python3
# tools/nyabula_dsp/test_dsp.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with this
# work for additional information regarding copyright ownership. The ASF
# licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations
# under the License.
"""Independent response checks and offline file-contract regression tests."""

import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

import render

LIBRARY = Path(sys.argv.pop(1)).resolve()
LIB = render.load_library(LIBRARY)


def base(**kwargs):
    return {"version": 1, "master_gain_db": 0, **kwargs}


def engine(data=None):
    return render.Engine(LIB, render.make_config(LIB, data or base()))


def process(instance, samples, block=1024):
    result = []
    for offset in range(0, len(samples), 2 * block):
        result.extend(instance.process(samples[offset:offset + 2 * block]))
    return result


def tone(frequency, frames=48000, amplitude=0.02, right_gain=1):
    samples = []
    for i in range(frames):
        value = amplitude * math.sin(2 * math.pi * frequency * i / render.RATE)
        samples.extend((value, value * right_gain))
    return samples


def component(samples, frequency):
    # Analyze the last half-second, after filter startup has settled.
    samples = samples[-24000:]
    omega = 2 * math.pi * frequency / render.RATE
    return 2 * sum(value * complex(math.cos(omega * i), -math.sin(omega * i))
                   for i, value in enumerate(samples)) / len(samples)


def response(data, frequency):
    instance = engine(data)
    try:
        result = process(instance, tone(frequency))
        return [component(result[channel::3], frequency) / 0.02
                for channel in range(3)]
    finally:
        instance.close()


def write_wave(path, rate=48000, channels=2, width=2, frames=3001):
    with wave.open(str(path), "wb") as wav:
        wav.setparams((channels, width, rate, 0, "NONE", "not compressed"))
        if channels == 2 and width == 2:
            wav.writeframes(render._pcm16(tone(500, frames=frames, amplitude=0.1)))
        else:
            wav.writeframes(bytes(frames * channels * width))


class ResponseTests(unittest.TestCase):
    def test_lr4_amplitude_and_sum(self):
        for crossover in (40, 500, 5000):
            for frequency in (100, 500, 1000, 10000):
                with self.subTest(crossover=crossover, frequency=frequency):
                    high, right, low = response(base(crossover_hz=crossover), frequency)
                    ratio = math.tan(math.pi * frequency / render.RATE) / math.tan(
                        math.pi * crossover / render.RATE)
                    expected_low = 1 / (1 + ratio**4)
                    self.assertAlmostEqual(abs(low), expected_low, delta=0.003)
                    self.assertAlmostEqual(abs(high), 1 - expected_low, delta=0.003)
                    self.assertAlmostEqual(abs(high + low), 1.0, delta=0.003)
                    self.assertEqual(high, right)

    def test_crossover_is_minus_six_db_per_branch(self):
        high, _, low = response(base(), 500)
        self.assertAlmostEqual(abs(high), 0.5, delta=0.0005)
        self.assertAlmostEqual(abs(low), 0.5, delta=0.0005)
        self.assertLess(abs(high - low), 0.0005)

    def test_global_peaking_center_gain(self):
        baseline = response(base(), 1000)
        for gain in (-12, -6, 6, 12):
            measured = response(base(eq=[{"frequency_hz": 1000, "q": 1,
                                          "gain_db": gain}]), 1000)
            for actual, expected in zip(measured, baseline):
                self.assertAlmostEqual(abs(actual / expected), 10**(gain / 20),
                                       delta=0.005)

    def test_branch_eq_only_changes_selected_channel(self):
        baseline = response(base(), 1000)
        data = base(outputs={"left": {"eq": [
            {"frequency_hz": 1000, "q": 2, "gain_db": -6}]}})
        measured = response(data, 1000)
        self.assertAlmostEqual(abs(measured[0] / baseline[0]), 10**(-6 / 20),
                               delta=0.001)
        self.assertEqual(measured[1:], baseline[1:])

    def test_antiphase_stereo_has_zero_mono_bass(self):
        instance = engine()
        try:
            result = process(instance, tone(100, frames=5000, right_gain=-1))
            self.assertTrue(all(x == 0 for x in result[2::3]))
            self.assertTrue(all(a == -b for a, b in zip(result[::3], result[1::3])))
        finally:
            instance.close()

    def test_left_only_does_not_leak_into_right(self):
        instance = engine()
        try:
            result = process(instance, tone(2000, frames=5000, right_gain=0))
            self.assertTrue(all(x == 0 for x in result[1::3]))
            self.assertGreater(max(abs(x) for x in result[::3]), 0.01)
        finally:
            instance.close()

    def test_delay_gain_polarity(self):
        samples = tone(1000, frames=6000)
        reference = engine()
        delayed = engine(base(outputs={"left": {"delay_frames": 137,
                                                 "gain_db": -6, "polarity": -1}}))
        try:
            expected = process(reference, samples)
            actual = process(delayed, samples)
            self.assertEqual(actual[1::3], expected[1::3])
            self.assertEqual(actual[2::3], expected[2::3])
            self.assertTrue(all(x == 0 for x in actual[:137 * 3:3]))
            for a, b in zip(actual[137 * 3::3], expected[::3]):
                self.assertAlmostEqual(a, -b * 10**(-6 / 20), delta=1e-7)
        finally:
            reference.close()
            delayed.close()

    def test_block_partition_and_reset_are_exact(self):
        rng = random.Random(47)
        samples = [rng.uniform(-0.02, 0.02) for _ in range(12000)]
        data = base(eq=[{"frequency_hz": 220, "q": 1.2, "gain_db": 3}],
                    outputs={"bass": {"delay_frames": 37}})
        instance = engine(data)
        try:
            reference = process(instance, samples, block=4096)
            for size in (1, 7, 127, 1024):
                instance.reset()
                self.assertEqual(process(instance, samples, size), reference)
            instance.reset()
            self.assertTrue(all(x == 0 for x in instance.process([0.0] * 100)))
        finally:
            instance.close()

    def test_safe_bypass(self):
        instance = engine(base(bypass=True, master_gain_db=-6,
                               outputs={"left": {"delay_frames": 4800,
                                                   "polarity": -1}}))
        try:
            result = instance.process([0.1, -0.2] * 20)
            self.assertTrue(all(x == 0 for x in result[2::3]))
            self.assertAlmostEqual(result[0], 0.1 * 10**(-6 / 20), delta=1e-7)
            self.assertAlmostEqual(result[1], -0.2 * 10**(-6 / 20), delta=1e-7)
        finally:
            instance.close()

    def test_limiter_ceiling_and_release(self):
        instance = engine(base(ceiling=0.2))
        try:
            result = process(instance, tone(2000, frames=48000, amplitude=0.9))
            self.assertLessEqual(max(abs(x) for x in result), 0.200001)
            self.assertGreater(instance.stats()["limited_frames"], 0)
            process(instance, [0.0] * 96000)
            recovered = process(instance, tone(2000))
            self.assertGreater(abs(component(recovered[::3], 2000)), 0.019)
        finally:
            instance.close()

    def test_invalid_block_does_not_advance_state(self):
        a, b = engine(), engine()
        try:
            for bad in ([float("nan"), 0.0], [0.0, float("inf")], [1.01, 0.0]):
                with self.assertRaises(ValueError):
                    a.process(bad)
            self.assertEqual(a.stats()["processed_frames"], 0)
            samples = tone(300, frames=100)
            self.assertEqual(a.process(samples), b.process(samples))
        finally:
            a.close()
            b.close()

    def test_config_rejection(self):
        invalid = [
            {"version": True}, base(sample_rate=44100), base(sample_rate=True),
            base(crossover_hz=0), base(ceiling=float("nan")), base(ceiling=1),
            base(master_gain_db=3), base(bypass=1), base(unknown=1),
            base(outputs={"bass": {"delay_frames": 2**32}}),
            base(outputs={"bass": {"delay_frames": -1}}),
            base(outputs={"bass": {"polarity": 0}}),
            base(outputs={"extra": {}}),
            base(eq=[{"frequency_hz": 10, "gain_db": 0, "q": 1}]),
            base(eq=[{"frequency_hz": 500, "gain_db": 0, "q": 0}]),
            base(eq=[{"frequency_hz": 500, "gain_db": 13, "q": 1}]),
            base(eq=[{"frequency_hz": 500, "gain_db": 0, "q": 1}] * 5),
        ]
        for config in invalid:
            with self.subTest(config=config), self.assertRaises((ValueError, OverflowError)):
                instance = engine(config)
                instance.close()

    def test_extreme_valid_filters_remain_finite(self):
        for frequency, q in ((20, 10), (18000, 0.2)):
            data = base(eq=[{"frequency_hz": frequency, "gain_db": 12, "q": q}] * 4)
            instance = engine(data)
            try:
                signal = [1.0, 1.0] + [0.0] * 95998
                output = process(instance, signal)
                self.assertTrue(all(math.isfinite(x) and abs(x) <= 0.900001
                                    for x in output))
            finally:
                instance.close()


class WavTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "input.wav"
        write_wave(self.source)

    def tearDown(self):
        self.temporary.cleanup()

    def test_render_duration_channels_and_pcm(self):
        result = render.render(self.source, self.root / "out", LIBRARY,
                               base(outputs={"bass": {"delay_frames": 17}}),
                               block=127, tail_ms=10)
        self.assertEqual(result["output_frames"], 3001 + 17 + 480)
        for filename, channels in (("satellites.wav", 2), ("bass.wav", 1)):
            with wave.open(str(self.root / "out" / filename), "rb") as wav:
                self.assertEqual(wav.getnchannels(), channels)
                self.assertEqual(wav.getnframes(), result["output_frames"])
                self.assertEqual(wav.getframerate(), 48000)
                self.assertEqual(wav.getsampwidth(), 2)

    def test_cli_with_shipped_preset(self):
        script = Path(render.__file__).resolve()
        preset = script.parents[2] / 'app/nyabula_dsp/presets/balanced.json'
        result = subprocess.run(
            [sys.executable, str(script), str(self.source), str(self.root / 'cli'),
             '--library', str(LIBRARY), '--config', str(preset), '--tail-ms', '0'],
            capture_output=True, text=True, check=True)
        report = json.loads(result.stdout)
        self.assertEqual(report['output_frames'], 3001)
        self.assertEqual(report['config']['crossover_hz'], 500)
        self.assertTrue((self.root / 'cli' / 'satellites.wav').is_file())
        self.assertTrue((self.root / 'cli' / 'bass.wav').is_file())
        self.assertTrue((self.root / 'cli' / 'report.json').is_file())

    def test_render_is_block_invariant(self):
        for block in (1, 127, 4096):
            render.render(self.source, self.root / str(block), LIBRARY,
                          base(), block=block, tail_ms=0)
        for filename in ("satellites.wav", "bass.wav"):
            expected = (self.root / "1" / filename).read_bytes()
            for block in (127, 4096):
                self.assertEqual(expected, (self.root / str(block) / filename).read_bytes())

    def test_unknown_odd_riff_chunk(self):
        raw = self.source.read_bytes()
        raw = raw[:12] + b"JUNK" + struct.pack("<I", 1) + b"x\0" + raw[12:]
        raw = raw[:4] + struct.pack("<I", len(raw) - 8) + raw[8:]
        self.source.write_bytes(raw)
        result = render.render(self.source, self.root / "out", LIBRARY, base(), tail_ms=0)
        self.assertEqual(result["input_frames"], 3001)

    def test_truncated_input_leaves_no_partial_output(self):
        self.source.write_bytes(self.source.read_bytes()[:-20])
        with self.assertRaises(ValueError):
            render.render(self.source, self.root / "out", LIBRARY, base())
        self.assertFalse((self.root / "out").exists())
        self.assertEqual(list(self.root.glob(".nyadsp-*")), [])

    def test_existing_output_is_preserved(self):
        out = self.root / "out"
        out.mkdir()
        (out / "keep.txt").write_text("keep")
        with self.assertRaises(ValueError):
            render.render(self.source, out, LIBRARY, base())
        self.assertEqual((out / "keep.txt").read_text(), "keep")

    def test_unsupported_formats_fail_before_publish(self):
        for rate, channels, width in ((44100, 2, 2), (48000, 1, 2), (48000, 2, 3)):
            write_wave(self.source, rate, channels, width)
            with self.assertRaises(ValueError):
                render.render(self.source, self.root / "out", LIBRARY, base())
            self.assertFalse((self.root / "out").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
