#!/usr/bin/env python3
"""RAM footprint of the data structure per inserted element, over the number of elements.

usage: plot_footprint.py OUTDIR RESULTS_DIR [RESULTS_DIR ...]
One point per run (the peak is measured once per run): N = elements inserted in the run's Batch lines,
y = structure memory / N in bytes. Structure memory: competitors = peak RSS above the key set (summary.txt),
BOA / BOA-PV = "Max internal memory bytes" of their log. Runs that timed out / failed and leanstore_ram are left out.
writes OUTDIR/footprint_per_element.{pdf,png}, footprint_total.{pdf,png}, footprint.csv
"""
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paper_figures as pf  # noqa: E402
from paper_figures import plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402
from plot_competitors_batches import NAMES, STYLE  # noqa: E402

SKIP = ("leveldb_batch", "stxxl_map", "leanstore_ram", "rocksdb_bloom_ram", "stxxl_map_ram", "stxxl_map_l5g")


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    outdir = Path(args.pop(0))
    pts = {}  # run -> {N: bytes per element}
    for d in map(Path, args):
        for line in list(open(d / "summary.txt"))[1:]:
            p = line.split(None, 4)
            run = p[0]
            if len(p) < 4 or run in SKIP or run not in NAMES or p[1] != "ok":
                continue
            log = d / f"{run}.txt"
            n, struct = 0, None
            m = re.match(r"([\d.]+)/", p[4].strip()) if len(p) > 4 else None
            if m:
                struct = float(m.group(1)) * 1048576
            for l in open(log):
                mb = re.match(r"Batch \d+ \| Insertions (\d+),", l)
                if mb:
                    n += int(mb.group(1))
                if struct is None and l.startswith("Max internal memory bytes:"):
                    struct = int(l.split()[-1])
            if n and struct:
                pts.setdefault(run, {})[n] = struct
    outdir.mkdir(parents=True, exist_ok=True)
    pf.paper_style(7)
    for kind, ylabel, f in (("per_element", "RAM of the structure (bytes per element)", lambda x, v: v / x),
                            ("total", "RAM of the structure (MB)", lambda x, v: v / 1048576)):
        fig, ax = plt.subplots(figsize=(4.4, 2.6), constrained_layout=True)
        order = [r for r in NAMES if r in pts]
        seen = set()
        for r in order:
            xs = sorted(pts[r])
            c, ls, mk = STYLE[r]
            ax.plot(xs, [f(x, pts[r][x]) for x in xs], color=c, linestyle=ls, marker=mk, markersize=2.5, linewidth=0.8)
            for x in xs[-1:]:  # value label at the last point only (identical values of several lines are written once)
                v = f(x, pts[r][x])
                if (x, round(v, 3)) in seen:
                    continue
                seen.add((x, round(v, 3)))
                ax.annotate((f"{v:.0f}" if v >= 100 else f"{v:.3g}"), (x, v), textcoords="offset points", xytext=(3, 0), ha="left", va="center", fontsize=4.5, color=c)
        ax.set_xscale("log"); ax.set_yscale("log"); ax.margins(x=0.12)
        ax.set_xlabel("Inserted elements"); ax.set_ylabel(ylabel)
        ax.xaxis.set_major_formatter(FuncFormatter(pf.count_fmt))
        ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
        ax.legend([Line2D([], [], color=STYLE[r][0], linestyle=STYLE[r][1], marker=STYLE[r][2], markersize=2.5, linewidth=1.0) for r in order],
                  [NAMES[r] for r in order], frameon=False, fontsize=5.8, ncol=2, loc="lower left" if kind == "per_element" else "upper left")
        for ext in ("pdf", "png"):
            fig.savefig(outdir / f"footprint_{kind}.{ext}", dpi=300)
        plt.close(fig)
    with open(outdir / "footprint.csv", "w", newline="") as fh:
        w = csv.writer(fh); w.writerow(["system", "elements", "structure_bytes", "bytes_per_element"])
        for r in order:
            for x in sorted(pts[r]):
                w.writerow([NAMES[r], x, int(pts[r][x]), f"{pts[r][x] / x:.3f}"])
    


if __name__ == "__main__":
    main()
