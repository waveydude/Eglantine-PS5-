/* Eden PS5 port: a heap that takes its memory as it grows, with per-thread arenas.
 *
 * Memory. PS5 direct memory is real from the first byte, and the CPU shares one 12 GiB pool with
 * the GPU. The heap used to take its whole 3 GiB at start; the largest game measured used
 * 1.16 GiB of it, and the rest was graphics memory that game then ran out of. So the heap now
 * reserves its range (6 GiB, PS5_OPENGL_HEAP_SIZE) as address space only and commits it a piece
 * (32 MiB) at a time, from the start of the range, when an allocation finds no room. Where no
 * range can be reserved, it takes 3 GiB at once, as it did before it grew. Every piece (or run of pieces, for one
 * request larger than a piece) is a Sony mspace of its own; an address tells its piece, and
 * the piece its mspace. Memory is not given back: a piece stays for the next game.
 *
 * Large blocks. A request of 32 MiB or more gets memory of its own, outside the range, and gives
 * it back when it is freed: a game file read whole, or a buffer that doubles as it grows, would
 * otherwise leave the heap larger for good and its pieces full of holes no other large block fits.
 *
 * Arenas. One Sony mspace serialised every allocation in the process. Its lock collapses
 * under contention: a malloc/free pair costs 225 ns alone but 33/67/138 us with
 * 2/4/8 contending threads (EDEN_PERF_ALLOC),
 * which made allocation-heavy shader compilation ~100-250x slower than on a
 * Zen 2 host. A fixed split of the heap into arenas (run 220741) exhausted a
 * partition at level load, so capacity is not partitioned here: each allocating thread (up to
 * EDEN_HEAP_FIRST_ARENAS) gets a lazily created 8 MiB arena allocated *from* the heap, aligned to
 * its own 8 MiB slot. Small requests use the thread's arena and fall back to the heap's own
 * spaces; frees and reallocs find the owner in O(1) from the address.
 *
 * More than one arena a thread. A thread that keeps more than 8 MiB of small blocks filled its
 * arena, and from then on every small request it made went to the heap's own spaces, under the
 * lock that collapses. The threads that build a game's shader cache keep what they build (a
 * pipeline and the driver's copy of its shaders, tens of KiB each): fifteen builders fill their
 * arenas after a few thousand pipelines, which is where Tears of the Kingdom's build of 16,000
 * fell from about 200 to 60 a second at every start, and where more CPUs stopped helping (that
 * this is the cause is read from the logs, not yet measured: EDEN_SHADER_START_HEAP counts it). A
 * thread whose arena is full now
 * moves to another of its own arenas with room, and makes a new one when none has (up to
 * EDEN_HEAP_THREAD_ARENAS, for requests under EDEN_HEAP_ARENA_GROW_REQUEST); a thread that
 * shares an arena because the first ones were taken makes one of its own the same way. A thread's
 * first arena is still one of EDEN_HEAP_FIRST_ARENAS: the many threads that allocate little do
 * not each take 8 MiB of the heap. Its arenas stay with
 * its record, so the next thread that takes the record uses them: they are made once for the
 * most a thread ever held, not again for each session. The small requests that still reach the
 * heap's own spaces, and the frees of blocks there, are counted with the time they took
 * (eden_heap_spill_stats; EDEN_SHADER_START_HEAP in stderr.log).
 *
 * Memory given back. The heap only grew: a Tears of the Kingdom session held 2.69 GB of pieces at
 * its end with 0.16 GB of them in use, and the next session grew from there. A space (the mspace
 * over one piece, or a run of pieces for a larger request) that holds no block is now given back
 * by eden_heap_trim: retired so no request uses it, its memory released to the system, its
 * address space kept, and its pieces made again (and the slot used again) by a later growth. A
 * space counts the blocks taken from it, and the threads inside it at the moment (so it is
 * retired only when none is); the first space, where the arenas are, stays. The app trims at the
 * end of a shader cache build, at the end of a session, when the pool the CPU shares with the GPU
 * is short, and before an allocation is refused (headless/performance.cpp TrimHeap).
 *
 * Large blocks the pool has no room for in one piece. A block of its own is one piece of direct
 * memory, and a growth of the heap was too: in a fragmented pool both failed for a 486 MiB request
 * with a gigabyte free in smaller pieces, and the session ended. A large block that finds no one
 * piece is now address space with memory behind it in pieces of whatever size the pool has
 * (eden_heap_split_pages), and a growth takes its pieces' memory the same way (src/memory_pages.cpp).
 *
 * Room in other threads' arenas. An arena is never given back, and its room is its thread's: what a
 * thread freed stays in pieces no other thread asks. Ocarina of Time reached the pool's limit with
 * 1097 MiB held and not in use, and the heap had grown 640 MiB for one scene load with 497 MiB of
 * that room already there. While the heap holds at least EDEN_HEAP_ADOPT_SLACK more than is in use,
 * a thread with no room in its own arenas now takes an arena no thread is using before it makes a
 * new one (EDEN_HEAP_NO_ADOPT in the environment turns this off). Arenas could already be shared:
 * mspaces take their own locks, and a block finds its arena by its address.
 *
 * Spaces for what stays and spaces for what goes. An arena is never freed, and a piece goes back
 * only with no block in it, so one arena in a piece keeps all 32 MiB: at nearly every refusal of
 * those sessions eden_heap_trim found no space to give back. With EDEN_HEAP_SCRATCH_SPACES in the
 * environment (a test, off by default) a new space holds either arenas and small blocks, or blocks
 * of 1 MiB and more, so that the latter empty and go back. Its price: the room beside a long-lived
 * block of 16 MiB is then no use to an arena. EDEN_HEAP_SPACES says which it is.
 *
 * What holds the room. EDEN_HEAP_GROWTH (every 20 s) says which requests made the heap grow and
 * who asked; at a refusal, eden_heap_arena_report asks every arena and space how much it has left.
 *
 * Empty arenas spared. An arena stayed with its record for good, full or empty: Ocarina of Time
 * (run 51) held 976 MiB it did not use, 589 MiB of it room in 458 arenas, 155 of them empty, and
 * the shader builders alone held 100 empty arenas of their 241; run 57 ended with 155 empty of
 * 296. An arena that holds no block now leaves its record when its thread ends
 * (eden_heap_thread_exit) and whenever the heap is trimmed (eden_heap_trim: the pool short, a
 * build or session over, an allocation about to be refused), from every record, the ended ones
 * too, and goes to the spares (eden_heap_spare): the next thread that needs an arena, its first
 * or one more, takes a spare before the heap makes a new one. A record keeps its first arena, so
 * the next thread on it starts with one; an arena a thread is on now, or one the record of an
 * ended thread still names, stays. Every arena counts its blocks for this (a block kept in a
 * thread's cache is one); the paths that read the list of arenas or a record's arenas (a thread
 * out of room, the reports) run under the creation lock, the lock a spare is taken and given
 * under. The arena's memory is not handed back to its space: an mspace cannot be destroyed with
 * this SDK (libSceLibcInternal has no sceLibcMspaceDestroy), and a 2 MiB block whose mspace libc
 * still knows went wrong in run 54. So the heap no longer grows for arenas past the most that
 * were ever full at once: run 57 grew 576 MiB for arenas with 141 in use at the end.
 * EDEN_HEAP_KEEP_ARENAS in the environment keeps every arena with its record, as before.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define EDEN_HEAP_ARENA_SHIFT 21u /* 2 MiB arenas and slots */
#define EDEN_HEAP_ARENA_SIZE (1u << EDEN_HEAP_ARENA_SHIFT)
/* Leave room for the parent mspace's chunk metadata before the next aligned slot.
 * A full-slot payload forces the next aligned allocation to skip a whole slot. */
#define EDEN_HEAP_ARENA_PAYLOAD (EDEN_HEAP_ARENA_SIZE - 16384u)
/* Absolute 8 MiB slots covering the heap (+1: the base need not be slot aligned). */
#define EDEN_HEAP_SLOTS ((PS5_OPENGL_HEAP_SIZE >> EDEN_HEAP_ARENA_SHIFT) + 1u)
/* Threads that get an arena of their own at their first small request; later ones share them. */
#define EDEN_HEAP_FIRST_ARENAS 32u
/* Every arena, with those made because a thread's were full: at most 2 GiB of the heap's 6 GiB. An
 * arena stays with its thread's record when its blocks are freed (the next thread on the record
 * uses it), so this is also the most the heap keeps for small blocks alone. */
#define EDEN_HEAP_MAX_ARENAS 1024u
/* One thread's arenas: at most 256 MiB of small blocks before they go to the heap's spaces. */
#define EDEN_HEAP_THREAD_ARENAS 128u
/* A request this large or larger that finds no room in the thread's arenas makes no new one. */
#define EDEN_HEAP_ARENA_GROW_REQUEST (64u * 1024u)
/* After a thread found all its arenas full and could make no other, the requests it makes before
 * it looks through them again (they take room in the heap's spaces meanwhile). */
#define EDEN_HEAP_ARENA_RETRY 256u
/* The heap holds this much more than is in use: a thread out of room looks in other arenas first. */
#define EDEN_HEAP_ADOPT_SLACK ((size_t)256 << 20)
/* Arenas a thread asks each time, and the room in one piece an arena must have to be taken: a
 * sliver would send the thread looking again after a few blocks. */
#define EDEN_HEAP_ADOPT_TRIES 16u
#define EDEN_HEAP_ADOPT_ROOM ((size_t)256 << 10)
/* The threads looked at, on the stack of whichever thread asks; past them two may share an arena. */
#define EDEN_HEAP_ADOPT_BUSY 128u
/* Larger requests (and all over-aligned ones) always use the heap's own spaces. */
#define EDEN_HEAP_SMALL_REQUEST (256u * 1024u)
/* The heap grows by pieces of this size. */
#define EDEN_HEAP_PIECE_SHIFT 25u /* 32 MiB: share remaining memory with graphics in smaller steps */
#define EDEN_HEAP_PIECE_SIZE ((size_t)1 << EDEN_HEAP_PIECE_SHIFT)
#define EDEN_HEAP_PIECES (PS5_OPENGL_HEAP_SIZE >> EDEN_HEAP_PIECE_SHIFT)

void *eden_heap_reserve(size_t size);             /* address space only; NULL when refused */
int eden_heap_commit(void *address, size_t size); /* zeroed memory for a part of it; 0 when done */
/* Its memory back to the system, the address space kept: 1 done, 0 nothing done, -1 in part. */
int eden_heap_decommit(void *address, size_t size);
void *eden_heap_split_pages(size_t size);         /* a large block in pieces of memory; NULL: none */
void eden_heap_split_pages_free(void *address, size_t size);
/* The block kept for a big image (EDEN_BIG_IMAGE_RESERVE_MIB, headless/heap_pages.cpp): for a large
 * block that found no single free range; NULL: no reserve, given out, or the wrong size. */
void *eden_heap_reserve_pages(size_t size);
void eden_heap_reserve_pages_free(void *address, size_t size);
/* 1: a block of this size is of the reserve's class and may not be taken in pieces
 * (EDEN_BIG_IMAGE_NO_SPLIT): the request fails instead. */
int eden_heap_big_image_refuses(size_t size);

static void *_Atomic eden_heap_slots[EDEN_HEAP_SLOTS]; /* arena mspace owning each slot */
static void *eden_heap_arena_list[EDEN_HEAP_MAX_ARENAS]; /* NULL: given back, free for a new one */
static void *_Atomic eden_heap_arena_blocks_probe[EDEN_HEAP_MAX_ARENAS];
static atomic_size_t eden_heap_arena_backing_probe;
static atomic_uint eden_heap_arena_count; /* entries of the list in use, those given back too */
static atomic_uint eden_heap_arena_holes; /* entries given back and not taken again */
static atomic_ullong eden_heap_arenas_returned; /* arenas that left a record for the spares */
static atomic_ullong eden_heap_arenas_taken_again; /* spares taken instead of a new arena */
/* Empty arenas no record holds, for the next thread that needs one (under the creation lock). */
static void *eden_heap_spare[EDEN_HEAP_MAX_ARENAS];
static atomic_uint eden_heap_spare_count;
/* The blocks alive in each slot's arena, one cache line each: the first arenas of threads made
 * together lie in neighbouring slots, and their every request would share one line otherwise. */
static struct {
  atomic_long blocks;
  char pad[64 - sizeof(atomic_long)];
} __attribute__((aligned(64))) eden_heap_arena_live[EDEN_HEAP_SLOTS];
static unsigned eden_heap_first_arenas; /* first arenas made (under the creation lock) */
static atomic_uint eden_heap_next_shared;
static pthread_key_t eden_heap_arena_key;
static int eden_heap_arena_key_valid;
static atomic_flag eden_heap_arena_create_lock = ATOMIC_FLAG_INIT;
/* Who holds the creation lock and how many threads wait for it (read by the development stall
 * watchdog, headless/stall_watchdog.h). */
static _Atomic unsigned long eden_heap_create_holder;
static atomic_uint eden_heap_create_waiters;

/* The heap's own mspaces, a slot each; each spans one or more pieces in a row. Slot 0 is the
 * first space and stays. A slot whose space was given back (eden_heap_trim) is retired, and a
 * later growth may use it again. */
static void *_Atomic eden_heap_spaces[EDEN_HEAP_PIECES];
static atomic_uint eden_heap_space_count;                    /* slots in use, retired ones too */
static atomic_uint eden_heap_space_users[EDEN_HEAP_PIECES];  /* threads in the slot's space now */
static atomic_int eden_heap_space_retired[EDEN_HEAP_PIECES]; /* 1: no request uses it */
static atomic_long eden_heap_space_blocks[EDEN_HEAP_PIECES]; /* blocks taken from it, not freed */
/* What a slot's space takes: anything, unless EDEN_HEAP_SCRATCH_SPACES made it one of the others. */
enum { EDEN_HEAP_CLASS_ANY = 0, EDEN_HEAP_CLASS_STAYS = 1, EDEN_HEAP_CLASS_SCRATCH = 2 };
static atomic_uchar eden_heap_space_class[EDEN_HEAP_PIECES];
/* A slot's pieces, [first, first + span); span 0: given back (both under the growth lock). */
static unsigned eden_heap_space_first[EDEN_HEAP_PIECES], eden_heap_space_span[EDEN_HEAP_PIECES];
/* Bumped by every growth: a request that found no room tries again before growing itself. */
static atomic_uint eden_heap_space_generation;
/* The mspace each piece belongs to, and its slot. */
static void *_Atomic eden_heap_piece_space[EDEN_HEAP_PIECES];
static atomic_uint eden_heap_piece_slot[EDEN_HEAP_PIECES];
/* A piece below `used`: 0 in a space, 1 given back (a growth may take it again), 2 given back in
 * part (never used again; not the heap's to free into). */
static atomic_uchar eden_heap_piece_state[EDEN_HEAP_PIECES];
/* What the pieces hold now, and what was given back since the process began. */
static atomic_size_t eden_heap_committed_bytes, eden_heap_returned_bytes;
/* Where no range can be reserved, the heap takes this much at once. Its 6 GiB taken whole would
 * leave graphics too little of the 12 GiB pool. */
#define EDEN_HEAP_WHOLE_SIZE ((size_t)3072 << 20)
/* Pieces committed so far: [0, used). All of them when the heap could not be reserved to grow. */
static atomic_uint eden_heap_pieces_used;
static int eden_heap_grows;
/* The heap's range: PS5_OPENGL_HEAP_SIZE when it grows, EDEN_HEAP_WHOLE_SIZE when taken whole. */
static size_t eden_heap_range_size;
static atomic_uint eden_heap_space_hint; /* where the last allocation from the spaces succeeded */
static atomic_flag eden_heap_grow_lock = ATOMIC_FLAG_INIT;

/* Requests of this size or more get memory of their own (eden_heap_pages), freed with them. */
#define EDEN_HEAP_LARGE_REQUEST ((size_t)32 << 20)
/* Entries for blocks with memory of their own. 1024 was full at the Castle Town gate with
 * EDEN_HEAP_LARGE_KIB=256 (crash-20261109-201137: 1062 MiB in 1024 blocks, ~900 more of them in the
 * last seconds), and a request that finds no entry goes back to the heap's 32 MiB pieces, which a
 * pool down to 2 MiB free blocks cannot give, where a block of its own would still have fitted.
 * 8192 entries are 192 KiB. A lookup only scans up to the highest entry ever used. */
#define EDEN_HEAP_LARGE_BLOCKS 8192u
/* EDEN_HEAP_LARGE_MIB=n in env.txt (1 to 32; without it 32, as before): requests of n MiB or more
 * get memory of their own. A block of 8 to 32 MiB took room in a 32 MiB piece, and a piece goes
 * back only with no block left in it: Ocarina of Time on a base console (crash-20261109-170104)
 * ended with the pool at 0 while the heap held 1728 MiB with 644 MiB in use, 768 MiB of its
 * growth asked by requests of 8 to 32 MiB. A block of its own goes back the moment it is freed.
 * Its price is a mapping and a release for each (counted: EDEN_HEAP_MID, every 20 s) and the
 * 2 MiB lead of a block of 2 MiB or more. Set from main after env.txt (eden_heap_set_large). */
static atomic_size_t eden_heap_large_from = EDEN_HEAP_LARGE_REQUEST;
/* Blocks under EDEN_HEAP_LARGE_REQUEST with memory of their own: taken, refused (those went to
 * the heap's spaces), alive now and at most, and the cycles their mappings and releases took. */
static atomic_ullong eden_heap_mid_taken, eden_heap_mid_refused, eden_heap_mid_cycles, eden_heap_mid_taken_bytes;
static atomic_size_t eden_heap_mid_bytes, eden_heap_mid_peak;
static atomic_uint eden_heap_mid_count;
/* Requests that do not go to an arena, by size, whichever way they went: under 1 MiB, under 2,
 * under 8, under 32 (count and bytes). They say what a lower EDEN_HEAP_LARGE_KIB would cost. */
static atomic_ullong eden_heap_class_count[4], eden_heap_class_bytes[4];
static void eden_heap_class_note(size_t bytes) {
  const unsigned kind = bytes < ((size_t)1 << 20) ? 0u : bytes < ((size_t)2 << 20) ? 1u : bytes < ((size_t)8 << 20) ? 2u : 3u;
  atomic_fetch_add_explicit(&eden_heap_class_count[kind], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&eden_heap_class_bytes[kind], bytes, memory_order_relaxed);
}
/* In KiB: 256 (the arenas take what is smaller) to 32768. EDEN_HEAP_LARGE_KIB, or
 * EDEN_HEAP_LARGE_MIB in MiB. A block of 2 MiB or more is whole 2 MiB pages of the pool; a
 * smaller one is 16 KiB pages, which leaves the pool in smaller pieces. */
void eden_heap_set_large(size_t kib) {
  if (kib < 256u)
    kib = 256u;
  if (kib > 32768u)
    kib = 32768u;
  atomic_store_explicit(&eden_heap_large_from, kib << 10, memory_order_relaxed);
}
/* The large blocks alive: address 0 marks a free entry, 1 one being filled in. */
static struct {
  _Atomic uintptr_t address;
  size_t size;
  int split; /* its memory in pieces (eden_heap_split_pages) */
} eden_heap_large[EDEN_HEAP_LARGE_BLOCKS];
static atomic_uint eden_heap_large_count;
/* One past the highest entry ever taken, so a lookup stops there; and requests that found every
 * entry taken (they went to the heap's spaces). */
static atomic_uint eden_heap_large_high;
static atomic_ullong eden_heap_large_table_full;
static atomic_size_t eden_heap_large_bytes;

int sceKernelUsleep(unsigned int microseconds);

/* Arena creation is rare (once per thread) but runs under a spin flag, and its holder also takes
 * an mspace lock. The emulated cores and the GPU thread run at real-time priority pinned to
 * one CPU each; spinning there forever starves a preempted lower-priority holder queued on that
 * CPU, which hung game starts. After a short spin, sleep so the holder can run. */
static void eden_heap_create_acquire(void) {
  if (atomic_flag_test_and_set_explicit(&eden_heap_arena_create_lock, memory_order_acquire)) {
    atomic_fetch_add_explicit(&eden_heap_create_waiters, 1, memory_order_relaxed);
    for (unsigned spins = 0;
         atomic_flag_test_and_set_explicit(&eden_heap_arena_create_lock, memory_order_acquire); ++spins) {
      if (spins < 64)
        __builtin_ia32_pause();
      else
        sceKernelUsleep(50);
    }
    atomic_fetch_sub_explicit(&eden_heap_create_waiters, 1, memory_order_relaxed);
  }
  atomic_store_explicit(&eden_heap_create_holder, (unsigned long)pthread_self(), memory_order_relaxed);
}

static void eden_heap_create_release(void) {
  atomic_store_explicit(&eden_heap_create_holder, 0, memory_order_relaxed);
  atomic_flag_clear_explicit(&eden_heap_arena_create_lock, memory_order_release);
}

/* Development stall reports: the creation lock's holder (0: free) and its waiters. */
unsigned long eden_heap_create_lock_state(unsigned *waiters) {
  if (waiters)
    *waiters = atomic_load_explicit(&eden_heap_create_waiters, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_create_holder, memory_order_relaxed);
}

static void *eden_heap_owner(const void *address);
static void eden_heap_free_owned(void *address);
struct eden_heap_thread;
static void eden_heap_return_record(struct eden_heap_thread *record, int ended);
static unsigned eden_heap_return_arenas(void);
static void eden_heap_use_arena(struct eden_heap_thread *self, void *arena);

/* Empty arenas stay with their records, as they did before (EDEN_HEAP_KEEP_ARENAS). */
static int eden_heap_keeps_arenas(void) {
  return getenv("EDEN_HEAP_KEEP_ARENAS") != NULL;
}

/* Per-thread allocation statistics. The live/peak/block counters are process-wide atomics, and
 * updating them on every malloc and free made all allocating threads bounce the same cache
 * lines: EDEN_PERF_ALLOC measured 210 ns per malloc/free pair alone and 874/1590 ns with 4/8
 * threads, while guest cores compile JIT blocks (allocation-heavy) together during loads. Each
 * thread now accumulates its changes and applies them every 64 operations, beyond 1 MiB, and at
 * thread exit; snapshots lag by that much. The record also keeps the thread's arena, and a
 * record freed by an exiting thread is reused with its arena by the next thread. */
/* Freed small blocks are kept per thread and reused without calling into the mspace, whose
 * calls serialise across threads even with separate arenas (EDEN_PERF_ALLOC with per-thread
 * arenas and statistics: 232 ns per pair alone, 803/1423 ns with 4/8 threads). A bin holds blocks
 * whose usable size is in [16 * bin, 16 * bin + 15], linked through their first word, with the
 * usable size in the second; a request of size s takes from bin ceil(s / 16). Bins are bounded,
 * and an exiting thread returns its blocks to their mspaces. */
#define EDEN_TCACHE_BINS 128u
#define EDEN_TCACHE_LIMIT 64u
struct eden_heap_thread {
  void *arena;   /* the arena small requests try first: one of `own`, or one shared at the limit */
  void *own[EDEN_HEAP_THREAD_ARENAS]; /* the arenas this record made, [0, owned) */
  unsigned owned;
  unsigned retry; /* requests to make before looking through `own` again (EDEN_HEAP_ARENA_RETRY) */
  unsigned grown; /* pieces the heap took for this record's requests */
  char name[16];  /* what the thread on this record called itself, for the reports */
  long live;
  long blocks;
  unsigned pending;
  /* Small requests that went to the heap's spaces, frees there, and their cycles: added to the
   * process's totals with live and blocks (eden_heap_flush). */
  unsigned long long spills, spill_cycles, shared_frees, shared_free_cycles;
  void *bins[EDEN_TCACHE_BINS];
  unsigned char counts[EDEN_TCACHE_BINS];
};
#define EDEN_HEAP_THREADS 512u
static struct eden_heap_thread eden_heap_threads[EDEN_HEAP_THREADS];
static atomic_uchar eden_heap_thread_used[EDEN_HEAP_THREADS];

static void eden_heap_apply(long bytes, long blocks) {
  if (blocks)
    atomic_fetch_add_explicit(&ps5_heap_blocks, (size_t)blocks, memory_order_relaxed);
  if (bytes >= 0)
    ps5_heap_resize_stats(0, (size_t)bytes);
  else
    ps5_heap_resize_stats((size_t)-bytes, 0);
}

/* The process's totals of the small requests the arenas could not take and of the frees of blocks
 * in the heap's spaces (eden_heap_spill_stats), with their processor cycles. */
static atomic_ullong eden_heap_spills, eden_heap_spill_cycles, eden_heap_shared_frees,
    eden_heap_shared_free_cycles;

/* The processor's cycle counter, where there is one (the console and the x86-64 host). */
static unsigned long long eden_heap_cycles(void) {
#if defined(__x86_64__)
  return __builtin_ia32_rdtsc();
#else
  return 0;
#endif
}

static void eden_heap_flush(struct eden_heap_thread *self) {
  eden_heap_apply(self->live, self->blocks);
  self->live = 0;
  self->blocks = 0;
  self->pending = 0;
  if (self->spills | self->shared_frees) {
    atomic_fetch_add_explicit(&eden_heap_spills, self->spills, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_spill_cycles, self->spill_cycles, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_frees, self->shared_frees, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_free_cycles, self->shared_free_cycles, memory_order_relaxed);
    self->spills = self->spill_cycles = self->shared_frees = self->shared_free_cycles = 0;
  }
}

/* Small requests that went to the heap's spaces and frees of blocks there since the process began,
 * and the cycles they took (the counter's rate is the caller's to find). A thread's last 64
 * operations may not be in them yet. */
void eden_heap_spill_stats(unsigned long long *spills, unsigned long long *spill_cycles,
                           unsigned long long *frees, unsigned long long *free_cycles) {
  if (spills)
    *spills = atomic_load_explicit(&eden_heap_spills, memory_order_relaxed);
  if (spill_cycles)
    *spill_cycles = atomic_load_explicit(&eden_heap_spill_cycles, memory_order_relaxed);
  if (frees)
    *frees = atomic_load_explicit(&eden_heap_shared_frees, memory_order_relaxed);
  if (free_cycles)
    *free_cycles = atomic_load_explicit(&eden_heap_shared_free_cycles, memory_order_relaxed);
}

static void eden_heap_thread_exit(void *value) {
  struct eden_heap_thread *self = value;
  for (unsigned bin = 0; bin < EDEN_TCACHE_BINS; ++bin) {
    while (self->bins[bin] != NULL) {
      void *block = self->bins[bin];
      self->bins[bin] = *(void **)block;
      eden_heap_free_owned(block);
    }
    __atomic_store_n(&self->counts[bin], 0, __ATOMIC_RELAXED);
  }
  eden_heap_flush(self);
  /* Its empty arenas go back; the record still holds the thread's, so no other thread takes it. */
  if (!eden_heap_keeps_arenas()) {
    eden_heap_create_acquire();
    eden_heap_return_record(self, 1);
    eden_heap_create_release();
  }
  atomic_store_explicit(&eden_heap_thread_used[self - eden_heap_threads], 0, memory_order_release);
}

/* The arenas there are now (the list's entries given back are not counted). */
unsigned eden_heap_arenas_created(void) {
  return atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed) -
         atomic_load_explicit(&eden_heap_arena_holes, memory_order_relaxed);
}

/* Empty arenas that left their records for the spares since the process began. */
unsigned long long eden_heap_arenas_given_back(void) {
  return atomic_load_explicit(&eden_heap_arenas_returned, memory_order_relaxed);
}

/* Spare arenas now: empty, held by no record, for the next thread that needs one. */
unsigned eden_heap_arenas_spare(void) {
  return atomic_load_explicit(&eden_heap_spare_count, memory_order_relaxed);
}

/* Spares taken instead of a new arena since the process began. */
unsigned long long eden_heap_arenas_reused(void) {
  return atomic_load_explicit(&eden_heap_arenas_taken_again, memory_order_relaxed);
}

/* Whether `arena` is a spare (under the creation lock). */
static int eden_heap_is_spare(const void *arena) {
  const unsigned count = atomic_load_explicit(&eden_heap_spare_count, memory_order_relaxed);
  for (unsigned i = 0; i < count; ++i)
    if (eden_heap_spare[i] == arena)
      return 1;
  return 0;
}

/* Memory the heap's pieces hold now (pieces given back are not counted). */
size_t eden_heap_committed(void) {
  return atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed);
}

/* What the heap holds (its pieces and its large blocks), what of that its blocks use (its own
 * count, which lags each thread's last 64 operations), and what it has given back. */
void eden_heap_usage(size_t *held, size_t *in_use, size_t *returned) {
  if (held)
    *held = atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed) +
            atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  if (in_use)
    *in_use = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  if (returned)
    *returned = atomic_load_explicit(&eden_heap_returned_bytes, memory_order_relaxed);
}

/* Memory of the large blocks alive now, and how many there are. */
size_t eden_heap_large_held(unsigned *blocks) {
  if (blocks)
    *blocks = atomic_load_explicit(&eden_heap_large_count, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
}

/* The entry of a large block, or NULL: most addresses are not one, and with none alive
 * nothing is searched. */
static int eden_heap_large_find(const void *address) {
  if (atomic_load_explicit(&eden_heap_large_count, memory_order_acquire) == 0 || address == NULL)
    return -1;
  const unsigned high = atomic_load_explicit(&eden_heap_large_high, memory_order_acquire);
  for (unsigned i = 0; i < high && i < EDEN_HEAP_LARGE_BLOCKS; ++i)
    if (atomic_load_explicit(&eden_heap_large[i].address, memory_order_acquire) == (uintptr_t)address)
      return (int)i;
  return -1;
}

/* A large block of its own (zeroed), or NULL: no entry free, or no memory. */
static void *eden_heap_large_allocate_inner(size_t bytes);
static void *eden_heap_large_allocate(size_t bytes) {
  if (bytes >= EDEN_HEAP_LARGE_REQUEST)
    return eden_heap_large_allocate_inner(bytes);
  const unsigned long long start = __builtin_ia32_rdtsc();
  void *block = eden_heap_large_allocate_inner(bytes);
  atomic_fetch_add_explicit(&eden_heap_mid_cycles, __builtin_ia32_rdtsc() - start, memory_order_relaxed);
  if (block == NULL) {
    atomic_fetch_add_explicit(&eden_heap_mid_refused, 1, memory_order_relaxed);
    return NULL;
  }
  atomic_fetch_add_explicit(&eden_heap_mid_taken, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&eden_heap_mid_taken_bytes, bytes, memory_order_relaxed);
  atomic_fetch_add_explicit(&eden_heap_mid_count, 1, memory_order_relaxed);
  const size_t now = atomic_fetch_add_explicit(&eden_heap_mid_bytes, bytes, memory_order_relaxed) + bytes;
  size_t peak = atomic_load_explicit(&eden_heap_mid_peak, memory_order_relaxed);
  while (now > peak &&
         !atomic_compare_exchange_weak_explicit(&eden_heap_mid_peak, &peak, now, memory_order_relaxed, memory_order_relaxed)) {
  }
  return block;
}

static void *eden_heap_large_allocate_inner(size_t bytes) {
  for (unsigned i = 0; i < EDEN_HEAP_LARGE_BLOCKS; ++i) {
    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&eden_heap_large[i].address, &expected, 1, memory_order_acquire,
                                                 memory_order_relaxed))
      continue;
    /* Raised before the entry names its block, so a lookup that finds the address scans far enough. */
    unsigned high = atomic_load_explicit(&eden_heap_large_high, memory_order_relaxed);
    while (high < i + 1 && !atomic_compare_exchange_weak_explicit(&eden_heap_large_high, &high, i + 1,
                                                                  memory_order_release, memory_order_relaxed)) {
    }
    void *block = eden_heap_pages(bytes);
    int split = 0;
    if (block == NULL) {
      /* No free piece of the pool that large: the block kept for a big image, when there is one
       * and the request is its size (EDEN_BIG_IMAGE_RESERVE_MIB). */
      block = eden_heap_reserve_pages(bytes);
      if (block != NULL)
        split = 2;
    }
    if (block == NULL && !eden_heap_big_image_refuses(bytes)) {
      /* Memory in smaller pieces behind one range; not for a big image's block that the reserve
       * could not serve, with EDEN_BIG_IMAGE_NO_SPLIT (eden_heap_allocate gives up on it). */
      block = eden_heap_split_pages(bytes);
      split = 1;
    }
    if (block == NULL) {
      atomic_store_explicit(&eden_heap_large[i].address, 0, memory_order_release);
      return NULL;
    }
    eden_heap_large[i].size = bytes;
    eden_heap_large[i].split = split;
    atomic_fetch_add_explicit(&eden_heap_large_bytes, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_large_count, 1, memory_order_release);
    atomic_store_explicit(&eden_heap_large[i].address, (uintptr_t)block, memory_order_release);
    return block;
  }
  atomic_fetch_add_explicit(&eden_heap_large_table_full, 1, memory_order_relaxed);
  return NULL;
}

static void eden_heap_large_free(int entry) {
  void *block = (void *)atomic_load_explicit(&eden_heap_large[entry].address, memory_order_relaxed);
  const size_t bytes = eden_heap_large[entry].size;
  /* The entry stops naming the address before the memory goes: the next large block may be
   * given the same address at once. */
  atomic_store_explicit(&eden_heap_large[entry].address, 1, memory_order_release);
  const int mid = bytes < EDEN_HEAP_LARGE_REQUEST;
  const unsigned long long start = mid ? __builtin_ia32_rdtsc() : 0;
  if (eden_heap_large[entry].split == 2)
    eden_heap_reserve_pages_free(block, bytes);
  else if (eden_heap_large[entry].split)
    eden_heap_split_pages_free(block, bytes);
  else
    eden_heap_pages_free(block, bytes);
  if (mid) {
    atomic_fetch_add_explicit(&eden_heap_mid_cycles, __builtin_ia32_rdtsc() - start, memory_order_relaxed);
    atomic_fetch_sub_explicit(&eden_heap_mid_bytes, bytes, memory_order_relaxed);
    atomic_fetch_sub_explicit(&eden_heap_mid_count, 1, memory_order_relaxed);
  }
  atomic_fetch_sub_explicit(&eden_heap_large_bytes, bytes, memory_order_relaxed);
  atomic_fetch_sub_explicit(&eden_heap_large_count, 1, memory_order_release);
  atomic_store_explicit(&eden_heap_large[entry].address, 0, memory_order_release);
}

static int ps5_heap_ready(void) {
  int state = atomic_load_explicit(&ps5_heap_state, memory_order_acquire);
  if (state == 2)
    return 1;
  if (state != 0)
    return 0;

  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &ps5_heap_state, &expected, 1, memory_order_acq_rel,
          memory_order_acquire))
    return expected == 2;

  /* Address space for the whole heap and memory for its first piece; where a range cannot be
   * reserved, 3 GiB at once as before. */
  void *base = eden_heap_reserve(PS5_OPENGL_HEAP_SIZE);
  size_t first_size = EDEN_HEAP_PIECE_SIZE;
  unsigned first_pieces = 1;
  if (base != NULL && eden_heap_commit(base, EDEN_HEAP_PIECE_SIZE) == 0) {
    eden_heap_grows = 1;
  } else {
    base = eden_heap_pages(EDEN_HEAP_WHOLE_SIZE);
    first_size = EDEN_HEAP_WHOLE_SIZE;
    first_pieces = (unsigned)(EDEN_HEAP_WHOLE_SIZE >> EDEN_HEAP_PIECE_SHIFT);
  }
  if (base == NULL) {
    atomic_store_explicit(&ps5_heap_state, -1, memory_order_release);
    return 0;
  }

  ps5_heap_base = base;
  eden_heap_range_size = eden_heap_grows ? PS5_OPENGL_HEAP_SIZE : EDEN_HEAP_WHOLE_SIZE;
  ps5_heap_mspace = sceLibcMspaceCreate("Eden-headless", base, first_size, 0);
  if (ps5_heap_mspace == NULL) {
    ps5_heap_base = NULL;
    if (!eden_heap_grows)
      eden_heap_pages_free(base, EDEN_HEAP_WHOLE_SIZE);
    atomic_store_explicit(&ps5_heap_state, -1, memory_order_release);
    return 0;
  }
  atomic_store_explicit(&eden_heap_spaces[0], ps5_heap_mspace, memory_order_relaxed);
  eden_heap_space_first[0] = 0;
  eden_heap_space_span[0] = first_pieces;
  for (unsigned piece = 0; piece < first_pieces; ++piece)
    atomic_store_explicit(&eden_heap_piece_space[piece], ps5_heap_mspace, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_pieces_used, first_pieces, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_committed_bytes, first_size, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_space_count, 1, memory_order_release);
  /* libthr keeps specific data in its own storage; no recursion into malloc. */
  eden_heap_arena_key_valid = pthread_key_create(&eden_heap_arena_key, eden_heap_thread_exit) == 0;

  atomic_store_explicit(&ps5_heap_state, 2, memory_order_release);
  return 1;
}

/* A block inside the heap's range: an arena's or a piece's. */
static int eden_heap_in_range(const void *address) {
  /* Acquire publication before reading non-atomic heap metadata. */
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2)
    return 0;
  uintptr_t value = (uintptr_t)address;
  uintptr_t base = (uintptr_t)ps5_heap_base;
  if (value < base || value - base >= eden_heap_range_size)
    return 0;
  /* A piece given back in part is not the heap's any more. */
  return atomic_load_explicit(&eden_heap_piece_state[(value - base) >> EDEN_HEAP_PIECE_SHIFT],
                              memory_order_relaxed) != 2;
}

/* Any block of this heap: one in its range, or a large block with memory of its own. */
static int ps5_heap_owns(const void *address) {
  return eden_heap_in_range(address) || eden_heap_large_find(address) >= 0;
}

/* The usable size of a block this heap owns. */
static size_t eden_heap_usable(const void *address) {
  if (eden_heap_in_range(address))
    return sceLibcMspaceMallocUsableSize(address);
  const int entry = eden_heap_large_find(address);
  return entry >= 0 ? eden_heap_large[entry].size : 0;
}

static uintptr_t eden_heap_slot(const void *address) {
  return ((uintptr_t)address >> EDEN_HEAP_ARENA_SHIFT) -
         ((uintptr_t)ps5_heap_base >> EDEN_HEAP_ARENA_SHIFT);
}

/* A block taken from the arena of its slot, and one given back to it. The count is the mspace's:
 * a block in a thread's cache is still taken. It is read, for an arena no thread is on, under the
 * creation lock (eden_heap_return_record): a block is counted after it is taken and after it is
 * freed, so a count read early says the arena is fuller than it is, never emptier. A realloc to
 * size 0 that frees the block leaves it counted (the console does not say); that arena stays. */
static void eden_heap_arena_took(const void *address) {
  atomic_fetch_add_explicit(&eden_heap_arena_live[eden_heap_slot(address)].blocks, 1, memory_order_relaxed);
}

static void eden_heap_arena_gave(const void *address) {
  atomic_fetch_sub_explicit(&eden_heap_arena_live[eden_heap_slot(address)].blocks, 1, memory_order_relaxed);
}

/* The slot tail belongs to the parent mspace and can hold ordinary allocations. */
static void *eden_heap_arena_owner(const void *address) {
  if (((uintptr_t)address & (EDEN_HEAP_ARENA_SIZE - 1u)) >= EDEN_HEAP_ARENA_PAYLOAD)
    return NULL;
  return atomic_load_explicit(&eden_heap_slots[eden_heap_slot(address)], memory_order_acquire);
}

/* The mspace owning an owned block: its slot's arena, else the space of its piece. */
static void *eden_heap_owner(const void *address) {
  void *arena = eden_heap_arena_owner(address);
  if (arena != NULL)
    return arena;
  const uintptr_t piece = ((uintptr_t)address - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT;
  return atomic_load_explicit(&eden_heap_piece_space[piece], memory_order_acquire);
}

/* ---- Whole free pages (a measurement, reported as EDEN_HEAP_PAGES) ----
 * The bytes of the blocks handed out, per 2 MiB page of the range; a block kept in a thread's cache
 * counts as free. A heap that gives whole free pages back could return the committed pages that hold
 * none (an idle arena's page among them). This one cannot: Sony's mspaces keep their free lists in
 * the free memory itself. Asked for before replacing them (Mario Kart World's heap held 1952 MiB with
 * 1574 MiB of it counted in use). */
static _Atomic long eden_page_live[EDEN_HEAP_SLOTS];

static void eden_pages_count(const void *address, size_t bytes, int sign) {
  if (address == NULL || bytes == 0 || !eden_heap_in_range(address))
    return;
  uintptr_t at = (uintptr_t)address;
  const uintptr_t end = at + bytes;
  while (at < end) {
    const uintptr_t page_end = (at | (uintptr_t)(EDEN_HEAP_ARENA_SIZE - 1u)) + 1u;
    const uintptr_t part = (page_end < end ? page_end : end) - at;
    const uintptr_t slot = eden_heap_slot((const void *)at);
    if (slot < EDEN_HEAP_SLOTS)
      atomic_fetch_add_explicit(&eden_page_live[slot], sign > 0 ? (long)part : -(long)part,
                                memory_order_relaxed);
    at += part;
  }
}

/* The committed pages of the heap's pieces that hold no block in use, those of them that are an
 * arena's, and the bytes in use in the other pages. */
void eden_heap_page_usage(size_t *free_pages, size_t *idle_arenas, size_t *live) {
  size_t pages = 0, arenas = 0;
  long total = 0;
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) == 2) {
    const uintptr_t base = (uintptr_t)ps5_heap_base;
    const unsigned used = atomic_load_explicit(&eden_heap_pieces_used, memory_order_acquire);
    for (unsigned piece = 0; piece < used && piece < EDEN_HEAP_PIECES; ++piece) {
      if (atomic_load_explicit(&eden_heap_piece_state[piece], memory_order_relaxed) != 0)
        continue;
      const uintptr_t start = base + ((uintptr_t)piece << EDEN_HEAP_PIECE_SHIFT);
      const uintptr_t end = start + EDEN_HEAP_PIECE_SIZE;
      for (uintptr_t page = (start + EDEN_HEAP_ARENA_SIZE - 1u) & ~(uintptr_t)(EDEN_HEAP_ARENA_SIZE - 1u);
           page + EDEN_HEAP_ARENA_SIZE <= end; page += EDEN_HEAP_ARENA_SIZE) {
        const uintptr_t slot = eden_heap_slot((const void *)page);
        if (slot >= EDEN_HEAP_SLOTS)
          continue;
        const long bytes = atomic_load_explicit(&eden_page_live[slot], memory_order_relaxed);
        if (bytes > 0) {
          total += bytes;
          continue;
        }
        ++pages;
        if (atomic_load_explicit(&eden_heap_slots[slot], memory_order_relaxed) != NULL)
          ++arenas;
      }
    }
  }
  *free_pages = pages << EDEN_HEAP_ARENA_SHIFT;
  *idle_arenas = arenas << EDEN_HEAP_ARENA_SHIFT;
  *live = total > 0 ? (size_t)total : 0;
}

/* ---- the heap's own spaces ---- */

enum { EDEN_HEAP_MALLOC, EDEN_HEAP_CALLOC, EDEN_HEAP_ALIGNED };

/* Frees a block of a space (not an arena's), counting it off its space. */
static void eden_heap_space_free(void *address) {
  const uintptr_t piece = ((uintptr_t)address - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT;
  const unsigned slot = atomic_load_explicit(&eden_heap_piece_slot[piece], memory_order_acquire);
  sceLibcMspaceFree(atomic_load_explicit(&eden_heap_piece_space[piece], memory_order_acquire), address);
  atomic_fetch_sub_explicit(&eden_heap_space_blocks[slot], 1, memory_order_release);
}

/* Frees an owned block (not a large one) to its arena or its space. */
static void eden_heap_free_owned(void *address) {
  void *arena = eden_heap_arena_owner(address);
  if (arena != NULL) {
    sceLibcMspaceFree(arena, address);
    eden_heap_arena_gave(address);
  } else
    eden_heap_space_free(address);
}

/* One request to one mspace: malloc(a), calloc(a, b) or an a-aligned block of b bytes. */
static void *eden_heap_space_try(void *space, int kind, size_t a, size_t b) {
  if (kind == EDEN_HEAP_MALLOC)
    return sceLibcMspaceMalloc(space, a);
  if (kind == EDEN_HEAP_CALLOC)
    return sceLibcMspaceCalloc(space, a, b);
  void *address = NULL;
  return sceLibcMspacePosixMemalign(space, &address, a, b) == 0 ? address : NULL;
}

int sceKernelDebugOutText(int channel, const char *text);

/* Why the heap could not grow, for the kernel log: the allocation that asked is about to fail.
 * No stdio here (it allocates), and only the first few times. */
static void eden_heap_grow_refused(const char *line) {
  static atomic_uint reported;
  if (atomic_fetch_add_explicit(&reported, 1, memory_order_relaxed) < 8)
    sceKernelDebugOutText(0, line);
}

/* The last allocation the heap could not make, for the crash report (headless/crash_report.cpp):
 * a std::bad_alloc names neither the size nor the reason, and the report's memory line is read
 * after the failure, when other threads may have given memory back. `why` is the last reason the
 * heap gave for not growing (EDEN_HEAP_WHY_*), on whichever thread asked last. */
enum {
  EDEN_HEAP_WHY_NONE = 0,     /* it did not need to grow, or it grew */
  EDEN_HEAP_WHY_NO_SPACE = 1, /* memory, but no allocator over it */
  EDEN_HEAP_WHY_NO_MEMORY = 2,
  EDEN_HEAP_WHY_AT_LIMIT = 3,
  EDEN_HEAP_WHY_OVERFLOW = 4  /* calloc's count * size does not fit */
};
static atomic_uint eden_heap_grow_why;
static atomic_size_t eden_heap_failed_bytes;
static atomic_uint eden_heap_failed_why;

static void eden_heap_note_failure(size_t bytes, unsigned why) {
  atomic_store_explicit(&eden_heap_failed_bytes, bytes, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_failed_why, why, memory_order_relaxed);
}

static void eden_heap_failed(size_t bytes, unsigned why) {
  eden_heap_note_failure(bytes, why);
  atomic_fetch_add_explicit(&ps5_heap_failures, 1, memory_order_relaxed);
}

/* How many allocations failed, and the size and reason of the last one (0: none). */
size_t eden_heap_last_failure(unsigned *why, size_t *count) {
  if (why)
    *why = atomic_load_explicit(&eden_heap_failed_why, memory_order_relaxed);
  if (count)
    *count = atomic_load_explicit(&ps5_heap_failures, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_failed_bytes, memory_order_relaxed);
}

static void eden_heap_grow_acquire(void) {
  for (unsigned spins = 0; atomic_flag_test_and_set_explicit(&eden_heap_grow_lock, memory_order_acquire); ++spins) {
    if (spins < 64)
      __builtin_ia32_pause();
    else
      sceKernelUsleep(50);
  }
}

/* What made the heap grow: the pieces each kind of request took, and the largest of them. */
enum {
  EDEN_HEAP_GREW_ARENA, EDEN_HEAP_GREW_SMALL, EDEN_HEAP_GREW_1M, EDEN_HEAP_GREW_8M, EDEN_HEAP_GREW_32M,
  EDEN_HEAP_GREW_LARGE, EDEN_HEAP_GREW_KINDS
};
static atomic_uint eden_heap_grew_pieces[EDEN_HEAP_GREW_KINDS];
static atomic_size_t eden_heap_grew_largest[EDEN_HEAP_GREW_KINDS];

static struct eden_heap_thread *eden_heap_self(void);

/* The spaces a request may use: an arena's (its size and alignment together) and anything under
 * 1 MiB in the spaces that stay, the rest in the others. */
static unsigned eden_heap_request_class(size_t bytes) {
  return bytes >= ((size_t)1 << 20) && bytes != (size_t)EDEN_HEAP_ARENA_SIZE + EDEN_HEAP_ARENA_PAYLOAD
             ? EDEN_HEAP_CLASS_SCRATCH
             : EDEN_HEAP_CLASS_STAYS;
}

/* Called under the growth lock. */
static void eden_heap_note_growth(size_t bytes, unsigned pieces) {
  const unsigned kind = bytes == (size_t)EDEN_HEAP_ARENA_SIZE + EDEN_HEAP_ARENA_PAYLOAD ? EDEN_HEAP_GREW_ARENA
                        : bytes < EDEN_HEAP_SMALL_REQUEST                               ? EDEN_HEAP_GREW_SMALL
                        : bytes < ((size_t)1 << 20)                                     ? EDEN_HEAP_GREW_1M
                        : bytes < ((size_t)8 << 20)                                     ? EDEN_HEAP_GREW_8M
                        : bytes < EDEN_HEAP_LARGE_REQUEST                               ? EDEN_HEAP_GREW_32M
                                                                                        : EDEN_HEAP_GREW_LARGE;
  atomic_fetch_add_explicit(&eden_heap_grew_pieces[kind], pieces, memory_order_relaxed);
  if (bytes > atomic_load_explicit(&eden_heap_grew_largest[kind], memory_order_relaxed))
    atomic_store_explicit(&eden_heap_grew_largest[kind], bytes, memory_order_relaxed);
  struct eden_heap_thread *self = eden_heap_self();
  if (self != NULL)
    __atomic_fetch_add(&self->grown, pieces, __ATOMIC_RELAXED);
}

/* Adds a space with room for `bytes`: memory for pieces of the range (pieces given back earlier
 * first, else the next ones) and an mspace over them, in a slot given back earlier or a new one.
 * `seen` is the growth the caller tried after: when another thread grew the heap meanwhile, that
 * space is tried first. Returns 0 when the heap is at its limit or the console has no memory left. */
static int eden_heap_grow(size_t bytes, unsigned seen) {
  eden_heap_grow_acquire();
  int grown = 0;
  atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NONE, memory_order_relaxed);
  const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_relaxed);
  const unsigned used = atomic_load_explicit(&eden_heap_pieces_used, memory_order_relaxed);
  /* Room for the request and for the mspace's own records. */
  const size_t need = bytes + ((size_t)1 << 20);
  const size_t pieces = (need + EDEN_HEAP_PIECE_SIZE - 1) >> EDEN_HEAP_PIECE_SHIFT;
  /* Pieces given back, `pieces` of them in a row, else the next ones. */
  unsigned first = used;
  if (eden_heap_grows && need > bytes) {
    for (unsigned at = 0, run = 0; at < used; ++at) {
      run = atomic_load_explicit(&eden_heap_piece_state[at], memory_order_relaxed) == 1 ? run + 1 : 0;
      if (run == pieces) {
        first = at + 1 - (unsigned)pieces;
        break;
      }
    }
  }
  /* A slot given back, else a new one. */
  unsigned slot = count;
  for (unsigned at = 1; at < count; ++at)
    if (eden_heap_space_span[at] == 0) {
      slot = at;
      break;
    }
  if (atomic_load_explicit(&eden_heap_space_generation, memory_order_relaxed) != seen) {
    grown = 1;
  } else if (eden_heap_grows && need > bytes && slot < EDEN_HEAP_PIECES &&
             (first < used || pieces <= EDEN_HEAP_PIECES - used)) {
    void *start = (char *)ps5_heap_base + ((size_t)first << EDEN_HEAP_PIECE_SHIFT);
    const size_t size = pieces << EDEN_HEAP_PIECE_SHIFT;
    if (eden_heap_commit(start, size) == 0) {
      void *space = sceLibcMspaceCreate("Eden-heap", start, size, 0);
      atomic_fetch_add_explicit(&eden_heap_committed_bytes, size, memory_order_relaxed);
      if (first == used)
        atomic_store_explicit(&eden_heap_pieces_used, used + (unsigned)pieces, memory_order_relaxed);
      if (space != NULL) {
        for (size_t piece = first; piece < first + pieces; ++piece) {
          atomic_store_explicit(&eden_heap_piece_space[piece], space, memory_order_release);
          atomic_store_explicit(&eden_heap_piece_slot[piece], slot, memory_order_release);
          atomic_store_explicit(&eden_heap_piece_state[piece], 0, memory_order_relaxed);
        }
        eden_heap_space_first[slot] = first;
        eden_heap_space_span[slot] = (unsigned)pieces;
        atomic_store_explicit(&eden_heap_space_blocks[slot], 0, memory_order_relaxed);
        atomic_store_explicit(&eden_heap_space_class[slot],
                              getenv("EDEN_HEAP_SCRATCH_SPACES") != NULL ? eden_heap_request_class(bytes) : EDEN_HEAP_CLASS_ANY,
                              memory_order_relaxed);
        atomic_store_explicit(&eden_heap_spaces[slot], space, memory_order_release);
        if (slot == count)
          atomic_store_explicit(&eden_heap_space_count, count + 1, memory_order_release);
        else
          atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
        atomic_fetch_add_explicit(&eden_heap_space_generation, 1, memory_order_release);
        eden_heap_note_growth(bytes, (unsigned)pieces);
        grown = 1;
      } else {
        /* The memory stays committed but no space is over it: given back at once. */
        if (eden_heap_decommit(start, size) == 1) {
          atomic_fetch_sub_explicit(&eden_heap_committed_bytes, size, memory_order_relaxed);
          for (size_t piece = first; piece < first + pieces; ++piece)
            atomic_store_explicit(&eden_heap_piece_state[piece], 1, memory_order_relaxed);
        }
        atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NO_SPACE, memory_order_relaxed);
        eden_heap_grow_refused("EDEN_HEAP_GROW refused: no allocator for a new piece\n");
      }
    } else {
      atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NO_MEMORY, memory_order_relaxed);
      eden_heap_grow_refused("EDEN_HEAP_GROW refused: the console has no memory for a new piece\n");
    }
  } else {
    atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_AT_LIMIT, memory_order_relaxed);
    eden_heap_grow_refused("EDEN_HEAP_GROW refused: the heap is at its limit\n");
  }
  atomic_flag_clear_explicit(&eden_heap_grow_lock, memory_order_release);
  return grown;
}

/* Gives the memory of every space (but the first) that holds no block back to the system: the
 * space is retired, its memory released, its address space kept for a later growth. Returns the
 * bytes given back. */
size_t eden_heap_trim(void) {
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2)
    return 0;
  /* Empty arenas to the spares first (their memory stays in their spaces). */
  eden_heap_return_arenas();
  if (!eden_heap_grows)
    return 0;
  eden_heap_grow_acquire();
  size_t returned = 0;
  const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_relaxed);
  for (unsigned slot = 1; slot < count; ++slot) {
    if (eden_heap_space_span[slot] == 0 ||
        atomic_load_explicit(&eden_heap_space_blocks[slot], memory_order_relaxed) != 0)
      continue;
    atomic_store_explicit(&eden_heap_space_retired[slot], 1, memory_order_seq_cst);
    if (atomic_load_explicit(&eden_heap_space_users[slot], memory_order_seq_cst) != 0 ||
        atomic_load_explicit(&eden_heap_space_blocks[slot], memory_order_acquire) != 0) {
      atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
      continue;
    }
    const unsigned first = eden_heap_space_first[slot], span = eden_heap_space_span[slot];
    const size_t size = (size_t)span << EDEN_HEAP_PIECE_SHIFT;
    const int done = eden_heap_decommit((char *)ps5_heap_base + ((size_t)first << EDEN_HEAP_PIECE_SHIFT), size);
    if (done == 0) {
      /* Kept as it was (dev-settings heap_return=off, or nothing to give back): usable again. */
      atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
      continue;
    }
    for (unsigned piece = first; piece < first + span; ++piece) {
      atomic_store_explicit(&eden_heap_piece_space[piece], NULL, memory_order_release);
      atomic_store_explicit(&eden_heap_piece_state[piece], done > 0 ? 1 : 2, memory_order_relaxed);
    }
    eden_heap_space_span[slot] = 0;
    atomic_store_explicit(&eden_heap_spaces[slot], NULL, memory_order_release);
    atomic_fetch_sub_explicit(&eden_heap_committed_bytes, size, memory_order_relaxed);
    if (done > 0)
      returned += size;
  }
  atomic_flag_clear_explicit(&eden_heap_grow_lock, memory_order_release);
  atomic_fetch_add_explicit(&eden_heap_returned_bytes, returned, memory_order_relaxed);
  return returned;
}

/* A block from the heap's spaces, then from a new one, and when there is no memory for one, from
 * a space of the other class. A small request starts at the space that served the last one (it is
 * asked for millions of times: compiled-code records); a request of 1 MiB or more starts at the
 * first space, which keeps the later ones free of long-lived blocks and able to take the next large
 * request. */
/* One request to one slot's space, unless the slot is retired. A thread counts itself in the
 * slot first and then looks at the flag; eden_heap_trim sets the flag first and then looks at the
 * count (both sequentially consistent): a space is given back only when no thread is in it, and
 * none can enter it once it is retired. */
static void *eden_heap_space_use(unsigned slot, int kind, size_t a, size_t b) {
  atomic_fetch_add_explicit(&eden_heap_space_users[slot], 1, memory_order_seq_cst);
  void *address = NULL;
  if (atomic_load_explicit(&eden_heap_space_retired[slot], memory_order_seq_cst) == 0) {
    address = eden_heap_space_try(atomic_load_explicit(&eden_heap_spaces[slot], memory_order_acquire), kind, a, b);
    if (address != NULL)
      atomic_fetch_add_explicit(&eden_heap_space_blocks[slot], 1, memory_order_relaxed);
  }
  atomic_fetch_sub_explicit(&eden_heap_space_users[slot], 1, memory_order_release);
  return address;
}

static void *eden_heap_main(int kind, size_t a, size_t b) {
  const size_t bytes = kind == EDEN_HEAP_MALLOC ? a : kind == EDEN_HEAP_CALLOC ? a * b : a + b;
  unsigned wanted = eden_heap_request_class(bytes);
  for (;;) {
    const unsigned generation = atomic_load_explicit(&eden_heap_space_generation, memory_order_acquire);
    const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_acquire);
    const int small = bytes < ((size_t)1 << 20);
    const unsigned hint = small ? atomic_load_explicit(&eden_heap_space_hint, memory_order_relaxed) : 0;
    int passed_over = 0;
    for (unsigned i = 0; i < count; ++i) {
      const unsigned index = (hint + i) % count;
      const unsigned kept = atomic_load_explicit(&eden_heap_space_class[index], memory_order_relaxed);
      if (wanted != EDEN_HEAP_CLASS_ANY && kept != wanted && kept != EDEN_HEAP_CLASS_ANY) {
        passed_over = 1;
        continue;
      }
      void *address = eden_heap_space_use(index, kind, a, b);
      if (address != NULL) {
        if (small && i != 0)
          atomic_store_explicit(&eden_heap_space_hint, index, memory_order_relaxed);
        return address;
      }
    }
    if (bytes == 0)
      return NULL;
    if (!eden_heap_grow(bytes, generation)) {
      if (!passed_over)
        return NULL;
      wanted = EDEN_HEAP_CLASS_ANY;
    }
  }
}

/* Allocate and register one arena block from the heap (NULL on failure, or at the limit: of first
 * arenas for a thread's first, of all arenas for another). Under the creation lock. */
static void *eden_heap_create_arena_locked(int first) {
  void *arena = NULL;
  /* A spare first: empty, with its mspace and its tables as they are, and no memory taken. A
   * thread's first arena may be one past the limit of first arenas, which limits what they cost. */
  const unsigned spares = atomic_load_explicit(&eden_heap_spare_count, memory_order_relaxed);
  if (spares != 0) {
    arena = eden_heap_spare[spares - 1];
    atomic_store_explicit(&eden_heap_spare_count, spares - 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_arenas_taken_again, 1, memory_order_relaxed);
    return arena;
  }
  const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
  /* An entry given back, else a new one. */
  unsigned at = count;
  if (atomic_load_explicit(&eden_heap_arena_holes, memory_order_relaxed) != 0)
    for (unsigned i = 0; i < count; ++i)
      if (eden_heap_arena_list[i] == NULL) {
        at = i;
        break;
      }
  if (at < EDEN_HEAP_MAX_ARENAS && (!first || eden_heap_first_arenas < EDEN_HEAP_FIRST_ARENAS)) {
    void *block = eden_heap_main(EDEN_HEAP_ALIGNED, EDEN_HEAP_ARENA_SIZE, EDEN_HEAP_ARENA_PAYLOAD);
    if (block != NULL) {
      arena = sceLibcMspaceCreate("Eden-arena", block, EDEN_HEAP_ARENA_PAYLOAD, 0);
      if (arena != NULL) {
        const uintptr_t slot = eden_heap_slot(block);
        eden_heap_arena_list[at] = arena;
        eden_heap_arena_blocks_probe[at] = block;
        atomic_fetch_add_explicit(&eden_heap_arena_backing_probe, sceLibcMspaceMallocUsableSize(block), memory_order_relaxed);
        atomic_store_explicit(&eden_heap_arena_live[slot].blocks, 0, memory_order_relaxed);
        atomic_store_explicit(&eden_heap_slots[slot], arena, memory_order_release);
        if (at == count)
          atomic_store_explicit(&eden_heap_arena_count, count + 1, memory_order_release);
        else
          atomic_fetch_sub_explicit(&eden_heap_arena_holes, 1, memory_order_relaxed);
        if (first)
          eden_heap_first_arenas++;
      } else {
        eden_heap_space_free(block);
      }
    }
  }
  return arena;
}

/* The arenas threads are on now (every record's, an ended thread's record too: the next thread
 * on it starts there). Under the creation lock, which the table is used under. */
static void *eden_heap_named_arenas[EDEN_HEAP_THREADS];
static unsigned eden_heap_arenas_named(void **named) {
  unsigned count = 0;
  for (unsigned i = 0; i < EDEN_HEAP_THREADS; ++i) {
    void *arena = __atomic_load_n(&eden_heap_threads[i].arena, __ATOMIC_RELAXED);
    if (arena != NULL)
      named[count++] = arena;
  }
  return count;
}

/* Moves a record's empty arenas to the spares: all but its first, and but those a thread is
 * on (the list of them is read here, so a thread that takes one and is not on it yet, in a path
 * under the creation lock, is excluded by the lock). `ended`: no thread is on the record (its own
 * is ending, or it is claimed with eden_heap_thread_used 2), so its arena is set to its first
 * before the look, and its empty current one may go. Under the creation lock. */
static void eden_heap_return_record(struct eden_heap_thread *record, int ended) {
  unsigned owned = __atomic_load_n(&record->owned, __ATOMIC_ACQUIRE);
  if (owned < 2)
    return;
  if (ended)
    eden_heap_use_arena(record, __atomic_load_n(&record->own[0], __ATOMIC_RELAXED));
  void **named = eden_heap_named_arenas;
  const unsigned names = eden_heap_arenas_named(named);
  const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
  for (unsigned i = 1; i < owned;) {
    void *arena = __atomic_load_n(&record->own[i], __ATOMIC_RELAXED);
    unsigned at = 0;
    while (at < names && named[at] != arena)
      ++at;
    unsigned index = 0;
    while (index < count && eden_heap_arena_list[index] != arena)
      ++index;
    void *block = index < count ? atomic_load_explicit(&eden_heap_arena_blocks_probe[index], memory_order_relaxed) : NULL;
    if (at != names || block == NULL ||
        atomic_load_explicit(&eden_heap_arena_live[eden_heap_slot(block)].blocks, memory_order_relaxed) != 0) {
      ++i;
      continue;
    }
    /* No block is in it and no thread is on it: nothing can take from it or free into it now.
     * Out of the record, into the spares; its slot, its list entry and its mspace stay as they are
     * (a block that lands in it later finds its owner as before). The spares cannot overflow: the
     * list holds every arena there is. */
    const unsigned spares = atomic_load_explicit(&eden_heap_spare_count, memory_order_relaxed);
    eden_heap_spare[spares] = arena;
    atomic_store_explicit(&eden_heap_spare_count, spares + 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_arenas_returned, 1, memory_order_relaxed);
    for (unsigned k = i; k + 1 < owned; ++k)
      __atomic_store_n(&record->own[k], __atomic_load_n(&record->own[k + 1], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    --owned;
    __atomic_store_n(&record->owned, owned, __ATOMIC_RELEASE);
  }
}

/* Every record's empty arenas to the spares (eden_heap_trim). A record no thread is on is
 * claimed meanwhile (eden_heap_thread_used 2: eden_heap_self passes it over), so none starts on an
 * arena being moved. Returns the arenas moved. */
static unsigned eden_heap_return_arenas(void) {
  if (eden_heap_keeps_arenas())
    return 0;
  const unsigned long long before = atomic_load_explicit(&eden_heap_arenas_returned, memory_order_relaxed);
  eden_heap_create_acquire();
  for (unsigned i = 0; i < EDEN_HEAP_THREADS; ++i) {
    unsigned char expected = 0;
    const int claimed = atomic_compare_exchange_strong_explicit(&eden_heap_thread_used[i], &expected, 2,
                                                                memory_order_acquire, memory_order_relaxed);
    if (!claimed && expected != 1)
      continue;
    eden_heap_return_record(&eden_heap_threads[i], claimed);
    if (claimed)
      atomic_store_explicit(&eden_heap_thread_used[i], 0, memory_order_release);
  }
  eden_heap_create_release();
  return (unsigned)(atomic_load_explicit(&eden_heap_arenas_returned, memory_order_relaxed) - before);
}

/* This thread's record, claimed on its first allocation. NULL: none free (process-wide counters). */
static struct eden_heap_thread *eden_heap_self(void) {
  if (!eden_heap_arena_key_valid)
    return NULL;
  struct eden_heap_thread *self = pthread_getspecific(eden_heap_arena_key);
  if (self != NULL)
    return self;
  for (unsigned i = 0; i < EDEN_HEAP_THREADS; ++i) {
    unsigned char expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&eden_heap_thread_used[i], &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed))
      continue;
    self = &eden_heap_threads[i];
    self->live = 0;
    self->blocks = 0;
    self->pending = 0;
    self->retry = 0;  /* its arenas (own) stay: this thread uses them */
    __atomic_store_n(&self->name[0], 0, __ATOMIC_RELAXED);
    if (pthread_setspecific(eden_heap_arena_key, self) != 0) {
      atomic_store_explicit(&eden_heap_thread_used[i], 0, memory_order_release);
      return NULL;
    }
    return self;
  }
  return NULL;
}

static void eden_heap_charge(struct eden_heap_thread *self, long bytes, long blocks) {
  self->live += bytes;
  self->blocks += blocks;
  if (++self->pending >= 64 || self->live > (1l << 20) || self->live < -(1l << 20))
    eden_heap_flush(self);
}

static void eden_heap_account(long bytes, long blocks) {
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    eden_heap_apply(bytes, blocks);
  else
    eden_heap_charge(self, bytes, blocks);
}

static void *eden_heap_record(void *address, size_t bytes) {
  if (address == NULL) {
    if (bytes != 0)
      eden_heap_note_failure(bytes, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
    return ps5_heap_record_allocation(address, bytes != 0); /* counts the failure */
  }
  const size_t usable = eden_heap_usable(address);
  eden_heap_account((long)usable, 1);
  eden_pages_count(address, usable, 1);
  return address;
}

/* Another thread reads what a record uses and holds (eden_heap_adopt, the reports). */
static void eden_heap_use_arena(struct eden_heap_thread *self, void *arena) {
  __atomic_store_n(&self->arena, arena, __ATOMIC_RELAXED);
}

static void eden_heap_keep_arena(struct eden_heap_thread *self, void *arena) {
  const unsigned owned = self->owned;
  __atomic_store_n(&self->own[owned], arena, __ATOMIC_RELAXED);
  __atomic_store_n(&self->owned, owned + 1, __ATOMIC_RELEASE);
}

/* This thread's arena, created on its first small allocation. NULL = the heap's own spaces. */
static void *eden_heap_thread_arena(void) {
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    return NULL;
  if (self->arena != NULL)
    return self->arena;
  eden_heap_create_acquire();
  void *arena = eden_heap_create_arena_locked(1);
  if (arena == NULL) {
    /* Arena limit reached: share the existing arenas round-robin (past the entries given back). */
    const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_acquire);
    for (unsigned i = 0; i < count && arena == NULL; ++i)
      arena = eden_heap_arena_list[atomic_fetch_add_explicit(&eden_heap_next_shared, 1, memory_order_relaxed) % count];
  } else if (self->owned < EDEN_HEAP_THREAD_ARENAS) {
    eden_heap_keep_arena(self, arena);
  }
  if (arena != NULL)
    eden_heap_use_arena(self, arena);
  eden_heap_create_release();
  return arena;
}

static void *eden_heap_arena_try(void *arena, size_t count, size_t size, int zero) {
  void *address = zero ? sceLibcMspaceCalloc(arena, count, size) : sceLibcMspaceMalloc(arena, count * size);
  if (address != NULL)
    eden_heap_arena_took(address);
  return address;
}

static atomic_uint eden_heap_adopt_next;
static atomic_ullong eden_heap_adopted, eden_heap_adopt_misses;

/* A block from an arena no thread is using, which this thread then uses; NULL when the heap holds
 * little more than is in use, or none of the arenas asked has room. The owner of an arena taken
 * here may come back to it: the two then share it, as threads past the limit always have. Runs
 * once a thread is out of room, which takes at least the room asked for here. Under the creation
 * lock: the list is read. */
static void *eden_heap_adopt(struct eden_heap_thread *self, size_t count, size_t size, int zero) {
  const size_t held = atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed) +
                      atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  const size_t live = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  if (held < live + EDEN_HEAP_ADOPT_SLACK || getenv("EDEN_HEAP_NO_ADOPT") != NULL)
    return NULL;
  /* The arenas threads are on now. A record whose thread has ended uses none. */
  void *busy[EDEN_HEAP_ADOPT_BUSY];
  unsigned busy_count = 0;
  for (unsigned i = 0; i < EDEN_HEAP_THREADS && busy_count < EDEN_HEAP_ADOPT_BUSY; ++i)
    if (atomic_load_explicit(&eden_heap_thread_used[i], memory_order_relaxed) != 0) {
      void *arena = __atomic_load_n(&eden_heap_threads[i].arena, __ATOMIC_RELAXED);
      if (arena != NULL)
        busy[busy_count++] = arena;
    }
  const unsigned arenas = atomic_load_explicit(&eden_heap_arena_count, memory_order_acquire);
  const unsigned start = atomic_fetch_add_explicit(&eden_heap_adopt_next, EDEN_HEAP_ADOPT_TRIES, memory_order_relaxed);
  unsigned asked = 0;
  for (unsigned i = 0; i < arenas && asked < EDEN_HEAP_ADOPT_TRIES; ++i) {
    void *other = eden_heap_arena_list[(start + i) % arenas];
    if (other == NULL || eden_heap_is_spare(other))
      continue; /* a spare goes whole to the next thread that needs an arena */
    unsigned at = 0;
    while (at < busy_count && busy[at] != other)
      ++at;
    if (at != busy_count)
      continue;
    ++asked;
    void *room = sceLibcMspaceMalloc(other, EDEN_HEAP_ADOPT_ROOM);
    if (room == NULL)
      continue;
    sceLibcMspaceFree(other, room);
    void *address = eden_heap_arena_try(other, count, size, zero);
    if (address != NULL) {
      eden_heap_use_arena(self, other);
      atomic_fetch_add_explicit(&eden_heap_adopted, 1, memory_order_relaxed);
      return address;
    }
  }
  atomic_fetch_add_explicit(&eden_heap_adopt_misses, 1, memory_order_relaxed);
  return NULL;
}

/* A small block from this thread's arenas: the one it uses; when that one is full, another of its
 * own with room, which it then uses; when none has, an arena no thread is using, while the heap
 * holds much more than is in use (eden_heap_adopt); else a new one (for a request under
 * EDEN_HEAP_ARENA_GROW_REQUEST, while the thread and the process are under their limits). NULL:
 * the heap's own spaces take the request. Past the arena the thread is on, under the creation
 * lock: the thread's arenas and the list are read, and an empty arena is given back under it. */
static void *eden_heap_arena_allocate(size_t count, size_t size, int zero) {
  void *arena = eden_heap_thread_arena();
  if (arena == NULL)
    return NULL;
  void *address = eden_heap_arena_try(arena, count, size, zero);
  if (address != NULL)
    return address;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    return NULL;
  if (self->retry != 0) {
    --self->retry;
    return NULL;
  }
  eden_heap_create_acquire();
  for (unsigned i = 0; i < self->owned; ++i) {
    void *other = self->own[i];
    if (other == arena)
      continue;
    address = eden_heap_arena_try(other, count, size, zero);
    if (address != NULL) {
      eden_heap_use_arena(self, other);
      eden_heap_create_release();
      return address;
    }
  }
  if (count * size < EDEN_HEAP_ARENA_GROW_REQUEST) {
    address = eden_heap_adopt(self, count, size, zero);
    if (address == NULL) {
      void *fresh = self->owned < EDEN_HEAP_THREAD_ARENAS ? eden_heap_create_arena_locked(0) : NULL;
      if (fresh == NULL) {
        /* At a limit, or no memory for one: each look through the arenas costs a request to each,
         * so the next few requests go straight to the heap's spaces. */
        self->retry = EDEN_HEAP_ARENA_RETRY;
      } else {
        eden_heap_keep_arena(self, fresh);
        eden_heap_use_arena(self, fresh);
        address = eden_heap_arena_try(fresh, count, size, zero);
      }
    }
  }
  eden_heap_create_release();
  return address;
}

/* A small request no arena took, from the heap's spaces, counted with its cycles. */
static void *eden_heap_spill(size_t count, size_t size, int zero) {
  const unsigned long long start = eden_heap_cycles();
  void *address = zero ? eden_heap_main(EDEN_HEAP_CALLOC, count, size)
                       : eden_heap_main(EDEN_HEAP_MALLOC, count * size, 0);
  const unsigned long long cycles = eden_heap_cycles() - start;
  struct eden_heap_thread *self = eden_heap_self();
  if (self != NULL) {
    self->spills++;
    self->spill_cycles += cycles;
  } else {
    atomic_fetch_add_explicit(&eden_heap_spills, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_spill_cycles, cycles, memory_order_relaxed);
  }
  return address;
}

/* Gives an owned block back to its arena, or to the heap's space, counting the latter (its lock is
 * the one every thread shares). */
static void eden_heap_release(void *address) {
  void *arena = eden_heap_arena_owner(address);
  if (arena != NULL) {
    sceLibcMspaceFree(arena, address);
    eden_heap_arena_gave(address);
    return;
  }
  const unsigned long long start = eden_heap_cycles();
  eden_heap_space_free(address);
  const unsigned long long cycles = eden_heap_cycles() - start;
  struct eden_heap_thread *self = eden_heap_self();
  if (self != NULL) {
    self->shared_frees++;
    self->shared_free_cycles += cycles;
  } else {
    atomic_fetch_add_explicit(&eden_heap_shared_frees, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_free_cycles, cycles, memory_order_relaxed);
  }
}

/* A cached block for bytes (0 < bytes <= 16 * (EDEN_TCACHE_BINS - 1)), charged as allocated. */
static void *eden_tcache_take(size_t bytes) {
  const size_t bin = (bytes + 15) >> 4;
  if (bytes == 0 || bin >= EDEN_TCACHE_BINS)
    return NULL;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL || self->bins[bin] == NULL)
    return NULL;
  void *block = self->bins[bin];
  self->bins[bin] = ((void **)block)[0];
  __atomic_store_n(&self->counts[bin], (unsigned char)(self->counts[bin] - 1), __ATOMIC_RELAXED);
  const size_t usable = ((size_t *)block)[1];
  eden_heap_charge(self, (long)usable, 1);
  eden_pages_count(block, usable, 1);
  return block;
}

/* Keeps a freed owned block of this usable size; 0 when the bin is full or out of range. */
static int eden_tcache_keep(void *address, size_t usable) {
  const size_t bin = usable >> 4;
  if (bin == 0 || bin >= EDEN_TCACHE_BINS)
    return 0;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL || self->counts[bin] >= EDEN_TCACHE_LIMIT)
    return 0;
  ((void **)address)[0] = self->bins[bin];
  ((size_t *)address)[1] = usable;
  self->bins[bin] = address;
  __atomic_store_n(&self->counts[bin], (unsigned char)(self->counts[bin] + 1), __ATOMIC_RELAXED);
  eden_heap_charge(self, -(long)usable, -1);
  return 1;
}

/* ---- Where the live blocks come from (a measurement, reported by eden_heap_sites_report) ----
 * One block in EDEN_SITE_RATE, and every block of 1 MiB or more, is followed from its allocation to
 * its free. At the allocation the stack above the allocator is read for the first return addresses
 * that fall in the app's code; the bytes of the block are added to the
 * group of that chain, and taken off again when it is freed. A group is a place in the emulator that
 * asks for memory, as far as the first calls up say. Sampled blocks count EDEN_SITE_RATE times. */
extern const char __eden_text_start[];
extern const char __eden_text_end[];
#define EDEN_SITE_BLOCKS 131072u
/* Run 61: 4096 places were full within 40 s, the words of the stack that are no return address
 * making many chains a new one, and a block from then on had no place: "followed blocks" stood at
 * 444 MiB for the rest of the run while the heap's use went from 741 to 1211 MiB. So: more places, a
 * place with no block alive is taken over, the requested size is no return address, and a block
 * that finds no place is counted (eden_site_missed). */
#define EDEN_SITE_TABLE 16384u
#define EDEN_SITE_PROBES 32u
#define EDEN_SITE_PCS 8u
/* What makes two blocks one place: the nearest words of the chain. The far ones are shown, from
 * the first block of the place, but a stale word out there would split one owner into a place per
 * block, and the list shows only the largest. */
#define EDEN_SITE_KEY 5u
#define EDEN_SITE_REPORTED 24u
#define EDEN_SITE_RATE 128u
#define EDEN_SITE_BIG ((size_t)1 << 20)

static struct {
  _Atomic uintptr_t address; /* 0 free, 1 given back */
  unsigned site;
  unsigned big;
  size_t size;
} eden_site_blocks[EDEN_SITE_BLOCKS];
static struct {
  _Atomic uintptr_t hash;
  uintptr_t pcs[EDEN_SITE_PCS];
  _Atomic long exact_bytes, sampled_bytes, exact_blocks, sampled_blocks;
} eden_site_table[EDEN_SITE_TABLE];

/* Bytes of the blocks that found no place since the process began (the sampled ones at their rate). */
static atomic_long eden_site_missed;

static int eden_site_sampled(const void *address) {
  return (((uintptr_t)address * 0x9E3779B97F4A7C15ull) >> 57) == 0;
}

static unsigned eden_site_slot(const void *address) {
  return (unsigned)((((uintptr_t)address >> 4) * 0x9E3779B97F4A7C15ull) >> 40) % EDEN_SITE_BLOCKS;
}

static __attribute__((noinline)) unsigned eden_site_collect(uintptr_t pcs[EDEN_SITE_PCS], size_t size) {
  const uintptr_t low = (uintptr_t)__eden_text_start + 8, high = (uintptr_t)__eden_text_end;
  const uintptr_t self = (uintptr_t)__builtin_return_address(0);
  const uintptr_t *stack = (const uintptr_t *)__builtin_frame_address(0);
  /* The result is written after the scan: the array of the caller lies in the stack being read. */
  uintptr_t local[EDEN_SITE_PCS] = {0};
  unsigned found = 0;
  /* Never past the page (16 KiB) that holds the frame: a thread's stack can end a few hundred bytes
   * above, and what lies after it is not mapped. */
  const uintptr_t page_end = (((uintptr_t)stack | 0x3FFFu) + 1);
  const unsigned room = (unsigned)((page_end - (uintptr_t)stack) / sizeof(uintptr_t));
  for (unsigned i = 0; i < 256 && i < room && found < EDEN_SITE_PCS; ++i) {
    const uintptr_t value = stack[i];
    /* The app's code is execute-only on the console: its bytes cannot be read to check that the
     * address follows a call (the system ends the process). A stale word of the stack can be taken
     * for a return address; the chain is read with that in mind. */
    if (value < low || value >= high || (found != 0 && local[found - 1] == value))
      continue;
    /* eden_site_alloc itself, and the request: 16 MiB is an address in the app's image. */
    if (value == self || value == size)
      continue;
    local[found++] = value;
  }
  for (unsigned i = 0; i < EDEN_SITE_PCS; ++i)
    pcs[i] = local[i];
  return found;
}

static __attribute__((noinline)) void eden_site_alloc(const void *address, size_t size) {
  if (address == NULL)
    return;
  const unsigned big = size >= EDEN_SITE_BIG;
  if (!big && !eden_site_sampled(address))
    return;
  uintptr_t pcs[EDEN_SITE_PCS] = {0};
  eden_site_collect(pcs, size);
  uintptr_t hash = 0xCBF29CE484222325ull;
  for (unsigned i = 0; i < EDEN_SITE_KEY; ++i)
    hash = (hash ^ pcs[i]) * 0x100000001B3ull;
  hash |= 1;
  unsigned site = EDEN_SITE_TABLE;
  for (unsigned probe = 0; probe < EDEN_SITE_PROBES; ++probe) {
    const unsigned at = (unsigned)((hash >> 12) + probe) % EDEN_SITE_TABLE;
    uintptr_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&eden_site_table[at].hash, &expected, hash, memory_order_acq_rel,
                                                memory_order_acquire)) {
      for (unsigned i = 0; i < EDEN_SITE_PCS; ++i)
        eden_site_table[at].pcs[i] = pcs[i];
      site = at;
      break;
    }
    if (expected == hash) {
      site = at;
      break;
    }
  }
  /* No room: a place whose blocks are all gone takes the new chain (the report needs no more of
   * it than what is alive; two threads racing here at worst put a block under the other's chain). */
  for (unsigned probe = 0; site == EDEN_SITE_TABLE && probe < EDEN_SITE_PROBES; ++probe) {
    const unsigned at = (unsigned)((hash >> 12) + probe) % EDEN_SITE_TABLE;
    if (atomic_load_explicit(&eden_site_table[at].exact_blocks, memory_order_relaxed) != 0 ||
        atomic_load_explicit(&eden_site_table[at].sampled_blocks, memory_order_relaxed) != 0)
      continue;
    uintptr_t expected = atomic_load_explicit(&eden_site_table[at].hash, memory_order_relaxed);
    if (atomic_compare_exchange_strong_explicit(&eden_site_table[at].hash, &expected, hash, memory_order_acq_rel,
                                                memory_order_acquire)) {
      for (unsigned i = 0; i < EDEN_SITE_PCS; ++i)
        eden_site_table[at].pcs[i] = pcs[i];
      site = at;
    }
  }
  if (site == EDEN_SITE_TABLE) {
    atomic_fetch_add_explicit(&eden_site_missed, big ? (long)size : (long)(size * EDEN_SITE_RATE), memory_order_relaxed);
    return;
  }
  const unsigned first = eden_site_slot(address);
  for (unsigned probe = 0; probe < 16; ++probe) {
    const unsigned at = (first + probe) % EDEN_SITE_BLOCKS;
    uintptr_t value = atomic_load_explicit(&eden_site_blocks[at].address, memory_order_relaxed);
    if (value > 1)
      continue;
    if (atomic_compare_exchange_strong_explicit(&eden_site_blocks[at].address, &value, (uintptr_t)address,
                                                memory_order_acq_rel, memory_order_relaxed)) {
      eden_site_blocks[at].site = site;
      eden_site_blocks[at].big = big;
      eden_site_blocks[at].size = size;
      if (big) {
        atomic_fetch_add_explicit(&eden_site_table[site].exact_bytes, (long)size, memory_order_relaxed);
        atomic_fetch_add_explicit(&eden_site_table[site].exact_blocks, 1, memory_order_relaxed);
      } else {
        atomic_fetch_add_explicit(&eden_site_table[site].sampled_bytes, (long)size, memory_order_relaxed);
        atomic_fetch_add_explicit(&eden_site_table[site].sampled_blocks, 1, memory_order_relaxed);
      }
      return;
    }
  }
}

static void eden_site_free(const void *address, size_t usable) {
  if (usable < EDEN_SITE_BIG && !eden_site_sampled(address))
    return;
  const unsigned first = eden_site_slot(address);
  for (unsigned probe = 0; probe < 16; ++probe) {
    const unsigned at = (first + probe) % EDEN_SITE_BLOCKS;
    uintptr_t value = atomic_load_explicit(&eden_site_blocks[at].address, memory_order_acquire);
    if (value == 0)
      return;
    if (value != (uintptr_t)address)
      continue;
    const unsigned site = eden_site_blocks[at].site, big = eden_site_blocks[at].big;
    const long size = (long)eden_site_blocks[at].size;
    if (!atomic_compare_exchange_strong_explicit(&eden_site_blocks[at].address, &value, 1, memory_order_acq_rel,
                                                 memory_order_relaxed))
      return;
    if (big) {
      atomic_fetch_sub_explicit(&eden_site_table[site].exact_bytes, size, memory_order_relaxed);
      atomic_fetch_sub_explicit(&eden_site_table[site].exact_blocks, 1, memory_order_relaxed);
    } else {
      atomic_fetch_sub_explicit(&eden_site_table[site].sampled_bytes, size, memory_order_relaxed);
      atomic_fetch_sub_explicit(&eden_site_table[site].sampled_blocks, 1, memory_order_relaxed);
    }
    return;
  }
}

/* The places that hold the most, to stderr.log: MiB alive, blocks alive, and the chain of calls
 * (offsets in the app's image, for tools/symbolize-crash.py or llvm-symbolizer). */
void eden_heap_sites_report(void) {
  struct {
    long bytes, blocks;
    unsigned site;
  } top[EDEN_SITE_REPORTED];
  unsigned count = 0;
  long total = 0;
  for (unsigned at = 0; at < EDEN_SITE_TABLE; ++at) {
    if (atomic_load_explicit(&eden_site_table[at].hash, memory_order_relaxed) == 0)
      continue;
    const long bytes =
        atomic_load_explicit(&eden_site_table[at].exact_bytes, memory_order_relaxed) +
        (long)EDEN_SITE_RATE * atomic_load_explicit(&eden_site_table[at].sampled_bytes, memory_order_relaxed);
    const long blocks =
        atomic_load_explicit(&eden_site_table[at].exact_blocks, memory_order_relaxed) +
        (long)EDEN_SITE_RATE * atomic_load_explicit(&eden_site_table[at].sampled_blocks, memory_order_relaxed);
    if (bytes <= 0)
      continue;
    total += bytes;
    unsigned place = count;
    if (count < EDEN_SITE_REPORTED)
      ++count;
    else if (bytes <= top[EDEN_SITE_REPORTED - 1].bytes)
      continue;
    else
      place = EDEN_SITE_REPORTED - 1;
    while (place > 0 && top[place - 1].bytes < bytes) {
      top[place] = top[place - 1];
      --place;
    }
    top[place].bytes = bytes;
    top[place].blocks = blocks;
    top[place].site = at;
  }
  fprintf(stderr, "EDEN_HEAP_SITES followed blocks hold %ld MiB, %ld MiB of blocks found no place so far; the largest places:\n",
          total >> 20, atomic_load_explicit(&eden_site_missed, memory_order_relaxed) >> 20);
  for (unsigned i = 0; i < count; ++i) {
    const unsigned at = top[i].site;
    fprintf(stderr, "EDEN_HEAP_SITE %ld MiB in %ld blocks:", top[i].bytes >> 20, top[i].blocks);
    for (unsigned k = 0; k < EDEN_SITE_PCS; ++k)
      fprintf(stderr, " %lx", (unsigned long)(eden_site_table[at].pcs[k] ? eden_site_table[at].pcs[k] - 0x400000ul : 0));
    fputc('\n', stderr);
  }
}

/* ---- Who holds what the heap does not use (reports, in stderr.log) ---- */

/* The name a thread gave itself, kept with its record (the SetCurrentThreadName hook). */
void eden_heap_name_thread(const char *name) {
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2)
    return;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    return;
  unsigned length = 0;
  for (; length + 1 < sizeof(self->name) && name[length] != 0; ++length)
    __atomic_store_n(&self->name[length], name[length], __ATOMIC_RELAXED);
  __atomic_store_n(&self->name[length], 0, __ATOMIC_RELAXED);
}

/* Every 20 s, and cheap: which requests made the heap grow (MiB, and the largest request), what the
 * arenas are, and how often a thread found room in another's. */
void eden_heap_growth_report(void) {
  static const char *const kinds[EDEN_HEAP_GREW_KINDS] = {"arena", "under256K", "under1M", "under8M", "under32M", "large"};
  char line[320];
  int length = snprintf(line, sizeof(line), "EDEN_HEAP_GROWTH arenas=%u spare=%u reused=%llu given_back=%llu adopted=%llu adopt_misses=%llu spills=%llu grew:",
                        eden_heap_arenas_created(), eden_heap_arenas_spare(),
                        (unsigned long long)atomic_load_explicit(&eden_heap_arenas_taken_again, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&eden_heap_arenas_returned, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&eden_heap_adopted, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&eden_heap_adopt_misses, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&eden_heap_spills, memory_order_relaxed));
  for (unsigned kind = 0; kind < EDEN_HEAP_GREW_KINDS && length > 0 && length < (int)sizeof(line); ++kind)
    length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=%zu/%zu", kinds[kind],
                       (size_t)atomic_load_explicit(&eden_heap_grew_pieces[kind], memory_order_relaxed)
                           << (EDEN_HEAP_PIECE_SHIFT - 20),
                       atomic_load_explicit(&eden_heap_grew_largest[kind], memory_order_relaxed) >> 10);
  fprintf(stderr, "%s (MiB/largest KiB)\n", line);
  /* Blocks under 32 MiB with memory of their own (EDEN_HEAP_LARGE_MIB). Cycles are the TSC's. */
  fprintf(stderr, "EDEN_HEAP_MID from=%zuKiB taken=%llu taken_mib=%llu refused=%llu alive=%u alive_mib=%zu peak_mib=%zu "
                  "cycles_m=%llu large_entries=%u large_high=%u table_full=%llu\n",
          atomic_load_explicit(&eden_heap_large_from, memory_order_relaxed) >> 10,
          (unsigned long long)atomic_load_explicit(&eden_heap_mid_taken, memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_mid_taken_bytes, memory_order_relaxed) >> 20,
          (unsigned long long)atomic_load_explicit(&eden_heap_mid_refused, memory_order_relaxed),
          atomic_load_explicit(&eden_heap_mid_count, memory_order_relaxed),
          atomic_load_explicit(&eden_heap_mid_bytes, memory_order_relaxed) >> 20,
          atomic_load_explicit(&eden_heap_mid_peak, memory_order_relaxed) >> 20,
          (unsigned long long)atomic_load_explicit(&eden_heap_mid_cycles, memory_order_relaxed) / 1000000u,
          atomic_load_explicit(&eden_heap_large_count, memory_order_relaxed),
          atomic_load_explicit(&eden_heap_large_high, memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_large_table_full, memory_order_relaxed));
  fprintf(stderr, "EDEN_HEAP_CLASSES requests (count/MiB): under1M=%llu/%llu under2M=%llu/%llu under8M=%llu/%llu under32M=%llu/%llu\n",
          (unsigned long long)atomic_load_explicit(&eden_heap_class_count[0], memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_class_bytes[0], memory_order_relaxed) >> 20,
          (unsigned long long)atomic_load_explicit(&eden_heap_class_count[1], memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_class_bytes[1], memory_order_relaxed) >> 20,
          (unsigned long long)atomic_load_explicit(&eden_heap_class_count[2], memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_class_bytes[2], memory_order_relaxed) >> 20,
          (unsigned long long)atomic_load_explicit(&eden_heap_class_count[3], memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&eden_heap_class_bytes[3], memory_order_relaxed) >> 20);
}

/* The room left in an mspace, found by asking it: blocks of `top` while it gives them, then of half
 * that, down to `floor`, all freed again. It takes nothing of the mspace's own records, so it works
 * on any of them; a thread that asks in these microseconds may find it fuller than it is. */
#define EDEN_HEAP_ROOM_BLOCKS 96u
static size_t eden_heap_room(void *space, size_t top, size_t floor, size_t *largest) {
  void *blocks[EDEN_HEAP_ROOM_BLOCKS];
  unsigned count = 0;
  size_t room = 0;
  *largest = 0;
  for (size_t size = top; size >= floor && count < EDEN_HEAP_ROOM_BLOCKS;) {
    void *block = sceLibcMspaceMalloc(space, size);
    if (block == NULL) {
      size >>= 1;
      continue;
    }
    blocks[count++] = block;
    room += size;
    if (*largest == 0)
      *largest = size;
  }
  while (count != 0)
    sceLibcMspaceFree(space, blocks[--count]);
  return room;
}

/* At a refusal, with the time to look: for each thread record with four arenas or 4 MiB of room,
 * what they hold and one digit an arena for its room (0 full, f empty); for each space, its blocks,
 * the arenas among them, and its room. A space whose blocks are all arenas is pinned by them alone.
 * The rest of what the heap holds and does not use is room in pieces under the sizes asked for
 * here, and the mspaces' headers. */
void eden_heap_arena_report(void) {
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2)
    return;
  const size_t arena_floor = (size_t)16 << 10, space_floor = (size_t)256 << 10;
  size_t arena_room = 0, cached = 0, largest = 0;
  unsigned empty = 0;
  for (unsigned record = 0; record < EDEN_HEAP_THREADS; ++record) {
    struct eden_heap_thread *thread = &eden_heap_threads[record];
    unsigned owned = __atomic_load_n(&thread->owned, __ATOMIC_ACQUIRE);
    size_t kept = 0;
    for (unsigned bin = 1; bin < EDEN_TCACHE_BINS; ++bin)
      kept += (size_t)__atomic_load_n(&thread->counts[bin], __ATOMIC_RELAXED) * bin * 16u;
    cached += kept;
    if (owned == 0)
      continue;
    char map[EDEN_HEAP_THREAD_ARENAS + 1];
    size_t room = 0;
    unsigned record_empty = 0;
    /* The record's arenas are read, and asked, under the creation lock: a trim on another thread
     * gives empty ones back under it. No stdio here: it allocates, and may want the lock. */
    eden_heap_create_acquire();
    owned = __atomic_load_n(&thread->owned, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < owned; ++i) {
      void *arena = __atomic_load_n(&thread->own[i], __ATOMIC_RELAXED);
      const size_t left = eden_heap_room(arena, (size_t)1 << 20, arena_floor, &largest);
      room += left;
      record_empty += left + ((size_t)64 << 10) >= EDEN_HEAP_ARENA_PAYLOAD;
      map[i] = "0123456789abcdef"[left >= EDEN_HEAP_ARENA_PAYLOAD ? 15 : left * 16 / EDEN_HEAP_ARENA_PAYLOAD];
    }
    eden_heap_create_release();
    map[owned] = 0;
    arena_room += room;
    empty += record_empty;
    if (owned < 4 && room < ((size_t)4 << 20))
      continue;
    char name[sizeof(thread->name)];
    for (unsigned i = 0; i < sizeof(name); ++i)
      name[i] = __atomic_load_n(&thread->name[i], __ATOMIC_RELAXED);
    name[sizeof(name) - 1] = 0;
    fprintf(stderr, "EDEN_HEAP_THREAD %u %s%s arenas=%u room=%zuMiB empty=%u tcache=%zuKiB grew=%uMiB map=%s\n", record,
            name[0] ? name : "?",
            atomic_load_explicit(&eden_heap_thread_used[record], memory_order_relaxed) ? "" : " (ended)", owned,
            room >> 20, record_empty, kept >> 10,
            __atomic_load_n(&thread->grown, __ATOMIC_RELAXED) << (EDEN_HEAP_PIECE_SHIFT - 20), map);
  }
  /* The spares: empty arenas no record holds, 2 MiB each, for the next threads. */
  {
    const unsigned spares = eden_heap_arenas_spare();
    arena_room += (size_t)spares * EDEN_HEAP_ARENA_PAYLOAD;
    empty += spares;
    if (spares != 0)
      fprintf(stderr, "EDEN_HEAP_SPARE arenas=%u (%zuMiB), %llu taken so far\n", spares,
              ((size_t)spares * EDEN_HEAP_ARENA_PAYLOAD) >> 20, eden_heap_arenas_reused());
  }
  /* The arenas in each space. */
  unsigned short arenas_in[EDEN_HEAP_PIECES];
  memset(arenas_in, 0, sizeof(arenas_in));
  const unsigned arenas = atomic_load_explicit(&eden_heap_arena_count, memory_order_acquire);
  for (unsigned i = 0; i < arenas; ++i) {
    const uintptr_t block = (uintptr_t)atomic_load_explicit(&eden_heap_arena_blocks_probe[i], memory_order_relaxed);
    if (block == 0)
      continue; /* given back */
    const unsigned slot = atomic_load_explicit(
        &eden_heap_piece_slot[(block - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT], memory_order_relaxed);
    if (slot < EDEN_HEAP_PIECES)
      arenas_in[slot]++;
  }
  size_t space_room = 0;
  unsigned only_arenas = 0, on_line = 0;
  char line[512];
  int length = 0;
  const unsigned spaces = atomic_load_explicit(&eden_heap_space_count, memory_order_acquire);
  for (unsigned slot = 0; slot < spaces; ++slot) {
    /* As eden_heap_space_use: a space with a thread in it is not given back. */
    atomic_fetch_add_explicit(&eden_heap_space_users[slot], 1, memory_order_seq_cst);
    void *space = atomic_load_explicit(&eden_heap_space_retired[slot], memory_order_seq_cst) == 0
                      ? atomic_load_explicit(&eden_heap_spaces[slot], memory_order_acquire)
                      : NULL;
    if (space != NULL) {
      const long blocks = atomic_load_explicit(&eden_heap_space_blocks[slot], memory_order_relaxed);
      const size_t room = eden_heap_room(space, EDEN_HEAP_PIECE_SIZE, space_floor, &largest);
      space_room += room;
      only_arenas += slot != 0 && blocks == arenas_in[slot];
      if (length == 0)
        length = snprintf(line, sizeof(line), "EDEN_HEAP_SPACES slot:blocks:arenas:largestKiB:roomMiB");
      length += snprintf(line + length, sizeof(line) - (size_t)length, " %u:%ld:%u:%zu:%zu", slot, blocks,
                         (unsigned)arenas_in[slot], largest >> 10, room >> 20);
      if (++on_line == 12) {
        fprintf(stderr, "%s\n", line);
        on_line = 0;
        length = 0;
      }
    }
    atomic_fetch_sub_explicit(&eden_heap_space_users[slot], 1, memory_order_release);
  }
  if (length != 0)
    fprintf(stderr, "%s\n", line);
  const size_t pieces = atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed);
  const size_t large = atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  const size_t live = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  const size_t slack = pieces + large > live ? pieces + large - live : 0;
  const size_t named = arena_room + space_room + cached;
  fprintf(stderr, "EDEN_HEAP_SLACK %zu MiB not in use: %zu MiB room in %u arenas (%u empty), %zu MiB room in spaces "
                  "(%u held by arenas alone), %zu MiB in thread caches, %zu MiB in smaller holes and headers\n",
          slack >> 20, arena_room >> 20, eden_heap_arenas_created(), empty, space_room >> 20, only_arenas, cached >> 20,
          (slack > named ? slack - named : 0) >> 20);
}

static void *eden_heap_allocate(size_t count, size_t size, int zero) {
  const size_t bytes = count * size;
  if (bytes >= EDEN_HEAP_SMALL_REQUEST && bytes < EDEN_HEAP_LARGE_REQUEST)
    eden_heap_class_note(bytes);
  if (bytes >= atomic_load_explicit(&eden_heap_large_from, memory_order_relaxed) && eden_heap_grows) {
    /* Memory of its own (it arrives zeroed). With every entry taken, or no memory left to take,
     * the heap's spaces: a piece committed earlier may still have the room. */
    void *block = eden_heap_large_allocate(bytes);
    if (block != NULL)
      return block;
    /* EDEN_BIG_IMAGE_NO_SPLIT: a big image's block the reserve could not serve is refused here
     * too, not grown into the heap's spaces (pieces that stay for good). */
    if (eden_heap_big_image_refuses(bytes)) {
      eden_heap_note_failure(bytes, EDEN_HEAP_WHY_NO_MEMORY); /* eden_heap_record counts it */
      return NULL;
    }
  }
  if (bytes < EDEN_HEAP_SMALL_REQUEST) {
    void *address = eden_heap_arena_allocate(count, size, zero);
    return address != NULL ? address : eden_heap_spill(count, size, zero);
  }
  return zero ? eden_heap_main(EDEN_HEAP_CALLOC, count, size) : eden_heap_main(EDEN_HEAP_MALLOC, bytes, 0);
}

void *__wrap_malloc(size_t size) {
  if (!ps5_heap_ready())
    return __real_malloc(size);
  void *cached = eden_tcache_take(size);
  void *block = cached != NULL ? cached : eden_heap_record(eden_heap_allocate(1, size, 0), size);
  eden_site_alloc(block, size);
  return block;
}

void *__wrap_calloc(size_t count, size_t size) {
  if (!ps5_heap_ready())
    return __real_calloc(count, size);
  if (size != 0 && count > (size_t)-1 / size) {
    eden_heap_failed((size_t)-1, EDEN_HEAP_WHY_OVERFLOW);
    return NULL;
  }
  void *cached = eden_tcache_take(count * size);
  if (cached != NULL) {
    eden_site_alloc(cached, count * size);
    return memset(cached, 0, count * size);
  }
  void *block = eden_heap_record(eden_heap_allocate(count, size, 1), count * size);
  eden_site_alloc(block, count * size);
  return block;
}

static void *eden_realloc_inner(void *address, size_t size) {
  if (address == NULL)
    return __wrap_malloc(size);
  const int in_range = eden_heap_in_range(address);
  const int large = in_range ? -1 : eden_heap_large_find(address);
  if (!in_range && large < 0)
    return __real_realloc(address, size);
  const size_t before = in_range ? sceLibcMspaceMallocUsableSize(address) : eden_heap_large[large].size;
  if (large >= 0) {
    if (size == 0) {
      eden_heap_account(-(long)before, -1);
      eden_heap_large_free(large);
      return NULL;
    }
    /* A large block keeps its memory while the new size still needs most of it. */
    if (size <= before && size >= before / 2)
      return address;
  } else if (size < atomic_load_explicit(&eden_heap_large_from, memory_order_relaxed) || !eden_heap_grows) {
    void *owner = eden_heap_owner(address);
    void *result = sceLibcMspaceRealloc(owner, address, size);
    if (result) {
      const size_t after = sceLibcMspaceMallocUsableSize(result);
      eden_heap_account((long)after - (long)before, 0);
      eden_pages_count(address, before, -1);
      eden_pages_count(result, after, 1);
      return result;
    }
    if (size == 0) {
      /* Preserve platform realloc(p,0) behavior; NULL does not tell us whether
       * p was freed. Mark the counters inconclusive instead of guessing. */
      atomic_fetch_add_explicit(&ps5_heap_ambiguous_zero_reallocs, 1, memory_order_relaxed);
      return NULL;
    }
  }
  /* The block cannot stay where it is (its arena or its piece of the heap is full, or it
   * crosses the size at which blocks get memory of their own): move it. */
  void *moved = eden_heap_allocate(1, size, 0);
  if (moved == NULL) {
    eden_heap_failed(size, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
    return NULL;
  }
  memcpy(moved, address, before < size ? before : size);
  const size_t after = eden_heap_usable(moved);
  eden_heap_account((long)after - (long)before, 0);
  eden_pages_count(moved, after, 1);
  if (large >= 0) {
    eden_heap_large_free(large);
  } else {
    eden_pages_count(address, before, -1);
    eden_heap_free_owned(address);
  }
  return moved;
}

void *__wrap_realloc(void *address, size_t size) {
  if (address == NULL)
    return __wrap_malloc(size);
  if (ps5_heap_owns(address))
    eden_site_free(address, eden_heap_usable(address));
  void *result = eden_realloc_inner(address, size);
  eden_site_alloc(result, size);
  return result;
}

void __wrap_free(void *address) {
  if (eden_heap_in_range(address)) {
    const size_t usable = sceLibcMspaceMallocUsableSize(address);
    eden_site_free(address, usable);
    eden_pages_count(address, usable, -1);
    if (eden_tcache_keep(address, usable))
      return;
    eden_heap_account(-(long)usable, -1);
    eden_heap_release(address);
    return;
  }
  const int large = eden_heap_large_find(address);
  if (large >= 0) {
    eden_site_free(address, eden_heap_large[large].size);
    eden_heap_account(-(long)eden_heap_large[large].size, -1);
    eden_heap_large_free(large);
  } else
    __real_free(address);
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size) {
  if (!ps5_heap_ready())
    return __real_posix_memalign(address, alignment, size);
  /* An alignment no space accepts must not make the heap grow looking for one that does. */
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  int result = ENOMEM;
  if (size < EDEN_HEAP_SMALL_REQUEST && alignment <= 4096) {
    void *arena = eden_heap_thread_arena();
    if (arena != NULL) {
      result = sceLibcMspacePosixMemalign(arena, address, alignment, size);
      if (result == 0)
        eden_heap_arena_took(*address);
    }
  }
  if (result != 0) {
    /* A large block's own memory starts on a page; the heap's spaces align anything. */
    void *block = NULL;
    if (alignment <= 4096 && size >= atomic_load_explicit(&eden_heap_large_from, memory_order_relaxed) && eden_heap_grows)
      block = eden_heap_large_allocate(size);
    if (block == NULL)
      block = eden_heap_main(EDEN_HEAP_ALIGNED, alignment, size);
    if (block != NULL) {
      *address = block;
      result = 0;
    } else {
      result = ENOMEM;
    }
  }
  if (result == 0) {
    eden_heap_record(*address, size);
    eden_site_alloc(*address, size);
  } else
    eden_heap_failed(size, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
  return result;
}

size_t __wrap_malloc_usable_size(const void *address) {
  return ps5_heap_owns(address) ? eden_heap_usable(address) : __real_malloc_usable_size(address);
}

