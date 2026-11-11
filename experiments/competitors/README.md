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

```
experiments/competitors/
  build_all.sh          downloads (submodules) and builds every competitor
  CMakeLists.txt        shared by leveldb, rocksdb, pachash: selects one (-DCOMPETITOR=…)
  common.h              shared by all drivers: workload options, ProcIOStats (copied from boa.cpp),
                        results-file helpers
  <competitor>/
    <competitor>_bench.cpp
    CMakeLists.txt      that competitor's build settings (leanstore, stxxl: inject.cmake, see their READMEs)
    build.sh            → build-competitors/<competitor>/
    run.sh              experiment grid → results/<competitor>/
    README.md           every setting used for that competitor
```

## Build and run

```bash
sudo apt-get install cmake libtbb-dev libaio-dev     # LeanStore's system packages (Ubuntu/Debian)
experiments/competitors/build_all.sh                 # all competitors; COMPETITORS="leveldb pachash" for some
experiments/competitors/<competitor>/build.sh        # or one at a time
experiments/competitors/<competitor>/run.sh          # settings: variables at the top of each run.sh
```

`build_all.sh` fetches each submodule (pinned commit) and runs each `build.sh`, writing logs to
`build-competitors/logs/`. A failing build does not stop the others; the summary lists each result.
Needs git, CMake ≥ 3.25 and a C++20 compiler. RocksDB is large (`JOBS=N` limits parallel compile jobs;
about 1 GB RAM per job). LeanStore downloads three small dependencies during its build.

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
  (PaCHash: no `--interleaved`).
- Interleaved mode (all except PaCHash), as in `boa`: uniform keys; query keys are `data[rand() % inserted]`,
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
- `End Insertions, time T` (PaCHash: the whole construction), `End Queries <dist>, time T`, each followed by
  `Phase I/O: …`; `Queries checksum <dist>` per distribution.
- Interleaved: one `Batch …` line per batch, in `boa`'s format.
- End: `Total` time, `OS read_bytes`/`write_bytes`/syscalls (as in `boa`), `Peak RSS bytes`, then the
  competitor's own statistics.

## Measurement notes (all competitors)

- LevelDB always, RocksDB by default, and PaCHash with `--cached-io` read through the OS page cache;
  STXXL uses whatever its disk configuration says (as for `boa`); LeanStore always uses `O_DIRECT`.
  Unless the data is much larger than RAM, queries are then served from memory (`OS bytes read 0`).
  For comparable numbers, run BOA and the competitors under the same memory limit (`MEMORY_MAX` in each
  `run.sh`: a cgroup limit that includes the page cache) and drop caches before each run (`DROP_CACHES=1`).
