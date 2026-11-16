#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import shlex
import os
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
# Meson quotes native Windows executable paths. Preserve backslashes and
# remove only the enclosing token quotes before subprocess receives argv.
argv = shlex.split(introspect, posix=(os.name != 'nt'))
if os.name == 'nt':
    argv = [part[1:-1] if len(part) > 1 and part[0] == part[-1]
            and part[0] in ('"', "'") else part for part in argv]
out = subprocess.run([*argv, '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        if not isinstance(e, OSError) or e.errno != errno.EEXIST:
            if os.name == 'nt':
                print('Please enable Developer Mode to support soft link '
                      'without Administrator permission')
            print(f'error making symbolic link {dest}', file=sys.stderr)
            raise e
