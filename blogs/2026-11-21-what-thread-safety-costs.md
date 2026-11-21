# What thread-safety costs: one lock helped a lot, three ideas did not

*2026-11-21 · lzfse-zazzcat · an LZFSE compressor written from scratch in C, with the goal of beating Apple's
`libcompression` on Apple Silicon.*

This post is the record of one working session: an audit of every place where thread-safety might be costing
speed, four proposed fixes, one pull request per fix, and a benchmark of each. **One fix was a large win, one was
a small win, and two measured as nothing.** It is written as source material for the series' main articles. The
last section lists the posts it could become and the facts each one needs.

At the time of writing all four pull requests are open and none is merged. Nothing in `src/` changed as part of
this post; the only file it adds is this one.

## The one-paragraph version

Only one file in the library does any synchronization: `src/parallel.c`, the persistent thread pool behind
`lzfse_zazzcat_encode_chunks`. Encode, decode, FSE and LZVN take no locks, use no atomics and hold no mutable
globals, so there was nothing to gain there. In `parallel.c` the audit found four candidates. The one that
mattered was a single global mutex that made concurrent callers take turns: with eight threads each compressing a
small buffer, a lock-free path was **3 to 8 times faster**. Replacing "wait for every worker to check in" with
"wait for every task to finish" gained about 6% in one case and nothing in the others. Padding and relaxed atomics
changed nothing. Copying the task buffers into the output in parallel, which the audit guessed might be worth
about 20%, changed nothing either.

## Part 1: the audit

The question: where is thread-safety costing performance, does that code need to be thread-safe at all, and if so,
is there a way to keep the safety and drop the cost?

A grep for `atomic`, `mutex`, `pthread`, `thread_local`, `volatile`, `memory_order` and `dispatch_` over `src/`,
`include/` and `tools/` found synchronization only in `src/parallel.c` (and the `bare_pthread_*` wrappers in
`src/bare_functions.h`). A second grep for mutable `static` variables found only `parallel.c`'s own state. The
other `static` objects in `src/` are `const` tables. Reusable encoders and decoders are documented as "one per
thread", so they need no locking by contract.

The pool, as it was:

- One persistent worker per online CPU. The calling thread is thread 0 and works too.
- A job is cut into tasks (about four per thread), claimed with one `atomic_fetch_add`.
- Workers sleep on a condition variable. A job broadcasts it, and the caller then waits until **all** workers have
  checked in, because the job struct lives on the caller's stack.
- One global `call_mutex` serializes whole calls, because the task scratch buffers and `encoders[0]` are shared.
- After the parallel phase, `gather` copies every task's buffer into the output one after another on the caller.

False sharing between the tasks' buffer headers had already been fixed earlier (each task works on a stack copy,
commit `c7970d9`), so it is not one of the findings.

## Part 2: the four findings and what became of them

| # | Finding | Idea | PR |
|---|---|---|---|
| 1 | The caller waits for every worker to wake and check out, even idle ones | Finish on a count of completed tasks; make the job a pool-owned slot with a generation tag so a late worker goes back to sleep without touching it; wake only as many workers as there are tasks | [#38](https://github.com/david-brandt-the-icelander/lzfse-compression/pull/38) |
| 2 | `call_mutex` is held for the whole call, so concurrent callers run one at a time | A call with one task uses a thread-local encoder and scratch and takes no lock; a larger call uses `trylock` and falls back to the same thread-local path if the pool is busy | [#40](https://github.com/david-brandt-the-icelander/lzfse-compression/pull/40) |
| 3 | `gather` is a serial copy after the parallel phase | Prefix sums, then copy and fill the offsets as a second phase on the pool, above a size threshold (512 KiB of output) | [#41](https://github.com/david-brandt-the-icelander/lzfse-compression/pull/41) |
| 4 | Sequentially consistent atomics, shared cache line for `next_task` and `status`, `pthread_once` on every call | Relaxed ordering, own cache line, cheaper initialized check | [#39](https://github.com/david-brandt-the-icelander/lzfse-compression/pull/39) |

Each pull request was written by its own fresh Claude Code session with only that finding's brief, so the four
could be reviewed and measured separately. All four changed only `src/parallel.c` (plus tests, and docs for #40
and #41). They conflict with each other textually, so expect to merge them one at a time.

The sessions were told not to run benchmarks, because four sessions building at once would make timings noisy.
Measurement happened afterwards, here.

## Part 3: the measurements

### What was measured, and how

A small purpose-written program (not `lzfse-bench`, and not committed) calls `lzfse_zazzcat_encode_chunks` with
64 KiB chunks against each tree's static library. Two modes:

- **Latency:** one caller, repeated calls on one input, median (and p10 and p90) per call after 50 warm-up calls.
- **Concurrent callers:** N threads, each looping on its own buffer for two seconds; total calls per second.

The inputs are synthetic: a repeating phrase with random bytes mixed in. "Hard" data (25% random) compresses to
about 55% of its size; "fast" data (about 2% random) compresses to about 5%. Base and pull-request builds were
run interleaved, three runs each, to spread out drift.

The machine was the owner's laptop with 8 performance and 2 efficiency cores, **not idle**: the load average was
7 to 10 throughout, from other applications. Absolute numbers are therefore rough. Where base and pull request
differ only slightly, treat the result as "no change".

### Finding 2 (PR #40): large win

Total calls per second, five runs each:

| Callers | Buffer | main | PR #40 |
|---|---|---|---|
| 4 | 16 KiB | about 34k | about 144k |
| 8 | 16 KiB | about 33k | about 260k to 280k |
| 8 | 60 KB | about 19k | about 69k |
| 8 | 64 KiB | about 14k to 21k | about 56k to 60k |

On `main` the process used about 108% of one CPU however many callers there were: the global lock ran everything
on one core. With the pull request, throughput scales with the number of callers. This only helps programs that
call from several threads at once, which is the case for a daemon compressing many small files.

### Finding 1 (PR #38): small win

One caller, median per call:

| Input | Data | main | PR #38 |
|---|---|---|---|
| 128 KiB | fast | 149 us | 152 us |
| 256 KiB | fast | 176 us | 172 us (p90 235 to 187) |
| 256 KiB | hard | 577 us | 532 to 544 us |
| 1 MiB | fast | 340 us | 339 us |
| 1 MiB | hard | 1066 to 1101 us | 1082 to 1107 us |

The audit expected the caller's wait for slow-waking workers to show up as tens of microseconds on small jobs.
It mostly did not: the caller works while the workers wake, so the wake-up latency is hidden. What remains is
about 6% on one size and a tighter p90.

### Finding 4 (PR #39): no change

256 KiB, 1 MiB and 4 MiB: 575 vs 576 us, 1070 vs 1072 us, 4108 vs 4112 us. Eight concurrent callers: within
noise. As expected for a cleanup; it can merge on its merits as a no-regression change.

### Finding 3 (PR #41): no change

Hard data, median per call:

| Input | main | PR #41 |
|---|---|---|
| 1 MiB | 1216 to 1275 us | 1266 to 1296 us |
| 4 MiB | 4746 to 4775 us | 4697 to 4784 us |
| 16 MiB | 17.8 to 18.3 ms | 17.7 to 17.9 ms |
| 64 MiB | 71.7 to 72.5 ms | 71.1 to 72.1 ms |
| 256 MiB | 258 to 262 ms | 255 to 267 ms |
| 1 GiB | 1031 to 1070 ms | 1003 to 1221 ms (one outlier) |

Fast data at 64 MiB and 256 MiB: the same. The audit's estimate that the serial copy might be a fifth of the
call was wrong. Encoding takes about a second per gigabyte even on all cores, while the copy of the compressed
output is a small fraction of that. The benchmark also reuses its output buffer, so the copy never pays first-touch
page faults, which would have been the case where parallel copying could help most. The conclusion for that case
is untested: a caller that hands in a fresh, cold output buffer on every call might see a gain.

## Part 4: what went wrong along the way

- **The first benchmark pass was invalid.** The sessions renamed their branches when they pushed, so the branch
  names in their completion notes pointed at the old tip, which is `main`. Two builds that were meant to be PR
  builds were `main`, and the results showed no difference. The giveaway was in the concurrent run: the "PR"
  build used 108% CPU, exactly like `main`. The fix was to take each PR's head commit from `gh pr view`, not
  from the session's note, and to check that `git diff main --stat` was not empty before building.
- **A single run is not evidence on this machine.** Identical builds swung by 2x between runs in the first
  concurrent test, because of other load. Five interleaved runs per cell, and the comparison of medians, were
  needed before the 4x to 8x result and the "no change" results could be told apart.
- **The audit's estimates were guesses.** Two of four predicted wins (a share of the call for wake-up latency, and
  about 20% for the serial copy) did not appear. This is the same lesson as in the earlier posts: a change lands
  only with a measurement.

## Part 5: what this does and does not show

- It shows that the global lock is the one piece of thread-safety that costs real throughput, and only when
  callers are concurrent.
- It does not show that the pool is optimal. It says nothing about the E-cores' effect on tail time, or about the
  case where several large concurrent calls compete for the same cores (PR #40 runs those serially on the caller
  when the pool is busy; that case was not measured).
- The results come from one machine with background load, synthetic data and a hand-written harness. A rerun of
  the PR #38 and #40 comparisons on an idle machine with `lzfse-bench`'s corpus would make a stronger record.
  The numbers in the pull-request comments are the same as the ones here.

## What this could become

Posts this source material could support, and the facts each needs:

1. **"The lock that ran eight threads on one core."** Facts: the `call_mutex` design, the 108% CPU observation,
   the thread-local fallback, the table for PR #40, the caveat about the cost of a thread-local encoder (memory
   per thread, and a destructor at thread exit).
2. **"Latency you cannot see: why waiting for every worker did not matter."** Facts: the stack-allocated job, the
   all-workers check-in, the generation-tagged job slot, the table for PR #38, the point that the caller's own work
   hides the wake-up.
3. **"Guessing 20% and finding 0%."** Facts: the `gather` estimate, the table for PR #41, the buffer-reuse caveat,
   and the earlier "measured and reverted" posts as a pattern.
4. **"Benchmarking the wrong commit."** Facts: the renamed branches, the CPU-use tell, the habit of reading the
   head commit from the pull request. A short post, useful as a method note.
