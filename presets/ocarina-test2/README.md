# Ocarina of Time (0400486024DEC000) presets for the Test 2 integration build

Copy ONE of these to the console as `env-0400486024dec000.txt` (next to the currently installed one).
The global `env.txt` (presets/ocarina-arena-reuse/env.txt) is unchanged and still applies.

| File | Frame wait | Use |
|---|---|---|
| `env-0400486024dec000.stable.txt` | `EDEN_UP_OOT_FRAME_WAIT=1` (full: the guest waits for this frame's GPU fence) | first test, correctness baseline |
| `env-0400486024dec000.fast.txt` | `EDEN_UP_OOT_FRAME_CAP=1`, `_CAP_MS=30` (waits at most 30 ms for this frame, then only for the previous one) | experimental; upstream reports flashes come back with it |

Both keep the six lines of the deployed title preset and add, by group (delete a group's lines to turn it off):

- Parser: `EDEN_NX2_DESYNC_FIX=1` (keep the submit copy of a list the game reshaped after a semaphore wait),
  `EDEN_NX2_BAD_COND_SKIP=1` (already the Ocarina default in this build; written out so it is visible).
- Movie (Lichii #659/#809): `EDEN_UP_VDEC_BUSY`, `EDEN_UP_FS_NO_THROTTLE`, `EDEN_UP_AM_THREADS`,
  `EDEN_UP_MOVIE_PACE`, `EDEN_UP_MOVIE_CLOCK`.

Memory is NOT changed: `EDEN_GUEST_SWAP_AHEAD_MIB=12288` and `EDEN_NX2_CRAM_RESERVE_MIB=128` stay as in the
global env.txt. Test 2 uses CRAM 256 for a New Game out-of-memory crash (not this crash; +128 MiB committed) and
suggests SWAP_AHEAD 2048 on firmware 13.x only if the Deku Tree load freezes. `memory-optional.txt` holds
`EDEN_NX2_AREA_EVICT=40` (Test 2's image eviction after area loads) to append only if memory runs short.

All switches are read once at start. Removing a line restores the previous behaviour of that part.
