# BOA benchmark

Benchmark program: `examples/containers/boa.cpp` (build target `boa`).

Benchmarks two external-memory hash table implementations:

| Implementation | Header | Selected by |
|---|---|---|
| Standard BOA (bins inside runs) | `include/stxxl/bits/containers/boa.h` | default (without `--boa-pv`) |
| Pointer-augmented BOA ("pv") | `include/stxxl/bits/containers/boa_pv.h` | `--boa-pv` |

`boa_opt.h` (`BOA_OPT`) is still compiled in but is not covered here.

Settings come in two kinds:

- **Compile-time macros**: structure parameters, `BOA_KEEP_MERGED_RUNS`, `IN_MEMORY_SORT`,
  `BOA_LAZY_DIRECTORY` and debug checks. Set them with `#define`
  at the top of `boa.cpp` or with `-D` on the compiler command line (see [Setting macros](#setting-macros)).
- **Command-line options**: everything else, including online-suspended mode and its materialization
  strategy, chosen per run without recompiling.

---

## Command-line options

```
./boa [options]
```

| Option | Default                                | Meaning |
|---|----------------------------------------|---|
| `--size N` | 1000000                                | Number of elements (keys) to insert |
| `--queries N` | 20000                                  | Number of queries per query distribution |
| `--output-every-inserts N` | 100000                                 | Insertion progress/report interval |
| `--output-every-queries N` | 1000                                   | Query progress/report interval |
| `--buffer-size N` | `BOA_BLOCK_SIZE / sizeof(run element)` | In-memory buffer (elements per tier-0 run). The default is the memory budget: one block's worth of run elements, so it depends on the element size (2520 for `boa.h`, 13-byte elements; 2048 for pv, 16-byte elements). Keep the default when comparing implementations; smaller elements legitimately get more elements in the same memory. |
| `--repeat N` | 1                                      | Run the experiment N times |
| `--key-dist NAME` | `uniform`                              | Keys to insert: `uniform`, `seq_asc`, `seq_desc`, `nearly_sorted`, `clustered` |
| `--query-dist LIST` | `shuffled`                             | Comma-separated query distributions, or `all`: `shuffled`, `uniform`, `zipf`, `zipf_sorted`, `hotspot`, `latest`, `exponential`. The structure is built once; each distribution then queries it in the given order. |
| `--routing-filter-mult X` | 1                                      | Directory (routing filter) size multiplier. Load factor α ≈ 1/X: smaller X = smaller directory, cheaper insertion, more runs visited per query. |
| `--log-file PATH` | generated                              | Results file. Used as given (appended to if it exists). Default name is built from the parameters plus `_exp_N`. |
| `--no-io-details` | off                                    | Omit per-checkpoint I/O details from the results file |
| `--boa-pv` | off                                    | Use `boa_pv.h` |
| `--interleaved` | off                                    | Run the interleaved benchmark (batches of insertions, each followed by queries to uniformly chosen keys inserted so far) instead of insert-then-query. Uses `--key-dist` and `--log-file`. With `--query-dist NAME` (one distribution, not `shuffled`) each batch's queries are drawn from that distribution over the keys inserted so far (generated before the batch's timer starts); without it, uniform `rand()` over the inserted keys, as in the competitor drivers. The log says which (`Query distribution …`). |
| `--insertions-per-batch N` | 50000                                  | With `--interleaved`: insertions per batch |
| `--queries-per-batch N` | 2000                                   | With `--interleaved`: queries per batch (capped by the number of keys inserted so far) |
| `--suspended` | off                                    | Online-suspended mode (see [Online-suspended mode](#online-suspended-mode)) |
| `--materialization NAME` | `hybrid`                               | Suspended-mode materialization strategy: `merge`, `sort`, `hybrid`. Implies `--suspended`. |
| `--help` |                                        | Print the options and their defaults |

Invalid values (non-positive numbers, unknown distribution names, unknown options) print an error and
exit with code 1.

### Examples

```bash
# pv, 1M keys, clustered keys, three query distributions on the same structure
./boa --boa-pv --size 1000000 --queries 20000 --key-dist clustered --query-dist uniform,zipf,latest

# all query distributions, smaller directory, own log file
./boa --boa-pv --routing-filter-mult 0.05 --query-dist all --log-file results/pv_all.txt

# boa.h with the same settings, for comparison
./boa --routing-filter-mult 0.05 --query-dist all --log-file results/boa_all.txt

# interleaved: insert-heavy, balanced and query-heavy phases
./boa --boa-pv --interleaved --insertions-per-batch 50000 --queries-per-batch 2000
./boa --boa-pv --interleaved --insertions-per-batch 50000 --queries-per-batch 2000 --query-dist latest   # recent keys
./boa --boa-pv --interleaved --insertions-per-batch 50000 --queries-per-batch 50000
./boa --boa-pv --interleaved --insertions-per-batch 500   --queries-per-batch 50000 --size 100000  # 200 batches

# online-suspended mode with each materialization strategy
./boa --boa-pv --suspended                    # merge-based
./boa --boa-pv --materialization sort
./boa --boa-pv --materialization hybrid --interleaved   # consolidates after every batch
```

### Benchmarks

- `insertions_then_queries_benchmark` (called by `main`): insert all keys, then run each query distribution.
- `insertions_with_queries_benchmark` (with `--interleaved`): repeats until `--size` keys are inserted:
  insert `--insertions-per-batch` keys, then run `--queries-per-batch` lookups of keys inserted so far
  (uniform `rand()`, or drawn from `--query-dist NAME` over the inserted keys). Logs one line per batch
  (insertion time, query time, I/O details, structure size, merges). Uses `--key-dist` and `--log-file`;
  ignores `--queries` and the `--output-every-*` options. Without `--log-file`, results go to
  `boa_interleaved[_pv][_static…]_<λ>_<mult>_<pages>_<block size>_<insertions per batch>_<queries per batch>[_<query dist>]_exp_<N>.txt`.

### What the results file contains

Per checkpoint: time and STXXL I/O since the phase started (reads, writes, bytes, read/write wait time in ms), plus the current structure size: `boa size` (data + runs + buffer) and `index bytes <total> (runs <B>, directories <B>)`, the index alone, as in the final `Index bytes` line. Per query distribution:

```
Search stats <dist>: Searches [..], Visited runs [..], Visited total elements [..], Directory reads [..]
```

(`Directory reads` is pv only). At the end, every run logs the index size (runs + directories, without
the key-value data vector):

```
Index bytes: runs <B>, directories <B>, total <B>, per element <B>
```

`runs` is the allocated size of all run vectors (bin padding included for `boa.h`; `boa_pv.h` runs are not
padded; merged runs count 0 unless `BOA_KEEP_MERGED_RUNS`), `directories` the full size of every tier
directory (also with `BOA_LAZY_DIRECTORY`: all its blocks are allocated, only unwritten ones are not
initialized), `per element` = total / `--size`.

pv runs also log:

| Line | Meaning |
|---|---|
| `Directory bytes` | Total size of all tier directories |
| `Pointer escapes` | Directory entries / element pointers whose position did not fit the 24-bit delta (should be 0; each one costs a run scan on lookup) |
| `Pointer max \|delta\|` | Largest stored position delta (limit ±8,388,607) |

Default file name: `boa[_pv][_static[_sort|_hybrid]]_<λ>_<mult>_<pages>_<block size>_<key dist>_<query dists>_exp_<N>.txt`.

---

## Online-suspended mode

Bulk-load mode, selected with `--suspended` or `--materialization NAME`. Inserts go to a staging log
(`lazy_insert`) instead of the tier structure; `consolidate_structure()` then builds the final BOA state
in one step. Both `boa.h` and `boa_pv.h` implement it (pv builds its runs with element pointers and
pointer directory entries, like online insertion).

| `--materialization` | How consolidation builds runs |
|---|---|
| `merge` | Each buffer is sorted when flushed to the staging log; consolidation k-way merges those sorted chunks into the final runs. |
| `sort` | Buffers are appended unsorted; the whole log is sorted once at consolidation (`stxxl::sort`), then copied into the final runs. Displaced old runs are re-sorted together with the log. |
| `hybrid` (default) | As `sort`, but displaced old runs are merged into the promoted run instead of re-sorted. |

Run layout: `boa.h` pads every run's hash bins to the largest bin (its lookups locate a bin by slot).
`boa_pv.h` does not pad, in either mode: its directory entries and element pointers store exact positions,
so its runs hold exactly their elements (16 B each).

One consolidation into an empty structure cuts the sorted log into runs tier 0 first, so each tier holds
its own contiguous hash range and its runs are consecutive slices of it.

What the benchmark does in this mode:

- `insertions_then_queries_benchmark`: `reserve_lazy_space_for_elements(size)`, `lazy_insert` for every
  key, prints `before consolidation, time …`, then `consolidate_structure()`. The reported insertion
  time includes consolidation.
- `insertions_with_queries_benchmark`: `consolidate_structure()` after every batch of inserts.
- Results file names get `_static_` (plus `_sort_` / `_hybrid_`), and the header says `STATIC`
  (plus `SORT` / `HYBRID`).

The strategy is the global `stxxl::materialization` (`boa_params.h`), which the benchmark sets from the
command line. The old macros still work as defaults: `ONLINE_SUSPENDED_MODE` turns `--suspended` on,
`MERGE_BASED_MATERIALIZATION` / `SORT_BASED_MATERIALIZATION` change the default strategy (hybrid).

---

## Compile-time macros

### Structure parameters (template arguments)

| Macro | Default | Meaning |
|---|---|---|
| `BOA_PAGES` | 2 | STXXL cache pages per vector |
| `BOA_BLOCK_SIZE` | `32*1024` | STXXL block size in bytes |
| `BOA_LAMBDA` | 16 | λ, runs per tier (pv: at most 127) |

These use `#ifndef`, so `-DBOA_LAMBDA=8` overrides them without editing the file.

### `BOA_KEEP_MERGED_RUNS` (online mode)

By default, when a tier is merged into the next one, its runs are cleared (`m_run.clear()` in
`compact_tiers()`, `boa.h` and `boa_pv.h`) and their blocks go back to the STXXL block manager. Defining
`BOA_KEEP_MERGED_RUNS` restores the old behaviour: merged runs keep their blocks, and the next run written
to the same slot reuses them (`stxxl::vector::resize` never shrinks capacity). Then the space of every
tier that has ever been full stays allocated, up to about 2× the run space right after a new tier
appears. Clearing costs no I/O, only block-manager bookkeeping. At 50M (boa, 32 KiB blocks, one run
each), insertion took 187 s with clearing and 205 s without, with the same query time.
Online-suspended mode is not affected by this macro (its `resize(0)` calls also keep the blocks).

### `IN_MEMORY_SORT` (online-suspended mode)

Keeps the staging log in RAM (`m_in_memory_lazy_table`, 24 B per element) instead of an external STXXL
vector, and sorts it with ips2ra (parallel in-memory radix sort) instead of `stxxl::sort`. The key-value
pairs still go to the external data vector. Every consolidation path reads the in-memory log, including
displacing old runs (`displace_old_runs`, lazy `merge_runs`), so later consolidations (interleaved
suspended mode) work too.

Tested: `sort` and `hybrid`, `boa.h` and `boa_pv.h`, insert-then-query and interleaved; `merge` with
`boa_pv.h`. The log's RAM grows with the number of staged elements (about 1.2 GB at 50M), so this does not
respect the memory budget; report it as such.

Measured (pv, hybrid, 32 KiB blocks, λ 16): at 10M the sort takes 0.17 s instead of 1.3 s with
`stxxl::sort` (whole construction about 10–12 s instead of 11–16 s); at 50M the sort takes 0.9 s and the
whole construction 17–18 s.

### `BOA_LAZY_DIRECTORY` (both implementations, both modes)

The directory (`routing_filter`) is never zero-filled. A RAM bitmap with one bit per directory block
records which blocks have been written since the directory was created; everything is inside
`routing_filter` in `boa.h` and `boa_pv.h`:

- create and `reset()`: drop the STXXL vector and allocate a new one. No I/O (STXXL creates pages
  uninitialized and never reads an uninitialized page).
- lookup in a block never written: empty, without touching the vector (no I/O).
- first write into a block: the block is zeroed in the cache (no read), then the entry is written.

Extra RAM: 1/(8·B) of the directory size, a few KB (pv, 500M, λ 64, 64 KiB blocks: about 8 KB).

Online-suspended mode gains the most. Construction drops the zero fill and the read-back of every
directory block. Queries gain because each tier holds its own hash range (see
[Online-suspended mode](#online-suspended-mode)), so lookups in the other tiers land in never-written
blocks and cost no I/O. At 50M (hybrid, `IN_MEMORY_SORT`, 32 KiB blocks, λ 16, 20,000 queries, 2 runs each):

| | construction | 20k queries | query block reads | disk read / written |
|---|---|---|---|---|
| pv | 17.1 / 18.4 s | 23.1 / 21.6 s | 106,049 | 3.93 / 2.08 GB |
| pv, `BOA_LAZY_DIRECTORY` | 11.6 / 13.3 s | 14.9 / 13.8 s | 59,853 | 1.96 / 1.51 GB |
| boa | 17.3 / 18.0 s | 21.8 / 20.2 s | 95,088 | 3.34 / 2.13 GB |
| boa, `BOA_LAZY_DIRECTORY` | 15.0 / 15.3 s | 13.5 / 14.0 s | 59,845 | 1.96 / 1.84 GB |

Online mode gains little (estimate, not measured): each run still reads and rewrites the tier's directory
blocks, and only the zero fill and the first run's read-back go away, about 2/(2λ+1) of the directory
traffic (6% at λ 16). Every tier covers the whole hash range there, so queries do not gain.

### Debug: `BOA_PV_CHECK_DIRECTORY`

Test builds only. After every pv directory write and element pointer write, reads it back and throws if it
does not decode to the stored position. Adds extra directory reads, so I/O numbers from these builds are
not meaningful.

---

## Setting macros

Either uncomment/add them at the top of `boa.cpp`:

```cpp
#define BOA_LAMBDA 8
#define BOA_BLOCK_SIZE (64*1024)
```

or pass them to the compiler, e.g. via CMake:

```bash
cmake -DCMAKE_CXX_FLAGS="-DBOA_LAMBDA=8" ..
make boa
```

or for a one-off build in an existing build directory (keeps the configured flags):

```bash
cd cmake-build-release-wsl
make boa CXX_FLAGS="$(grep '^CXX_FLAGS' examples/containers/CMakeFiles/boa.dir/flags.make | sed 's/^CXX_FLAGS = //') -DBOA_PV_CHECK_DIRECTORY"
```

Touch `boa.cpp` (or the header) before such a build if nothing else changed, so `make` recompiles it.
