#!/usr/bin/env python3
"""Paper figures comparing BOA and BOA-PV (colour = implementation, line style/marker = routing
filter multiplier, written as alpha). plot_results.py runs this for every results folder; it can also be run alone:

    .venv/bin/python paper_figures.py results12 [-o plots_dir] [--width 3.3] [--format pdf png]

Figures (written to <plots folder>/paper_plots_mult/, each with a .csv of its data):
  pv_vs_boa_insertion_throughput   insertion throughput over the number of inserted elements
  pv_vs_boa_query_throughput_*     query throughput over the number of queries, one panel per --query-dist
  pv_vs_boa_tradeoff               insertion vs query throughput, one point per alpha
  pv_vs_boa_costs_over_time   throughput, I/Os and data moved per operation over the number of inserted elements / queries
  pv_vs_boa_costs_over_time_wide  the same in 2 rows x 3 columns (insertions row, queries row)
  pv_vs_boa_costs                  throughput, I/Os, data transferred and I/O wait time per operation over alpha
Both implementations must be present in the folder, otherwise the figures are skipped.
"""
import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import matplotlib  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, MaxNLocator, NullFormatter, NullLocator  # noqa: E402

import plot_results  # noqa: E402  (log parser + batching)

pc = sys.modules[__name__]  # the figure functions below are written as pc.<helper>

VARIANTS = {"BOA": "BOA", "PV": "BOA-PV"}  # variant name in the log -> panel title
INK, INK2, GRID = "#111111", "#444444", "#dddddd"
MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]
LINESTYLES = ["-", "--", "-.", ":"]


def paper_style(font_size=8):
    """Compact style for a one-column paper: small fonts, embedded TrueType, no frame clutter."""
    plt.rcParams.update({
        "font.family": "serif", "mathtext.fontset": "stix",
        "font.size": font_size, "axes.labelsize": font_size, "axes.titlesize": font_size,
        "xtick.labelsize": font_size - 1, "ytick.labelsize": font_size - 1,
        "legend.fontsize": font_size - 1,
        "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK2, "ytick.color": INK2,
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.linewidth": 0.6, "xtick.major.width": 0.6, "ytick.major.width": 0.6,
        "xtick.major.size": 2.5, "ytick.major.size": 2.5,
        "axes.grid": True, "axes.grid.axis": "y", "grid.color": GRID, "grid.linewidth": 0.5,
        "axes.axisbelow": True, "legend.frameon": False,
        "lines.linewidth": 1.1, "lines.markersize": 3.5,
        "pdf.fonttype": 42, "ps.fonttype": 42,  # embedded TrueType text, accepted by publishers
        "savefig.bbox": "tight", "savefig.pad_inches": 0.02,
    })


def base_args(description):
    ap = argparse.ArgumentParser(description=description,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results_dir", type=Path, help="results folder with the boa_*.txt logs (e.g. results12)")
    ap.add_argument("-o", "--outdir", type=Path,
                    help="plots folder; the figures go into <outdir>/paper_plots_mult (default <results_dir>/plots)")
    ap.add_argument("--width", type=float, default=5.0,
                    help="figure width in inches (default %(default)s; set to your column width)")
    ap.add_argument("--format", nargs="+", default=["pdf", "png"], choices=["pdf", "png", "svg"],
                    help="output formats (default: pdf png)")
    ap.add_argument("--points", type=int, default=30,
                    help="points per line: the logged checkpoints are grouped into this many batches "
                         "(default %(default)s; 0 = one point per checkpoint)")
    ap.add_argument("--linear-y", action="store_true", help="linear y axis (default: logarithmic)")
    ap.add_argument("--query-dist", nargs="+", default=["uniform"],
                    help="query distributions of the query throughput figure, one panel each (default: %(default)s)")
    ap.add_argument("--alphas", nargs="+", type=float, default=[1, 20],
                    help="values of alpha (= 1 / routing filter multiplier) in the costs-over-time figure (default: 1 20)")
    ap.add_argument("--point-fraction", type=float, default=1.0,
                    help="interleaved figure: fraction of the batches drawn as points, evenly spaced (default 1 = all)")
    ap.add_argument("--batch-group", type=int, default=1,
                    help="interleaved figure: average every N consecutive batches into one point "
                         "(sums counts and times, so the spikes of single merges are smoothed; default 1 = none)")
    ap.add_argument("--log-x", action="store_true",
                    help="interleaved figure: logarithmic x axis (written to ..._logx.*)")
    ap.add_argument("--cost-query-dist", default="uniform",
                    help="query distribution of the costs figure and the trade-off (default: %(default)s)")
    return ap


def load_runs(results_dir, glob="boa_*.txt"):
    """All runs (one per log and query distribution) found directly in results_dir."""
    runs = []
    for p in sorted(Path(results_dir).glob(glob)):
        if "_interleaved_" in p.name or "_static_" in p.name:
            continue  # interleaved logs are read by load_interleaved, static ones by load_suspended
        rs = plot_results.parse_log(p)
        if rs is None:
            print(f"skip (not an insert-then-query log): {p.name}")
            continue
        runs.extend(rs)
    return runs


# ------------------------------------------------------------------------------ curves

def collect(runs, part, usable, getter, points):
    """{(variant, mult): [(x, y) per repeat]} with every curve cut into `points` batches.

    getter(run, batches) -> (x, y), where `batches` come from plot_results.batches (rates
    use summed counts / summed time per batch).
    """
    plot_results.N_POINTS = points
    out = defaultdict(list)
    for r in runs:
        if r["variant"] not in VARIANTS or not usable(r):
            continue
        b = plot_results.batches(r[part])
        if b is not None:
            out[(r["variant"], r["rf_mult"])].append(getter(r, b))
    return out


def mean_band(curves):
    m = min(len(c[0]) for c in curves)
    x = curves[0][0][:m]
    ys = np.vstack([c[1][:m] for c in curves])
    return x, ys.mean(0), ys.min(0), ys.max(0)


VARIANT_COLORS = {"BOA": "#2a78d6", "PV": "#eb6834"}  # categorical slots 1 and 2 (CVD-safe pair)


def mult_styles(mults):
    """Line style + marker per multiplier (the same for both variants); colour is the variant's."""
    return {m: dict(marker=MARKERS[i % len(MARKERS)], linestyle=LINESTYLES[i % len(LINESTYLES)])
            for i, m in enumerate(sorted(mults))}


def count_fmt(x, _):
    return f"{x / 1e6:g}M" if x >= 1e6 else f"{x / 1e3:g}k" if x >= 1e3 else f"{x:g}"


def throughput_figure(args, panels, ylabel, xlabel, name, band=False):
    """BOA and BOA-PV on the same axes: colour = implementation, line style/marker = multiplier.

    `panels` is a list of (title or None, curves); the panels are stacked and share both axes.
    """
    from matplotlib.lines import Line2D
    all_curves = {}
    for _, curves in panels:
        for k, v in curves.items():
            all_curves.setdefault(k, []).extend(v)
    present = [v for v in VARIANTS if any(k[0] == v for k in all_curves)]
    if set(present) != set(VARIANTS):
        have = ", ".join(VARIANTS[v] for v in present) or "none"
        print(f"skipping {Path(sys.argv[0]).name}: needs BOA and BOA-PV logs (found: {have})")
        return
    mults = sorted({m for _, m in all_curves})
    styles = mult_styles(mults)
    for title, curves in panels:
        for v in VARIANTS:
            for m in mults:
                n = len(curves.get((v, m), []))
                tag = f"[{title}] " if title else ""
                print(f"{tag}{VARIANTS[v]:7s} multiplier {m:<5g}: {n} repeat(s)" if n
                      else f"warning: {tag}no complete runs for {VARIANTS[v]} at multiplier {m:g}")

    n = len(panels)
    fig, axes = plt.subplots(n, 1, sharex=True, sharey=True, squeeze=False,
                             figsize=(args.width, (0.26 * n + 0.4) * args.width),
                             constrained_layout=True)
    rows = []
    for ax, (title, curves) in zip(axes[:, 0], panels):
        for v, vname in VARIANTS.items():
            for m in mults:
                if (v, m) not in curves:
                    continue
                x, y, lo, hi = mean_band(curves[(v, m)])
                ax.plot(x, y, color=VARIANT_COLORS[v], markevery=(0, max(1, len(x) // 8)), **styles[m])
                if band and len(curves[(v, m)]) > 1:
                    ax.fill_between(x, lo, hi, color=VARIANT_COLORS[v], alpha=0.15, linewidth=0)
                rows += [[title or "", vname, m, xi, yi, l, h] for xi, yi, l, h in zip(x, y, lo, hi)]
        if title:
            ax.set_title(title, loc="left", fontsize=8, pad=2)
        if not args.linear_y:
            ax.set_yscale("log")
            # plain tick labels (0.1, 1, 2, 5, 100, ...) instead of powers of ten
            ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            ax.yaxis.set_minor_locator(NullLocator())
        ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
    axes[-1, 0].set_xlabel(xlabel)
    fig.supylabel(ylabel, fontsize=8)

    # two legends: colour -> implementation, line style/marker -> multiplier
    variant_handles = [Line2D([], [], color=VARIANT_COLORS[v], linestyle="-", linewidth=2)
                       for v in VARIANTS]
    mult_handles = [Line2D([], [], color=INK2, **styles[m]) for m in mults]
    fig.legend(variant_handles, list(VARIANTS.values()), loc="outside upper left", ncol=2,
               handlelength=1.6, columnspacing=1.2)
    fig.legend(mult_handles, [rf"$\alpha={1 / m:g}$" for m in mults], loc="outside upper right",
               ncol=len(mults), handlelength=2.2, columnspacing=1.0)

    outdir = (args.outdir or Path(args.results_dir) / "plots") / "paper_plots_mult"
    outdir.mkdir(parents=True, exist_ok=True)
    for fmt in args.format:
        fig.savefig(outdir / f"{name}.{fmt}", dpi=300)
        print("wrote", outdir / f"{name}.{fmt}")
    plt.close(fig)
    with open(outdir / f"{name}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["panel", "variant", "rf_mult", "x", "mean", "min", "max"])
        w.writerows(rows)
    print("wrote", outdir / f"{name}.csv")


# ------------------------------------------------------------------------------ figures



NAME_INS = "pv_vs_boa_insertion_throughput"


def fig_insertion_throughput(args, runs):
    seen = set()

    def first_of_log(r):  # the insertion phase is shared by all query distributions of a log
        if r["file"] in seen:
            return False
        seen.add(r["file"])
        return True

    curves = pc.collect(runs, "ins", first_of_log,
                        lambda r, b: (b["x"], b["n"] / (b["dt_ms"] / 1000.0) / 1e6), args.points)
    pc.throughput_figure(args, [(None, curves)], "Insertion throughput (M elements/s)",
                         "Elements inserted", NAME_INS)





def fig_query_throughput(args, runs):
    panels = []
    for qd in args.query_dist:
        curves = pc.collect(runs, "qry", lambda r, qd=qd: r["query_dist"] == qd and r["complete"],
                            lambda r, b: (b["x"], b["n"] / (b["dt_ms"] / 1000.0)), args.points)
        panels.append((qd, curves))
    pc.throughput_figure(args, panels, "Query throughput (queries/s)", "Queries",
                         "pv_vs_boa_query_throughput_" + "-".join(args.query_dist))






NAME_TRADEOFF = "pv_vs_boa_tradeoff"


def collect_metrics(runs, query_dist):
    """{(variant, X): dict(ins_tput [M elements/s], q_tput [queries/s])} as means over the repeats."""
    ins, qry, seen = defaultdict(list), defaultdict(list), set()
    for r in runs:
        if r["variant"] not in VARIANTS:
            continue
        key = (r["variant"], r["rf_mult"])
        if r["file"] not in seen and "insert_ms" in r:
            seen.add(r["file"])  # the insertion phase is shared by all query distributions of a log
            ins[key].append(r["elements"] / (r["insert_ms"] / 1000.0) / 1e6)
        if r["query_dist"] == query_dist and r["complete"] and len(r["qry"].get("x", [])):
            qry[key].append(r["qry"]["x"][-1] / (r["query_ms"] / 1000.0))
    out = defaultdict(list)
    for (v, x) in ins.keys() & qry.keys():
        out[v].append(dict(X=x, ins_tput=float(np.mean(ins[(v, x)])), q_tput=float(np.mean(qry[(v, x)]))))
    return {v: sorted(rows, key=lambda r: r["X"]) for v, rows in out.items()}


def fig_tradeoff(args, runs):
    pts = collect_metrics(runs, args.cost_query_dist)
    if set(pts) != set(VARIANTS):
        print("skipping pv_vs_boa_tradeoff.py: needs BOA and BOA-PV logs with uniform queries")
        return
    xs = sorted({r["X"] for rows in pts.values() for r in rows})
    styles = pc.mult_styles(xs)
    for v, rows in pts.items():
        print(f"{VARIANTS[v]:7s} multipliers: " + ", ".join(f"alpha={1 / r['X']:g}" for r in rows))

    fig, ax = plt.subplots(figsize=(args.width, 0.72 * args.width), constrained_layout=True)
    for v, rows in pts.items():
        c = VARIANT_COLORS[v]
        ax.plot([r["q_tput"] for r in rows], [r["ins_tput"] for r in rows], color=c, linewidth=0.9, zorder=2)
        for r in rows:
            ax.plot([r["q_tput"]], [r["ins_tput"]], linestyle="none", marker=styles[r["X"]]["marker"],
                    markersize=5, color=c, zorder=3)
            ax.annotate(f"{1 / r['X']:g}", (r["q_tput"], r["ins_tput"]), textcoords="offset points",
                        xytext=(4, 4), fontsize=6.5, color=INK2)
    for axis, setscale in ((ax.xaxis, ax.set_xscale), (ax.yaxis, ax.set_yscale)):
        setscale("log")
        axis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
        axis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
        axis.set_minor_locator(NullLocator())
    ax.margins(0.12)
    ax.set_xlabel("Query throughput (queries/s)")
    ax.set_ylabel("Insertion throughput (M elements/s)")
    ax.annotate("better", xy=(0.96, 0.96), xytext=(0.78, 0.78), xycoords="axes fraction", fontsize=7,
                color=INK2, ha="right", va="top",
                arrowprops=dict(arrowstyle="-|>", color=INK2, lw=0.7, shrinkA=2, shrinkB=0))
    ax.legend([plt.Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS],
              list(VARIANTS.values()), loc="lower left")

    outdir = (args.outdir or Path(args.results_dir) / "plots") / "paper_plots_mult"
    outdir.mkdir(parents=True, exist_ok=True)
    for fmt in args.format:
        fig.savefig(outdir / f"{NAME_TRADEOFF}.{fmt}", dpi=300)
        print("wrote", outdir / f"{NAME_TRADEOFF}.{fmt}")
    plt.close(fig)
    with open(outdir / f"{NAME_TRADEOFF}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "rf_mult", "insertion_throughput_M_per_s", "query_throughput_per_s"])
        for v, rows in pts.items():
            w.writerows([VARIANTS[v], r["X"], f"{r['ins_tput']:.6g}", f"{r['q_tput']:.6g}"] for r in rows)
    print("wrote", outdir / f"{NAME_TRADEOFF}.csv")






NAME_COSTS = "pv_vs_boa_costs"

# (row label, insertion unit factor, query unit factor, insertion unit, query unit); the metric
# functions below return the value per single operation in SI units, scaled by the factor
ROWS = [
    ("Throughput", "M elem./s", "queries/s"),
    ("I/Os", "per 1k insertions", "per query"),
    ("Data moved", "B per insertion", "KiB per query"),
    ("I/O wait", "µs per insertion", "ms per query"),
]


def metrics(r, phase):
    """The four metrics of one run for phase 'insert' or 'query' (None if the log lacks them)."""
    st = r["stats"][phase]
    if phase == "insert":
        n, ms = r["elements"], r.get("insert_ms")
    else:
        n, ms = (r["qry"]["x"][-1] if len(r["qry"].get("x", [])) else None), r["query_ms"]
    if not n or not ms or not {"reads", "writes", "bytes_read", "bytes_written", "io_wait_s"} <= st.keys():
        return None
    ops = n / (ms / 1000.0)
    ios = (st["reads"] + st["writes"]) / n
    byts = (st["bytes_read"] + st["bytes_written"]) / n
    wait = st["io_wait_s"] / n
    if phase == "insert":
        return [ops / 1e6, ios * 1e3, byts, wait * 1e6]
    return [ops, ios, byts / 1024, wait * 1e3]


def fig_costs(args, runs):

    vals = {"insert": defaultdict(list), "query": defaultdict(list)}  # (variant, X) -> [metrics per repeat]
    seen = set()
    for r in runs:
        if r["variant"] not in VARIANTS or not r["complete"]:
            continue
        key = (r["variant"], r["rf_mult"])
        if r["file"] not in seen:  # insertion phase is shared by all query distributions of a log
            seen.add(r["file"])
            m = metrics(r, "insert")
            if m:
                vals["insert"][key].append(m)
        if r["query_dist"] == args.cost_query_dist:
            m = metrics(r, "query")
            if m:
                vals["query"][key].append(m)
    present = {k[0] for k in vals["insert"]} & {k[0] for k in vals["query"]}
    if present != set(VARIANTS):
        print("skipping pv_vs_boa_costs.py: needs BOA and BOA-PV logs with I/O statistics")
        return
    mults = sorted({k[1] for k in vals["insert"]})
    styles = {1 / m: st for m, st in pc.mult_styles(mults).items()}  # x axis: alpha = 1 / multiplier

    fig, axes = plt.subplots(len(ROWS), 2, sharex=True, figsize=(args.width, 0.2 * args.width * len(ROWS) + 0.35),
                             constrained_layout=True)
    csv_rows = []
    for col, phase in enumerate(("insert", "query")):
        axes[0, col].set_title("Insertions" if phase == "insert" else f"Queries ({args.cost_query_dist})",
                               loc="left", fontsize=8, pad=2)
        for i, (name, uins, uqry) in enumerate(ROWS):
            ax = axes[i, col]
            unit = uins if phase == "insert" else uqry
            for v, vname in VARIANTS.items():
                xs, mean, lo, hi = [], [], [], []
                for m in mults:
                    a = vals[phase].get((v, m))
                    if not a:
                        continue
                    col_i = np.array(a)[:, i]
                    xs.append(1 / m); mean.append(col_i.mean()); lo.append(col_i.min()); hi.append(col_i.max())
                    csv_rows.append([phase, vname, f"{1 / m:g}", name, unit, f"{col_i.mean():.6g}",
                                     f"{col_i.min():.6g}", f"{col_i.max():.6g}", len(col_i)])
                c = VARIANT_COLORS[v]
                mean, lo, hi = map(np.array, (mean, lo, hi))
                ax.plot(xs, mean, color=c, linewidth=0.9, zorder=2)
                ax.errorbar(xs, mean, yerr=[mean - lo, hi - mean], fmt="none", ecolor=c, elinewidth=0.6,
                            capsize=1.5, zorder=2)
                for x, y in zip(xs, mean):
                    ax.plot([x], [y], linestyle="none", marker=styles[x]["marker"], markersize=4, color=c, zorder=3)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            ax.set_ylabel(f"{name}\n({unit})", fontsize=7)
    for ax in axes[-1]:
        ax.set_xticks([1 / m for m in mults])
        ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}"))
        ax.minorticks_off()
        ax.set_xlabel(r"$\alpha$")
    fig.legend([plt.Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS],
               list(VARIANTS.values()), loc="outside upper center", ncol=2, handlelength=1.6)

    outdir = (args.outdir or Path(args.results_dir) / "plots") / "paper_plots_mult"
    outdir.mkdir(parents=True, exist_ok=True)
    for fmt in args.format:
        fig.savefig(outdir / f"{NAME_COSTS}.{fmt}", dpi=300)
        print("wrote", outdir / f"{NAME_COSTS}.{fmt}")
    plt.close(fig)
    with open(outdir / f"{NAME_COSTS}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["phase", "variant", "alpha", "metric", "unit", "mean", "min", "max", "repeats"])
        w.writerows(csv_rows)
    print("wrote", outdir / f"{NAME_COSTS}.csv")


NAME_COSTS_T = "pv_vs_boa_costs_over_time"


def fig_costs_over_time(args, runs, wide=False):
    """Same metrics as fig_costs, but the x axis is the number of inserted elements (insertions) and
    of queries (queries). Tall layout: 3 rows x 2 columns (insertions | queries); wide layout:
    2 rows (insertions, queries) x 3 columns (one per metric), which needs less height in a paper.
    The logs have no per-checkpoint I/O wait time, so that metric is missing."""
    from matplotlib.lines import Line2D
    keys = ("reads", "writes", "bytes_read", "bytes_written")
    rows = [  # (label, unit insertion, unit query, per-batch value for insertions, for queries)
        ("Throughput", "M elem./s", "queries/s",
         lambda b: b["n"] / (b["dt_ms"] / 1000.0) / 1e6, lambda b: b["n"] / (b["dt_ms"] / 1000.0)),
        ("I/Os", "per 1k insertions", "per query",
         lambda b: (b["d_reads"] + b["d_writes"]) / b["n"] * 1e3, lambda b: (b["d_reads"] + b["d_writes"]) / b["n"]),
        ("Data moved", "B per insertion", "KiB per query",
         lambda b: (b["d_bytes_read"] + b["d_bytes_written"]) / b["n"],
         lambda b: (b["d_bytes_read"] + b["d_bytes_written"]) / b["n"] / 1024),
    ]
    seen = set()

    def first_of_log(r):
        if r["file"] in seen:
            return False
        seen.add(r["file"])
        return True

    def get(part, usable, f):
        plot_results.N_POINTS = args.points
        out = defaultdict(list)
        for r in runs:
            if r["variant"] not in VARIANTS or not usable(r):
                continue
            b = plot_results.batches(r[part], keys)
            if b is not None:
                out[(r["variant"], r["rf_mult"])].append((b["x"], f(b)))
        return out

    qd = args.cost_query_dist
    data = {}
    for i, (_, _, _, fi, fq) in enumerate(rows):
        seen.clear()
        data[i, 0] = get("ins", first_of_log, fi)
        data[i, 1] = get("qry", lambda r: r["query_dist"] == qd and r["complete"], fq)
    if {k[0] for c in data.values() for k in c} != set(VARIANTS):
        print("skipping pv_vs_boa_costs_over_time: needs BOA and BOA-PV logs with I/O details")
        return
    mults = sorted(m for m in {k[1] for c in data.values() for k in c}
                   if any(abs(1 / m - al) < 1e-6 * al for al in args.alphas))
    if not mults:
        print("skipping pv_vs_boa_costs_over_time: none of the --alphas is in the logs")
        return
    styles = mult_styles(mults)

    nm = len(rows)
    if wide:
        fig, axes = plt.subplots(2, nm, sharex="row", figsize=(args.width, 0.55 * args.width),
                                 constrained_layout=True)
    else:
        fig, axes = plt.subplots(nm, 2, sharex="col", figsize=(args.width, 0.2 * args.width * nm + 0.5),
                                 constrained_layout=True)
    panel = (lambda i, col: axes[col, i]) if wide else (lambda i, col: axes[i, col])
    csv_rows = []
    for col, phase in enumerate(("insertions", f"queries ({qd})")):
        for i, (name, uins, uqry, _, _) in enumerate(rows):
            ax = panel(i, col)
            if wide and col == 0:
                ax.set_title(name, loc="left", fontsize=8, pad=2)
            for v, vname in VARIANTS.items():
                for m in mults:
                    cur = data[i, col].get((v, m))
                    if not cur:
                        continue
                    x, y, lo, hi = mean_band(cur)
                    ax.plot(x, y, color=VARIANT_COLORS[v], markevery=(0, max(1, len(x) // 6)), **styles[m])
                    csv_rows += [[phase, name, uins if col == 0 else uqry, vname, m, xi, yi, l, h]
                                 for xi, yi, l, h in zip(x, y, lo, hi)]
            if not args.linear_y:
                ax.set_yscale("log")
                ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                ax.yaxis.set_minor_locator(NullLocator())
            ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
            unit = uins if col == 0 else uqry
            ax.set_ylabel(unit if wide else f"{name}\n({unit})", fontsize=7)
            if wide:
                ax.set_xlabel("Elements inserted" if col == 0 else "Queries")
                ax.tick_params(axis="x", labelsize=6)
    if not wide:
        axes[-1, 0].set_xlabel("Elements inserted")
        axes[-1, 1].set_xlabel("Queries")
    fig.legend([Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS],
               list(VARIANTS.values()), loc="outside upper left", ncol=2, handlelength=1.6)
    fig.legend([Line2D([], [], color=INK2, **styles[m]) for m in mults], [rf"$\alpha={1 / m:g}$" for m in mults],
               loc="outside upper right", ncol=len(mults), handlelength=2.2, columnspacing=1.0)

    outdir = (args.outdir or Path(args.results_dir) / "plots") / "paper_plots_mult"
    outdir.mkdir(parents=True, exist_ok=True)
    out_name = NAME_COSTS_T + ("_wide" if wide else "")
    for fmt in args.format:
        fig.savefig(outdir / f"{out_name}.{fmt}", dpi=300)
        print("wrote", outdir / f"{out_name}.{fmt}")
    plt.close(fig)
    with open(outdir / f"{out_name}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["phase", "metric", "unit", "variant", "alpha", "x", "mean", "min", "max"])
        w.writerows(csv_rows)
    print("wrote", outdir / f"{out_name}.csv")


# ------------------------------------------------------------------------------ interleaved

NAME_INTERLEAVED = "pv_vs_boa_interleaved"


def parse_interleaved(path):
    """One boa_interleaved*.txt log -> dict(variant, rf_mult, ins_per_batch, batches=[...]) or None.

    Every batch has the insertion time/details, then the query time/details of that batch
    (counts and times are per batch, not cumulative)."""
    import re
    lines = Path(path).read_text().splitlines()
    if not lines or not lines[0].startswith("Interleaved Start BOA"):
        return None
    toks = lines[0][len("Interleaved Start BOA "):].split()
    try:
        mult = int(toks[-1]) / 1e6
    except (ValueError, IndexError):
        return None
    run = dict(file=Path(path).name, variant=" ".join(toks[:-1]) or "BOA", rf_mult=mult,
               complete="End" in lines, batches=[], block=None, lam=None, buf=None, pages=None, qdist=None)
    for l in lines[:12]:
        if l.startswith("Block size "):
            run["block"] = int(l.split()[-1])
        elif l.startswith("Lambda "):
            run["lam"] = int(l.split()[-1])
        elif l.startswith("Pages "):
            run["pages"] = int(l.split()[-1])
        elif l.startswith("Query distribution "):
            run["qdist"] = l.split()[2]
        elif l.startswith("Buffer: "):
            run["buffer"] = int(l.split()[-1])
    if run.get("buffer"):  # buffer sweep key: the block size (KiB) whose default buffer holds this many elements
        fp = 16 if run["variant"] == "PV" else 13  # run element footprint in bytes (BOA 13, BOA-PV 16)
        run["buf"] = int(round(run["buffer"] * fp / 1024))
    num = r"([-+0-9.eE]+)"
    for l in lines:
        m = re.match(rf"^Batch (\d+) \| Insertions (\d+), Insertions time {num} ms, Queries (\d+), "
                     rf"Queries time {num} ms, total time .*?insert details: \[(.*?)\], query details: \[(.*?)\]", l)
        if not m:
            continue
        # "reads 5, cached_reads 0, bytes read 7, ..." -> exact keys ("cached writes" is not "writes")
        det = lambda t: {k: float(v) for k, v in re.findall(rf"(?:^|, )([a-z_ ]+?) {num}(?: ms)?(?=,|$)", t)}
        di, dq = det(m[6]), det(m[7])
        run["batches"].append(dict(
            n_ins=int(m[2]), t_ins=float(m[3]), n_q=int(m[4]), t_q=float(m[5]),
            ios_ins=di["reads"] + di["writes"], wait_ins=di["read wait time"] + di["write wait time"],
            ios_q=dq["reads"] + dq["writes"], wait_q=dq["read wait time"] + dq["write wait time"],
            bw_ins=di["bytes written"], br_q=dq["bytes read"],
            btot_ins=di["bytes read"] + di["bytes written"], btot_q=dq["bytes read"] + dq["bytes written"]))
    return run if run["batches"] else None


def load_interleaved(results_dir):
    out = []
    for p in sorted(Path(results_dir).glob("boa_interleaved*.txt")):
        if "_static_" in p.name:
            continue  # online-suspended logs: load_suspended
        r = parse_interleaved(p)
        if r is None:
            print(f"skip (not an interleaved log with I/O details): {p.name}")
        elif not r["complete"]:
            print(f"skip (incomplete run): {p.name}")
        else:
            out.append(r)
    return out


def group_batches(batches, n):
    """Merge every n consecutive batches into one (counts, times and I/O summed), so that a point
    covers a whole merge cycle instead of one cheap or expensive batch. n = 1 keeps all batches."""
    if n <= 1:
        return batches
    return [{k: sum(b[k] for b in batches[i:i + n]) for k in batches[0]} for i in range(0, len(batches), n)]


def fig_interleaved(args, runs, tall=False, row_layout=False):
    """Interleaved benchmark: per batch (insertions, then queries) throughput, I/Os, I/O wait time and
    bytes (written for insertions, read for queries) per operation over the number of inserted elements. Row 1: the insertions of each batch, row 2: the
    queries of each batch. Colour = implementation, line style/marker = alpha."""
    from matplotlib.lines import Line2D
    runs = [r for r in runs if r["variant"] in VARIANTS]
    if {r["variant"] for r in runs} != set(VARIANTS):
        print("skipping pv_vs_boa_interleaved: needs BOA and BOA-PV interleaved logs")
        return
    # (title, unit insertion, unit query, per-batch value for insertions, for queries)
    cols = [
        ("Throughput", "M elem./s", "queries/s",
         lambda b: b["n_ins"] / b["t_ins"] / 1e3, lambda b: b["n_q"] / b["t_q"] * 1e3),
        ("I/Os", "per 1k ins.", "per query",
         lambda b: b["ios_ins"] / b["n_ins"] * 1e3, lambda b: b["ios_q"] / b["n_q"]),
        ("Data written / read", "B written/ins.", "KiB read/query",
         lambda b: b["bw_ins"] / b["n_ins"], lambda b: b["br_q"] / b["n_q"] / 1024),
        ("I/O wait", "µs per ins.", "ms per query",
         lambda b: b["wait_ins"] / b["n_ins"] * 1e3, lambda b: b["wait_q"] / b["n_q"]),
    ]
    if row_layout:  # one row of 4 panels (insertion/query throughput and I/Os); no data volume, no I/O wait
        cols = cols[:2]
    mults = sorted({r["rf_mult"] for r in runs})
    styles = mult_styles(mults)
    groups = defaultdict(list)
    for r in runs:
        groups[r["variant"], r["rf_mult"]].append(r)

    nc = len(cols)
    if row_layout:
        fig, axes1 = plt.subplots(1, 2 * nc, sharex=True, figsize=(args.width, 0.3 * args.width),
                                  constrained_layout=True)
        axes = {(r_, j_): axes1[r_ * nc + j_] for r_ in (0, 1) for j_ in range(nc)}
    elif tall:  # one row per metric, insertions left, queries right
        fig, axes = plt.subplots(nc, 2, sharex=True, figsize=(args.width, 0.25 * args.width * nc + 0.4),
                                 constrained_layout=True)
    else:     # insertions top, queries bottom, one column per metric
        fig, axes = plt.subplots(2, nc, sharex=True, figsize=(args.width, 0.5 * args.width),
                                 constrained_layout=True)
    csv_rows = []
    for row, phase in enumerate(("insertions", "queries")):
        for j, (title, uins, uqry, fi, fq) in enumerate(cols):
            ax, f = (axes[j, row] if tall else axes[row, j]), (fi if row == 0 else fq)
            for v, vname in VARIANTS.items():
                for m in mults:
                    rs = groups.get((v, m))
                    if not rs:
                        continue
                    gb = [group_batches(r["batches"], args.batch_group) for r in rs]
                    k = min(len(g) for g in gb)
                    y = np.mean([[f(b) for b in g[:k]] for g in gb], axis=0)
                    x = np.cumsum([b["n_ins"] for b in gb[0][:k]])
                    sel = np.unique(np.round(np.linspace(0, k - 1, max(2, round(k * args.point_fraction)))).astype(int))
                    x, y = x[sel], y[sel]  # a subset of the batches (evenly spaced) makes the lines calmer
                    ax.plot(x, y, color=VARIANT_COLORS[v], linewidth=0.7, markersize=2.5,
                            **styles[m])  # a marker on every point
                    csv_rows += [[phase, title, uins if row == 0 else uqry, vname, f"{1 / m:g}", int(xi), yi]
                                 for xi, yi in zip(x, y)]
            unit = uins if row == 0 else uqry
            if tall:
                if j == 0:
                    ax.set_title("Insertions" if row == 0 else "Queries", loc="left", fontsize=8, pad=2)
                ax.set_ylabel(f"{title}\n({unit})", fontsize=6.5)
            else:
                if row_layout:
                    ax.set_title(f"{'Insertion' if row == 0 else 'Query'} {title if title.startswith('I/O') else title.lower()}",
                                 loc="left", fontsize=6.5, pad=2)
                elif row == 0:
                    ax.set_title(title, loc="left", fontsize=7.5, pad=2)
                ax.set_ylabel(unit, fontsize=6.5)
            if not args.linear_y:
                ax.set_yscale("log")
                ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                ax.yaxis.set_minor_locator(NullLocator())
                ax.yaxis.set_minor_formatter(NullFormatter())
                if row_layout:
                    from matplotlib.ticker import MaxNLocator
                    ax.yaxis.set_major_locator(MaxNLocator(nbins=4))
            if args.log_x:
                ax.set_xscale("log")
                ax.xaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 3.0)))
                ax.xaxis.set_minor_locator(NullLocator())
            ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
            ax.tick_params(axis="x", labelsize=6)
            if not row_layout and ((j == nc - 1) if tall else (row == 1)):
                ax.set_xlabel("Inserted elements", fontsize=7)
    fig.legend([Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS],
               list(VARIANTS.values()), loc="outside upper left", ncol=2, handlelength=1.6)
    fig.legend([Line2D([], [], color=INK2, linewidth=0.8, markersize=3, **styles[m]) for m in mults], [rf"$\alpha={1 / m:g}$" for m in mults],
               loc="outside upper right", ncol=len(mults), handlelength=2.2, columnspacing=1.0)

    outdir = (args.outdir or Path(args.results_dir) / "plots") / "paper_plots_mult"
    outdir.mkdir(parents=True, exist_ok=True)
    name_out = NAME_INTERLEAVED + ("_tall" if tall else "") + ("_row" if row_layout else "") + ("_logx" if args.log_x else "")
    for fmt in args.format:
        fig.savefig(outdir / f"{name_out}.{fmt}", dpi=300)
        print("wrote", outdir / f"{name_out}.{fmt}")
    plt.close(fig)
    with open(outdir / f"{name_out}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["phase", "metric", "unit", "variant", "alpha", "elements_inserted", "value"])
        w.writerows(csv_rows)
    print("wrote", outdir / f"{name_out}.csv")


SWEEPS = {
    "block": dict(folder="paper_plots_block", prefix="blocksize", csv="block_size", title="block size",
                  label=lambda b: f"{b // 1024} KiB" if b < 1 << 20 else f"{b >> 20} MiB"),
    "buf": dict(folder="paper_plots_buffer", prefix="buffer", csv="buffer_kib", title="buffer size",
                label=lambda v: f"{v} KiB"),
    "pages": dict(folder="paper_plots_pages", prefix="pages", csv="pages", title="pages", label=lambda v: str(v)),
    "qdist": dict(folder="paper_plots_qdist", prefix="qdist", csv="query_dist", title="query distribution",
                  label=lambda v: v.capitalize()),
    "lam": dict(folder="paper_plots_lambda", prefix="lambda", csv="lambda", title="$\\lambda$",
                label=lambda v: f"$\\lambda={v}$"),
}


def fig_block_sizes(args, runs, key="block", full=False, extra=""):
    """Interleaved runs for several block sizes: one figure per variant, one line per block size,
    per batch over the number of inserted elements. Row 1: insertions, row 2: queries; columns
    throughput, I/Os, total data moved, I/O wait."""
    from matplotlib.lines import Line2D
    runs = [r for r in runs if r["variant"] in VARIANTS and r[key]]
    cols = [
        ("Throughput", "M elem./s", "queries/s",
         lambda b: b["n_ins"] / b["t_ins"] / 1e3, lambda b: b["n_q"] / b["t_q"] * 1e3),
        ("I/Os", "per 1k ins.", "per query",
         lambda b: b["ios_ins"] / b["n_ins"] * 1e3, lambda b: b["ios_q"] / b["n_q"]),
        ("Data moved", "KiB per ins.", "KiB per query",
         lambda b: b["btot_ins"] / b["n_ins"] / 1024, lambda b: b["btot_q"] / b["n_q"] / 1024),
        ("I/O wait", "µs per ins.", "ms per query",
         lambda b: b["wait_ins"] / b["n_ins"] * 1e3, lambda b: b["wait_q"] / b["n_q"]),
    ]
    if key in ("lam", "buf", "pages", "qdist") and not full:  # lambda / buffer sweep: only throughput and I/Os (no data volume, no I/O wait)
        cols = cols[:2]
    sfx = extra + ("_full" if full else "")  # full: all four metric columns (also data moved and I/O wait)
    blocks = sorted({r[key] for r in runs})
    kb = SWEEPS[key]["label"]
    from matplotlib.colors import LinearSegmentedColormap
    cmap = LinearSegmentedColormap.from_list("orange_blue", ["#f28e2b", "#e15759", "#b07aa1", "#4e79a7", "#1f3f7a"])
    color = {bl: cmap(i / max(1, len(blocks) - 1)) for i, bl in enumerate(blocks)}  # small blocks orange, large blue
    nc = len(cols)
    outdir = (args.outdir or Path(args.results_dir) / "plots") / SWEEPS[key]["folder"]
    outdir.mkdir(parents=True, exist_ok=True)
    for v, vname in VARIANTS.items():
        vr = {r[key]: r for r in runs if r["variant"] == v}
        if not vr:
            continue
        fig, axes = plt.subplots(2, nc, sharex=True, figsize=(args.width, (0.5 if nc > 2 else 0.48) * args.width),
                                 constrained_layout=True)
        csv_rows = []
        for row, phase in enumerate(("insertions", "queries")):
            for j, (title, uins, uqry, fi, fq) in enumerate(cols):
                ax, f = axes[row, j], (fi if row == 0 else fq)
                for bl in blocks:
                    r = vr.get(bl)
                    if r is None:
                        continue
                    g = group_batches(r["batches"], args.batch_group)
                    x = np.cumsum([b["n_ins"] for b in g])
                    y = np.array([f(b) for b in g])
                    ax.plot(x, y, color=color[bl], linewidth=0.7, marker="o", markersize=1.4)
                    csv_rows += [[phase, title, uins if row == 0 else uqry, vname, bl, int(xi), yi]
                                 for xi, yi in zip(x, y)]
                if row == 0:
                    ax.set_title(title, loc="left", fontsize=7.5, pad=2)
                ax.set_ylabel(uins if row == 0 else uqry, fontsize=6.5)
                ys = [yy for line in ax.get_lines() for yy in line.get_ydata() if yy > 0]
                # log axis only for a wide data range: with 1/2/5 ticks a narrow range (e.g. 0.11-0.18) gets no or overlapping labels
                if not args.linear_y and ys and max(ys) / min(ys) > 3:
                    ax.set_yscale("log")
                    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
                    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                    ax.yaxis.set_minor_locator(NullLocator())
                else:
                    ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
                    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
                ax.tick_params(axis="x", labelsize=6)
                if row == 1:
                    ax.set_xlabel("Inserted elements", fontsize=7)
        fig.legend([Line2D([], [], color=color[bl], linewidth=1.5, marker="o", markersize=1.8) for bl in blocks if bl in vr],
                   [kb(bl) for bl in blocks if bl in vr], loc="outside upper center", ncol=len(vr),
                   handlelength=1.4, columnspacing=0.9, title=f"{vname}, {SWEEPS[key]['title']}", title_fontsize=7)
        tag = "boa" if v == "boa" else v
        for fmt in args.format:
            fig.savefig(outdir / f"{SWEEPS[key]['prefix']}_{tag}{sfx}.{fmt}", dpi=300)
            print("wrote", outdir / f"{SWEEPS[key]['prefix']}_{tag}{sfx}.{fmt}")
        plt.close(fig)
        with open(outdir / f"{SWEEPS[key]['prefix']}_{tag}{sfx}.csv", "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["phase", "metric", "unit", "variant", SWEEPS[key]["csv"], "elements_inserted", "value"])
            w.writerows(csv_rows)


def fig_block_sizes_combined(args, runs, sizes=(4096, 65536, 524288, 2097152), key="block", full=False, extra=""):
    """Selected block sizes, BOA (solid) and BOA-PV (dashed) in one figure, one line per block size,
    per batch over the number of inserted elements. Row 1: insertions, row 2: queries; columns
    throughput, I/Os, total data moved, I/O wait."""
    from matplotlib.lines import Line2D
    runs = [r for r in runs if r["variant"] in VARIANTS and r[key] in sizes]
    cols = [
        ("Throughput", "M elem./s", "queries/s",
         lambda b: b["n_ins"] / b["t_ins"] / 1e3, lambda b: b["n_q"] / b["t_q"] * 1e3),
        ("I/Os", "per 1k ins.", "per query",
         lambda b: b["ios_ins"] / b["n_ins"] * 1e3, lambda b: b["ios_q"] / b["n_q"]),
        ("Data moved", "KiB per ins.", "KiB per query",
         lambda b: b["btot_ins"] / b["n_ins"] / 1024, lambda b: b["btot_q"] / b["n_q"] / 1024),
        ("I/O wait", "µs per ins.", "ms per query",
         lambda b: b["wait_ins"] / b["n_ins"] * 1e3, lambda b: b["wait_q"] / b["n_q"]),
    ]
    if key in ("lam", "buf", "pages", "qdist") and not full:  # lambda / buffer sweep: only throughput and I/Os (no data volume, no I/O wait)
        cols = cols[:2]
    sfx = extra + ("_full" if full else "")  # full: all four metric columns (also data moved and I/O wait)
    blocks = [b for b in sizes if b in {r[key] for r in runs}]  # in the order given
    kb = SWEEPS[key]["label"]
    from matplotlib.colors import LinearSegmentedColormap
    cmap = LinearSegmentedColormap.from_list("orange_blue", ["#f28e2b", "#e15759", "#b07aa1", "#4e79a7", "#1f3f7a"])
    color = {bl: cmap(i / max(1, len(blocks) - 1)) for i, bl in enumerate(blocks)}  # small blocks orange, large blue
    nc = len(cols)
    outdir = (args.outdir or Path(args.results_dir) / "plots") / SWEEPS[key]["folder"]
    outdir.mkdir(parents=True, exist_ok=True)
    if True:
        vr = {(r["variant"], r[key]): r for r in runs}
        one_row = key in ("lam", "buf", "pages", "qdist") and not full  # lambda: insertions and queries side by side in a single row of 4 panels
        if one_row:
            fig, axes1 = plt.subplots(1, 2 * nc, sharex=True, figsize=(args.width, 0.3 * args.width),
                                      constrained_layout=True)
            axes = {(r_, j_): axes1[r_ * nc + j_] for r_ in (0, 1) for j_ in range(nc)}
        else:
            fig, axes2 = plt.subplots(2, nc, sharex=True, figsize=(args.width, (0.5 if nc > 2 else 0.48) * args.width),
                                      constrained_layout=True)
            axes = {(r_, j_): axes2[r_, j_] for r_ in (0, 1) for j_ in range(nc)}
        csv_rows = []
        for row, phase in enumerate(("insertions", "queries")):
            for j, (title, uins, uqry, fi, fq) in enumerate(cols):
                ax, f = axes[(row, j)], (fi if row == 0 else fq)
                for v, vname in VARIANTS.items():
                  for bl in blocks:
                    r = vr.get((v, bl))
                    if r is None:
                        continue
                    g = group_batches(r["batches"], args.batch_group)
                    x = np.cumsum([b["n_ins"] for b in g])
                    y = np.array([f(b) for b in g])
                    ax.plot(x, y, color=color[bl], linewidth=0.7, marker="o", markersize=1.4,
                            linestyle="-" if v == "BOA" else (0, (3, 1.5)))
                    csv_rows += [[phase, title, uins if row == 0 else uqry, vname, bl, int(xi), yi]
                                 for xi, yi in zip(x, y)]
                if one_row:
                    ax.set_title(f"{'Insertion' if row == 0 else 'Query'} {title if title.startswith('I/O') else title.lower()}",
                                 loc="left", fontsize=6.5, pad=2)
                elif row == 0:
                    ax.set_title(title, loc="left", fontsize=7.5, pad=2)
                ax.set_ylabel(uins if row == 0 else uqry, fontsize=6.5)
                if not args.linear_y:
                    ax.set_yscale("log")
                    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
                    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
                    ax.yaxis.set_minor_locator(NullLocator())
                    ax.yaxis.set_minor_formatter(NullFormatter())
                    if one_row:  # few, evenly readable ticks on the small panels
                        from matplotlib.ticker import MaxNLocator
                        ax.yaxis.set_major_locator(MaxNLocator(nbins=4))
                ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
                ax.tick_params(axis="x", labelsize=6)
                if row == 1 and not one_row:  # the one-row lambda figure has no x label (caption says it)
                    ax.set_xlabel("Inserted elements", fontsize=7)
        handles = [Line2D([], [], color=color[bl], linewidth=1.5, marker="o", markersize=1.8) for bl in blocks]
        if key == "lam":  # lambda symbol inline, to the left of the values
            fig.legend([Line2D([], [], color="none")] + handles, [SWEEPS[key]["title"]] + [str(bl) for bl in blocks],
                       loc="outside upper left", ncol=len(blocks) + 1, handlelength=1.4, columnspacing=0.9,
                       handletextpad=0.4)
        else:
            fig.legend(handles, [kb(bl) for bl in blocks], loc="outside upper left", ncol=len(blocks),
                       handlelength=1.4, columnspacing=0.9, title=SWEEPS[key]["title"].capitalize(),
                       title_fontsize=7)
        fig.legend([Line2D([], [], color=INK2, linewidth=0.9, linestyle=ls) for ls in ("-", (0, (3, 1.5)))],
                   list(VARIANTS.values()), loc="outside upper right", ncol=2, handlelength=2.2,
                   columnspacing=1.0)
        for fmt in args.format:
            fig.savefig(outdir / f"{SWEEPS[key]['prefix']}_selected{sfx}.{fmt}", dpi=300)
            print("wrote", outdir / f"{SWEEPS[key]['prefix']}_selected{sfx}.{fmt}")
        plt.close(fig)
        with open(outdir / f"{SWEEPS[key]['prefix']}_selected{sfx}.csv", "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["phase", "metric", "unit", "variant", SWEEPS[key]["csv"], "elements_inserted", "value"])
            w.writerows(csv_rows)


# ------------------------------------------------------------------ online-suspended mode

MODES = {"merge": "merge", "sort": "sort", "hybrid": "hybrid"}
MODE_STYLE = {"online": dict(linestyle="-", marker=None, linewidth=1.5), "merge": dict(linestyle="-.", marker="o"), "sort": dict(linestyle="--", marker="s"),
              "hybrid": dict(linestyle=":", marker="^")}


def suspended_header(first_line):
    """'[Interleaved ]Start BOA [PV] [STATIC [SORT|HYBRID]] <mult digits>' -> (variant, mode, rf_mult) or None.
    Mode is merge/sort/hybrid for STATIC logs and online for continuous online insertion."""
    import re
    m = re.match(r"^(?:Interleaved )?Start BOA( PV)?(?: STATIC(?: (SORT|HYBRID))?)?\s+(\d+)\s*$", first_line)
    if not m:
        return None
    static = " STATIC" in first_line
    mode = (m[2] or "merge").lower() if static else "online"
    return ("PV" if m[1] else "BOA"), mode, int(m[3]) / 1e6


def parse_static(path):
    """Online-suspended insert-then-query log -> dict(variant, mode, n, time_ms, reads, writes, bytes, wait_s)
    for the insertion phase (staging + consolidation, up to 'End Insertions' and its I/O statistics)."""
    import re
    lines = Path(path).read_text().splitlines()
    if not lines or suspended_header(lines[0]) is None or "End" not in lines:
        return None
    variant, mode, mult = suspended_header(lines[0])
    n = next((int(l.split()[-1]) for l in lines if l.startswith("Elements:")), None)
    try:
        i = next(k for k, l in enumerate(lines) if l.startswith("End Insertions"))
    except StopIteration:
        return None
    t_ms = float(re.search(r"time ([-+0-9.eE]+)", lines[i])[1])
    num = lambda key: next(float(re.search(r":\s*([0-9.eE+-]+)", l)[1]) for l in lines[i:] if l.strip().startswith(key))
    return dict(file=Path(path).name, variant=variant, mode=mode, rf_mult=mult, n=n, time_ms=t_ms,
                ios=num("total number of reads") + num("total number of writes"),
                bytes=num("number of bytes read from disks") + num("number of bytes written to disks"),
                wait_ms=num("I/O wait time") * 1e3)


def load_suspended(results_dir):
    """(insert-then-query runs, interleaved runs) of online-suspended mode plus the continuous online
    runs with the same parameters (mode "online"). Each run has variant 'BOA'/'PV', mode
    online/merge/sort/hybrid and rf_mult."""
    import re
    d = Path(results_dir)
    # pairing key: file name without the static tag and without the experiment counter (_exp_N), which differs
    # when a file with the same name already existed in the working directory
    strip = lambda name: re.sub(r"_exp_\d+\.txt$", ".txt", re.sub(r"_static(?:__(?:sort|hybrid))?_", "_", name))
    static = [p for p in sorted(d.glob("boa_*.txt")) if "_static_" in p.name]
    keys = {strip(p.name) for p in static}
    files = static + [p for p in sorted(d.glob("boa_*.txt")) if "_static_" not in p.name and strip(p.name) in keys]
    stat, inter = [], []
    for p in files:
        hdr = suspended_header(p.read_text().split("\n", 1)[0])
        if hdr is None:
            continue
        if "_interleaved_" in p.name:
            r = parse_interleaved(p)
            if r is None or not r["complete"]:
                print(f"skip (incomplete or without I/O details): {p.name}")
                continue
            r["variant"], r["mode"], r["rf_mult"] = hdr
            inter.append(r)
        else:
            r = parse_static(p)
            if r is None:
                print(f"skip (incomplete): {p.name}")
                continue
            stat.append(r)
    return stat, inter


def fig_buffer_bars(args, runs):
    """Buffer sweep: total insertion time and total query time (summed over all batches) as grouped bars,
    one group per buffer size, BOA and BOA-PV side by side."""
    runs = [r for r in runs if r["variant"] in VARIANTS and r["buf"]]
    sizes = sorted({r["buf"] for r in runs})
    vr = {(r["variant"], r["buf"]): r for r in runs}
    parts = (("Total insertion time", "t_ins"), ("Total query time", "t_q"))
    fig, axes = plt.subplots(1, 2, figsize=(args.width, 0.3 * args.width), constrained_layout=True)
    w = 0.38
    csv_rows = []
    for ax, (title, key) in zip(axes, parts):
        for i, (v, vname) in enumerate(VARIANTS.items()):
            xs, ys = [], []
            for k, sz in enumerate(sizes):
                r = vr.get((v, sz))
                if r is None:
                    continue
                t = sum(b[key] for b in r["batches"]) / 1e3  # s
                xs.append(k + (i - 0.5) * w)
                ys.append(t)
                csv_rows.append([title, vname, sz, t])
            ax.bar(xs, ys, w, color=VARIANT_COLORS[v], label=vname)
        ax.set_xticks(range(len(sizes)), [SWEEPS["buf"]["label"](sz) for sz in sizes])
        ax.set_title(title, loc="left", fontsize=7.5, pad=2)
        ax.set_ylabel("seconds", fontsize=6.5)
        ax.set_xlabel("Buffer size", fontsize=7)
        ax.tick_params(axis="x", labelsize=6)
    axes[0].legend(loc="upper left", fontsize=6.5, handlelength=1.2)
    outdir = (args.outdir or Path(args.results_dir) / "plots") / SWEEPS["buf"]["folder"]
    outdir.mkdir(parents=True, exist_ok=True)
    with open(outdir / "buffer_total_time.csv", "w", newline="") as fh:
        w_ = csv.writer(fh)
        w_.writerow(["metric", "variant", "buffer_kib", "seconds"])
        w_.writerows(csv_rows)
    save_fig(fig, outdir, "buffer_total_time", args)


def fig_buffer_bursts(args, runs):
    """Buffer sweep, why insertions get slower: (a) insertion time of every single batch (not grouped),
    (b) I/O wait per I/O, (c) data moved per second of I/O wait (batches 6.. only, no warm-up)."""
    from matplotlib.colors import LinearSegmentedColormap
    from matplotlib.lines import Line2D
    runs = [r for r in runs if r["variant"] in VARIANTS and r["buf"] and r["buf"] != 64]  # 64 KiB left out: less clutter
    sizes = sorted({r["buf"] for r in runs})
    cmap = LinearSegmentedColormap.from_list("orange_blue", ["#f28e2b", "#e15759", "#b07aa1", "#4e79a7", "#1f3f7a"])
    color = {sz: cmap(i / max(1, len(sizes) - 1)) for i, sz in enumerate(sizes)}
    vr = {(r["variant"], r["buf"]): r for r in runs}
    fig, axes = plt.subplots(1, 3, figsize=(args.width, 0.36 * args.width), constrained_layout=True,
                             gridspec_kw=dict(width_ratios=[1.6, 1, 1]))
    csv_rows = []
    ax = axes[0]
    for v in VARIANTS:
        for sz in sizes:
            r = vr.get((v, sz))
            if r is None:
                continue
            g = group_batches(r["batches"], args.batch_group)  # fewer points: --batch-group batches per point
            x = np.cumsum([b["n_ins"] for b in g])
            y = np.array([b["t_ins"] for b in g]) / 1e3
            ax.plot(x, y, color=color[sz], linewidth=0.6, marker="o", markersize=1.2,
                    linestyle="-" if v == "BOA" else (0, (3, 1.5)))
            csv_rows += [["insertion_time_s_per_batch_group", VARIANTS[v], sz, int(xi), yi] for xi, yi in zip(x, y)]
    ax.set_title("Insertion time" + (f" per {args.batch_group} batches" if args.batch_group > 1 else " per batch"), loc="left", fontsize=6.5, pad=2)
    ax.set_ylabel("seconds", fontsize=6.5)
    ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
    ax.tick_params(axis="x", labelsize=6)
    ax.set_xlabel("Inserted elements", fontsize=7)
    ax.set_xticks([0, 250e6, 500e6])
    w = 0.38
    for ax, title, ylabel, fn_ in (
            (axes[1], "Wait per I/O", "ms", lambda b: 1e3 * sum(x["wait_ins"] for x in b) / 1e3 / sum(x["ios_ins"] for x in b)),
            (axes[2], "Throughput while waiting", "MiB/s",
             lambda b: sum(x["btot_ins"] for x in b) / 2**20 / (sum(x["wait_ins"] for x in b) / 1e3))):
        for i, (v, vname) in enumerate(VARIANTS.items()):
            xs, ys = [], []
            for k, sz in enumerate(sizes):
                r = vr.get((v, sz))
                if r is None:
                    continue
                xs.append(k + (i - 0.5) * w)
                ys.append(fn_(r["batches"][5:]))
                csv_rows.append([title, vname, sz, "", ys[-1]])
            ax.bar(xs, ys, w, color=VARIANT_COLORS[v], label=vname)
        ax.set_xticks(range(len(sizes)), [str(sz) for sz in sizes])
        ax.set_title(title, loc="left", fontsize=6.5, pad=2)
        ax.set_ylabel(ylabel, fontsize=6.5)
        ax.set_xlabel("Buffer size (KiB)", fontsize=7)
        ax.tick_params(axis="x", labelsize=6)
    axes[1].set_ylim(0, axes[1].get_ylim()[1] * 1.25)
    axes[1].legend(loc="upper left", fontsize=6, handlelength=1.2, ncol=2, columnspacing=0.8)
    fig.legend([Line2D([], [], color=color[sz], linewidth=1.5, marker="o", markersize=1.8) for sz in sizes],
               [f"{sz} KiB" for sz in sizes], loc="outside upper left", ncol=len(sizes), handlelength=1.4,
               columnspacing=0.9, title="Buffer size (lines)", title_fontsize=6.5)
    fig.legend([Line2D([], [], color=INK2, linewidth=0.9, linestyle=ls) for ls in ("-", (0, (3, 1.5)))],
               list(VARIANTS.values()), loc="outside upper right", ncol=2, handlelength=2.2, columnspacing=1.0)
    outdir = (args.outdir or Path(args.results_dir) / "plots") / SWEEPS["buf"]["folder"]
    outdir.mkdir(parents=True, exist_ok=True)
    with open(outdir / "buffer_bursts.csv", "w", newline="") as fh:
        w_ = csv.writer(fh)
        w_.writerow(["metric", "variant", "buffer_kib", "elements_inserted", "value"])
        w_.writerows(csv_rows)
    save_fig(fig, outdir, "buffer_bursts", args)


def fig_pages_query_gain(args, runs, pages=(2, 16)):
    """Query phase per query distribution at two page counts: throughput and I/Os per query, BOA and BOA-PV;
    light bar = fewer pages, dark bar = more pages, the label above the dark bar is the change."""
    from matplotlib.colors import to_rgb
    order = ["uniform", "zipf", "hotspot", "latest"]
    vr = {(r["variant"], r["pages"], r["qdist"]): r for r in runs if r["qdist"] in order}
    lo, hi = pages
    if not all((v, p_, q) in vr for v in VARIANTS for p_ in pages for q in order):
        print("skipping pages query-gain figure: needs both variants, pages", pages, "and all distributions")
        return
    def tot(r, key):
        return sum(b[key] for b in r["batches"])
    metrics = (("Query throughput", "queries/s", lambda r: tot(r, "n_q") / tot(r, "t_q") * 1e3, True),
               ("I/Os per query", "I/Os", lambda r: tot(r, "ios_q") / tot(r, "n_q"), False))
    fig, axes = plt.subplots(1, 2, figsize=(args.width, 0.36 * args.width), constrained_layout=True)
    w = 0.2
    csv_rows = []
    for ax, (title, unit, f, up_good) in zip(axes, metrics):
        for k, q in enumerate(order):
            for i, (v, vname) in enumerate(VARIANTS.items()):
                base = to_rgb(VARIANT_COLORS[v])
                vals = [f(vr[v, p_, q]) for p_ in pages]
                for j, (p_, val) in enumerate(zip(pages, vals)):
                    x = k + (2 * i + j - 1.5) * w
                    light = tuple(0.45 * c + 0.55 for c in base)
                    ax.bar(x, val, w, color=light if j == 0 else base,
                           label=f"{vname}, {p_} pages" if k == 0 else None)
                    csv_rows.append([title, vname, p_, q, val])
                x_hi = k + (2 * i + 0.5 - 1.5) * w
                ax.text(x_hi, vals[1], f"{(vals[1] / vals[0] - 1) * 100:+.0f}%", ha="center", va="bottom", fontsize=4.8)
        ax.set_xticks(range(len(order)), [o.capitalize() for o in order])
        ax.tick_params(axis="x", labelsize=6)
        ax.set_title(title, loc="left", fontsize=7, pad=2)
        ax.set_ylabel(unit, fontsize=6.5)
        ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="outside upper center", ncol=4, handlelength=1.0, columnspacing=0.8, fontsize=6)
    outdir = (args.outdir or Path(args.results_dir) / "plots") / SWEEPS["qdist"]["folder"]
    outdir.mkdir(parents=True, exist_ok=True)
    with open(outdir / "qdist_pages_gain.csv", "w", newline="") as fh:
        w_ = csv.writer(fh)
        w_.writerow(["metric", "variant", "pages", "query_dist", "value"])
        w_.writerows(csv_rows)
    save_fig(fig, outdir, "qdist_pages_gain", args)


def suspended_outdir(args):
    d = (args.outdir or Path(args.results_dir) / "plots") / "paper_os_mode"
    d.mkdir(parents=True, exist_ok=True)
    return d


def save_fig(fig, outdir, name, args):
    for fmt in args.format:
        fig.savefig(outdir / f"{name}.{fmt}", dpi=300)
        print("wrote", outdir / f"{name}.{fmt}")
    plt.close(fig)


def fig_suspended_insert_then_query(args, runs, sfx="", row=False, two=False):
    """Online-suspended insert-then-query: construction (staging log + consolidation) per materialization
    mode, BOA and BOA-PV as bars. Totals of the whole construction: time, I/O count, I/O wait, bytes."""
    from matplotlib.patches import Patch
    # two=True: only total time and I/O count, modes online, hybrid, sort (no merge)
    order = ("online", "hybrid", "sort") if two else ("online", "merge", "sort", "hybrid")
    modes = [m for m in order if any(r["mode"] == m for r in runs)]
    cols = [
        ("Total time", "s", lambda r: r["time_ms"] / 1e3),
        ("I/O count", "thousands", lambda r: r["ios"] / 1e3),
        ("I/O wait time", "s", lambda r: r["wait_ms"] / 1e3),
        ("Bytes read + written", "GiB", lambda r: r["bytes"] / 2**30),
    ]
    if two:
        cols = cols[:2]
        fig, axes = plt.subplots(1, 2, figsize=(args.width, 0.3 * args.width), constrained_layout=True)
    elif row:  # one row of four panels, mode labels reduced to their initials (o, m, s, h)
        fig, axes = plt.subplots(1, 4, figsize=(args.width, 0.27 * args.width), constrained_layout=True)
    else:    # 2x2: room for the full labels
        fig, axes = plt.subplots(2, 2, figsize=(args.width, 0.55 * args.width), constrained_layout=True)
    csv_rows, w = [], 0.38
    for ax, (title, unit, f) in zip(axes.flat, cols):
        for k, (v, vname) in enumerate(VARIANTS.items()):
            ys = []
            for m in modes:
                rs = [r for r in runs if r["variant"] == v and r["mode"] == m]
                ys.append(np.mean([f(r) for r in rs]) if rs else np.nan)
                csv_rows.append([title, unit, vname, m, ys[-1]])
            ax.bar(np.arange(len(modes)) + (k - 0.5) * w, ys, w, color=VARIANT_COLORS[v], linewidth=0)
        ax.set_xticks(range(len(modes)))
        ax.set_xticklabels([m[0] for m in modes] if row else modes, fontsize=7)
        ax.set_title(title, loc="left", fontsize=7.5, pad=2)
        ax.set_ylabel(unit, fontsize=6.5)
        ax.grid(axis="x", visible=False)
    fig.legend([Patch(color=VARIANT_COLORS[v]) for v in VARIANTS], list(VARIANTS.values()),
               loc="outside upper left", ncol=2, handlelength=1.2)
    out = suspended_outdir(args)
    name = "suspended_insert_then_query" + ("_two" if two else "_row" if row else "") + sfx
    save_fig(fig, out, name, args)
    with open(out / f"{name}.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["metric", "unit", "variant", "materialization", "value"])
        wr.writerows(csv_rows)


def fig_suspended_interleaved(args, runs, sfx=""):
    """Online-suspended interleaved: insertion batches (each ends with consolidate_structure()) over the
    number of inserted elements. Colour = BOA / BOA-PV, line style and marker = materialization mode.
    Columns (absolute values per batch, summed over --batch-group batches): time, I/O wait, I/O count,
    bytes read + written."""
    from matplotlib.lines import Line2D
    modes = [m for m in ("online", "merge", "sort", "hybrid") if any(r["mode"] == m for r in runs)]
    cols = [
        ("Time per batch", "s", lambda b: b["t_ins"] / 1e3),
        ("I/O wait per batch", "s", lambda b: b["wait_ins"] / 1e3),
        ("I/O count per batch", "thousands", lambda b: b["ios_ins"] / 1e3),
        ("Bytes per batch", "GiB", lambda b: b["btot_ins"] / 2**30),
    ]
    fig, axes = plt.subplots(1, len(cols), figsize=(args.width, 0.27 * args.width), constrained_layout=True)
    csv_rows = []
    for ax, (title, unit, f) in zip(axes, cols):
        for v, vname in VARIANTS.items():
            for m in modes:
                rs = [r for r in runs if r["variant"] == v and r["mode"] == m]
                if not rs:
                    continue
                gb = [group_batches(r["batches"], args.batch_group) for r in rs]
                k = min(len(g) for g in gb)
                y = np.mean([[f(b) for b in g[:k]] for g in gb], axis=0)
                x = np.cumsum([b["n_ins"] for b in gb[0][:k]])
                ax.plot(x, y, color=VARIANT_COLORS[v], markersize=2.5, **{"linewidth": 0.8, **MODE_STYLE[m]})
                csv_rows += [[title, unit, vname, m, int(xi), yi] for xi, yi in zip(x, y)]
        ax.set_title(title, loc="left", fontsize=7.5, pad=2)
        ax.set_ylabel(unit, fontsize=6.5)
        ax.set_xlabel("Inserted elements", fontsize=7)
        ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
        ax.tick_params(axis="x", labelsize=6)
        if not args.linear_y:
            ax.set_yscale("log")
            ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            ax.yaxis.set_minor_locator(NullLocator())
    fig.legend([Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS], list(VARIANTS.values()),
               loc="outside upper left", ncol=2, handlelength=1.6)
    fig.legend([Line2D([], [], color=INK2, markersize=3, **{"linewidth": 0.8, **MODE_STYLE[m]}) for m in modes], modes,
               loc="outside upper right", ncol=len(modes), handlelength=2.2, columnspacing=1.0)
    out = suspended_outdir(args)
    save_fig(fig, out, "suspended_interleaved" + sfx, args)
    with open(out / f"suspended_interleaved{sfx}.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["metric", "unit", "variant", "materialization", "elements_inserted", "value"])
        wr.writerows(csv_rows)


def fig_suspended_interleaved_two(args, runs, sfx=""):
    """Online-suspended interleaved, insertions only: insertion throughput and I/Os per insertion per batch
    (each batch ends with consolidate_structure()) over the number of inserted elements. Colour = BOA / BOA-PV,
    line style and marker = materialization mode (online, hybrid, sort)."""
    from matplotlib.lines import Line2D
    modes = [m for m in ("online", "hybrid", "sort") if any(r["mode"] == m for r in runs)]
    cols = [
        ("Insertion throughput", "M elem./s", lambda b: b["n_ins"] / b["t_ins"] / 1e3),
        ("Insertion I/Os", "per 1k ins.", lambda b: b["ios_ins"] / b["n_ins"] * 1e3),
    ]
    fig, axes = plt.subplots(1, 2, figsize=(args.width, 0.3 * args.width), constrained_layout=True)
    csv_rows = []
    for ax, (title, unit, f) in zip(axes, cols):
        for v, vname in VARIANTS.items():
            for m in modes:
                rs = [r for r in runs if r["variant"] == v and r["mode"] == m]
                if not rs:
                    continue
                gb = [group_batches(r["batches"], args.batch_group) for r in rs]
                k = min(len(g) for g in gb)
                y = np.mean([[f(b) for b in g[:k]] for g in gb], axis=0)
                x = np.cumsum([b["n_ins"] for b in gb[0][:k]])
                ax.plot(x, y, color=VARIANT_COLORS[v], markersize=1.4, **{"linewidth": 0.8, **MODE_STYLE[m]})
                csv_rows += [[title, unit, vname, m, int(xi), yi] for xi, yi in zip(x, y)]
        ax.set_title(title, loc="left", fontsize=7.5, pad=2)
        ax.set_ylabel(unit, fontsize=6.5)
        ax.set_xlabel("Inserted elements", fontsize=7)
        ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
        ax.tick_params(axis="x", labelsize=6)
        if not args.linear_y:
            ax.set_yscale("log")
            ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
            ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            ax.yaxis.set_minor_locator(NullLocator())
            ax.yaxis.set_minor_formatter(NullFormatter())
    fig.legend([Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS], list(VARIANTS.values()),
               loc="outside upper left", ncol=2, handlelength=1.6)
    fig.legend([Line2D([], [], color=INK2, markersize=2, **{"linewidth": 0.8, **MODE_STYLE[m]}) for m in modes], modes,
               loc="outside upper right", ncol=len(modes), handlelength=2.2, columnspacing=1.0)
    out = suspended_outdir(args)
    name = "suspended_interleaved_two" + sfx
    save_fig(fig, out, name, args)
    with open(out / f"{name}.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["metric", "unit", "variant", "materialization", "elements_inserted", "value"])
        wr.writerows(csv_rows)


CUM_STYLE = {"online": dict(linestyle="-", marker=None, linewidth=1.5), "merge": dict(linestyle="-.", marker="o"),
             "sort": dict(linestyle="--", marker="s"), "hybrid": dict(linestyle=":", marker="^")}


def fig_suspended_cumulative(args, runs, sfx=""):
    """Repeated batches (staging log + consolidate_structure() after every batch) against continuous online
    insertion: cumulative insertion time, I/O wait, I/O count and bytes read + written over the number of
    inserted elements. Colour = BOA / BOA-PV; line = online (thick, solid) or materialization mode. Only the
    best mode per variant (lowest total insertion time) is shown."""
    from matplotlib.lines import Line2D
    # only the best materialization mode per variant (lowest total insertion time), plus the online baseline
    total = lambda rs: np.mean([sum(b["t_ins"] for b in r["batches"]) for r in rs])
    keep = []
    for v in VARIANTS:
        cand = {m: [r for r in runs if r["variant"] == v and r["mode"] == m] for m in ("merge", "sort", "hybrid")}
        cand = {m: rs for m, rs in cand.items() if rs}
        if cand:
            best = min(cand, key=lambda m: total(cand[m]))
            print(f"best materialization for {VARIANTS[v]}: {best} ({total(cand[best]) / 1e3:.2f} s)")
            keep += [r for r in runs if r["variant"] == v and r["mode"] == best]
        keep += [r for r in runs if r["variant"] == v and r["mode"] == "online"]
    runs = keep
    modes = [m for m in ("online", "merge", "sort", "hybrid") if any(r["mode"] == m for r in runs)]
    cols = [
        ("Cumulative time", "s", lambda b: b["t_ins"] / 1e3),
        ("Cumulative I/O wait", "s", lambda b: b["wait_ins"] / 1e3),
        ("Cumulative I/O count", "thousands", lambda b: b["ios_ins"] / 1e3),
        ("Cumulative bytes", "GiB", lambda b: b["btot_ins"] / 2**30),
    ]
    fig, axes = plt.subplots(1, len(cols), figsize=(args.width, 0.27 * args.width), constrained_layout=True)
    csv_rows = []
    for ax, (title, unit, f) in zip(axes, cols):
        for v, vname in VARIANTS.items():
            for m in modes:
                rs = [r for r in runs if r["variant"] == v and r["mode"] == m]
                if not rs:
                    continue
                k = min(len(r["batches"]) for r in rs)
                y = np.mean([np.cumsum([f(b) for b in r["batches"][:k]]) for r in rs], axis=0)
                x = np.cumsum([b["n_ins"] for b in rs[0]["batches"][:k]])
                st = dict(CUM_STYLE[m])
                st.setdefault("linewidth", 0.8)
                ax.plot(x, y, color=VARIANT_COLORS[v], markersize=2.2, **st)
                csv_rows += [[title, unit, vname, m, int(xi), yi] for xi, yi in zip(x, y)]
        ax.set_title(title, loc="left", fontsize=7.5, pad=2)
        ax.set_ylabel(unit, fontsize=6.5)
        ax.set_xlabel("Inserted elements", fontsize=7)
        ax.xaxis.set_major_formatter(FuncFormatter(count_fmt))
        ax.tick_params(axis="x", labelsize=6)
        ax.set_ylim(bottom=0)
    fig.legend([Line2D([], [], color=VARIANT_COLORS[v], linewidth=2) for v in VARIANTS], list(VARIANTS.values()),
               loc="outside upper left", ncol=2, handlelength=1.6)
    fig.legend([Line2D([], [], color=INK2, markersize=3, **{"linewidth": 0.8, **CUM_STYLE[m]}) for m in modes], modes,
               loc="outside upper right", ncol=len(modes), handlelength=2.2, columnspacing=1.0)
    out = suspended_outdir(args)
    save_fig(fig, out, "suspended_cumulative" + sfx, args)
    with open(out / f"suspended_cumulative{sfx}.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["metric", "unit", "variant", "materialization", "elements_inserted", "value"])
        wr.writerows(csv_rows)
    if "online" not in modes:
        print("note: no continuous online interleaved logs next to the static ones (baseline missing)")


FIGURES = [fig_insertion_throughput, fig_query_throughput, fig_tradeoff, fig_costs, fig_costs_over_time,
           lambda a, r: fig_costs_over_time(a, r, wide=True)]


def main():
    args = base_args(__doc__).parse_args()
    paper_style()
    runs = load_runs(args.results_dir)
    inter = load_interleaved(args.results_dir)
    stat, sinter = load_suspended(args.results_dir)
    mults = sorted({r["rf_mult"] for r in stat + sinter})
    for m in mults:  # one set of figures per routing filter multiplier (alpha = 1 / multiplier)
        sfx = f"_alpha{1 / m:g}" if len(mults) > 1 else ""
        sub = lambda rs: [r for r in rs if r["rf_mult"] == m]
        if sub(stat):
            fig_suspended_insert_then_query(args, sub(stat), sfx)
            fig_suspended_insert_then_query(args, sub(stat), sfx, row=True)
            fig_suspended_insert_then_query(args, sub(stat), sfx, two=True)
        if sub(sinter):
            fig_suspended_interleaved(args, sub(sinter), sfx)
            fig_suspended_interleaved_two(args, sub(sinter), sfx)
            fig_suspended_cumulative(args, sub(sinter), sfx)
    if not runs and not inter:
        if stat or sinter:
            return
        sys.exit(f"no usable logs in {args.results_dir}")
    if runs:
        for f in FIGURES:
            f(args, runs)
    swept = [k for k in ("block", "lam", "buf") if len({r[k] for r in inter if r[k]}) > 1]
    for k in swept:  # block size / lambda sweep
        fig_block_sizes(args, inter, key=k)
        fig_block_sizes_combined(args, inter, key=k,
                                 sizes=(4096, 65536, 524288, 2097152) if k == "block" else sorted({r[k] for r in inter}))
    pg = [r for r in inter if r["qdist"] == "uniform" and r["pages"]]
    if len({r["pages"] for r in pg}) > 1:  # memory budget: number of cached pages (uniform queries)
        pages = sorted({r["pages"] for r in pg})
        for full in (False, True):
            fig_block_sizes(args, pg, key="pages", full=full)
            fig_block_sizes_combined(args, pg, key="pages", sizes=pages, full=full)
        swept.append("pages")
    order = ["uniform", "zipf", "hotspot", "latest"]
    for npg in sorted({r["pages"] for r in inter if r["pages"]}):  # query distributions at one page count
        qd = [r for r in inter if r["pages"] == npg and r["qdist"] in order]
        if len({r["qdist"] for r in qd}) > 1:
            for full in (False, True):
                fig_block_sizes(args, qd, key="qdist", full=full, extra=f"_pages{npg}")
                fig_block_sizes_combined(args, qd, key="qdist", sizes=order, full=full, extra=f"_pages{npg}")
            if "qdist" not in swept:
                swept.append("qdist")
    if any(r["qdist"] for r in inter):
        fig_pages_query_gain(args, inter)
    if "buf" in swept:
        fig_buffer_bars(args, inter)
        fig_buffer_bursts(args, inter)  # buffer sweep: also the versions with data moved and I/O wait
        fig_block_sizes(args, inter, key="buf", full=True)
        fig_block_sizes_combined(args, inter, key="buf", full=True, sizes=sorted({r["buf"] for r in inter if r["buf"]}))
    if inter and not swept:
        fig_interleaved(args, inter)
        fig_interleaved(args, inter, tall=True)
        fig_interleaved(args, inter, row_layout=True)


if __name__ == "__main__":
    main()
