#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Private provisioning output must be exclusive, restricted, and non-logging."""
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

script = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='nyabot-provision-') as directory:
    output = Path(directory) / 'request.json'
    args = [sys.executable, str(script), '--id', 'fixture', '--title', 'Fixture',
            '--revision', '0', '--scope', 'nyabula_time', '--output', str(output)]
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    data = json.loads(output.read_text())
    assert re.fullmatch('[0-9a-f]{64}', data['token'])
    assert data['token'] not in result.stdout + result.stderr
    assert stat.S_IMODE(output.stat().st_mode) == 0o600
    assert data['scopes'] == ['nyabula_time']
    original = output.read_bytes()
    assert subprocess.run(args, capture_output=True).returncode != 0
    assert output.read_bytes() == original
    assert subprocess.run(args + ['--scope', 'shell'], capture_output=True).returncode != 0
    print('PASS: secure source token, 0600 exclusive output, no token logging, no overwrite')
