# Competitors

Competitor benchmarks that run the same workloads as the BOA benchmark (`examples/containers/boa.cpp`).
Each external competitor is a pinned git submodule under `extern/`; STXXL's own map and unordered_map come
from this repository. Every competitor has its own driver, build script, run script and README in its
own folder here. They are built separately from your stxxl build, so the normal `make boa` build is
unchanged.

| Competitor | Source (submodule) | Version | Compare with BOA's | Details |
|---|---|---|---|---|
| LevelDB | `extern/leveldb` — https://github.com/google/leveldb | release `1.23` (`99b3c03`) | online mode, interleaved | [leveldb/README.md](leveldb/README.md) |
| RocksDB | `extern/rocksdb` — https://github.com/meta/rocksdb | release `v11.13.2` (`17b624b`) | online mode, interleaved | [rocksdb/README.md](rocksdb/README.md) |
| PaCHash | `extern/pachash` — https://github.com/ByteHamster/PaCHash | commit `f7f18fd` (no releases) | online-suspended mode (`--suspended` / `--materialization`) only | [pachash/README.md](pachash/README.md) |
| LeanStore (B-tree) | `extern/leanstore` — https://github.com/leanstore/leanstore | commit `fcb64bf` (no releases) | online mode, interleaved | [leanstore/README.md](leanstore/README.md) |
| STXXL `map` (B+-tree), `unordered_map` | this repository | same build as `boa` | online mode, interleaved | [stxxl/README.md](stxxl/README.md) |
| SILT | `extern/silt` — https://github.com/ByteHamster/silt (build fixes of https://github.com/silt/silt) | branch `build-fix`, commit `55ee50b` | combined store: online mode, interleaved; sorted store: online-suspended mode | [silt/README.md](silt/README.md) |

```
experiments/competitors/
  build_all.sh          downloads (submodules) and builds every competitor
  CMakeLists.txt        shared by leveldb, rocksdb, pachash, silt: selects one (-DCOMPETITOR=…)
  common.h              shared by all drivers: workload options, ProcIOStats (copied from boa.cpp),
                        results-file helpers
  <competitor>/
    <competitor>_bench.cpp
    CMakeLists.txt      that competitor's build settings (leanstore, stxxl: inject.cmake, see their READMEs)
    build.sh            → build-competitors/<competitor>/
    run.sh              experiment grid → results/<competitor>/
    README.md           every setting used for that competitor
```

## System packages

Everything else is pinned in submodules (or, for LeanStore, downloaded at pinned commits during its
build). Package names for Ubuntu/Debian (tested on Ubuntu 24.04):

| Needed by | Packages |
|---|---|
| all (and `boa`) | `build-essential` (GCC/G++ with C++20), `cmake` (≥ 3.25), `git` |
| LevelDB | none |
| RocksDB | none (built without compression libraries, gflags, liburing, jemalloc) |
| PaCHash | none (`liburing-dev` only with `PACHASH_WITH_URING=ON`) |
| LeanStore | `libtbb-dev`, `libaio-dev` (x86-64 CPU with AVX2) |
| STXXL map / unordered_map | none beyond `boa`'s |
| SILT | `libxerces-c-dev`, `libssl-dev`, `libgsl-dev`, `libtbb-dev`, `libboost-dev` |

All at once:

```bash
sudo apt-get install build-essential cmake git libtbb-dev libaio-dev libxerces-c-dev libssl-dev libgsl-dev libboost-dev
```

## Build and run

```bash
experiments/competitors/build_all.sh                 # all competitors; COMPETITORS="leveldb pachash" for some
experiments/competitors/<competitor>/build.sh        # or one at a time
experiments/competitors/<competitor>/run.sh          # settings: variables at the top of each run.sh
```

`build_all.sh` fetches each submodule (pinned commit) and runs each `build.sh`, writing logs to
`build-competitors/logs/`. A failing build does not stop the others; the summary lists each result.
RocksDB is large (`JOBS=N` limits parallel compile jobs; about 1 GB RAM per job). LeanStore downloads
three small dependencies during its build.

## Same data as BOA (all competitors)

- Keys and queries come from the same generators with the same seeds: every driver includes
  `include/stxxl/bits/utils/workloads.h`, as `boa.cpp` does. Same key type (`uint32_t`), 1-byte values.
- The generators use `std::shuffle` and the `std::*_distribution` classes, whose output differs between
  standard library implementations. **Build BOA and the competitors with the same compiler and standard
  library.** Every results file records `Keys checksum` and `Queries checksum <dist>`, so this can be
  checked.
- The workload options have the same names, meanings and defaults as in `boa`: `--size`, `--queries`,
  `--key-dist`, `--query-dist`, `--output-every-inserts`, `--output-every-queries`, `--repeat`,
  `--log-file`, `--no-io-details`, `--interleaved`, `--insertions-per-batch`, `--queries-per-batch`
  (PaCHash and SILT's sorted store: no `--interleaved`).
- Interleaved mode (all except PaCHash and SILT's sorted store), as in `boa`: uniform keys; query keys are `data[rand() % inserted]`,
  with the same never-reseeded `rand()` sequence.
- Phases match `boa`: build/insert all keys, then each query distribution in the given order on the same
  structure. Every lookup is checked (`did not find` / `query error` stops the run).

Differences from `boa`, in all drivers:
- `--log-file` is also honoured in interleaved mode (`boa` ignores it there).
- Timings use `std::chrono::steady_clock`. (`boa.cpp` uses `high_resolution_clock`, which is the wall
  clock in libstdc++ and can jump, e.g. under WSL.)
- I/O is measured from `/proc/self/io` (the competitors have no STXXL counters): `OS bytes read`/`written`
  (actual block-device I/O), `read`/`write syscalls`, `rchar`/`wchar` (bytes through read/write calls,
  including page-cache hits). `boa` logs the same `OS read_bytes`/`write_bytes` totals at the end.

## Results files (all competitors)

- Header: workload, `Keys checksum`, every setting of the competitor.
- `Insertions N, time T, details: …` / `Queries N, time T, details: …`: like `boa`, time since the previous
  checkpoint; I/O since the phase started.
- `End Insertions, time T` (PaCHash, SILT sorted store: the whole construction), `End Queries <dist>, time T`, each followed by
  `Phase I/O: …`; `Queries checksum <dist>` per distribution.
- Interleaved: one `Batch …` line per batch, in `boa`'s format.
- End: `Total` time, `OS read_bytes`/`write_bytes`/syscalls (as in `boa`), `Peak RSS bytes`, then the
  competitor's own statistics.

## Measurement notes (all competitors)

- LevelDB always, RocksDB by default, and PaCHash with `--cached-io` read through the OS page cache;
  STXXL uses whatever its disk configuration says (as for `boa`); LeanStore always uses `O_DIRECT`;
  SILT uses direct I/O for random reads of synced data.
  Unless the data is much larger than RAM, queries are then served from memory (`OS bytes read 0`).
  For comparable numbers, run BOA and the competitors under the same memory limit (`MEMORY_MAX` in each
  `run.sh`: a cgroup limit that includes the page cache) and drop caches before each run (`DROP_CACHES=1`).

## Fair comparison

**The drivers are not tuned for fairness by default.** On purpose, each driver runs its competitor
with that competitor's own defaults (STXXL map/unordered_map: the settings of `map1.cpp` /
`unordered_map1.cpp`). It writes every setting into the results file and exposes the settings that
matter as options. Out of the box, the systems differ a lot:

| | Memory by default | I/O by default | Background threads |
|---|---|---|---|
| BOA | buffer of one block (32 KiB) + STXXL page caches; logged as `Max internal memory bytes` (grows with the number of tiers) | STXXL disk config (default `direct=try`) | none |
| LevelDB | 4 MiB memtable (+ 1 immutable) + 8 MiB block cache + index/filter blocks of up to 1000 open tables | page cache (no direct I/O) | 1 compaction thread (fixed) |
| RocksDB | 64 MiB memtable × 2 + 32 MiB block cache + index/filter blocks outside the cache | page cache | 2 jobs |
| PaCHash | index of about 6 bits per block; construction sorts all keys in RAM | `O_DIRECT` | none |
| LeanStore | 1 GiB buffer pool | `O_DIRECT` | page provider + WAL writer |
| STXXL map | 32 MiB node cache + 32 MiB leaf cache | STXXL disk config | none |
| STXXL unordered_map | 1 GiB buffer | STXXL disk config | none |
| SILT | in-memory hash tables and trie index, sized by its design and XML (not a budget) | direct for random reads | conversion/merge threads (combined store) |

Fairness is therefore set **at run time**, with the rules and parameters below. Report the settings
with every result; each results file already contains them.

### Rules

1. **Same machine, same device.** Put every system's files on the same disk and file system:
   `boa`'s and STXXL's `.stxxl` disk, `--db-dir`, `--store-file`, `--ssd_path`. Drop the page cache
   before every run (`DROP_CACHES=1`), and repeat each configuration (`REPEAT=3`).
2. **Same data.** Build `boa` and all competitors with the same compiler and standard library, and check
   that `Keys checksum` / `Queries checksum` agree across results files.
3. **Same mode.** Online and interleaved runs: BOA online against LevelDB, RocksDB, LeanStore, STXXL map,
   STXXL unordered_map and SILT `--mode combi`. Bulk loading: BOA `--suspended` / `--materialization`
   against PaCHash and SILT `--mode sorted`.
4. **Same memory budget M.** Take M from BOA: run `boa` with the configuration you report and read
   `Max internal memory bytes`. Give each competitor M through its own memory options (table below).
   Also run **every system, `boa` included, under the same cgroup memory limit** (`MEMORY_MAX` in the
   `run.sh` scripts, or `systemd-run --scope -p MemoryMax=…` for `boa`). The limit includes the page
   cache, so systems without direct I/O (LevelDB) cannot use free RAM as extra cache. The limit must
   also cover the benchmark's own arrays: about 4 bytes per key for the keys, 4 bytes per query for each
   query list, a temporary copy of the keys made by some query generators, plus the process baseline.
   Use roughly `MEMORY_MAX` = M + 12 bytes × N + 50 MB, and check `Peak RSS bytes` in the results.
5. **Direct I/O wherever possible.** `boa` / STXXL: `direct=on` in `.stxxl`
   (e.g. `disk=/data/stxxl,100G,syscall unlink direct=on`). RocksDB: `--direct-reads --direct-writes`.
   PaCHash, LeanStore, SILT: their defaults. LevelDB has no direct I/O; rule 4's limit is what bounds it.
6. **Same I/O unit where it is a parameter.** BOA uses 32 KiB blocks (`BOA_BLOCK_SIZE`). Set LevelDB and
   RocksDB `--block-size 32768` and PaCHash `PACHASH_BLOCK_LENGTH=32768`; STXXL map already uses 32 KiB.
   LeanStore (4 KiB pages) and SILT have fixed sizes; state them. Alternatively, run a second series
   with each system's default block size, and say which one a plot shows.
7. **Same work per operation.** BOA has no transactions, no write-ahead log and no fsync. RocksDB and
   LevelDB keep their WAL (LevelDB cannot turn it off), with `sync` off (default). LeanStore: use the
   plain B-tree without WAL (`--vi=false --isolation_level=ru --wal=false`). Queries run one at a time
   everywhere (PaCHash `--queue-depth 1`, its driver default).
8. **Same CPUs.** BOA is single-threaded. Use one worker thread and the fewest background threads each
   system allows (table below), and run every system under the same `taskset -c …` CPU set, so that
   background threads cannot use cores that BOA cannot.
9. **Optionally, an out-of-the-box series.** Also run every system with its defaults (no options) and
   report it separately. That shows how each system behaves as shipped.

### Parameters per competitor (memory budget M in bytes)

| System | Build | Run parameters |
|---|---|---|
| BOA (`boa`) | Release, `BOA_BLOCK_SIZE` 32 KiB | default `--buffer-size` (the memory budget); `.stxxl` with `direct=on`; online, `--interleaved`, or `--suspended` / `--materialization …` |
| LevelDB | `leveldb/build.sh` | `--write-buffer-size M/4` (memtable + immutable memtable = M/2; LevelDB's minimum is 64 KiB) `--cache-size M/2 --block-size 32768 --bloom-bits 10`; `MEMORY_MAX` (no direct I/O) |
| RocksDB | `rocksdb/build.sh` | `--write-buffer-size M/4` (2 memtables = M/2) `--cache-size M/2 --cache-index-and-filter-blocks --bloom-bits 10 --block-size 32768 --direct-reads --direct-writes --max-background-jobs 1` |
| PaCHash | `PACHASH_BLOCK_LENGTH=32768 pachash/build.sh` | `--io posix --queue-depth 1` (defaults; `O_DIRECT`); its index is far below M. Construction sorts all keys in RAM, so state that in the comparison with BOA's suspended mode |
| LeanStore | `leanstore/build.sh` | `--dram_gib=<M / 2^30> --worker_threads=1 --pp_threads=1 --vi=false --isolation_level=ru --wal=false` |
| STXXL map | `stxxl/build.sh` | `--node-cache M/2 --leaf-cache M/2` (each at least a few 32 KiB blocks); same `.stxxl` as `boa` |
| STXXL unordered_map | `stxxl/build.sh` | `--buffer-size M`; set `--initial-buckets` deliberately (the default 10000 from `unordered_map1.cpp` means about 1000 keys per bucket at N = 10^7, and insertion was very slow in tests); same `.stxxl` as `boa` |
| SILT | `silt/build.sh` | `--mode combi` (online, interleaved) or `--mode sorted` (bulk); direct I/O is the default. Its memory follows from its design (no budget option), so compare `Peak RSS bytes` and keep the same `MEMORY_MAX` |

With small M (BOA's budget is a few MB), the LSM-trees get small memtables and therefore many small
files and frequent compactions. That is the cost of equal memory, and it is why rule 9's out-of-the-box
series is worth reporting next to it.

