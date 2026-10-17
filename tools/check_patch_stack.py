#!/usr/bin/env python3
# tools/check_patch_stack.py
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

"""Check an applied patch stack in reverse without changing the workspace."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def applied_patches(target, patches):
    # Later patches may change an earlier patch's context. Reverse the whole
    # stack on copies of only its affected files, not on the user's checkout.
    with tempfile.TemporaryDirectory(prefix='nyabula-patches-') as temp:
        root = Path(temp)
        names = set()
        for patch in patches:
            for name in re.findall(r'^(?:--- a/|\+\+\+ b/)([^\t\n]+)',
                                   patch.read_text(), re.M):
                rel = Path(name)
                if rel.is_absolute() or '..' in rel.parts:
                    raise ValueError('patch path escapes workspace')
                names.add(rel)
        for rel in names:
            source = target / rel
            if source.exists() or source.is_symlink():
                dest = root / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, dest, follow_symlinks=False)
        applied = []
        for patch in reversed(patches):
            command = ['patch', '--batch', '-d', str(root), '-p1', '-s']
            def run(*options):
                return subprocess.run(command + list(options),
                                      input=patch.read_bytes(),
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL).returncode
            if run('--reverse', '--force', '--dry-run') == 0:
                if run('--reverse', '--force') != 0:
                    raise ValueError(f'cannot reverse {patch.name}')
                applied.append(patch.name)
            elif run('--forward', '--dry-run') != 0:
                # An earlier missing dependency can prevent checking this
                # patch yet. Leave it for the ordered forward application.
                continue
    return applied


if __name__ == '__main__':
    for name in applied_patches(Path(sys.argv[1]),
                                [Path(x) for x in sys.argv[2:]]):
        print(name)
