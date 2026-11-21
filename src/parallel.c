// Parallel compression of independent chunks (each chunk becomes its own complete stream) on a persistent pool of
// threads. Consecutive chunks are grouped into a few tasks per thread; each task encodes its chunks with its
// thread's own encoder into one buffer reserved up front. The buffers, the per-chunk end offsets and the encoders
// persist between calls, so a warmed-up call allocates nothing.
//
// Build with LZFSE_ZAZZCAT_NO_THREADS to get a serial version of the same API (for platforms without pthreads).
#include "bare_functions.h"
#include "buffer.h"
#include "common.h"

// Tasks per thread: enough for load balancing, few enough that each task's buffer is large.
#define TASKS_PER_THREAD 4

// Room to reserve for encoding `length` bytes as `chunks` streams: the encoder may briefly hold a block coded at
// up to 10 bits per literal (plus its header) before deciding to store it raw.
static size_t task_bound(size_t length, size_t chunks) {
    return length + length / 4 + chunks * 4096;
}

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

// Encode the chunks of task number `task` into that task's buffer, recording where each chunk's stream ends.
static lzfse_zazzcat_status encode_task(lzfse_zazzcat_encoder *encoder, const chunk_layout *layout, size_t task,
                                        task_scratch *scratch) {
    // Read the layout once: it is shared with other threads, and next to fields they keep writing (see `job`).
    const uint8_t *source = layout->source;
    size_t length = layout->length, chunk_size = layout->chunk_size;
    size_t first_chunk = task * layout->chunks_per_task;
    size_t chunk_count = min_size(layout->chunks_per_task, layout->chunk_count - first_chunk);
    size_t start = first_chunk * chunk_size;
    size_t task_length = min_size(chunk_count * chunk_size, length - start);
    size_t *ends = scratch->ends + first_chunk;
    // Work on a copy of the buffer's fields on this thread's stack. The encoder updates the length every few
    // bytes; in the shared array, neighbouring tasks' buffers share a cache line, and those stores from different
    // cores would keep taking the line from each other ("false sharing").
    lzfse_zazzcat_buffer local = scratch->task_buffers[task];
    local.length = 0;
    lzfse_zazzcat_status status = LZFSE_ZAZZCAT_OK;
    if (!buffer_reserve(&local, task_bound(task_length, chunk_count))) status = LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    for (size_t c = 0; c < chunk_count && status == LZFSE_ZAZZCAT_OK; c++) {
        size_t at = start + c * chunk_size;
        status = lzfse_zazzcat_encoder_encode(encoder, source + at, min_size(chunk_size, length - at), &local);
        ends[c] = local.length;
    }
    scratch->task_buffers[task] = local;
    return status;
}

// Grow an offsets array to hold at least `capacity` entries.
static bool offsets_reserve(lzfse_zazzcat_offsets *offsets, size_t capacity) {
    if (offsets->capacity >= capacity) return true;
    size_t *grown = zz_realloc(offsets->data, capacity * sizeof *grown);
    if (grown == NULL) return false;
    offsets->data = grown;
    offsets->capacity = capacity;
    return true;
}

void lzfse_zazzcat_offsets_free(lzfse_zazzcat_offsets *offsets) {
    zz_free(offsets->data);
    offsets->data = NULL;
    offsets->length = 0;
    offsets->capacity = 0;
}

// Copy the tasks' buffers to `output` back to back and fill `offsets` with every stream's start and the end.
static lzfse_zazzcat_status gather(const chunk_layout *layout, const task_scratch *scratch,
                                   lzfse_zazzcat_buffer *output, lzfse_zazzcat_offsets *offsets) {
    const lzfse_zazzcat_buffer *task_buffers = scratch->task_buffers;
    size_t total = 0;
    for (size_t t = 0; t < layout->task_count; t++) total += task_buffers[t].length;
    if (!buffer_reserve(output, total) || !offsets_reserve(offsets, layout->chunk_count + 1)) {
        return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    }
    offsets->data[0] = output->length;
    offsets->length = 1;
    for (size_t t = 0; t < layout->task_count; t++) {
        size_t at = output->length;
        buffer_append_reserved(output, task_buffers[t].data, task_buffers[t].length);
        size_t first = t * layout->chunks_per_task;
        size_t count = min_size(layout->chunks_per_task, layout->chunk_count - first);
        for (size_t c = 0; c < count; c++) offsets->data[offsets->length++] = at + scratch->ends[first + c];
    }
    return LZFSE_ZAZZCAT_OK;
}

// Make sure the scratch arrays hold one buffer per task and one end per chunk of `layout`.
static bool scratch_reserve(task_scratch *scratch, const chunk_layout *layout) {
    if (scratch->task_buffer_capacity < layout->task_count) {
        lzfse_zazzcat_buffer *grown = zz_realloc(scratch->task_buffers, layout->task_count * sizeof *grown);
        if (grown == NULL) return false;
        // New entries start as empty buffers.
        clear_bytes(grown + scratch->task_buffer_capacity,
                    (layout->task_count - scratch->task_buffer_capacity) * sizeof *grown);
        scratch->task_buffers = grown;
        scratch->task_buffer_capacity = layout->task_count;
    }
    if (scratch->ends_capacity < layout->chunk_count) {
        size_t *grown = zz_realloc(scratch->ends, layout->chunk_count * sizeof *grown);
        if (grown == NULL) return false;
        scratch->ends = grown;
        scratch->ends_capacity = layout->chunk_count;
    }
    return true;
}

#ifdef LZFSE_ZAZZCAT_NO_THREADS

// Serial version: one encoder and one set of scratch arrays, kept between calls (not safe to call from two
// threads).
static lzfse_zazzcat_encoder *serial_encoder;
static task_scratch serial_scratch;

size_t lzfse_zazzcat_parallelism(void) {
    return 1;
}

lzfse_zazzcat_status lzfse_zazzcat_encode_chunks(const void *source, size_t length, size_t chunk_size,
                                                 const lzfse_zazzcat_options *options, lzfse_zazzcat_buffer *output,
                                                 lzfse_zazzcat_offsets *offsets) {
    if (chunk_size == 0) return LZFSE_ZAZZCAT_INVALID_ARGUMENT;
    offsets->length = 0;
    size_t chunk_count = div_ceil(length, chunk_size);
    if (!offsets_reserve(offsets, chunk_count + 1)) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    offsets->data[offsets->length++] = output->length;
    if (chunk_count == 0) return LZFSE_ZAZZCAT_OK;
    if (serial_encoder == NULL && (serial_encoder = lzfse_zazzcat_encoder_create(options)) == NULL) {
        return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    }
    lzfse_zazzcat_encoder_set_options(serial_encoder, options);
    // One task holding every chunk.
    chunk_layout layout = {
        .source = source,
        .length = length,
        .chunk_size = chunk_size,
        .chunk_count = chunk_count,
        .chunks_per_task = chunk_count,
        .task_count = 1,
    };
    if (!scratch_reserve(&serial_scratch, &layout)) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    lzfse_zazzcat_status status = encode_task(serial_encoder, &layout, 0, &serial_scratch);
    if (status != LZFSE_ZAZZCAT_OK) return status;
    return gather(&layout, &serial_scratch, output, offsets);
}

#else

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

// The pool never has more threads than this, so a job's task count (at most TASKS_PER_THREAD per thread) fits the
// 16 bits it gets in a claim word.
#define MAX_POOL_THREADS 4096

// Pack a job's claim word: the job's generation (low 32 bits), its task count and the next task to hand out.
static uint64_t claim_make(uint64_t generation, size_t task_count, size_t next_task) {
    return (generation & 0xFFFFFFFFu) << 32 | (uint64_t)task_count << 16 | (uint64_t)next_task;
}

// The pool's single job slot. It belongs to the pool, not to a caller's stack, so a worker that wakes late only
// ever reads the claim word (which says whether its job is still current) and never stale job data.
typedef struct job {
    // Written by the caller before it publishes the job through `claim`; read by threads that claimed a task.
    chunk_layout layout;
    lzfse_zazzcat_options options;
    // The pool's scratch arrays; each task writes only its own buffer and its own chunks' ends.
    task_scratch *scratch;
    // These change while the job runs; keep them on their own cache lines, away from the read-only fields above.
    // Generation, task count and next task to hand out (see claim_make). A task is claimed by a compare-exchange
    // that succeeds only while the generation matches and tasks remain.
    _Alignas(128) atomic_uint_fast64_t claim;
    // Tasks finished so far (a release increment; the caller's acquire load makes their output visible), and the
    // first error.
    _Alignas(128) atomic_size_t finished_tasks;
    atomic_int status;
} job;

// The persistent pool. Thread 0 is whichever thread calls lzfse_zazzcat_encode_chunks; threads 1.. are workers.
typedef struct worker_pool {
    size_t thread_count;
    // One encoder per thread, created on first use and kept.
    lzfse_zazzcat_encoder **encoders;
    // Serializes calls: one job runs at a time.
    pthread_mutex_t call_mutex;
    // Guards `generation`; workers sleep on `job_ready` and the caller on `job_finished`.
    pthread_mutex_t mutex;
    pthread_cond_t job_ready;
    pthread_cond_t job_finished;
    // Incremented for every job, so a worker can tell a new job from one it has already seen.
    uint64_t generation;
    // The job slot (guarded by call_mutex for writing).
    job slot;
    // Task buffers and chunk ends, kept between calls (guarded by call_mutex).
    task_scratch scratch;
} worker_pool;

static worker_pool pool;
static pthread_once_t pool_once = PTHREAD_ONCE_INIT;

// Claim and run tasks of the job with generation `generation` with thread `thread_index`'s encoder until none are
// left or the slot has moved on to another job.
static void run_tasks(uint64_t generation, size_t thread_index) {
    job *j = &pool.slot;
    lzfse_zazzcat_encoder **encoder = &pool.encoders[thread_index];
    uint64_t seen = atomic_load_explicit(&j->claim, memory_order_acquire);
    for (;;) {
        size_t task_count = (size_t)(seen >> 16 & 0xFFFF), task = (size_t)(seen & 0xFFFF);
        if ((seen >> 32) != (generation & 0xFFFFFFFFu) || task >= task_count) return;
        // The acquire on success pairs with the caller's release store of the claim word, so the job's fields are
        // visible; on failure `seen` is reloaded.
        if (!atomic_compare_exchange_weak_explicit(&j->claim, &seen, seen + 1, memory_order_acquire,
                                                   memory_order_acquire)) {
            continue;
        }
        lzfse_zazzcat_status status = LZFSE_ZAZZCAT_OK;
        if (*encoder == NULL) *encoder = lzfse_zazzcat_encoder_create(&j->options);
        if (*encoder == NULL) {
            status = LZFSE_ZAZZCAT_OUT_OF_MEMORY;
        } else {
            lzfse_zazzcat_encoder_set_options(*encoder, &j->options);
            status = encode_task(*encoder, &j->layout, task, j->scratch);
        }
        if (status != LZFSE_ZAZZCAT_OK) {
            int ok = LZFSE_ZAZZCAT_OK;
            atomic_compare_exchange_strong_explicit(&j->status, &ok, (int)status, memory_order_relaxed,
                                                    memory_order_relaxed);
        }
        // Done with this job's data; whoever finishes the last task wakes the caller if it is asleep. Taking the
        // mutex first means the caller is either not yet waiting (and will see the count) or already waiting.
        if (atomic_fetch_add_explicit(&j->finished_tasks, 1, memory_order_release) + 1 == task_count) {
            bare_pthread_mutex_lock(&pool.mutex);
            bare_pthread_cond_signal(&pool.job_finished);
            bare_pthread_mutex_unlock(&pool.mutex);
        }
        seen = atomic_load_explicit(&j->claim, memory_order_acquire);
    }
}

// A worker thread: wait for each new job, help with its tasks, go back to sleep.
static void *worker_main(void *argument) {
    size_t thread_index = (size_t)(uintptr_t)argument;
    uint64_t seen_generation = 0;
    for (;;) {
        bare_pthread_mutex_lock(&pool.mutex);
        while (pool.generation == seen_generation) bare_pthread_cond_wait(&pool.job_ready, &pool.mutex);
        seen_generation = pool.generation;
        bare_pthread_mutex_unlock(&pool.mutex);
        run_tasks(seen_generation, thread_index);
    }
    return NULL;
}

// Start the pool: one thread per online CPU (counting the caller). If a thread cannot be created, the pool simply
// has fewer threads.
static void pool_start(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    size_t wanted = cpus > 0 ? (size_t)cpus : 1;
    if (wanted > MAX_POOL_THREADS) wanted = MAX_POOL_THREADS;
    bare_pthread_mutex_init(&pool.call_mutex, NULL);
    bare_pthread_mutex_init(&pool.mutex, NULL);
    bare_pthread_cond_init(&pool.job_ready, NULL);
    bare_pthread_cond_init(&pool.job_finished, NULL);
    pool.encoders = zz_calloc(wanted, sizeof *pool.encoders);
    pool.thread_count = 1;
    if (pool.encoders == NULL) return;
    for (size_t i = 1; i < wanted; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker_main, (void *)(uintptr_t)i) != 0) break;
        bare_pthread_detach(thread);
        pool.thread_count++;
    }
}

size_t lzfse_zazzcat_parallelism(void) {
    bare_pthread_once(&pool_once, pool_start);
    return pool.thread_count;
}

// Wait until all `task_count` tasks of the current job have finished: spin briefly (small jobs end within
// microseconds), then sleep on the condition variable.
static void wait_for_tasks(job *j, size_t task_count) {
    for (int spin = 0; spin < 2000; spin++) {
        if (atomic_load_explicit(&j->finished_tasks, memory_order_acquire) == task_count) return;
    }
    bare_pthread_mutex_lock(&pool.mutex);
    while (atomic_load_explicit(&j->finished_tasks, memory_order_acquire) != task_count) {
        bare_pthread_cond_wait(&pool.job_finished, &pool.mutex);
    }
    bare_pthread_mutex_unlock(&pool.mutex);
}

lzfse_zazzcat_status lzfse_zazzcat_encode_chunks(const void *source, size_t length, size_t chunk_size,
                                                 const lzfse_zazzcat_options *options, lzfse_zazzcat_buffer *output,
                                                 lzfse_zazzcat_offsets *offsets) {
    if (chunk_size == 0) return LZFSE_ZAZZCAT_INVALID_ARGUMENT;
    offsets->length = 0;
    size_t chunk_count = div_ceil(length, chunk_size);
    if (chunk_count == 0) {
        if (!offsets_reserve(offsets, 1)) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
        offsets->data[offsets->length++] = output->length;
        return LZFSE_ZAZZCAT_OK;
    }
    size_t threads = lzfse_zazzcat_parallelism();
    if (pool.encoders == NULL) return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    size_t chunks_per_task = div_ceil(chunk_count, threads * TASKS_PER_THREAD);
    chunk_layout layout = {
        .source = source,
        .length = length,
        .chunk_size = chunk_size,
        .chunk_count = chunk_count,
        .chunks_per_task = chunks_per_task,
        .task_count = div_ceil(chunk_count, chunks_per_task),
    };

    bare_pthread_mutex_lock(&pool.call_mutex);
    if (!scratch_reserve(&pool.scratch, &layout)) {
        bare_pthread_mutex_unlock(&pool.call_mutex);
        return LZFSE_ZAZZCAT_OUT_OF_MEMORY;
    }
    // Every worker of the previous job has stopped touching the slot (its tasks all finished), so fill it in.
    job *j = &pool.slot;
    j->layout = layout;
    j->options = options != NULL ? *options : lzfse_zazzcat_default_options();
    j->scratch = &pool.scratch;
    atomic_store_explicit(&j->finished_tasks, 0, memory_order_relaxed);
    atomic_store_explicit(&j->status, LZFSE_ZAZZCAT_OK, memory_order_relaxed);
    // Wake only as many workers as there are tasks beyond the caller's own, and only when there are any.
    size_t wake = min_size(pool.thread_count - 1, layout.task_count - 1);
    bare_pthread_mutex_lock(&pool.mutex);
    uint64_t generation = ++pool.generation;
    // Publish the job: the release store makes the fields above visible to whoever claims a task.
    atomic_store_explicit(&j->claim, claim_make(generation, layout.task_count, 0), memory_order_release);
    bare_pthread_mutex_unlock(&pool.mutex);
    // A signal that finds no sleeping worker is not lost: a worker that is awake checks the generation before it
    // sleeps again.
    for (size_t i = 0; i < wake; i++) bare_pthread_cond_signal(&pool.job_ready);
    run_tasks(generation, 0);
    wait_for_tasks(j, layout.task_count);
    lzfse_zazzcat_status status = (lzfse_zazzcat_status)atomic_load_explicit(&j->status, memory_order_relaxed);
    if (status == LZFSE_ZAZZCAT_OK) {
        status = gather(&layout, &pool.scratch, output, offsets);
    }
    bare_pthread_mutex_unlock(&pool.call_mutex);
    return status;
}

#endif
