#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds and runs tools/check-mme-split-regression.cpp (split-entry decision, macro write atomicity,
device range bounds) under ASan/UBSan. Needs only a C++20 compiler: the test includes the core's
header-only helpers (Eglantine/src/video_core/split_macro_state.h)."""
import os, pathlib, shutil, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parent.parent
cxx = os.environ.get('CXX') or next((c for c in ('clang++-18', 'clang++', 'g++') if shutil.which(c)), None)
if cxx is None:
    sys.exit('no C++ compiler found')
with tempfile.TemporaryDirectory() as tmp:
    exe = pathlib.Path(tmp) / 'check-mme-split-regression'
    subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fno-omit-frame-pointer',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(root / 'Eglantine/src'), str(root / 'tools/check-mme-split-regression.cpp'),
                    '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)])
    sys.exit(result.returncode)
