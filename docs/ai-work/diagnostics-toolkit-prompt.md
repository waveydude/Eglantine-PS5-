# Diagnostics toolkit: the prompt, rewritten so an agent can finish it

Paste everything below the line. It replaces the long "build a complete debugging toolkit" prompt:
same goal, cut to what helps the Castle Town blocker first, with each stage done and verified
before the next starts.

---

You are working on my EglantinePS5 fork (Ocarina of Time, Switch 2 1.1.1, base PS5, firmware
13.60). Repo `/Users/waves/antigravity/eglantine-ocarina-t85`, branch `waves/t85-playable`.
Read `docs/AGENT_HANDOFF.md` first and keep it current. Build in the Docker container
`eglantine-ocarina-oom-build` (`/ocarina`) the way `docs/AGENT_HANDOFF.md` says, including the
source sync before every build. Never write to the console without my approval.

**Goal:** the emulator explains its own memory. First target: why the shared pool runs out at the
Castle Town gate (driver memory 1.7 → 4.4 GB in seconds, largest free block 1 MiB).

**What already exists — use it, don't rebuild it:**
- Logs every run already writes: `RADV_PS5_MEMORY` (driver memory by category), `EDEN_POOL_LEDGER`
  (guest, heap, JIT, tables), heap lines and `EDEN_HEAP_SITE` call sites, `EDEN_REFUSED_*` at a
  refusal, fps lines (`nx2:vi2 5 s`), crash reports with `tools/symbolize-crash.py`.
- `EDEN_GFX_CENSUS=1` (`headless/gfx_census.cpp`): every Vulkan allocation charged to an owner
  (render target, depth, block textures, storage, buffer cache, stream, staging, download,
  descriptor, raw memory), a line every 5 s plus one at each refusal. Tests:
  `tools/test-gfx-census.cpp`.
- `tools/eden-mem-report.py LOG_DIR -o report.md`: turns those logs into a Markdown report (what
  grew into the shortage, timeline, refusals). Tests: `tools/test-eden-mem-report.py`.
- The stack sampler (`EDEN_SAMPLE_THREADS`, `headless/mkw_native_sample.inc`) and the 5 s perf
  report (`EDEN_PERF_REPORT=1`).

**What doesn't fit this console — don't spend time on it now:**
- PS5_Lichii `coordination/heapsite` (Linux LD_PRELOAD) and `vkmem` (a Vulkan loader layer): PC
  only, and they can't see RADV on the console. Read their notes for ideas only.
- `ps5-perf` (not validated on 13.60), ps5debug process-map reads (have hung the console),
  GPU capture/replay, visual regression, PMC counters. Later, only if a finding needs them.

**Stages.** Do one at a time. Each ends with tests passing and a short result. Stop after each and
report.

1. **Census run (in progress).** A console package with `EDEN_GFX_CENSUS=1` on top of the
   current build. Done when: the package is built and verified (switch strings in
   `llvm-pie.elf`, `__wrap_vma*` symbols present), host tests pass, and I have an install packet.
   After my Town attempt: run the report on the logs and return the "What grew" table.
2. **Close the gap.** If census owners plus unused VMA space differ from the driver's `app`
   category by more than 5 %, find what's missing (allocations made outside the VMA entry points,
   driver-internal memory) and account for it. Done when the report's gap line is under 5 % on a
   real run.
3. **Lifetimes for the biggest owner.** For whichever owner grows most into the shortage, record
   creation and deletion frame for each allocation in a bounded ring (no allocation on the
   allocation path), and report how long they live and why they aren't freed (still bound, waiting
   for a download, protected by a rule). Done when the report names the rule or reference that
   holds the memory.
4. **Then the fix**, aimed at that owner, as a separate patch behind a switch, measured with the
   same report on the same route.

**Rules for every stage:**
- Diagnostics are off unless switched on, cost nothing measurable when off, never allocate on the
  allocation path, and never take a lock the allocator or the GPU thread already holds.
- No new OOM guards, retries or limit tweaks while measuring.
- Don't claim a console result from a build or a host test.
- Keep attribution and licences on anything taken from another project.
- Reply in short plain English: what you built, whether it works, what it found, what's unverified,
  what's next.
