# SplinterDB

Source: `extern/splinterdb`, https://github.com/vmware/splinterdb, commit `73f89d8` (`main`, no releases).
Compared with BOA's online mode, interleaved.
Shared behaviour (same data as BOA, results format, measurement notes): [../README.md](../README.md).

## Build

```bash
experiments/competitors/splinterdb/build.sh     # → build-competitors/splinterdb/splinterdb_bench
```

Needs `libaio-dev` and `libxxhash-dev` (`install_dependencies.sh`) and a C compiler: SplinterDB's own Makefile builds `libsplinterdb.a` (release mode, out of tree).

## Run

```bash
build-competitors/splinterdb/splinterdb_bench --size 10000000 --key-dist uniform --query-dist all
build-competitors/splinterdb/splinterdb_bench --interleaved --size 10000000 --insertions-per-batch 50000 --queries-per-batch 2000
experiments/competitors/splinterdb/run.sh       # full grid, see the variables at the top of the script
```

`splinterdb_bench --help` lists all options.

## Every SplinterDB setting

Everything not listed here is SplinterDB's default. The driver writes every setting in effect into each results file.

SplinterDB opens its database file with O_DIRECT. Keys are 4-byte big-endian, `default_data_config` (lexicographic).

| Option | SplinterDB setting | Default |
|---|---|---|
| `--cache-size B` | `splinterdb_config::cache_size` | 67108864 (SplinterDB has no default; its intro example uses 64 MiB) |
| `--disk-size B` | `splinterdb_config::disk_size`, fixed at creation | 256 GiB (sparse file) |
| `--use-log` | `splinterdb_config::use_log` | off (SplinterDB's default) |
| `--memtable-bg-threads N`, `--normal-bg-threads N` | background threads | 0 = SplinterDB's defaults |

The key is a 4-byte value, the value one byte (`a`), as in `boa.cpp`; the benchmark loops are in `../kv_bench.h`.
