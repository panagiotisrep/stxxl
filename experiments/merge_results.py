#!/usr/bin/env python3
"""Copy the results of several run_all.sh folders into one folder (nothing is deleted or overwritten in the sources) and
rebuild its summary.txt and table.txt, so that the plot scripts can read every run from one place.
usage: merge_results.py DEST SRC [SRC ...]    (a run that appears in several sources: the later source wins)"""
import re, shutil, sys
from pathlib import Path

dest = Path(sys.argv[1]); dest.mkdir(parents=True, exist_ok=True)
summ, table, sh, th = {}, {}, None, None
for src in map(Path, sys.argv[2:]):
    if not (src / "summary.txt").exists():
        continue
    for f in src.iterdir():
        if f.suffix in (".txt", ".out") and f.name not in ("summary.txt", "table.txt", "params.txt", "params_file.txt", "environment.txt", "experiment_description.txt"):
            shutil.copy2(f, dest / f.name)
    lines = (src / "summary.txt").read_text().splitlines()
    sh = sh or lines[0]
    for l in lines[1:]:
        if l.split():
            summ[l.split()[0]] = l
    if (src / "table.txt").exists():
        lines = (src / "table.txt").read_text().splitlines()
        th = th or lines[0]
        for l in lines[1:]:
            if l.split():
                table.setdefault(l.split()[0], {})[l.split("(il)")[0] if "(il)" in l else l[:40]] = l
        for run in {l.split()[0] for l in lines[1:] if l.split()}:
            table[run] = {k: v for k, v in table[run].items()}
    for name in ("params.txt", "environment.txt"):
        if (src / name).exists():
            shutil.copy2(src / name, dest / (src.name + "_" + name))
(dest / "summary.txt").write_text("\n".join([sh or ""] + list(summ.values())) + "\n")
(dest / "table.txt").write_text("\n".join([th or ""] + [l for r in table.values() for l in r.values()]) + "\n")
print("merged", len(summ), "runs into", dest)
