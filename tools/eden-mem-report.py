#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Who held the shared memory pool, from a session's logs, as a Markdown report.

    python3 tools/eden-mem-report.py LOG_DIR_OR_FILES... [-o report.md]

Reads what the build already writes, in stderr.log (or crash-*-stderr.log), eden_log.txt and a
crash-*.txt, in any order:
  RADV_PS5_MEMORY       pool, free and driver MiB with the driver's categories (t_s = seconds)
  EDEN_POOL_LEDGER      guest memory, heap, JIT and tables out of the pool
  [EglantinePS5] heap:  what the heap holds and uses
  EDEN_GFX_CENSUS       Vulkan memory by owner (headless/gfx_census.cpp, EDEN_GFX_CENSUS=1)
  EDEN_REFUSED_*        the allocator and caches at a refusal
  nx2:vi2 5 s           frame rate per 5 s
The stderr lines carry no clock of their own: each is given the t_s of the RADV_PS5_MEMORY line
before it. Numbers are what the logs say; nothing is inferred beyond the arithmetic shown.
"""
import argparse
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

RADV = re.compile(r'RADV_PS5_MEMORY t_s=(\d+) pool_mib=(\d+) available_mib=(\d+) driver_mib=(\d+)(.*)')
RADV_CAT = re.compile(r'(\w+)=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)')
LEDGER = re.compile(r'EDEN_POOL_LEDGER .*?guest (\d+) reserve (\d+) heap (\d+) jit (\d+) tables (\d+) MiB')
HEAP = re.compile(r'heap: holds (\d+) MiB .*?, (\d+) MiB of it in use')
CENSUS = re.compile(r'EDEN_GFX_CENSUS t=(\d+)( refusal)?(.*)')
CENSUS_OWNER = re.compile(r' ([a-z_]+)=(\d+)/(\d+)/(\d+)(?:/fail(\d+))?')
CENSUS_VMA = re.compile(r' vma=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)')
CENSUS_FAIL = re.compile(r'EDEN_GFX_CENSUS_FAIL owner=(\w+) result=(-?\d+) (.*)')
REFUSED = re.compile(r'(EDEN_REFUSED_(?:IMAGES|BUFFERS|VMA type 0|STAGING stream)\b.*)')
REFUSED_IMAGE = re.compile(r'EDEN_REFUSED_IMAGE ([\d.]+) MiB (\d+) (\d+x\d+x\d+), (\d+) layers')
FPS = re.compile(r'^\[\s*([\d.]+)\].*nx2:vi2 5 s: (\d+) frames, ([\d.]+) fps.*?worst ([\d.]+) ms')
CRASH = re.compile(r'^(what|where|thread|memory|time): (.*)')


@dataclass
class Sample:
    t: int
    pool: int
    free: int
    driver: int
    radv: dict = field(default_factory=dict)      # category -> live MiB
    guest: int | None = None
    heap_pool: int | None = None
    jit: int | None = None
    tables: int | None = None
    heap_held: int | None = None
    heap_used: int | None = None
    census: dict | None = None                    # owner -> (live MiB, count, peak MiB, fails)
    vma: tuple | None = None                      # blocks, allocated, unused, largest hole MiB, block count


@dataclass
class Session:
    samples: list = field(default_factory=list)
    refused: list = field(default_factory=list)
    refused_images: list = field(default_factory=list)
    census_fails: list = field(default_factory=list)
    fps: list = field(default_factory=list)       # (t, fps, worst ms)
    crash: dict = field(default_factory=dict)
    sources: list = field(default_factory=list)


def parse_stderr(lines, session):
    current = None
    for line in lines:
        m = RADV.search(line)
        if m:
            t, pool, free, driver, rest = int(m[1]), int(m[2]), int(m[3]), int(m[4]), m[5]
            radv = {c[1]: int(c[2]) >> 10 for c in RADV_CAT.finditer(rest)}
            # Several lines can share a second; keep the newest, carrying what came after the last.
            if current is not None and current.t == t:
                current.pool, current.free, current.driver, current.radv = pool, free, driver, radv
            else:
                current = Sample(t, pool, free, driver, radv)
                if session.samples:
                    previous = session.samples[-1]
                    current.guest, current.heap_pool, current.jit, current.tables = (
                        previous.guest, previous.heap_pool, previous.jit, previous.tables)
                    current.heap_held, current.heap_used = previous.heap_held, previous.heap_used
                    current.census, current.vma = previous.census, previous.vma
                session.samples.append(current)
            continue
        if current is None:
            continue
        if (m := LEDGER.search(line)):
            current.guest, current.heap_pool, current.jit, current.tables = int(m[1]), int(m[3]), int(m[4]), int(m[5])
        elif (m := HEAP.search(line)):
            current.heap_held, current.heap_used = int(m[1]), int(m[2])
        elif (m := CENSUS_FAIL.search(line)):
            session.census_fails.append((current.t, m[1], int(m[2]), m[3].strip()))
        elif (m := CENSUS.search(line)):
            owners = {o[1]: (int(o[2]), int(o[3]), int(o[4]), int(o[5] or 0)) for o in CENSUS_OWNER.finditer(m[3])}
            owners.pop('live_mib', None)
            owners.pop('vma', None)
            current.census = owners
            v = CENSUS_VMA.search(m[3])
            current.vma = tuple(int(x) for x in v.groups()) if v else None
        elif (m := REFUSED_IMAGE.search(line)):
            session.refused_images.append((float(m[1]), m[3], int(m[4]), current.t))
        elif (m := REFUSED.search(line)):
            session.refused.append((current.t, m[1].strip()))


def parse_eden_log(lines, session):
    for line in lines:
        if (m := FPS.search(line)):
            session.fps.append((float(m[1]), float(m[3]), float(m[4])))


def parse_crash(lines, session):
    for line in lines:
        if (m := CRASH.match(line)):
            session.crash.setdefault(m[1], m[2].strip())


def load(paths):
    session = Session()
    files = []
    for path in map(Path, paths):
        files += sorted(p for p in path.iterdir() if p.is_file()) if path.is_dir() else [path]
    stderr = [p for p in files if p.name.endswith('stderr.log')]
    eden = [p for p in files if p.name.endswith('eden_log.txt')]
    crash = [p for p in files if re.fullmatch(r'crash-[\d-]+\.txt', p.name)]
    for p in stderr[:1]:
        parse_stderr(p.read_text(errors='replace').splitlines(), session)
    for p in eden[:1]:
        parse_eden_log(p.read_text(errors='replace').splitlines(), session)
    for p in crash[:1]:
        parse_crash(p.read_text(errors='replace').splitlines(), session)
    session.sources = [str(p) for p in stderr[:1] + eden[:1] + crash[:1]]
    return session


def steady_and_peak(samples):
    """The sample just before the pool first ran short (free under 256 MiB after the first minute
    with a game loaded), and the sample with the least free memory after it.
    If the logs end before recording a shortage, compare the first minimum with its predecessor."""
    loaded = [s for s in samples if s.driver > 0]
    if not loaded:
        return None, None
    short = next((i for i, s in enumerate(loaded) if s.free < 256 and s.t >= loaded[0].t + 60), None)
    if short is None:
        low = min(range(len(loaded)), key=lambda i: loaded[i].free)
        return loaded[max(low - 1, 0)], loaded[low]
    steady = loaded[max(short - 1, 0)]
    peak = min(loaded[short:], key=lambda s: (s.free, -s.driver))
    return steady, peak


def components(s):
    rows = {'driver (RADV, all)': s.driver}
    for name, value in (('guest memory', s.guest), ('emulator heap', s.heap_pool), ('JIT code', s.jit),
                        ('page tables', s.tables)):
        if value is not None:
            rows[name] = value
    for category, mib in s.radv.items():
        rows[f'driver: {category}'] = mib
    if s.census:
        for owner, (live, _count, _peak, _fails) in s.census.items():
            rows[f'vulkan: {owner}'] = live
    if s.vma:
        rows['vulkan: unused inside VMA blocks'] = s.vma[2]
    return rows


def report(session):
    out = []
    w = out.append
    samples = session.samples
    w('# Memory report\n')
    w('Sources: ' + ', '.join(f'`{s}`' for s in session.sources) + '\n')
    if not samples:
        w('No RADV_PS5_MEMORY lines: run with RADV_PS5_MEMORY_REPORT=1.\n')
        return '\n'.join(out)
    least = min(samples, key=lambda s: s.free)
    w('## What happened\n')
    w(f'- {len(samples)} memory samples over {samples[-1].t - samples[0].t} s; pool {samples[0].pool} MiB.')
    w(f'- Least free in periodic driver samples: **{least.free} MiB** at t={least.t} s, '
      f'driver then {least.driver} MiB.')
    w(f'- Most driver memory: {max(s.driver for s in samples)} MiB.')
    if session.crash:
        w(f"- Crash: {session.crash.get('what', '?')} on {session.crash.get('thread', '?')}, "
          f"{session.crash.get('where', '?')}; {session.crash.get('memory', '')}")
    else:
        w('- No crash report among the sources.')
    if session.fps:
        play = [f for f in session.fps if f[1] < 50]
        if play:
            w(f'- Frame rate in play (windows under 50 fps): mean {statistics.mean(f[1] for f in play):.1f}, '
              f'lowest {min(f[1] for f in play):.1f}, worst frame {max(f[2] for f in play):.0f} ms.')
    has_census = any(s.census for s in samples)
    if not has_census:
        w('- No EDEN_GFX_CENSUS lines: Vulkan memory is not split by owner in this run (EDEN_GFX_CENSUS=1).')
    w('')

    steady, peak = steady_and_peak(samples)
    if steady and peak and steady is not peak:
        w(f'## What grew between driver samples (t={steady.t} s to t={peak.t} s)\n')
        before, after = components(steady), components(peak)
        w('| Component | Before MiB | After MiB | Change |')
        w('|---|---:|---:|---:|')
        names = sorted(set(before) | set(after), key=lambda n: -(after.get(n, 0) - before.get(n, 0)))
        for name in names:
            b, a = before.get(name, 0), after.get(name, 0)
            if a or b:
                w(f'| {name} | {b} | {a} | {a - b:+d} |')
        w(f'| **free** | {steady.free} | {peak.free} | {peak.free - steady.free:+d} |')
        w('')
        if peak.census:
            vulkan = sum(v[0] for v in peak.census.values()) + (peak.vma[2] if peak.vma else 0)
            app = peak.radv.get('app')
            if app is not None:
                w(f'Vulkan owners plus unused VMA space: {vulkan} MiB, against the driver\'s app category '
                  f'{app} MiB: {app - vulkan:+d} MiB not explained by the census.\n')

    w('## Timeline\n')
    w('| t s | free | driver | guest | heap held/used | top Vulkan owners (MiB) |')
    w('|---:|---:|---:|---:|---|---|')
    picks = samples[::max(1, len(samples) // 24)] + samples[-6:]
    seen = set()
    for s in picks:
        if s.t in seen:
            continue
        seen.add(s.t)
        heap = f'{s.heap_held}/{s.heap_used}' if s.heap_held is not None else ''
        top = ''
        if s.census:
            ranked = sorted(s.census.items(), key=lambda kv: -kv[1][0])[:4]
            top = ', '.join(f'{k} {v[0]}' for k, v in ranked)
        w(f"| {s.t} | {s.free} | {s.driver} | {s.guest if s.guest is not None else ''} | {heap} | {top} |")
    w('')

    if session.census_fails or session.refused:
        w('## Refusals\n')
        for t, owner, result, what in session.census_fails[:12]:
            w(f'- t={t} s: {owner} refused ({result}): {what}')
        last = {}
        for t, text in session.refused:
            last[text.split(' ')[0] + (' type 0' if 'type 0' in text else '')] = (t, text)
        for t, text in last.values():
            w(f'- t={t} s: `{text[:220]}`')
        if session.refused_images:
            biggest = sorted(set(session.refused_images), key=lambda x: -x[0])[:8]
            w('- Largest images at the refusals: ' +
              '; '.join(f'{mib:.0f} MiB {dims} x{layers}' for mib, dims, layers, _t in biggest))
        w('')

    w('## Reading it\n')
    if steady and peak and steady is not peak:
        before, after = components(steady), components(peak)
        growth = sorted(((after.get(n, 0) - before.get(n, 0), n) for n in after
                         if not n.startswith('driver (RADV')), reverse=True)[:3]
        w('Largest growth in that interval: ' + ', '.join(f'{n} {d:+d} MiB' for d, n in growth if d > 0) + '.')
        if not has_census:
            w('The driver line is not split by owner here; a run with EDEN_GFX_CENSUS=1 names the Vulkan owners.')
    return '\n'.join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('paths', nargs='+')
    parser.add_argument('-o', '--output')
    args = parser.parse_args(argv)
    text = report(load(args.paths))
    if args.output:
        Path(args.output).write_text(text + '\n')
    else:
        sys.stdout.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
