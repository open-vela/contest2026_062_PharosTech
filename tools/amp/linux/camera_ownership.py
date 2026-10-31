#!/usr/bin/env python3
# tools/amp/linux/camera_ownership.py
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Keep K7 capture hardware out of the AMP Linux compute domain."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess

# The NPU, RGA and decode engines remain compute-domain resources. Capture,
# the shared camera DCPHY and the encoder used by openvela do not.
DISABLED_CONFIG = (
    'VIDEO_CAMERA_SENSOR', 'VIDEO_OV5647', 'VIDEO_IMX415',
    'VIDEO_ROCKCHIP_CIF', 'VIDEO_ROCKCHIP_ISP', 'VIDEO_ROCKCHIP_ISP1',
    'VIDEO_ROCKCHIP_ISPP', 'VIDEO_ROCKCHIP_VPSS',
    'PHY_ROCKCHIP_CSI2_DPHY', 'PHY_ROCKCHIP_SAMSUNG_DCPHY',
    'ROCKCHIP_MPP_RKVENC', 'ROCKCHIP_MPP_RKVENC2',
)

CAMERA_COMPAT = frozenset((
    'rockchip,rk3576-csi2-dphy', 'rockchip,rk3576-csi2-dphy-hw',
    'rockchip,rk3576-mipi-csi2', 'rockchip,rk3576-mipi-csi2-hw',
    'rockchip,rk3576-mipi-dcphy', 'rockchip,rk3576-cif',
    'rockchip,rkcif-dvp', 'rockchip,rkcif-sditf', 'rockchip,rkcif-mipi-lvds',
    'rockchip,rk3576-rkisp', 'rockchip,rkisp-vir', 'rockchip,rkisp-sditf',
    'rockchip,rk3576-rkvpss', 'rockchip,rkvpss-vir',
    'ovti,ov5647', 'sony,imx415',
))

# Exact K7 nodes: SCCB bus, capture MMUs and VEPU (including its MMUs).
# Do not disable all I2C, IOMMU or MPP nodes: NPU and other compute users
# still need their own providers.
OWNED_NODES = frozenset((
    '/i2c@2ac70000', '/iommu@27c07f00', '/iommu@27c10800',
    '/iommu@27c33f00', '/rkvenc-core@27a00000', '/rkvenc-core@27a10000',
    '/iommu@27a0f000', '/iommu@27a1f000', '/rkvenc-ccu',
))


def config_disable(path):
    text = path.read_text()
    for option in DISABLED_CONFIG:
        pattern = rf'^(?:CONFIG_{option}=.*|# CONFIG_{option} is not set)$'
        text = re.sub(pattern + r'\n?', '', text, flags=re.MULTILINE)
        text += f'# CONFIG_{option} is not set\n'
    path.write_text(text)


def config_check(path):
    values = dict(re.findall(r'^CONFIG_(\w+)=(.*)$', path.read_text(), re.M))
    active = [x for x in DISABLED_CONFIG if values.get(x) in ('y', 'm')]
    if active:
        raise ValueError('Linux still owns camera drivers: ' + ', '.join(active))


def property_value(path, node, name):
    return subprocess.check_output(
        ['fdtget', '-t', 's', '-d', '', str(path), node, name], text=True).strip()


def camera_nodes(path):
    pending = ['/']
    found = []
    while pending:
        node = pending.pop()
        children = subprocess.check_output(
            ['fdtget', '-l', str(path), node], text=True).split()
        pending.extend(node.rstrip('/') + '/' + child for child in children)
        compatible = set(property_value(path, node, 'compatible').split())
        props = subprocess.check_output(
            ['fdtget', '-p', str(path), node], text=True).split()
        if (node in OWNED_NODES or compatible & CAMERA_COMPAT or
                'rockchip,camera-module-index' in props):
            found.append(node)
    if not found:
        raise ValueError('No K7 camera nodes found; refusing an unrelated DTB')
    return sorted(found)


def dtb_disable(source, output):
    if source.resolve() == output.resolve():
        raise ValueError('Use a separate output DTB to preserve the input')
    nodes = camera_nodes(source)
    shutil.copyfile(source, output)
    for node in nodes:
        subprocess.run(['fdtput', '-t', 's', str(output), node,
                        'status', 'disabled'], check=True)
    return nodes


def dtb_check(path):
    nodes = camera_nodes(path)
    active = [node for node in nodes
              if property_value(path, node, 'status') != 'disabled']
    if active:
        raise ValueError('Linux still owns camera nodes: ' + ', '.join(active))
    return nodes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['disable-config', 'check-config',
                                           'disable-dtb', 'check-dtb'])
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path, nargs='?')
    args = parser.parse_args()
    try:
        if args.action == 'disable-config':
            config_disable(args.input)
        elif args.action == 'check-config':
            config_check(args.input)
        elif args.action == 'disable-dtb':
            if args.output is None:
                parser.error('disable-dtb requires OUTPUT')
            nodes = dtb_disable(args.input, args.output)
            print(f'disabled {len(nodes)} camera nodes')
        else:
            print(f'checked {len(dtb_check(args.input))} camera nodes')
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
