#!/usr/bin/env python3
"""Writes experiment_description.txt into every results_comp* folder: for each run the exact parameters (command line, budget),
the outcome, why the experiment was done and whether / why it is a fair comparison.  results_comp500M_all gets the descriptions of all
500M runs.   usage: describe_runs.py [FOLDER ...]   (default: every results_comp* folder)"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEADER = """HOW THE COMPARISON IS SET UP (applies to every run below)
- Workload: the same key set for every system (uniform random keys, the same --key-dist for every run), inserted online in batches; after every batch
  of insertions a batch of point queries (uniform over the keys inserted so far) is run ("interleaved" mode). Batch sizes are in the commands.
- Memory budget M = (lambda + 3) * Pages * PageSize * Block = 8,781,824 bytes (lambda 64, 2 pages, 64 KiB blocks) = what BOA/BOA-PV use
  (BOA's default buffer for 64 KiB blocks). Equal-budget competitors get caches that add up to M (see each run). Their memory is MEASURED
  (peak RSS above the point where the key set has been generated) and compared with M + the system's fixed overhead + 4 MiB slack;
  runs above that are marked "over M" in summary.txt. Runs are never killed for using memory.
- I/O model: BOA uses STXXL with direct I/O (no OS page cache). RocksDB uses direct reads/writes; stock LevelDB has no direct I/O and SILT
  cannot fully bypass the page cache either (see their entries). Data moved for competitors comes from /proc/self/io (OS bytes);
  BOA's numbers are STXXL block counts, so I/O COUNTS of BOA and competitors are not comparable, bytes moved are.
- Everything ran on the same machine, one experiment at a time, pinned to CPUs 0,1, data on /var/tmp (NVMe, ext4). Single runs (no repetition).
- Timeouts: competitors get a timeout (10x the slower of BOA / BOA-PV for the first runs, later 2x, as stated per run). A timeout is reported as such.
- Larger-RAM / restrained-RAM variants were ADDED (never replacing) when an equal-budget run was too slow (budget raised) or faster than
  BOA / BOA-PV only because of much more RAM (budget restrained), so the effect of RAM on each competitor is visible.
"""

DESC = {
 "boa": ("Reference: BOA online (insert + query interleaved), routing-filter multiplier 1 (alpha = 1).",
         "Fair by construction: it defines the budget M (default buffer, direct I/O through STXXL, no OS page cache)."),
 "boa_pv": ("BOA-PV (the paper's variant, --boa-pv) online, alpha = 1.",
            "Fair: same budget M and I/O model as BOA. Run element is 16 bytes instead of 13, so its default buffer holds fewer elements."),
 "boa_a20": ("BOA online with routing-filter multiplier 0.05 (alpha = 20): a smaller filter, cheaper insertions, more reads per query.",
             "Fair w.r.t. RAM (same M). It is a different operating point of BOA's own parameter, not a competitor; shown dashed next to alpha = 1 so both are visible."),
 "boa_pv_a20": ("BOA-PV online with alpha = 20.", "As boa_a20, for the variant."),
 "leveldb": ("Stock LevelDB 1.23 (LSM tree), one Put per key, as the standard LSM baseline. Memtable = M/4, block cache = M/2, 64 KiB blocks, no Bloom filter (LevelDB keeps filters outside its cache without a bound).",
             "NOT fully fair: LevelDB has no direct I/O, it memory-maps its tables and relies on the OS page cache, so its real RAM use is far above M (~300 MB above the key set against a ~13 MB allowance) and its queries are answered from cache (almost no reads). Numbers favour LevelDB, especially for queries. Marked 'over M'."),
 "leveldb_batch": ("LevelDB with 1000 keys per WriteBatch (cheaper log handling).", "Same caveats as leveldb; not part of the interleaved comparison (skipped later)."),
 "leveldb_batch_then": ("LevelDB, WriteBatch, static insert-then-query.", "Same caveats as leveldb."),
 "leveldb_lim2300m": ("LevelDB as 'leveldb', but under a cgroup memory limit of 2300 MB (key set 1.9 GB + ~300 MB) to restrain the page cache.",
                      "Added because stock LevelDB was faster than BOA-PV with ~25x the budget. The limit did not slow it (clean file pages are reclaimed freely), so it is not a real restriction; kept as a record."),
 "leveldb_lim2150m": ("As leveldb_lim2300m with 2150 MB (key set + ~150 MB).", "Same: did not bind; kept as a record."),
 "leveldb_direct": ("LevelDB with O_DIRECT on its table (.ldb) files via a custom leveldb::Env (experiments/competitors/leveldb/direct_env.h), otherwise as 'leveldb'.",
                    "The fairest LevelDB run: same I/O model as BOA (no page cache for tables) and memory at its allowance. Caveats: the write-ahead log and MANIFEST stay buffered (small sequential appends); the Env is my own implementation, checked only by a 2M smoke test and by the run completing."),
 "rocksdb": ("RocksDB, direct reads/writes, write buffer = M/4, block cache = M/2 holding index and filter blocks, 1 background job, 64 KiB blocks, no Bloom filter.",
             "Fair: direct I/O like BOA and the cache accounts for all index data. Its structure memory is a few MB above M + overhead + slack (marked 'over M' when it exceeds the computed allowance)."),
 "rocksdb_bloom": ("RocksDB as 'rocksdb' with 10-bit Bloom filters.",
                   "Partly fair: the filters live in the (M/2) block cache but RocksDB still used ~130 MB (marked over M); filters thrash the tiny cache, which is why queries read far more. Shows the cost of filters at this budget."),
 "rocksdb_bloom_ram": ("RocksDB with Bloom filters and a 256 MiB block cache.", "Larger-RAM variant (not equal budget): shows how much cache RocksDB Bloom needs."),
 "rocksdb_bulk": ("RocksDB bulk-load mode, static.", "Not part of the interleaved comparison."),
 "rocksdb_bulk_il": ("RocksDB bulk-load mode, interleaved.", "Not part of the main comparison."),
 "leanstore": ("LeanStore B-tree with a buffer pool of M, no WAL / transactions (BOA has neither), one worker thread, 4-byte keys, 1-byte values.",
               "Fair on RAM (pool = M; fixed overhead measured separately). Random B-tree inserts need a leaf read and write each, so it is very slow; it timed out in the large runs (reported as timeout)."),
 "leanstore_ram": ("LeanStore with a 4 GiB pool.", "NOT equal budget (about 500x M): at 100M keys the whole tree fits in RAM, so it does no I/O. A reference for 'what RAM buys', not a competitor."),
 "leanstore_6g": ("LeanStore with a 6 GiB pool at 500M keys.", "Larger-RAM run added after the equal-budget run timed out; the tree is bigger than the pool so it still does I/O. Not equal budget."),
 "leanstore_12g": ("LeanStore with a 12 GiB pool.", "Larger-RAM run (added only if the 6 GiB run timed out). Not equal budget."),
 "leanstore_3g": ("LeanStore with a 3 GiB pool.", "Restrained-RAM variant (added only if the 6 GiB run was faster than BOA-PV). Not equal budget."),
 "leanstore_1_5g": ("LeanStore with a 1.5 GiB pool.", "Restrained-RAM variant. Not equal budget."),
 "stxxl_map": ("STXXL B+-tree (stxxl::map), node cache M/2 + leaf cache M/2, 32 KiB nodes.", "Fair on RAM, but extremely slow at this budget (4 leaf blocks); skipped in the large runs, shown only where it finished."),
 "stxxl_unordered_map": ("STXXL unordered_map (hash map), buffer M/2, block cache M/2.", "Fair on RAM; it failed at start in the 20M run and was skipped (suspected problem, to be examined)."),
 "stxxl_map_ram": ("STXXL B+-tree with a 10 GiB leaf cache + 512 MiB node cache at 500M keys.",
                   "NOT equal budget (about 1000x M): it holds the whole tree in RAM and does no disk I/O during the run. Added so the B+-tree can finish at 500M; shows what RAM it needs."),
 "stxxl_map_l5g": ("STXXL B+-tree, 5 GiB leaf cache.", "Restrained variant after the 10 GiB run beat BOA-PV: still holds the whole tree (no I/O), so it is not yet a restriction. Not equal budget."),
 "stxxl_map_l4g": ("STXXL B+-tree, 4 GiB leaf cache.", "Added to find where the tree stops fitting (between 2.5 and 4 GiB); still faster than BOA-PV with no I/O. Not equal budget."),
 "stxxl_map_l3g": ("STXXL B+-tree, 3 GiB leaf cache.", "Added to bracket the cache size between 2.5 and 4 GiB. Not equal budget."),
 "stxxl_map_l3_5g": ("STXXL B+-tree, 3.5 GiB leaf cache.", "Added to find where the cliff between 3 GiB (timeout) and 4 GiB (finishes) lies. Not equal budget."),
 "stxxl_map_l2_5g": ("STXXL B+-tree, 2.5 GiB leaf cache.", "Restrained variant: once the tree outgrew the cache (after ~220M keys, batch 23) random inserts became I/O bound and the run timed out. Not equal budget."),
 "stxxl_map_l1_25g": ("STXXL B+-tree, 1.25 GiB leaf cache.", "Planned only if 2.5 GiB had finished faster than BOA-PV; not run."),
 "silt_combi": ("SILT combined store (log store -> hash stores -> sorted store) with SILT's own test configuration, online interleaved.",
                "NOT equal budget: SILT has no memory setting; its anonymous memory grew to ~45-55 bytes per key (4.3 GB above the key set at 100M keys, 13.7 GB peak RSS). Reported with its measured RAM. Not run at 500M (it would need ~65 GB; the machine has 31 GB)."),
 "silt_combi_2g": ("SILT under a cgroup memory limit of 2 GiB at 100M keys.", "Restrained variant: killed by the OOM killer after 3 batches; shows SILT cannot run below its natural memory use."),
 "silt_combi_8g": ("SILT under a cgroup memory limit of 8 GiB at 500M keys.", "Restrained variant: killed by the OOM killer after 10 batches (100M keys); SILT's memory grows with the number of keys."),
 "silt_combi_5g": ("SILT under 5 GiB.", "Only run if the 8 GiB run had finished faster than BOA-PV; not run."),
 "silt_combi_3g": ("SILT under 3 GiB.", "Only run if the 5 GiB run had finished faster than BOA-PV; not run."),
 "silt_sorted": ("SILT static sorted store.", "Static (insert-then-query); not part of the interleaved comparison."),
}

FOLDER_NOTE = {
 "results_comp20M_boa_failed": "First attempt at 20M; the BOA runs failed (BOA rejected a per-run query distribution in interleaved mode). Kept for the record; the valid runs are in results_comp20M.",
 "results_comp20M_a20": "alpha = 20 runs of BOA and BOA-PV at 20M, same workload as results_comp20M.",
 "results_comp20M_ram": "RocksDB Bloom with a 256 MiB cache at 20M (larger-RAM variant).",
 "results_comp2M": "Small test (2M keys, 50,000 insertions + 2,000 queries per batch, no timeout) to see how each system behaves before the larger sizes.",
 "results_comp100M": "100M keys, 10M insertions + 1000 queries per batch, same parameters as 20M. leanstore_ram is a 4 GiB (not equal budget) reference.",
 "results_comp100M_silt_limited": "SILT at 100M under a 2 GiB cgroup limit (how it behaves with restricted RAM).",
}


def parse_params(folder):
    cmds, mem, tmo, m_line = {}, {}, {}, ""
    p = folder / "params.txt"
    if not p.exists():
        return cmds, mem, tmo, m_line
    for line in p.read_text().splitlines():
        if line.startswith("M = "):
            m_line = line
        m = re.match(r"^(\S+): (taskset .*)$", line)
        if m:
            cmds[m.group(1)] = m.group(2)
            continue
        m = re.match(r"^  (\S+) memory: (.*)$", line)
        if m:
            mem[m.group(1)] = m.group(2)
        m = re.match(r"^  (\S+): (timeout .*)$", line)
        if m:
            tmo[m.group(1)] = m.group(2)
    return cmds, mem, tmo, m_line


def parse_summary(folder):
    rows = {}
    p = folder / "summary.txt"
    if p.exists():
        for line in p.read_text().splitlines()[1:]:
            parts = line.split(None, 4)
            if len(parts) >= 4:
                rows[parts[0]] = parts[1:]
    return rows


def size_info(cmd):
    g = lambda k: (re.search(k + r" (\d+)", cmd) or [None, "?"])[1]
    return g("--size"), g("--insertions-per-batch"), g("--queries-per-batch")


def describe(folder, extra_note=""):
    cmds, mem, tmo, m_line = parse_params(folder)
    rows = parse_summary(folder)
    out = [f"EXPERIMENT DESCRIPTION  -  folder {folder.name}", ""]
    note = FOLDER_NOTE.get(folder.name, extra_note)
    if note:
        out += [note, ""]
    out += [HEADER, m_line, ""]
    for run in cmds:
        why, fair = DESC.get(run, ("(no description written for this run name)", ""))
        n, bi, bq = size_info(cmds[run])
        out += ["=" * 100, f"RUN {run}", f"Size {n} keys, {bi} insertions + {bq} queries per batch",
                f"Why: {why}", f"Fair?: {fair}", f"Command: {cmds[run]}"]
        if run in tmo:
            out.append(f"Timeout: {tmo[run]}")
        if run in mem:
            out.append(f"Memory check: {mem[run]}")
        if run in rows:
            st, secs, rss = rows[run][:3]
            out.append(f"Outcome: status {st}, {secs} s total, peak RSS {rss} MB" + (f", structure/budget MB: {rows[run][3]}" if len(rows[run]) > 3 else ""))
        else:
            out.append("Outcome: no result recorded in this folder (still running or interrupted)")
        out.append("")
    return "\n".join(out) + "\n"


def main():
    folders = [Path(a) for a in sys.argv[1:]] or sorted(p for p in ROOT.glob("results_comp*") if p.is_dir())
    for f in folders:
        if f.name == "results_comp500M_all":
            continue
        (f / "experiment_description.txt").write_text(describe(f))
        print("wrote", f / "experiment_description.txt")
    allf = ROOT / "results_comp500M_all"
    if allf.exists() and not sys.argv[1:]:
        parts = [describe(f) for f in sorted(ROOT.glob("results_comp500M*")) if f.is_dir() and f.name != "results_comp500M_all" and (f / "params.txt").exists()]
        (allf / "experiment_description.txt").write_text(
            "EXPERIMENT DESCRIPTION - all 500M runs merged in this folder (one section per source folder; "
            "the general setup text repeats at the start of each section)\n\n" + "\n\n".join(parts))
        print("wrote", allf / "experiment_description.txt")


if __name__ == "__main__":
    main()
