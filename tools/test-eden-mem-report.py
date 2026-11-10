#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/eden-mem-report.py on small synthetic logs.   python3 tools/test-eden-mem-report.py"""
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('memreport', Path(__file__).with_name('eden-mem-report.py'))
memreport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(memreport)

STDERR = """\
RADV_PS5_MEMORY t_s=0 pool_mib=12352 available_mib=11190 driver_mib=0 retired_mib=0 sparse_va_mib=0/0
RADV_PS5_MEMORY t_s=60 pool_mib=12352 available_mib=2000 driver_mib=2200 retired_mib=0 sparse_va_mib=0/0 app=2048000/100/2048000/300000/400 rings=131072/3/131072/131072/4
EDEN_POOL_LEDGER out of the pool: 1 MiB; guest 4600 reserve 64 heap 600 jit 400 tables 150 MiB; flexible free 80 MiB
[EglantinePS5] heap: holds 900 MiB (600 MiB of pieces, 300 MiB in 20 large blocks), 700 MiB of it in use; 0 MiB given back so far (every 20 s)
EDEN_GFX_CENSUS t=58 rt_color=300/40/310 rt_depth=320/3/320 tex_block=900/800/900 buf_device=400/200/420 live_mib=1920 vma=2100/1920/180/12/150
RADV_PS5_MEMORY t_s=120 pool_mib=12352 available_mib=400 driver_mib=2300 retired_mib=0 sparse_va_mib=0/0 app=2150000/100/2150000/300000/400 rings=131072/3/131072/131072/4
RADV_PS5_MEMORY t_s=150 pool_mib=12352 available_mib=5 driver_mib=4200 retired_mib=0 sparse_va_mib=0/0 app=4000000/400/4000000/300000/900 rings=131072/3/131072/131072/4
EDEN_POOL_LEDGER out of the pool: 1 MiB; guest 5700 reserve 64 heap 640 jit 450 tables 160 MiB; flexible free 80 MiB
EDEN_GFX_CENSUS t=148 refusal rt_color=1100/400/1100/fail3 rt_depth=320/3/320 tex_block=1700/1500/1700 buf_device=420/210/430 live_mib=3540 vma=3700/3540/160/1/300
EDEN_GFX_CENSUS_FAIL owner=rt_color result=-2 image 1920x1080x1 format=37
EDEN_REFUSED_IMAGES 1800 images hold 3100 MiB, 0 of them with nothing bound; 1400 MiB written by the GPU
EDEN_REFUSED_IMAGE 297.0 MiB 106 12288x12288x1, 1 layers, 1 levels, flags 0x10, at 0x8180fa0000
"""
EDEN = """\
[  70.000] Service <Info> x: nx2:vi2 5 s: 160 frames, 32.0 fps, mean 31 ms, 1% low 50 ms, worst 60.0 ms, fence 0.0 ms
[ 150.000] Service <Info> x: nx2:vi2 5 s: 10 frames, 2.0 fps, mean 500 ms, 1% low 900 ms, worst 4000.0 ms, fence 0.0 ms
"""
CRASH = "what: SIGSEGV (invalid memory access)\nwhere: eboot+0x1\nthread: CPUCore_2 (0x1)\nmemory: largest free block 1 MiB\n"


class Report(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        root = Path(self.dir.name)
        (root / 'crash-20261110-1-stderr.log').write_text(STDERR)
        (root / 'crash-20261110-1-eden_log.txt').write_text(EDEN)
        (root / 'crash-20261110-1.txt').write_text(CRASH)
        self.session = memreport.load([root])

    def tearDown(self):
        self.dir.cleanup()

    def test_samples_carry_the_lines_after_them(self):
        samples = self.session.samples
        self.assertEqual([s.t for s in samples], [0, 60, 120, 150])
        self.assertEqual(samples[1].guest, 4600)
        self.assertEqual(samples[2].guest, 4600)          # carried until the next ledger line
        self.assertEqual(samples[3].guest, 5700)
        self.assertEqual(samples[1].radv['app'], 2000)    # KiB to MiB
        self.assertEqual(samples[3].census['rt_color'], (1100, 400, 1100, 3))
        self.assertEqual(samples[3].vma, (3700, 3540, 160, 1, 300))
        self.assertEqual(samples[1].heap_held, 900)

    def test_steady_is_the_last_sample_before_the_shortage(self):
        steady, peak = memreport.steady_and_peak(self.session.samples)
        # 120 s still had 400 MiB free; the pool first fell under 256 MiB at 150 s.
        self.assertEqual((steady.t, peak.t), (120, 150))

    def test_report_names_the_growth_and_the_gap(self):
        text = memreport.report(self.session)
        self.assertIn('| vulkan: rt_color | 300 | 1100 | +800 |', text)
        self.assertIn('| guest memory | 4600 | 5700 | +1100 |', text)
        # 1100+320+1700+420 = 3540 owners + 160 unused = 3700 against app 3906 MiB
        self.assertIn('3700 MiB, against the driver\'s app category 3906 MiB: +206 MiB', text)
        self.assertIn('rt_color refused (-2): image 1920x1080x1 format=37', text)
        self.assertIn('297 MiB 12288x12288x1', text)
        self.assertIn('Crash: SIGSEGV (invalid memory access) on CPUCore_2', text)
        self.assertIn('lowest 2.0', text)

    def test_without_census_it_says_so(self):
        for sample in self.session.samples:
            sample.census = sample.vma = None
        text = memreport.report(self.session)
        self.assertIn('No EDEN_GFX_CENSUS lines', text)
        self.assertNotIn('not explained by the census', text)

    def test_empty_logs(self):
        text = memreport.report(memreport.Session())
        self.assertIn('No RADV_PS5_MEMORY lines', text)


if __name__ == '__main__':
    unittest.main()
