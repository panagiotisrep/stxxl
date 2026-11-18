#!/usr/bin/env python3
"""Per-batch plots of the competitor comparison, in the style of the other paper figures (paper_figures.py):
row 1 = the insertions of each batch, row 2 = the queries of each batch, over the number of inserted elements.
Columns: throughput and data moved per operation. I/O wait is not plotted and I/O counts are left out on purpose: BOA counts STXXL block
requests, the competitors only syscalls, which are not comparable.

usage: plot_competitors_batches.py OUTDIR [--batch-group N] [--width W] [--exclude RUN,RUN] RESULTS_DIR [RESULTS_DIR ...]
  RESULTS_DIR  run_all.sh output folders (the <run>.txt files with Batch lines); a run in several folders: the later one wins
writes OUTDIR/competitors_batches.{pdf,png,csv}
"""
import csv
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paper_figures as pf  # noqa: E402  (style, count formatter, log-axis ticks)
from paper_figures import plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, MaxNLocator, NullLocator  # noqa: E402

NAMES = {"boa": "BOA", "boa_pv": "BOA-PV", "boa_a20": "BOA α = 20", "boa_pv_a20": "BOA-PV α = 20",
         "leveldb": "LevelDB", "leveldb_batch": "LevelDB batch", "rocksdb": "RocksDB", "rocksdb_bloom": "RocksDB Bloom", "rocksdb_bloom_ram": "RocksDB Bloom 256 MiB",
         "leanstore": "LeanStore", "leanstore_ram": "LeanStore 4 GiB", "stxxl_map": "STXXL map", "stxxl_unordered_map": "STXXL unordered_map", "silt_combi": "SILT", "leveldb_lim2300m": "LevelDB 2.3 GB limit", "leveldb_lim2150m": "LevelDB 2.15 GB limit", "leanstore_6g": "LeanStore 6 GiB", "leanstore_12g": "LeanStore 12 GiB", "leanstore_3g": "LeanStore 3 GiB", "leanstore_1_5g": "LeanStore 1.5 GiB", "stxxl_map_ram": "STXXL B+-tree 10 GiB", "stxxl_map_l5g": "STXXL B+-tree 5 GiB", "stxxl_map_l2_5g": "STXXL B+-tree 2.5 GiB", "stxxl_map_l1_25g": "STXXL B+-tree 1.25 GiB", "silt_combi_8g": "SILT 8 GiB limit", "silt_combi_5g": "SILT 5 GiB limit", "silt_combi_3g": "SILT 3 GiB limit", "silt_combi_2g": "SILT 2 GiB limit", "leveldb_direct": "LevelDB direct I/O", "stxxl_map_l4g": "STXXL B+-tree 4 GiB", "stxxl_map_l3g": "STXXL B+-tree 3 GiB", "stxxl_map_l3_5g": "STXXL B+-tree 3.5 GiB"}
STYLE = {  # colour, linestyle, marker
    "boa": ("#4e79a7", "-", "o"), "boa_pv": ("#e15759", "-", "o"),
    "boa_a20": ("#4e79a7", "--", "o"), "boa_pv_a20": ("#e15759", "--", "o"),  # same colour as BOA / BOA-PV, dashed
    "leveldb": ("#59a14f", "-", "s"), "leveldb_batch": ("#8cd17d", "--", "s"),
    "rocksdb": ("#f28e2b", "-", "^"), "rocksdb_bloom": ("#b6992d", "--", "^"), "rocksdb_bloom_ram": ("#b6992d", "-", "^"),
    "leanstore": ("#b07aa1", "-", "D"), "leanstore_ram": ("#b07aa1", "--", "D"), "stxxl_map": ("#76b7b2", "-", "v"),
    "stxxl_unordered_map": ("#9c755f", "-", "P"), "silt_combi": ("#444444", "-", "X"),
    "leveldb_lim2300m": ('#59a14f', '--', 's'),
    "leveldb_lim2150m": ('#2e7d32', ':', 's'),
    "leanstore_6g": ('#b07aa1', '-', 'D'),
    "leanstore_12g": ('#8e5a82', '--', 'D'),
    "leanstore_3g": ('#c9a0bf', ':', 'D'),
    "leanstore_1_5g": ('#d9bcd2', '-.', 'D'),
    "stxxl_map_ram": ('#76b7b2', '-', 'v'),
    "stxxl_map_l5g": ('#4f9a95', '--', 'v'),
    "stxxl_map_l2_5g": ('#3a7c78', ':', 'v'),
    "stxxl_map_l1_25g": ('#2a5d5a', '-.', 'v'),
    "silt_combi_8g": ('#444444', '--', 'X'),
    "silt_combi_5g": ('#666666', ':', 'X'),
    "silt_combi_3g": ('#888888', '-.', 'X'),
    "silt_combi_2g": ('#222222', '-', 'X'),
    "leveldb_direct": ('#1b5e20', '-', 's'),
    "stxxl_map_l4g": ('#5aa8a3', '-.', 'v'),
    "stxxl_map_l3g": ('#2f6f6b', '--', 'v'),
    "stxxl_map_l3_5g": ('#3f8a85', ':', 'v'),
}
LINE = re.compile(r"Batch \d+ \| Insertions (\d+), Insertions time ([\d.]+) ms, Queries (\d+), Queries time ([\d.]+) ms.*?"
                  r"insert details: \[(.*?)\], query details: \[(.*?)\]")


def num(key, text):
    m = re.search(key + r" (\d+(?:\.\d+)?)", text)
    return float(m.group(1)) if m else None


def moved(details):
    """bytes read + written, and I/O wait seconds (None if not logged)"""
    rd = num("OS bytes read", details)
    if rd is not None:  # competitor driver: /proc/self/io
        return rd + num("OS bytes written", details), None
    rd, wr = num("bytes read", details), num("bytes written", details)
    wait = (num("read wait time", details) or 0) + (num("write wait time", details) or 0)
    return rd + wr, wait / 1e3


def parse(path):
    batches = []
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        ni, ti, nq, tq = int(m.group(1)), float(m.group(2)) / 1e3, int(m.group(3)), float(m.group(4)) / 1e3
        bi, wi = moved(m.group(5))
        bq, wq = moved(m.group(6))
        batches.append(dict(n_ins=ni, t_ins=ti, n_q=nq, t_q=tq, b_ins=bi, b_q=bq, w_ins=wi, w_q=wq))
    return batches


def group(batches, n):
    if n <= 1 or not batches:
        return batches
    out = []
    for i in range(0, len(batches), n):
        g = batches[i:i + n]
        d = {k: sum(b[k] for b in g) if all(b[k] is not None for b in g) else None for k in g[0]}
        out.append(d)
    return out


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    outdir = Path(args.pop(0))
    n_group, width = 1, 6.2
    for flag in ("--batch-group", "--width"):
        if flag in args:
            i = args.index(flag)
            val = args[i + 1]
            del args[i:i + 2]
            if flag == "--batch-group":
                n_group = int(val)
            else:
                width = float(val)
    skip = set()
    if "--exclude" in args:  # --exclude run1,run2: leave these runs out of the plot
        i = args.index("--exclude")
        skip = set(args[i + 1].split(","))
        del args[i:i + 2]
    runs = {}
    for d in args:
        for run in [r for r in NAMES if r not in ("leveldb_batch", "stxxl_map") and r not in skip]:  # leveldb_batch is not part of the interleaved comparison
            f = Path(d) / f"{run}.txt"
            if f.exists():
                b = parse(f)
                if b:
                    runs[run] = b
    order = [r for r in NAMES if r in runs]
    outdir.mkdir(parents=True, exist_ok=True)
    pf.paper_style(7)

    cols = [
        ("Throughput", "M elem./s", "queries/s",
         lambda b: b["n_ins"] / b["t_ins"] / 1e6, lambda b: b["n_q"] / b["t_q"]),
        ("Data moved", "KiB per ins.", "KiB per query",
         lambda b: b["b_ins"] / b["n_ins"] / 1024, lambda b: b["b_q"] / b["n_q"] / 1024),
    ]
    fig, axes = plt.subplots(2, len(cols), sharex=True, figsize=(width * 0.72, 0.46 * width), constrained_layout=True)
    csv_rows = []
    for row, phase in enumerate(("insertions", "queries")):
        for j, (title, uins, uqry, fi, fq) in enumerate(cols):
            ax, f = axes[row, j], (fi if row == 0 else fq)
            for run in order:
                g = group(runs[run], n_group)
                y = [f(b) for b in g]
                if any(v is None for v in y):
                    continue  # no such counter for this system
                x = np.cumsum([b["n_ins"] for b in g])
                c, ls, mk = STYLE[run]
                ax.plot(x, y, color=c, linestyle=ls, marker=mk, markersize=1.3, linewidth=0.8)
                csv_rows += [[phase, title, uins if row == 0 else uqry, NAMES[run], int(xi), yi] for xi, yi in zip(x, y)]
            if row == 0:
                ax.set_title(title, loc="left", fontsize=7.5, pad=2)
            ax.set_ylabel(uins if row == 0 else uqry, fontsize=6.5)
            ys = [yy for line in ax.get_lines() for yy in line.get_ydata() if yy > 0]
            if ys and max(ys) / min(ys) > 3:  # log axis only for a wide data range (see paper_figures.fig_block_sizes)
                ax.set_yscale("log")
                ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 3.0), numticks=20))
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                ax.yaxis.set_minor_locator(NullLocator())
            else:
                ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            ax.xaxis.set_major_formatter(FuncFormatter(pf.count_fmt))
            ax.tick_params(axis="x", labelsize=6)
            if row == 1:
                ax.set_xlabel("Inserted elements", fontsize=7)
    handles = [Line2D([], [], color=STYLE[r][0], linestyle=STYLE[r][1], marker=STYLE[r][2], markersize=1.6, linewidth=1.1) for r in order]
    fig.legend(handles, [NAMES[r] for r in order], loc="outside upper center", ncol=min(len(order), 5), handlelength=1.6,
               columnspacing=1.0, fontsize=6.5)
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"competitors_batches.{ext}", dpi=300)
    plt.close(fig)
    with open(outdir / "competitors_batches.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["phase", "metric", "unit", "system", "elements_inserted", "value"])
        w.writerows(csv_rows)
    print("wrote", outdir / "competitors_batches.png")


if __name__ == "__main__":
    main()
