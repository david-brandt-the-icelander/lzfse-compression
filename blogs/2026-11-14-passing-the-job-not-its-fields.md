# Passing the job, not its fields

*2026-11-14 · lzfse-zazzcat*

`src/parallel.c` compresses a large input as many independent chunks, spread over a pool of threads. A review of
the code base looked for parameters *tunnelled* through the call stack: values that every function in a chain
receives only to hand them to the next one. The clearest case was in this file, and it had an odd shape. The values
were already in a struct, but the code took them out and passed them one by one.

## What it looked like

The threaded build describes one call's work in a `job` struct, shared by all threads:

```c
typedef struct job {
    const uint8_t *source;
    size_t length;
    size_t chunk_size;
    size_t chunk_count;
    size_t chunks_per_task;
    size_t task_count;
    lzfse_zazzcat_options options;
    lzfse_zazzcat_buffer *task_buffers;
    size_t *ends;
    atomic_size_t next_task;
    atomic_int status;
} job;
```

A worker claims a task number and calls `encode_task`. It had the whole `job` in hand, but passed six of its
fields separately, plus two values worked out from them:

```c
size_t first = task * j->chunks_per_task;
status = encode_task(*encoder, j->source, j->length, j->chunk_size, first,
                     min_size(j->chunks_per_task, j->chunk_count - first), &j->task_buffers[task],
                     j->ends + first);
```

When every task has finished, `gather` copies the tasks' buffers into the caller's output. It took seven
parameters, and five of them were `job` fields again:

```c
status = gather(pool.task_buffers, task_count, pool.ends, chunks_per_task, chunk_count, output, offsets);
```

`scratch_reserve` grows the per-task buffer array and the per-chunk end array. It took both arrays and both
capacities as four separate out-pointers:

```c
scratch_reserve(&pool.task_buffers, &pool.task_buffer_capacity, task_count, &pool.ends, &pool.ends_capacity,
                chunk_count)
```

Those same four fields were declared twice: once in the thread pool's struct, and once as four `serial_*` globals
in the build without threads.

## Why it was like that

The `job` struct is defined only in the threaded half of the file, inside `#else` of
`#ifdef LZFSE_ZAZZCAT_NO_THREADS`. `encode_task` and `gather` sit above that `#ifdef` because both builds use them.
They could not take a `job`, because the serial build has no `job` type, so they took its fields. Each later
change to the chunk layout had to be made in the struct, in every argument list and at both call sites.

Splitting the fields up also hid how they relate. Seen alone, `encode_task(encoder, source, length, chunk_size,
first, count, buffer, ends)` looks like it could be given any `first` and `count`. In fact both must come from the
task number and `chunks_per_task`, the way `gather` works them out. Two pieces of code computed the same layout
independently, and nothing checked that they agreed.

## The change

The shared fields moved into two small structs above the `#ifdef`, so both builds can use them:

```c
// How one call's input is cut up: `chunk_count` chunks of `chunk_size` bytes (the last may be shorter), grouped
// into `task_count` tasks of `chunks_per_task` consecutive chunks (the last task may have fewer).
typedef struct chunk_layout {
    const uint8_t *source;
    size_t length;
    size_t chunk_size;
    size_t chunk_count;
    size_t chunks_per_task;
    size_t task_count;
} chunk_layout;

// Working arrays kept between calls, with their capacities: one output buffer per task, and where each chunk's
// stream ends within its task's buffer.
typedef struct task_scratch {
    lzfse_zazzcat_buffer *task_buffers;
    size_t task_buffer_capacity;
    size_t *ends;
    size_t ends_capacity;
} task_scratch;
```

`job` now contains a `chunk_layout`, the options, a pointer to the pool's `task_scratch` and its two atomics. The
pool holds one `task_scratch`, and the serial build has one static `task_scratch` instead of four globals. The
three helpers take the structs:

```c
static lzfse_zazzcat_status encode_task(lzfse_zazzcat_encoder *encoder, const chunk_layout *layout, size_t task,
                                        task_scratch *scratch);
static lzfse_zazzcat_status gather(const chunk_layout *layout, const task_scratch *scratch,
                                   lzfse_zazzcat_buffer *output, lzfse_zazzcat_offsets *offsets);
static bool scratch_reserve(task_scratch *scratch, const chunk_layout *layout);
```

`encode_task` now works out which chunks belong to a task itself, the same way `gather` does, from
`task * layout->chunks_per_task`. The call sites shrink to:

```c
status = encode_task(*encoder, &j->layout, task, j->scratch);  // a worker
status = gather(&j.layout, &pool.scratch, output, offsets);     // the caller, at the end
```

The serial build describes its single task with the same struct. One task holds every chunk, so it sets
`chunks_per_task = chunk_count` and `task_count = 1`. Before, it passed `1` and `chunk_count` in the right argument
positions and relied on the reader to see what they meant.

## The cache line next door

Passing a pointer to shared memory, instead of copies of its values, has a cost that is easy to miss. The `job`
lives on the caller's stack, and its `layout` sits near `next_task`. Every thread does `atomic_fetch_add` on
`next_task` to claim work. Each such write takes that cache line away from the other cores. A thread reading
`layout->chunk_size` in its per-chunk loop could therefore miss the cache whenever another thread had just claimed
a task.

So `encode_task` reads the layout fields it needs into locals once, at the start, with a comment saying why. The
cost is a few loads per task, against encoding a whole chunk (64 KiB for decmpfs). The function already copies its output
buffer to the stack for the same reason: a task's buffer and its neighbour's share a cache line in the array.

## What did not change

This is a reorganisation, not a speed change. Every helper runs once per task or once per call, never per byte,
and the encoder's inner loops are untouched, so we did not run the benchmark. The output is byte-for-byte the
same.

`make format && make all test test-alloc test-sanitize` passed, including the libcompression and kernel
compatibility tests on macOS. `make DEFINES=-DLZFSE_ZAZZCAT_NO_THREADS all test` passed too. That build mattered
most here, because the serial path is the one that used to pass `1` and `chunk_count` in place of a layout.

## The case we left for later

The same review found a bigger instance in the encoder. `source`, `source_length` and `output` are passed beside
the `block_writer` through six levels: `encode_segment`, `encode_small`, `parse`, `block_writer_push`,
`block_writer_token` and `block_writer_flush`. The block writer also stores `block_start`, an offset into
`source`, so it is only correct if every call passes the same `source`. Putting the pointer in the struct would
make that impossible to get wrong.

That chain is different, though. `block_writer_token` is inlined into the parser's hot loop, and the literal
copies write through `uint8_t *`, which C lets alias anything. With `source` and `output` read through
`blocks->`, the compiler would have to reload them after every token. That is probably cheap, but it is a change
to a hot loop, so the project's rules require a callgrind or benchmark measurement before it lands. It needs its
own change.

The decoder repeats `output, output_capacity, position` through its token functions. Those were left as separate
values on purpose: `decode_tokens` copies them into locals, and the cold path uses `preserve_most`, so the loop
keeps its state in registers. Gathering them into a struct read through a pointer would undo that. See [Calling
conventions](2026-11-14-calling-conventions.md).
