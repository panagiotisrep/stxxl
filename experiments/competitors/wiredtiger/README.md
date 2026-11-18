# WiredTiger

Source: `extern/wiredtiger`, https://github.com/wiredtiger/wiredtiger, tag `mongodb-9.0.8` (`0e87e9f`).
Compared with BOA's online mode, interleaved.
Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/wiredtiger/build.sh     # → build-competitors/wiredtiger/wiredtiger_bench
```

Needs CMake ≥ 3.21, a C and a C++17 compiler. WiredTiger is built on its own (static library, no compressors, Python API, tests) into `<build dir>/wiredtiger-lib`.

## Run

```bash
build-competitors/wiredtiger/wiredtiger_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/wiredtiger/wiredtiger_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/wiredtiger/run.sh       # full grid, see the variables at the top of the script
```

`wiredtiger_bench --help` lists all options.

## Every WiredTiger setting

Everything not listed here is WiredTiger's default. The driver writes every setting in effect into each results file.

Table `key_format=I` (uint32), `value_format=B`. No compression, no write-ahead log (WiredTiger's defaults).

| Option | WiredTiger setting | Default |
|---|---|---|
| `--cache-size B` | connection `cache_size` | 104857600 (100 MB, WiredTiger's default) |
| `--leaf-page-max B` | table `leaf_page_max` | WiredTiger's (32 KB) |
| `--internal-page-max B` | table `internal_page_max` | WiredTiger's (4 KB) |
| `--allocation-size B` | table `allocation_size` | WiredTiger's (4 KB) |
| `--direct-io` | connection `direct_io=[data]` | off |
| `--connection-config STR` | appended to the `wiredtiger_open` configuration | none |

The key is a 4-byte value, the value one byte (`a`), as in `boa.cpp`; the benchmark loops are in `../kv_bench.h`.
