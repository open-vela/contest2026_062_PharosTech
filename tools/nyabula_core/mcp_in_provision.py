#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare a private local Core credential import; never contact a device."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import time

SCOPES = ('nyabula_time', 'nyabula_system', 'nyabula_timers',
          'nyabula_tasks', 'nyabula_calendar', 'nyabula_chat',
          'nyabula_run', 'nyabula_cancel')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--id', required=True)
    parser.add_argument('--title', required=True)
    parser.add_argument('--revision', type=int, required=True)
    parser.add_argument('--hours', type=int, default=24)
    parser.add_argument('--scope', choices=SCOPES, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('Use a trusted POSIX host so the private file mode is enforced.')
    if not re.fullmatch(r'[a-z0-9_-]{1,40}', args.id):
        parser.error('id must contain 1-40 lowercase letters, digits, hyphens, or underscores')
    if not args.title.strip() or len(args.title.encode('utf-8')) > 96:
        parser.error('title must contain 1-96 UTF-8 bytes')
    if not 0 <= args.revision <= 9007199254740991 or not 1 <= args.hours <= 720:
        parser.error('revision must be an exact nonnegative integer; hours must be 1-720')
    if len(set(args.scope)) != len(args.scope):
        parser.error('scopes must be unique')
    request = {'id': args.id, 'title': args.title, 'revision': args.revision,
               'expiresAt': int(time.time() * 1000) + args.hours * 3600000,
               'scopes': args.scope, 'token': secrets.token_hex(32)}
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, 'O_NOFOLLOW'):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(args.output, flags, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
        json.dump(request, stream, ensure_ascii=False)
        stream.write('\n')
    print(f'Private import file created: {args.output}')
    print('Import via the local Core CLI only. This command did not configure or contact a device.')
    print('Read its token privately to configure the trusted MCP client. Do not log or commit this file.')


if __name__ == '__main__':
    main()
