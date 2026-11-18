# LMDB

Source: `extern/lmdb`, https://github.com/LMDB/lmdb, release `LMDB_0.9.36` (`028f61b`).
Compared with BOA's online mode, interleaved.
Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/lmdb/build.sh     # → build-competitors/lmdb/lmdb_bench
```

Needs CMake ≥ 3.13, a C and a C++17 compiler (LMDB's C files are compiled by `CMakeLists.txt`, not by its Makefile).

## Run

```bash
build-competitors/lmdb/lmdb_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/lmdb/lmdb_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/lmdb/run.sh       # full grid, see the variables at the top of the script
```

`lmdb_bench --help` lists all options.

## Every LMDB setting

Everything not listed here is LMDB's default. The driver writes every setting in effect into each results file.

LMDB is a memory-mapped copy-on-write B+-tree. It has no cache of its own: the OS page cache is its cache, so its memory cannot be set, and `run_all.sh` will mark it "over M" (like stock LevelDB). Queries run in one read transaction per batch.

| Option | LMDB setting | Default |
|---|---|---|
| `--map-size B` | `mdb_env_set_mapsize` | 256 GiB (address space only; LMDB has no default) |
| `--commit-every N` | insertions per write transaction | 100000 (the benchmark's choice; LMDB has no default) |
| `--nosync` | `MDB_NOSYNC` | off (LMDB syncs at every commit) |
| `--writemap` | `MDB_WRITEMAP` | off |
| `--no-readahead` | `MDB_NORDAHEAD` | off (readahead on) |

The key is a 4-byte value, the value one byte (`a`), as in `boa.cpp`; the benchmark loops are in `../kv_bench.h`.
