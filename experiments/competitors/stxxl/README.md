# STXXL map and unordered_map

Source: this repository's STXXL (`include/stxxl/bits/containers/map.h`, `unordered_map.h`), the same code
and build as `boa`. Two drivers from one source (`stxxl_bench.cpp`):

| Binary | Structure | Compare with BOA's |
|---|---|---|
| `stxxl_map_bench` | `stxxl::map` (external B+-tree) | online mode, interleaved |
| `stxxl_unordered_map_bench` | `stxxl::unordered_map` (external hash map) | online mode, interleaved |

Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/stxxl/build.sh     # → build-competitors/stxxl/stxxl_map_bench, stxxl_unordered_map_bench
```

- Builds STXXL in its own build directory (not your `boa` build), as a Release build with exactly the
  same compiler flags as `boa`.
- STXXL must be the top-level CMake project (it uses `CMAKE_SOURCE_DIR`), and its include/library
  variables are set late in its CMakeLists. `build.sh` adds the two driver targets through CMake's
  `CMAKE_PROJECT_stxxl_INCLUDE` hook (`inject.cmake`), deferred until STXXL's CMakeLists has been processed
  (`cmake_language(DEFER)`, CMake ≥ 3.19). No STXXL CMake file is modified.
- STXXL's own submodules (`extern/tlx`, `extern/ips2ra`, `extern/util`) are fetched over https if they
  are not checked out yet (some of the repository's `.gitmodules` URLs are `git@github.com:`).

## Run

```bash
build-competitors/stxxl/stxxl_map_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/stxxl/stxxl_unordered_map_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/stxxl/run.sh       # full grid, see the variables at the top of the script
```

`--help` lists each binary's options. **Use the same STXXL disk configuration as for `boa`**
(`.stxxl` file in the working directory, or `STXXL_CONFIG` in `run.sh`).

## Every setting

**Compile time** (template arguments; `build.sh` environment variables → `inject.cmake`). Defaults are the
values in `examples/containers/map1.cpp` and `unordered_map1.cpp`:

| Variable | Default | Meaning |
|---|---|---|
| `STXXL_MAP_NODE_BLOCK_SIZE` | 32768 | `stxxl::map` node block size in bytes |
| `STXXL_MAP_LEAF_BLOCK_SIZE` | 32768 | `stxxl::map` leaf block size in bytes |
| `STXXL_UM_SUB_BLOCK_SIZE` | 8192 | `stxxl::unordered_map` sub-block size in bytes |
| `STXXL_UM_SUB_BLOCKS_PER_BLOCK` | 256 | `stxxl::unordered_map` sub-blocks per block |

**Run time** (defaults again from `map1.cpp` / `unordered_map1.cpp`):

| Option | Binary | Meaning | Default |
|---|---|---|---|
| `--node-cache B` | map | node cache in bytes | 33554432 (1024 node blocks) |
| `--leaf-cache B` | map | leaf cache in bytes | 33554432 (1024 leaf blocks) |
| `--initial-buckets N` | unordered_map | initial number of buckets | 10000 |
| `--buffer-size B` | unordered_map | internal-memory buffer in bytes | 1073741824 (1 GiB) |

Fixed by the driver:

| Setting | Value |
|---|---|
| Key / value | `uint32_t` key, `char` value `'a'` (as in `boa`) |
| `stxxl::map` comparator | `a < b`, sentinel `max_value()` = `UINT32_MAX` (never a key). `map1.cpp` uses a `>` comparator on `int` instead; this one is valid for every `uint32_t` key below the sentinel |
| `stxxl::unordered_map` hash / comparator | as in `unordered_map1.cpp`: `key * 2654435761u`, `a < b` |
| Structure | a fresh one per experiment (`--repeat`) |

All of these are written at the top of every results file (`STXXL …` lines).

## Results file

Name: `stxxl_map_<node block>_<node cache>_<leaf block>_<leaf cache>_<key dist>_<query dists>_exp_<N>.txt`,
`stxxl_unordered_map_<sub-block>_<sub-blocks per block>_<initial buckets>_<buffer>_<key dist>_<query dists>_exp_<N>.txt`
(interleaved: `…_interleaved_<params>_<ins>_<queries>_exp_<N>.txt`), in the current directory, or `--log-file`.

STXXL-specific lines: `disk bytes` (external memory currently allocated by STXXL) at each insertion
checkpoint and batch, and at the end `Max internal memory bytes` (caches / buffer, as in `map1.cpp` and
`unordered_map1.cpp`), `STXXL allocated bytes`, and STXXL's own I/O statistics for the whole experiment
(the same statistics `boa` logs).

## Notes

- With the `unordered_map1.cpp` defaults (10,000 initial buckets, 1 GiB buffer), inserting 300k keys
  took about 228 s in a test on WSL, against about 0.15 s for `stxxl::map`. These are the existing
  settings, not tuned here; state them with any result.
- The memory budgets differ a lot between structures (BOA's default buffer is one 32 KiB block; `map`
  uses 64 MiB of caches; `unordered_map` a 1 GiB buffer). Set them deliberately when comparing.
