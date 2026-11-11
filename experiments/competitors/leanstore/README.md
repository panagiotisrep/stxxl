# LeanStore (B-tree)

Source: `extern/leanstore`, https://github.com/leanstore/leanstore, branch `master` at commit `fcb64bf`
(2026-10-09; the repository has no releases). Shallow submodule. Compared with BOA's online mode (insert,
then query) and with the interleaved benchmark. Shared behaviour (same data as BOA, results format,
measurement notes): [../README.md](../README.md).

## Build

```bash
sudo apt-get install cmake libtbb-dev libaio-dev   # system packages LeanStore needs (Ubuntu/Debian)
experiments/competitors/leanstore/build.sh          # → build-competitors/leanstore/leanstore_bench
```

- LeanStore must be the top-level CMake project (it uses `CMAKE_SOURCE_DIR` throughout). `build.sh`
  configures `extern/leanstore` itself and adds the driver target through CMake's
  `CMAKE_PROJECT_leanstoredb_INCLUDE` hook (`inject.cmake`). LeanStore's own files are not modified.
- LeanStore downloads gflags, tabulate and rapidjson itself during the build (pinned commits in its
  `libs/*.cmake`), so the build needs network access.
- LeanStore's CMake adds `-mavx2 -mcx16` on x86: an x86-64 CPU with AVX2 is needed.

## Run

```bash
build-competitors/leanstore/leanstore_bench --size 10000000 --key-dist uniform --query-dist all --dram_gib=1
build-competitors/leanstore/leanstore_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/leanstore/run.sh     # full grid, see the variables at the top of the script
```

`leanstore_bench --help` lists the driver's options, and `--helpfull` lists every LeanStore flag.

## Every LeanStore setting

LeanStore is configured through its gflags (`backend/leanstore/Config.cpp`). The driver handles the
workload options itself and passes every other `--name=value` to LeanStore. **The value of every LeanStore
flag is written to the results file** (`LeanStore flag <name> <value>`, marked `(set)` when not the default).

**Build time** (`build.sh`):

| Setting | Value | Why |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | like the other competitors (LeanStore's README uses `RelWithDebInfo`; LeanStore adds `-g3` in both) |
| `COUNTERS_LEVEL`, `CHECKS_LEVEL`, `PARANOID`, `SANI` | LeanStore's defaults (`all`, `default` → release checks, off, off) | |

**Run time: defaults the driver changes** (each can still be overridden on the command line):

| Flag | Driver default | LeanStore default | Why |
|---|---|---|---|
| `--worker_threads` | 1 | 4 | one thread runs the benchmark, like `boa` |
| `--ssd_path` | `leanstore_bench_ssd` | `./leanstore` | LeanStore's data file (opened with `O_DIRECT`), removed after each experiment unless `--keep-db` |
| `--trunc` | true | false | fresh file for every experiment |

**Run time: LeanStore defaults worth stating with results** (unchanged):

| Flag | Default | Meaning |
|---|---|---|
| `--dram_gib` | 1 | buffer pool size in GiB: LeanStore's memory budget |
| `--vi`, `--mv`, `--isolation_level` | true, true, `si` | versioned B-tree (`BTreeVI`) with snapshot isolation |
| `--wal` | true | write-ahead logging |
| `--pp_threads` | 1 | page provider (eviction) threads |
| `--contention_split`, `--xmerge` | true, false | B-tree techniques from CIDR'21 |

LeanStore checks flag combinations at startup: `--vi` needs `--wal`, and `--isolation_level=si` needs `--vi`
and `--mv`. For the plain B-tree (`BTreeLL`) use `--vi=false --isolation_level=rc` (or `ru`, which also
allows `--wal=false`).

Fixed by the driver:

| Setting | Value |
|---|---|
| Tree | `registerBTreeVI` if `--vi`, else `registerBTreeLL`, with `enable_wal = --wal`, `use_bulk_insert = false` (as LeanStore's own `LeanStoreAdapter`) |
| Transactions | one per operation (`startTX(OLTP, --isolation_level)` … `commitTX`), as in LeanStore's YCSB driver |
| Threads | the whole benchmark runs as one job on worker thread 0 |
| Profiling thread | not started (no CSV files) |
| Key | 4-byte big-endian encoding of the `uint32_t` key (byte order = numeric order) |
| Value | 1 byte (`'a'`), like BOA's `char` value |

## Results file

Name: `leanstore_<dram_gib>_<vi|ll>_<isolation level>_<key dist>_<query dists>_exp_<N>.txt`
(interleaved: `leanstore_interleaved_<dram_gib>_<vi|ll>_<isolation level>_<ins>_<queries>_exp_<N>.txt`),
in the current directory, or `--log-file`.

LeanStore-specific lines: all `LeanStore flag …` values in the header, `disk bytes` (space allocated for
the data file; it is sparse because the WAL starts at `--wal_offset_gib`) at each insertion checkpoint
and batch, and at the end the buffer pool size, the tree height and the disk bytes.

## Notes

- LeanStore always uses `O_DIRECT`, so `--dram_gib` (the buffer pool) is its whole cache. With the
  default 1 GiB, small data sets never touch the disk. In a small test (1M keys, `--dram_gib=0.01`),
  inserting took about 94 s on WSL, against about 0.6 s for 200k keys in a 1 GiB pool.
- `Peak RSS bytes` includes the whole buffer pool.
