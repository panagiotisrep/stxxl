# FASTER

Source: `extern/faster`, https://github.com/microsoft/FASTER, release `v2.6.5` (`e25782f`).
Compared with BOA's online mode, interleaved.
Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/faster/build.sh     # → build-competitors/faster/faster_bench
```

Needs `libaio-dev`, `uuid-dev`, `libtbb-dev`. FASTER's own CMake project downloads googletest at configure time, so `CMakeLists.txt` compiles its library sources (`cc/src`) directly.

## Run

```bash
build-competitors/faster/faster_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/faster/faster_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/faster/run.sh       # full grid, see the variables at the top of the script
```

`faster_bench --help` lists all options.

## Every FASTER setting

Everything not listed here is FASTER's default. The driver writes every setting in effect into each results file.

FASTER keeps its whole hash index in memory and needs at least a few 32 MiB log pages, so it cannot run within M: it is run as a larger-RAM reference. Keys are 4-byte integers, values 1 byte (records are 8-byte aligned).

| Option | FASTER setting | Default |
|---|---|---|
| `--table-size N` | hash index buckets (power of two, 64 bytes each, always in memory) | 1048576 (the benchmark's choice; FASTER has no default) |
| `--log-size B` | in-memory part of the hybrid log (multiple of the 32 MiB log page) | 536870912 (the benchmark's choice) |
| `--mutable-fraction F` | `log_mutable_fraction` | 0.9 (FasterKv's default) |

The key is a 4-byte value, the value one byte (`a`), as in `boa.cpp`; the benchmark loops are in `../kv_bench.h`.
