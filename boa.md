# BOA benchmark

Benchmark program: `examples/containers/boa.cpp` (build target `boa`).

Benchmarks two external-memory hash table implementations:

| Implementation | Header | Selected by |
|---|---|---|
| Standard BOA (bins inside runs) | `include/stxxl/bits/containers/boa.h` | default (without `--boa-pv`) |
| Pointer-augmented BOA ("pv") | `include/stxxl/bits/containers/boa_pv.h` | `--boa-pv` |

`boa_opt.h` (`BOA_OPT`) is still compiled in but is not covered here.

Settings come in two kinds:

- **Compile-time macros**: change data layout or code paths. Set them with `#define` at the top of
  `boa.cpp` or with `-D` on the compiler command line (see [Setting macros](#setting-macros)).
- **Command-line options**: everything else, chosen per run without recompiling.

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
| `--interleaved` | off                                    | Run the interleaved benchmark (batches of insertions, each followed by queries) instead of insert-then-query |
| `--insertions-per-batch N` | 50000                                  | With `--interleaved`: insertions per batch |
| `--queries-per-batch N` | 2000                                   | With `--interleaved`: queries per batch (capped by the number of keys inserted so far) |
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

## Compile-time macros

### Structure parameters (template arguments)

| Macro | Default | Meaning |
|---|---|---|
| `BOA_PAGES` | 2 | STXXL cache pages per vector |
| `BOA_BLOCK_SIZE` | `32*1024` | STXXL block size in bytes |
| `BOA_LAMBDA` | 16 | λ, runs per tier (pv: at most 127) |

These use `#ifndef`, so `-DBOA_LAMBDA=8` overrides them without editing the file.

### Online-suspended mode

| Macro | Meaning |
|---|---|
| `ONLINE_SUSPENDED_MODE` | Bulk-load mode. Inserts go to a staging log (`lazy_insert`) instead of the tier structure; `consolidate_structure()` then builds the final BOA state in one step. |
| `SORT_BASED_MATERIALIZATION` | Consolidation strategy: global sort of the whole staging log, then copy into the final runs. |
| `HYBRID_MATERIALIZATION` | Consolidation strategy: merge displaced old runs into the promoted run, global sort for the rest of the staged elements. |
| `IN_MEMORY_SORT` | Keep the staging log in RAM (`m_in_memory_lazy_table`) and sort it with ips2ra, instead of an external STXXL vector sorted with `stxxl::sort`. Needs RAM for all staged elements. |

How they combine:

- **Neither strategy macro** = merge-based materialization: each buffer is sorted when flushed to the
  staging log, and consolidation k-way merges those sorted chunks into the final runs.
- **`SORT_BASED_MATERIALIZATION`**: buffers are appended unsorted; the whole log is sorted once at
  consolidation (in memory with `IN_MEMORY_SORT`, otherwise `stxxl::sort`).
- **`HYBRID_MATERIALIZATION`**: as sort-based for staged elements, but displaced old runs are merged.
- Define at most one of `SORT_BASED_MATERIALIZATION` / `HYBRID_MATERIALIZATION`.
- `IN_MEMORY_SORT` and the strategy macros only matter together with `ONLINE_SUSPENDED_MODE`.

What the benchmark does in this mode:

- `insertions_then_queries_benchmark`: `reserve_lazy_space_for_elements(size)`, `lazy_insert` for every
  key, prints `before consolidation, time …`, then `consolidate_structure()`. The reported insertion
  time includes consolidation.
- `insertions_with_queries_benchmark`: `consolidate_structure()` after every batch of inserts.
- Results file names get `_static_` (plus `_sort_` / `_hybrid_`), and the header says `STATIC`.

Both `boa.h` and `boa_pv.h` support this mode.

### Debug: `BOA_PV_CHECK_DIRECTORY`

Test builds only. After every pv directory write and element pointer write, reads it back and throws if it
does not decode to the stored position. Adds extra directory reads, so I/O numbers from these builds are
not meaningful.

---

## Setting macros

Either uncomment/add them at the top of `boa.cpp`:

```cpp
#define ONLINE_SUSPENDED_MODE
#define SORT_BASED_MATERIALIZATION
```

or pass them to the compiler, e.g. via CMake:

```bash
cmake -DCMAKE_CXX_FLAGS="-DONLINE_SUSPENDED_MODE -DSORT_BASED_MATERIALIZATION" ..
make boa
```

or for a one-off build in an existing build directory (keeps the configured flags):

```bash
cd cmake-build-release-wsl
make boa CXX_FLAGS="$(grep '^CXX_FLAGS' examples/containers/CMakeFiles/boa.dir/flags.make | sed 's/^CXX_FLAGS = //') -DBOA_PV_CHECK_DIRECTORY"
```

Touch `boa.cpp` (or the header) before such a build if nothing else changed, so `make` recompiles it.
