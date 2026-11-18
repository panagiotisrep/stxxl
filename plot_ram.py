#!/usr/bin/env python3
"""Maximum RAM per run (experiments/run_all.sh results).

usage: plot_ram.py OUTDIR RESULTS_DIR [RESULTS_DIR ...]
bars (log scale), per run:
  peak RSS      the whole process, from /usr/bin/time (includes the benchmark's key set, code and libraries)
  structure     competitors: peak RSS above the baseline taken after the keys are generated (the structure's memory);
                BOA / BOA-PV: "Max internal memory bytes" of the log (BOA's own counter, excludes the key set)
black line: the budget M of the run (params.txt).  writes OUTDIR/competitors_ram.{pdf,png} and competitors_ram.csv
"""
import csv
import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_competitors import NAMES, color, OTHER_STXXL  # noqa: E402

SKIP = ("leveldb_batch", "stxxl_map") + OTHER_STXXL


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    outdir = Path(args.pop(0))
    rows, budget_mb = {}, None
    for d in args:
        d = Path(d)
        with open(d / "summary.txt") as f:
            for line in list(f)[1:]:
                parts = line.split(None, 4)
                if len(parts) < 4 or parts[0] in SKIP or parts[0] not in NAMES:
                    continue
                run, status, secs, rss = parts[:4]
                mem = parts[4].strip() if len(parts) > 4 else "-"
                if status.startswith("FAILED") or status == "NO-BINARY":
                    continue
                m = re.match(r"([\d.]+)/([\d.]+)", mem)
                struct = float(m.group(1)) if m else None
                if struct is None and (d / f"{run}.txt").exists():  # BOA: its own counter
                    for line2 in open(d / f"{run}.txt"):
                        if line2.startswith("Max internal memory bytes:"):
                            struct = int(line2.split()[-1]) / 1048576
                rows[run] = dict(status=status, rss=float(rss), struct=struct, budget=float(m.group(2)) if m else None)
        pm = d / "params.txt"
        if pm.exists():
            for line in open(pm):
                m = re.match(r"M = (\d+) bytes", line)
                if m:
                    budget_mb = int(m.group(1)) / 1048576
    order = [r for r in NAMES if r in rows]
    outdir.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.size": 7, "axes.linewidth": 0.5, "pdf.fonttype": 42})
    fig, ax = plt.subplots(figsize=(6.2, 3.0), constrained_layout=True)
    w = 0.38
    for i, r in enumerate(order):
        d = rows[r]
        hatch = "////" if r.endswith("_a20") else None
        ax.bar(i - w / 2, d["rss"], width=w, color=color(r), alpha=0.45, hatch=hatch, edgecolor="white" if hatch else "none", linewidth=0.3)
        ax.text(i - w / 2, d["rss"], f"{d['rss']:.0f}", ha="center", va="bottom", fontsize=4.8, rotation=90)
        if d["struct"] is not None:
            ax.bar(i + w / 2, d["struct"], width=w, color=color(r), hatch=hatch, edgecolor="white" if hatch else "none", linewidth=0.3)
            ax.text(i + w / 2, d["struct"], f"{d['struct']:.0f}", ha="center", va="bottom", fontsize=4.8, rotation=90)
    if budget_mb:
        ax.axhline(budget_mb, color="black", linewidth=0.8, linestyle=":")
        ax.text(len(order) - 0.45, budget_mb, f"M = {budget_mb:.1f} MB", ha="right", va="bottom", fontsize=5.5)
    ax.set_yscale("log")
    ax.margins(y=0.35)
    ax.set_xticks(range(len(order)), [NAMES[r] for r in order], fontsize=5.5, rotation=40, ha="right", rotation_mode="anchor")
    ax.set_ylabel("MB (log scale)")
    ax.set_title("Maximum RAM per run", loc="left", fontsize=7, pad=2)
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(facecolor="#777777", alpha=0.45, label="peak RSS of the process"),
                       Patch(facecolor="#777777", label="structure (above key set; BOA: own counter)")],
              frameon=False, fontsize=5.5, loc="upper left")
    for ext in ("pdf", "png"):
        fig.savefig(outdir / f"competitors_ram.{ext}", dpi=200)
    plt.close(fig)
    with open(outdir / "competitors_ram.csv", "w", newline="") as fh:
        w_ = csv.writer(fh)
        w_.writerow(["run", "status", "peak_rss_mb", "structure_mb", "budget_mb"])
        for r in order:
            d = rows[r]
            w_.writerow([r, d["status"], d["rss"], d["struct"], d["budget"] if d["budget"] else budget_mb])


if __name__ == "__main__":
    main()
