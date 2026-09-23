#!/usr/bin/env python3
# tools/nyabula_dsp/render.py
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
"""Offline WAV adapter for the same C DSP core intended for board integration."""

import argparse
import array
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile
import wave

RATE = 48000
BANDS = 4
CHANNELS = ("left", "right", "bass")
MAX_BLOCK = 4096


class Equalizer(ct.Structure):
    _fields_ = [("frequency_hz", ct.c_float), ("q", ct.c_float),
                ("gain_db", ct.c_float)]


class Output(ct.Structure):
    _fields_ = [("gain_db", ct.c_float), ("polarity", ct.c_int32),
                ("delay_frames", ct.c_uint32), ("eq", Equalizer * BANDS)]


class Config(ct.Structure):
    _fields_ = [("sample_rate", ct.c_uint32), ("crossover_hz", ct.c_float),
                ("master_gain_db", ct.c_float), ("ceiling", ct.c_float),
                ("bypass", ct.c_int32), ("eq", Equalizer * BANDS),
                ("output", Output * 3)]


class Stats(ct.Structure):
    _fields_ = [("processed_frames", ct.c_uint64), ("limited_frames", ct.c_uint64),
                ("input_peak", ct.c_float), ("output_peak", ct.c_float)]


def load_library(path):
    lib = ct.CDLL(str(Path(path).resolve()))
    lib.nyadsp_api_version.argtypes = []
    lib.nyadsp_api_version.restype = ct.c_uint32
    if lib.nyadsp_api_version() != 1:
        raise ValueError("Unsupported DSP library API version")
    lib.nyadsp_default_config.argtypes = [ct.POINTER(Config)]
    lib.nyadsp_default_config.restype = None
    lib.nyadsp_create.argtypes = [ct.POINTER(Config), ct.POINTER(ct.c_int)]
    lib.nyadsp_create.restype = ct.c_void_p
    lib.nyadsp_destroy.argtypes = [ct.c_void_p]
    lib.nyadsp_destroy.restype = None
    lib.nyadsp_reset.argtypes = [ct.c_void_p]
    lib.nyadsp_reset.restype = None
    lib.nyadsp_process.argtypes = [ct.c_void_p, ct.POINTER(ct.c_float),
                                  ct.POINTER(ct.c_float), ct.c_size_t]
    lib.nyadsp_process.restype = ct.c_int
    lib.nyadsp_get_stats.argtypes = [ct.c_void_p, ct.POINTER(Stats)]
    lib.nyadsp_get_stats.restype = ct.c_int
    return lib


def _object(value, allowed, label):
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    extra = value.keys() - allowed
    if extra:
        raise ValueError(f"{label}: unknown fields {sorted(extra)}")


def _number(value, label, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be numeric")
    if not math.isfinite(value):
        raise ValueError(f"{label} must be finite")
    if integer and (not isinstance(value, int) or not -(2**31) <= value < 2**31):
        raise ValueError(f"{label} must be a signed 32-bit integer")
    return value


def _equalizers(value, target):
    if not isinstance(value, list) or len(value) > BANDS:
        raise ValueError("eq must be a list of at most four peaking bands")
    for index, item in enumerate(value):
        _object(item, {"frequency_hz", "q", "gain_db"}, "eq band")
        if item.keys() != {"frequency_hz", "q", "gain_db"}:
            raise ValueError("eq band needs frequency_hz, q and gain_db")
        for key, val in item.items():
            setattr(target[index], key, _number(val, key))


def make_config(lib, data):
    _object(data, {"version", "sample_rate", "crossover_hz", "master_gain_db",
                   "ceiling", "bypass", "eq", "outputs"}, "config")
    if type(data.get("version")) is not int or data["version"] != 1:
        raise ValueError("Config version must be 1")
    config = Config()
    lib.nyadsp_default_config(ct.byref(config))
    for key in ("sample_rate", "crossover_hz", "master_gain_db", "ceiling"):
        if key in data:
            val = _number(data[key], key, integer=(key == "sample_rate"))
            if key == "sample_rate" and val != RATE:
                raise ValueError("Only 48000 Hz is supported")
            setattr(config, key, val)
    if "bypass" in data:
        if type(data["bypass"]) is not bool:
            raise ValueError("bypass must be boolean")
        config.bypass = int(data["bypass"])
    _equalizers(data.get("eq", []), config.eq)
    outputs = data.get("outputs", {})
    _object(outputs, set(CHANNELS), "outputs")
    for index, name in enumerate(CHANNELS):
        item = outputs.get(name, {})
        _object(item, {"gain_db", "polarity", "delay_frames", "eq"}, name)
        for key in ("gain_db", "polarity", "delay_frames"):
            if key in item:
                val = _number(item[key], key, integer=(key != "gain_db"))
                if key == "delay_frames" and not 0 <= val <= 4800:
                    raise ValueError("delay_frames must be in 0..4800")
                setattr(config.output[index], key, val)
        _equalizers(item.get("eq", []), config.output[index].eq)
    return config


class Engine:
    def __init__(self, lib, config):
        self.lib = lib
        error = ct.c_int()
        self.handle = lib.nyadsp_create(ct.byref(config), ct.byref(error))
        if not self.handle:
            raise ValueError(f"DSP configuration rejected: {error.value}")

    def close(self):
        if self.handle:
            self.lib.nyadsp_destroy(self.handle)
            self.handle = None

    def process(self, samples):
        if not self.handle:
            raise ValueError("DSP is closed")
        if len(samples) % 2:
            raise ValueError("Incomplete stereo frame")
        frames = len(samples) // 2
        if frames > MAX_BLOCK:
            raise ValueError("Block exceeds 4096 frames")
        src = (ct.c_float * len(samples))(*samples)
        dst = (ct.c_float * (frames * 3))()
        result = self.lib.nyadsp_process(self.handle, src, dst, frames)
        if result:
            raise ValueError(f"DSP block rejected: {result}")
        return list(dst)

    def stats(self):
        stats = Stats()
        if not self.handle or self.lib.nyadsp_get_stats(self.handle, ct.byref(stats)):
            raise ValueError("Cannot read DSP statistics")
        return {key: getattr(stats, key) for key, _ in Stats._fields_}

    def reset(self):
        if not self.handle:
            raise ValueError("DSP is closed")
        self.lib.nyadsp_reset(self.handle)


def _pcm16(samples):
    result = array.array("h", (max(-32768, min(32767, round(x * 32768)))
                               for x in samples))
    if sys.byteorder != "little":
        result.byteswap()
    return result.tobytes()


def render(source, destination, library, config_data, block=1024, tail_ms=250):
    source, destination, library = map(Path, (source, destination, library))
    if destination.exists():
        raise ValueError("Output directory exists; refusing to overwrite")
    if type(block) is not int or not 1 <= block <= MAX_BLOCK:
        raise ValueError("block must be in 1..4096")
    if type(tail_ms) is not int or not 0 <= tail_ms <= 1000:
        raise ValueError("tail_ms must be in 0..1000")
    lib = load_library(library)
    config = make_config(lib, config_data)
    engine = Engine(lib, config)
    staging = None
    try:
        with wave.open(str(source), "rb") as wav:
            if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth(),
                    wav.getcomptype()) != (RATE, 2, 2, "NONE"):
                raise ValueError("Input must be uncompressed 48 kHz stereo PCM16 WAV")
            frames = wav.getnframes()
            if frames == 0:
                raise ValueError("Input WAV is empty")
            destination.parent.mkdir(parents=True, exist_ok=True)
            staging = Path(tempfile.mkdtemp(prefix=".nyadsp-", dir=destination.parent))
            with wave.open(str(staging / "satellites.wav"), "wb") as high, \
                    wave.open(str(staging / "bass.wav"), "wb") as low:
                high.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
                low.setparams((1, 2, RATE, 0, "NONE", "not compressed"))

                def emit(samples):
                    output = engine.process(samples)
                    high.writeframesraw(_pcm16(x for i, x in enumerate(output)
                                              if i % 3 != 2))
                    low.writeframesraw(_pcm16(output[2::3]))

                remaining = frames
                while remaining:
                    count = min(block, remaining)
                    raw = wav.readframes(count)
                    if len(raw) != count * 4:
                        raise ValueError("Truncated WAV sample data")
                    values = array.array("h")
                    values.frombytes(raw)
                    if sys.byteorder != "little":
                        values.byteswap()
                    emit([x / 32768 for x in values])
                    remaining -= count
                delay = 0 if config.bypass else max(o.delay_frames for o in config.output)
                padding = delay + tail_ms * RATE // 1000
                remaining = padding
                while remaining:
                    count = min(block, remaining)
                    emit([0.0] * (count * 2))
                    remaining -= count
            report = {
                "api_version": 1, "sample_rate": RATE, "input_frames": frames,
                "padding_frames": padding, "output_frames": frames + padding,
                "config": config_data, "stats": engine.stats(),
                "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                "note": "Offline signal processing only; no device synchronization or acoustic calibration.",
            }
            (staging / "report.json").write_text(
                json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
            # Publish only after every output has been closed successfully.
            if destination.exists():
                raise ValueError("Output directory appeared during rendering")
            os.rename(staging, destination)
            staging = None
            return report
    finally:
        engine.close()
        if staging is not None:
            shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--block", type=int, default=1024)
    parser.add_argument("--tail-ms", type=int, default=250)
    args = parser.parse_args()
    try:
        data = json.loads(args.config.read_text(encoding="utf-8"))
        result = render(args.input, args.output_directory, args.library, data,
                        args.block, args.tail_ms)
    except (ValueError, OSError, EOFError, wave.Error, OverflowError) as error:
        parser.exit(2, f"error: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
