# PaCHash

Source: `extern/pachash`, https://github.com/ByteHamster/PaCHash, commit `f7f18fd` (2025-04-07; the
repository has no releases). License: GPLv3.

PaCHash is a **static** object store: it is constructed once from all keys and then queried. It is
compared only with BOA's **online-suspended mode** (`boa --suspended` / `--materialization merge|sort|hybrid`),
which also builds the structure in one step. The driver has no interleaved mode (`--interleaved` is
rejected). Shared behaviour (same data as BOA, results format, measurement notes):
[../README.md](../README.md).

## Build

```bash
experiments/competitors/pachash/build.sh     # → build-competitors/pachash/pachash_bench
PACHASH_WITH_URING=ON experiments/competitors/pachash/build.sh   # with io_uring (needs liburing)
```

Needs CMake ≥ 3.25 and a C++20 compiler (PaCHash's requirements). PaCHash's own `.gitmodules` use
`git@github.com:` URLs; `build.sh` fetches them over https instead, and only the ones the library needs
(`tlx`, `ips2ra`, `util` and their submodules, not `lz4`, which only one of PaCHash's examples uses).

## Run

```bash
build-competitors/pachash/pachash_bench --size 10000000 --key-dist uniform --query-dist all
experiments/competitors/pachash/run.sh       # full grid, see the variables at the top of the script
```

`pachash_bench --help` lists all options.

## Every PaCHash setting

**Build time** (`CMakeLists.txt`, set through `build.sh` environment variables):

| Setting | Value | Why |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | |
| `PACHASH_BLOCK_LENGTH` → `VARIABLE_SIZE_STORE_BLOCK_LENGTH` | 4096 (PaCHash's default) | block size is a compile-time constant in PaCHash |
| `PACHASH_WITH_URING` | `OFF` | PaCHash uses io_uring (for reading the file back during construction and for `--io uring`) only if liburing happens to be installed. `OFF`: never; `ON`: required, configuration fails without liburing |
| libaio | never used | PaCHash links it if installed; the driver does not offer PaCHash's PosixAIO |
| `-march=native` | not used | PaCHash adds it only when built as the top-level project; BOA is not built with it either |
| PaCHash's examples, benchmarks, `-Werror` | not built / not used | only when PaCHash is the top-level project |

**Run time** (`pachash_bench` options):

| Option | Meaning | Default |
|---|---|---|
| `--a N` | PaCHash parameter *a* (bins per block): 1, 2, 4, 8, 16, 32, 64, 128 | 8 (the value in PaCHash's README example) |
| `--io NAME` | query I/O: `posix` (`pread`), `io_submit` (Linux AIO syscalls), `uring` (only if built with io_uring) | `posix` |
| `--queue-depth N` | queries in flight | 1 (one query at a time, like `boa`); PaCHash's own benchmark uses 128 |
| `--cached-io` | open the store without `O_DIRECT` | off: `O_DIRECT`, as in PaCHash's own benchmark |
| `--store-file PATH` | object store file, removed before and after each experiment (unless `--keep-store`) | `pachash_store.db` |

Fixed by the driver:

| Setting | Value |
|---|---|
| Object store | `PaCHashObjectStore<a>` with its default index (Elias-Fano); load factor ignored by PaCHash (always 100%) |
| Key hash | `MurmurHash64` of the 4-byte key, the same function as BOA's `HashFunction` in `boa.cpp`. PaCHash stores objects by this hash; a key hashing to 0 (reserved by PaCHash) stops the run |
| Value | 1 byte (`'a'`), like BOA's `char` value |
| Construction input | a copy of the keys in insertion order (PaCHash sorts its input in place; the original order is kept for the query generators, e.g. `latest`). The copy is made before the timer starts |
| `io_submit` queue depth | at most 63 (PaCHash limitation) |

All of these are written at the top of every results file (`PaCHash …` lines).

## Results file

Name: `pachash_<a>_<block length>_<io>_<queue depth>[_cached]_<key dist>_<query dists>_exp_<N>.txt`,
in the current directory, or `--log-file`.

- Construction: `Construction write` (sort by hash + write the blocks, with I/O details),
  `Construction index` (read the file back and build the in-memory index), then
  `End Insertions, time T` = whole construction, so it lines up with `boa`'s suspended-mode insertion time
  (which includes consolidation).
- Queries: as in `boa`. With `--queue-depth` > 1, a checkpoint is logged when the N-th query completes.
- End: `PaCHash blocks`, `PaCHash file bytes`, `PaCHash index bytes` (in-memory index),
  `PaCHash index bits per block`, and PaCHash's own `construction timer` (in nanoseconds).

## Notes

- The queue depth matters a lot: in a small test (200k keys, `O_DIRECT`), 20,000 uniform queries took
  about 3,000 ms one at a time and about 150 ms with `--io io_submit --queue-depth 32`. State the queue
  depth with every result.
- `O_DIRECT` needs a file system that supports it (not tmpfs).
