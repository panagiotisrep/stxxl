# RocksDB

Source: `extern/rocksdb`, https://github.com/meta/rocksdb, release `v11.13.2` (commit `17b624b`).
Shallow submodule (only the pinned commit is fetched). Compared with BOA's online mode (insert, then
query) and with the interleaved benchmark. Shared behaviour (same data as BOA, results format,
measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/rocksdb/build.sh             # → build-competitors/rocksdb/rocksdb_bench
JOBS=8 experiments/competitors/rocksdb/build.sh      # fewer parallel compile jobs (RocksDB needs ~1 GB RAM per job)
```

Needs CMake and a C++20 compiler (RocksDB 11's requirement). The first build takes a while.

## Run

```bash
build-competitors/rocksdb/rocksdb_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/rocksdb/rocksdb_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/rocksdb/run.sh       # full grid, see the variables at the top of the script
```

`rocksdb_bench --help` lists all options.

## Every RocksDB setting

Everything not listed here is RocksDB v11.13.2's default.

**Build time** (`CMakeLists.txt`):

| Setting | Value | Why |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | |
| `CMAKE_CXX_STANDARD` | 20 | required by RocksDB 11 |
| `WITH_TESTS`, `WITH_ALL_TESTS`, `WITH_BENCHMARK_TOOLS`, `WITH_BENCHMARK`, `WITH_CORE_TOOLS`, `WITH_TOOLS`, `WITH_TRACE_TOOLS`, `WITH_EXAMPLES`, `WITH_JNI`, `ROCKSDB_BUILD_SHARED` | `OFF` | static library only |
| `PORTABLE` | 1 | baseline CPU instructions. RocksDB's default 0 adds `-march=native`, which makes the binary depend on the build machine; BOA is not built with it either |
| `WITH_LIBURING` | `OFF` | RocksDB's default `ON` uses io_uring only if liburing happens to be installed |
| `WITH_GFLAGS` | `OFF` | only needed for RocksDB's tools |
| `FAIL_ON_WARNINGS` | `OFF` | compilers newer than RocksDB's CI must not break the build |
| `WITH_SNAPPY`, `WITH_LZ4`, `WITH_ZLIB`, `WITH_ZSTD`, `WITH_BZ2` | `OFF` (RocksDB's default) | no compression; BOA does not compress either |
| `WITH_JEMALLOC`, `WITH_NUMA`, folly | `OFF` (RocksDB's default) | system malloc, as for BOA |

**Run time** (`rocksdb_bench` options; defaults are RocksDB's own):

| Option | RocksDB setting | Default |
|---|---|---|
| `--write-buffer-size B` | `Options::write_buffer_size` | 67108864 (64 MiB) |
| `--target-file-size B` | `Options::target_file_size_base` | 67108864 (64 MiB) |
| `--max-background-jobs N` | `Options::max_background_jobs` | 2 |
| `--block-size B` | `BlockBasedTableOptions::block_size` | 4096 |
| `--cache-size B` | `BlockBasedTableOptions::block_cache = NewLRUCache(B)` | RocksDB's internal 32 MiB cache |
| `--bloom-bits N` | `BlockBasedTableOptions::filter_policy = NewBloomFilterPolicy(N)` | no filter |
| `--cache-index-and-filter-blocks` | `BlockBasedTableOptions::cache_index_and_filter_blocks`: index and filter blocks live in (and count against) the block cache | false (they are kept outside the cache, unbounded) |
| `--direct-reads` | `Options::use_direct_reads` | false |
| `--direct-writes` | `Options::use_direct_io_for_flush_and_compaction` | false |
| `--sync` | `WriteOptions::sync` | false |
| `--compact-before-queries` | `DB::CompactRange(CompactRangeOptions(), nullptr, nullptr)` after the insertions, timed and logged separately as `Compaction` | off |
| `--db-dir PATH` | database directory, `DestroyDB` before each experiment and after it (unless `--keep-db`) | `rocksdb_bench_db` |

Fixed by the driver:

| Setting | Value |
|---|---|
| `Options::create_if_missing` / `error_if_exists` | true / true (fresh database per experiment) |
| `Options::table_factory` | block-based table with the `BlockBasedTableOptions` above (all other fields default) |
| `Options::compression` | RocksDB's default: Snappy only if built with it; built without → `kNoCompression` |
| `WriteOptions` | defaults except `sync` (write-ahead log on) |
| `ReadOptions` | defaults (`verify_checksums` true, `fill_cache` true) |
| Key | 4-byte big-endian encoding of the `uint32_t` key (byte order = numeric order) |
| Value | 1 byte (`"a"`), like BOA's `char` value |

All of these, plus further defaults (level compaction, `num_levels`, `max_bytes_for_level_base`,
`level_compaction_dynamic_level_bytes`, `max_write_buffer_number`, `max_open_files`), are written at the
top of every results file (`RocksDB …` lines).

## Results file

Name: `rocksdb_<write buffer>_<block size>_<cache>_<bloom bits>_<key dist>_<query dists>_exp_<N>.txt`
(interleaved: `rocksdb_interleaved_<write buffer>_<block size>_<cache>_<bloom bits>_<ins>_<queries>_exp_<N>.txt`),
in the current directory, or `--log-file`.

RocksDB-specific lines: `disk bytes` at each insertion checkpoint and batch, `Compaction` (with
`--compact-before-queries`), and at the end memtable, table-reader and block-cache memory, `RocksDB disk
bytes`, files per level and `rocksdb.stats`.

## Notes

- Unlike LevelDB, RocksDB supports direct I/O (`--direct-reads`, `--direct-writes`), which bypasses the
  page cache like PaCHash's default `O_DIRECT`. The default (off) is RocksDB's own; state the setting
  with every result.
- RocksDB flushes and compacts in background threads, so work started by the insertions can continue into
  the query phase. `--compact-before-queries` finishes it first and logs its cost separately.
- With the default 64 MiB write buffer, small runs never flush to disk (everything stays in the memtable).
