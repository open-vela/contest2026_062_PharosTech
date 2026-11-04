#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Freeze the NanoTrack v2 exemplar graph to 127 before RKNN conversion."""
import argparse
import ast
import re
from pathlib import Path
import numpy as np
import onnx
import onnxruntime as ort
from rknn.api import RKNN

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('backbone', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
model = onnx.load(args.backbone)
assert len(model.graph.input) == len(model.graph.output) == 1
for dim, value in zip(model.graph.input[0].type.tensor_type.shape.dim, [1, 3, 127, 127]):
    dim.dim_value = value
model.graph.output[0].type.tensor_type.ClearField('shape')
del model.graph.value_info[:]
model = onnx.shape_inference.infer_shapes(model, strict_mode=True)
onnx.checker.check_model(model)
frozen = args.output / 'template-127.onnx'
onnx.save(model, frozen)
session = ort.InferenceSession(str(frozen), providers=['CPUExecutionProvider'])
result = session.run(None, {session.get_inputs()[0].name: np.zeros((1, 3, 127, 127), np.float32)})
assert result[0].shape == (1, 48, 8, 8), result[0].shape
print('TEMPLATE_ONNX_127_TO_48x8x8_PASS', flush=True)
rknn = RKNN(verbose=False)
try:
    assert rknn.config(target_platform='rk3576', mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]]) == 0
    assert rknn.load_onnx(model=str(frozen)) == 0
    assert rknn.build(do_quantization=False) == 0
    output = args.output / 'template.rknn'
    assert rknn.export_rknn(str(output)) == 0
finally:
    rknn.release()
metadata = next(ast.literal_eval(text.decode()) for text in re.findall(rb'[ -~]{7,}', output.read_bytes()) if text.startswith(b"{'attrs':"))
attrs = metadata['attrs']
inputs = [v['shape'] for v in attrs.values() if not v.get('is_output')]
outputs = [v['shape'] for v in attrs.values() if v.get('is_output')]
assert inputs == [[1, 3, 127, 127]] and outputs == [[1, 48, 8, 8]], (inputs, outputs)
print('TEMPLATE_RKNN_ACTUAL_SHAPE_PASS', inputs, outputs, output.stat().st_size, flush=True)
