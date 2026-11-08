#!/usr/bin/env python3
"""Regenerate configuration codecs with the project's installed Nanopb version."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
generator = root / '.pio/libdeps/dc1/Nanopb/generator/nanopb_generator.py'
if not generator.is_file():
    raise SystemExit('Install project dependencies first: pio pkg install')
for project in [root, root / 'lib/esp_framework']:
    proto = project / 'nanopb' / ('DC1Config.proto' if project == root else 'GlobalConfig.proto')
    with tempfile.TemporaryDirectory() as output:
        subprocess.run([sys.executable, str(generator), '-I', str(proto.parent),
                        '-D', output, str(proto)], check=True)
        for suffix, directory in [('.pb.h', 'include'), ('.pb.c', 'src')]:
            name = proto.stem + suffix
            content = (Path(output) / name).read_bytes().rstrip() + b'\n'
            (project / directory / name).write_bytes(content)
