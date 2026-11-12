# BOA benchmark

Benchmark program: `examples/containers/boa.cpp` (build target `boa`).

Benchmarks two external-memory hash table implementations:

| Implementation | Header | Selected by |
|---|---|---|
| Standard BOA (bins inside runs) | `include/stxxl/bits/containers/boa.h` | default (without `--boa-pv`) |
| Pointer-augmented BOA ("pv") | `include/stxxl/bits/containers/boa_pv.h` | `--boa-pv` |

`boa_opt.h` (`BOA_OPT`) is still compiled in but is not covered here.

Settings come in two kinds:

- **Compile-time macros**: structure parameters, `IN_MEMORY_SORT` and debug checks. Set them with `#define`
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
| `--interleaved` | off                                    | Run the interleaved benchmark (batches of insertions, each followed by queries to uniformly chosen keys inserted so far) instead of insert-then-query. Uses `--key-dist` and `--log-file`; `--query-dist` does not apply. |
| `--insertions-per-batch N` | 50000                                  | With `--interleaved`: insertions per batch |
| `--queries-per-batch N` | 2000                                   | With `--interleaved`: queries per batch (capped by the number of keys inserted so far) |
| `--suspended` | off                                    | Online-suspended mode (see [Online-suspended mode](#online-suspended-mode)) |
| `--materialization NAME` | `merge`                                | Suspended-mode materialization strategy: `merge`, `sort`, `hybrid`. Implies `--suspended`. |
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
  insert `--insertions-per-batch` keys, then run `--queries-per-batch` lookups of random keys inserted so
  far. Logs one line per batch (insertion time, query time, I/O details, structure size, merges).
  It always uses uniform keys and random queries, and ignores `--queries`, `--key-dist`, `--query-dist`,
  `--log-file` and the `--output-every-*` options. Results go to
  `boa_interleaved[_pv][_static…]_<λ>_<mult>_<pages>_<block size>_<insertions per batch>_<queries per batch>_exp_<N>.txt`.

### What the results file contains

Per checkpoint: time and STXXL I/O since the phase started (reads, writes, bytes, read/write wait time in ms). Per query distribution:

```
Search stats <dist>: Searches [..], Visited runs [..], Visited total elements [..], Directory reads [..]
```

(`Directory reads` is pv only). At the end, pv runs also log:

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
| `merge` (default) | Each buffer is sorted when flushed to the staging log; consolidation k-way merges those sorted chunks into the final runs. |
| `sort` | Buffers are appended unsorted; the whole log is sorted once at consolidation (`stxxl::sort`), then copied into the final runs. Displaced old runs are re-sorted together with the log. |
| `hybrid` | As `sort`, but displaced old runs are merged into the promoted run instead of re-sorted. |

What the benchmark does in this mode:

- `insertions_then_queries_benchmark`: `reserve_lazy_space_for_elements(size)`, `lazy_insert` for every
  key, prints `before consolidation, time …`, then `consolidate_structure()`. The reported insertion
  time includes consolidation.
- `insertions_with_queries_benchmark`: `consolidate_structure()` after every batch of inserts.
- Results file names get `_static_` (plus `_sort_` / `_hybrid_`), and the header says `STATIC`
  (plus `SORT` / `HYBRID`).

The strategy is the global `stxxl::materialization` (`boa_params.h`), which the benchmark sets from the
command line. The old macros still work as defaults: `ONLINE_SUSPENDED_MODE` turns `--suspended` on,
`SORT_BASED_MATERIALIZATION` / `HYBRID_MATERIALIZATION` set the default strategy.

---

## Compile-time macros

### Structure parameters (template arguments)

| Macro | Default | Meaning |
|---|---|---|
| `BOA_PAGES` | 2 | STXXL cache pages per vector |
| `BOA_BLOCK_SIZE` | `32*1024` | STXXL block size in bytes |
| `BOA_LAMBDA` | 16 | λ, runs per tier (pv: at most 127) |

These use `#ifndef`, so `-DBOA_LAMBDA=8` overrides them without editing the file.

### `IN_MEMORY_SORT` (compile-time, untested)

Keeps the staging log in RAM (`m_in_memory_lazy_table`) and sorts it with ips2ra instead of an external
STXXL vector. It compiles with all strategies, but some consolidation paths still read the external log,
so it is not expected to work; don't use it for results.

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
