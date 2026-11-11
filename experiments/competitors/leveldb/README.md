# LevelDB

Source: `extern/leveldb`, https://github.com/google/leveldb, release `1.23` (commit `99b3c03`).
Compared with BOA's online mode (insert, then query) and with the interleaved benchmark.
Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/leveldb/build.sh     # → build-competitors/leveldb/leveldb_bench
```

Needs CMake ≥ 3.13 and a C++17 compiler. LevelDB's own submodules (googletest, benchmark) are not needed.

## Run

```bash
build-competitors/leveldb/leveldb_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/leveldb/leveldb_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/leveldb/run.sh       # full grid, see the variables at the top of the script
```

`leveldb_bench --help` lists all options.

## Every LevelDB setting

Everything not listed here is LevelDB 1.23's default.

**Build time** (`CMakeLists.txt`):

| Setting | Value | Why |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | |
| `LEVELDB_BUILD_TESTS`, `LEVELDB_BUILD_BENCHMARKS`, `LEVELDB_INSTALL` | `OFF` | library only |
| `HAVE_SNAPPY` | `0` | LevelDB would link Snappy only if installed; off everywhere → data stored uncompressed (BOA does not compress either) |
| `HAVE_CRC32C` | `0` | LevelDB's portable CRC32C instead of an optional system library |
| `HAVE_TCMALLOC` | `0` | system malloc, as for BOA |

**Run time** (`leveldb_bench` options; defaults are LevelDB's own):

| Option | LevelDB setting | Default |
|---|---|---|
| `--write-buffer-size B` | `Options::write_buffer_size` | 4194304 (4 MiB) |
| `--max-file-size B` | `Options::max_file_size` | 2097152 (2 MiB) |
| `--block-size B` | `Options::block_size` | 4096 |
| `--block-restart-interval N` | `Options::block_restart_interval` | 16 |
| `--max-open-files N` | `Options::max_open_files` | 1000 |
| `--cache-size B` | `Options::block_cache = NewLRUCache(B)` | LevelDB's internal 8 MiB cache |
| `--bloom-bits N` | `Options::filter_policy = NewBloomFilterPolicy(N)` | no filter |
| `--sync` | `WriteOptions::sync` | false |
| `--compact-before-queries` | `DB::CompactRange(nullptr, nullptr)` after the insertions, timed and logged separately as `Compaction` | off |
| `--db-dir PATH` | database directory, `DestroyDB` before each experiment and after it (unless `--keep-db`) | `leveldb_bench_db` |

Fixed by the driver:

| Setting | Value |
|---|---|
| `Options::create_if_missing` / `error_if_exists` | true / true (fresh database per experiment) |
| `Options::compression` | left at default `kSnappyCompression`; has no effect because LevelDB is built without Snappy |
| `ReadOptions` | defaults (`verify_checksums` false, `fill_cache` true) |
| Key | 4-byte big-endian encoding of the `uint32_t` key (LevelDB's byte order = numeric order) |
| Value | 1 byte (`"a"`), like BOA's `char` value |

All of these are written at the top of every results file (`LevelDB …` lines).

## Results file

Name: `leveldb_<write buffer>_<block size>_<cache>_<bloom bits>_<key dist>_<query dists>_exp_<N>.txt`
(interleaved: `leveldb_interleaved_<write buffer>_<block size>_<cache>_<bloom bits>_<ins>_<queries>_exp_<N>.txt`),
in the current directory, or `--log-file`.

LevelDB-specific lines: `disk bytes` at each insertion checkpoint and batch, `Compaction` (with
`--compact-before-queries`), and at the end `LevelDB approximate memory usage`, `LevelDB disk bytes`,
files per level and `leveldb.stats`.

## Notes

- LevelDB has no direct I/O: it always reads and writes through the OS page cache (see the shared
  measurement notes).
- LevelDB compacts in background threads, so compaction started by the insertions can continue into the
  query phase. `--compact-before-queries` finishes it first and logs its cost separately.
