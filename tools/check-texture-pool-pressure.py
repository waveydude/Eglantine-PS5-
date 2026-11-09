#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds and runs tools/check-texture-pool-pressure.cpp (the texture collector's pool rule) under ASan/UBSan."""
import os, pathlib, shutil, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parent.parent
cxx = os.environ.get('CXX') or next((c for c in ('clang++-18', 'clang++', 'g++') if shutil.which(c)), None)
with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / 'check'
    subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(root / 'tools/check-texture-pool-pressure.cpp'), '-o', str(exe)], check=True)
    sys.exit(subprocess.run([str(exe)]).returncode)
