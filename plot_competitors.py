#!/usr/bin/env python3
"""Plots for the competitor comparison (experiments/run_all.sh results: summary.txt and table.txt).

usage: plot_competitors.py OUTDIR --queries N RESULTS_DIR [RESULTS_DIR ...]
  --queries N   total number of queries of the interleaved run (batches x queries per batch), used for per-query values
  RESULTS_DIR   run_all.sh output folders; their runs are merged (a run name that appears twice: the later folder wins)

writes OUTDIR/competitors_comparison.{pdf,png}  (insertion time, data moved by insertions, time per query, data read per query)
       OUTDIR/competitors_insertions.{pdf,png}  (the first two panels only)
       OUTDIR/competitors_memory.{pdf,png}      (memory of the structure against the budget M + overhead + slack)
       OUTDIR/competitors.csv
"""
import csv
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

NAMES = {"boa": "BOA", "boa_pv": "BOA-PV", "boa_a20": "BOA α = 20", "boa_pv_a20": "BOA-PV α = 20",
         "leveldb": "LevelDB", "leveldb_batch": "LevelDB batch", "rocksdb": "RocksDB", "rocksdb_bloom": "RocksDB Bloom", "rocksdb_bloom_ram": "RocksDB Bloom 256 MiB",
         "leanstore": "LeanStore", "leanstore_ram": "LeanStore 4 GiB", "stxxl_map": "STXXL map", "stxxl_unordered_map": "STXXL unordered_map", "silt_combi": "SILT", "leveldb_lim2300m": "LevelDB 2.3 GB limit", "leveldb_lim2150m": "LevelDB 2.15 GB limit", "leanstore_6g": "LeanStore 6 GiB", "leanstore_12g": "LeanStore 12 GiB", "leanstore_3g": "LeanStore 3 GiB", "leanstore_1_5g": "LeanStore 1.5 GiB", "stxxl_map_ram": "STXXL B+-tree 10 GiB", "stxxl_map_l5g": "STXXL B+-tree 5 GiB", "stxxl_map_l2_5g": "STXXL B+-tree 2.5 GiB", "stxxl_map_l1_25g": "STXXL B+-tree 1.25 GiB", "silt_combi_8g": "SILT 8 GiB limit", "silt_combi_5g": "SILT 5 GiB limit", "silt_combi_3g": "SILT 3 GiB limit", "silt_combi_2g": "SILT 2 GiB limit", "leveldb_direct": "LevelDB direct I/O", "stxxl_map_l4g": "STXXL B+-tree 4 GiB", "stxxl_map_l3g": "STXXL B+-tree 3 GiB", "stxxl_map_l3_5g": "STXXL B+-tree 3.5 GiB"}
OTHER_STXXL = ("stxxl_map_ram", "stxxl_map_l5g", "stxxl_map_l3g", "stxxl_map_l3_5g", "stxxl_map_l2_5g", "stxxl_map_l1_25g")  # only the finished run with the least RAM (4 GiB) is plotted
ORDER = [r for r in NAMES if r not in ("leveldb_batch", "stxxl_map") + OTHER_STXXL]  # leveldb_batch and stxxl_map are not part of the comparison
GIB = 1024.0


def color(run):  # alpha = 20 has the colour of its variant (hatched in the bar plots)
    if run in ("boa", "boa_a20"):
        return "#4e79a7"
    if run in ("boa_pv", "boa_pv_a20"):
        return "#e15759"
    return "#8c8c8c"


BATCH = re.compile(r"Batch \d+ \| Insertions (\d+), Insertions time ([\d.]+) ms, Queries (\d+), Queries time ([\d.]+) ms")


def log_totals(path):
    """(insertions, insertion s, queries, query s) summed over the Batch lines of a results file"""
    ni = nq = 0
    ti = tq = 0.0
    try:
        for line in open(path):
            m = BATCH.match(line)
            if m:
                ni += int(m.group(1)); ti += float(m.group(2)) / 1e3; nq += int(m.group(3)); tq += float(m.group(4)) / 1e3
    except OSError:
        pass
    return ni, ti, nq, tq


def read_folder(d):
    rows = {}
    with open(os.path.join(d, "summary.txt")) as f:
        for line in list(f)[1:]:
            parts = line.split(None, 4)
            if len(parts) < 4:
                continue
            run, status, secs, rss = parts[:4]
            mem = parts[4].strip() if len(parts) > 4 else "-"
            rows[run] = dict(status=status, seconds=float(secs) if secs != "-" else 0, rss_mb=float(rss) if rss != "-" else 0, mem=mem)
    with open(os.path.join(d, "table.txt")) as f:
        for line in list(f)[1:]:
            parts = line.split()
            m = re.match(r"(\S+)\s+(insert|queries) \(il\)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)", line)
            if not m:
                continue
            run, phase = m.group(1), m.group(2)
            if run in rows:
                rows[run][phase] = dict(time=float(m.group(3)), read_mb=float(m.group(4)), write_mb=float(m.group(5)),
                                        reads_op=float(m.group(6)), kb_read_op=float(m.group(7)), ops=0)
    for run, r in rows.items():
        ni, ti, nq, tq = log_totals(os.path.join(d, run + ".txt"))
        if "insert" in r and ni:
            r["insert"].update(time=ti, ops=ni)
        if "queries" in r and nq:
            r["queries"].update(time=tq, ops=nq)
    return rows


def main():
    args = sys.argv[1:]
    if len(args) < 4 or "--queries" not in args:
        sys.exit(__doc__)  # --queries is only a fallback; counts come from the logs
    outdir = args.pop(0)
    i = args.index("--queries")
    nq = int(args[i + 1])
    del args[i:i + 2]
    runs = {}
    for d in args:
        runs.update(read_folder(d))
    os.makedirs(outdir, exist_ok=True)
    order = [r for r in ORDER if r in runs and runs[r]["status"] != "NO-BINARY"]
    # runs that I stopped by hand or that failed at once are not results
    order = [r for r in order if not (runs[r]["status"].startswith("FAILED") and "insert" not in runs[r])]
    plt.rcParams.update({"font.size": 7, "axes.linewidth": 0.5, "pdf.fonttype": 42})
    labels = [NAMES[r] for r in order]
    x = np.arange(len(order))

    def bars(ax, getter, ylabel, title, fmt="{:.3g}", log=False, timeout_label=True):
        top = 0
        for i, r in enumerate(order):
            d = runs[r]
            v = getter(d)
            if v is None:
                if d["status"] == "TIMEOUT" and timeout_label:
                    ax.text(i, 0.02 * ax.get_ylim()[1], "timeout (> %d s)" % d["seconds"], ha="center", va="bottom", fontsize=5, color="#444444", rotation=90)
                continue
            ax.bar(i, v, width=0.65, color=color(r), hatch="////" if r.endswith("_a20") else None, edgecolor="white" if r.endswith("_a20") else "none", linewidth=0.3)
            ax.text(i, v, fmt.format(v), ha="center", va="bottom", fontsize=5)
            top = max(top, v)
        ax.set_xticks(x, labels, fontsize=5.5, rotation=40, ha="right", rotation_mode="anchor")
        ax.set_ylabel(ylabel)
        ax.set_title(title, loc="left", fontsize=6.5, pad=2)
        if log:
            ax.set_yscale("log")
            ax.margins(y=0.4)
        else:
            ax.set_ylim(0, top * 1.18 if top else 1)

    def ph(name, key):
        return lambda d: d[name][key] if name in d else None

    panels = [
        (lambda d: ph("insert", "time")(d), "seconds", "Insertion time", "{:.0f}"),
        (lambda d: (d["insert"]["read_mb"] + d["insert"]["write_mb"]) / 1024 if "insert" in d else None, "GiB",
         "Data moved by insertions (read + written)", "{:.1f}"),
        (lambda d: d["queries"]["time"] * 1000 / (d["queries"]["ops"] or nq) if "queries" in d else None, "ms", "Time per query", "{:.3f}"),
        (lambda d: d["queries"]["kb_read_op"] if "queries" in d else None, "KiB", "Data read per query", "{:.0f}"),
    ]
    for name, sel, size in (("competitors_comparison", panels, (9.2, 2.9)), ("competitors_insertions", panels[:2], (5.4, 2.9))):
        fig, axes = plt.subplots(1, len(sel), figsize=size, constrained_layout=True)
        for ax, (g, yl, t, f) in zip(np.atleast_1d(axes), sel):
            bars(ax, g, yl, t, f, timeout_label=(t == "Insertion time"))
        for ext in ("pdf", "png"):
            fig.savefig(os.path.join(outdir, f"{name}.{ext}"), dpi=200)
        plt.close(fig)

    # memory: structure memory above the key set against its budget (M + overhead + slack)
    fig, ax = plt.subplots(figsize=(4.6, 2.9), constrained_layout=True)
    mem_runs = []
    for r in order:
        m = re.match(r"([\d.]+)/([\d.]+)", runs[r]["mem"])
        if m:
            mem_runs.append((r, float(m.group(1)), float(m.group(2))))
    for i, (r, used, budget) in enumerate(mem_runs):
        ax.bar(i, used, width=0.65, color=color(r), hatch="////" if r.endswith("_a20") else None, edgecolor="white" if r.endswith("_a20") else "none", linewidth=0.3)
        ax.plot([i - 0.4, i + 0.4], [budget, budget], color="black", linewidth=1.0)
        ax.text(i, used, f"{used:.0f}", ha="center", va="bottom", fontsize=5)
    ax.set_yscale("log")
    ax.set_xticks(range(len(mem_runs)), [NAMES[r] for r, _, _ in mem_runs], fontsize=5.5, rotation=40, ha="right", rotation_mode="anchor")
    ax.set_ylabel("MB (log scale)")
    ax.set_title("Memory of the structure (bars) and budget (lines)", loc="left", fontsize=6.5, pad=2)
    ax.margins(y=0.4)
    for ext in ("pdf", "png"):
        fig.savefig(os.path.join(outdir, f"competitors_memory.{ext}"), dpi=200)
    plt.close(fig)

    with open(os.path.join(outdir, "competitors.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["run", "status", "seconds", "peak_rss_mb", "insertion_s", "insert_read_mb", "insert_write_mb",
                    "query_s", "ms_per_query", "kib_read_per_query", "structure_mb/budget_mb"])
        for r in order:
            d = runs[r]
            i, q = d.get("insert"), d.get("queries")
            w.writerow([r, d["status"], d["seconds"], d["rss_mb"], i and i["time"], i and i["read_mb"], i and i["write_mb"],
                        q and q["time"], q and q["time"] * 1000 / (q["ops"] or nq), q and q["kb_read_op"], d["mem"]])


if __name__ == "__main__":
    main()
