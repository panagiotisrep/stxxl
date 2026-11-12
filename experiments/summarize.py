#!/usr/bin/env python3
"""Per-phase table from the logs of one experiments/run_all.sh run.

Usage: experiments/summarize.py results/run_<SIZE>_<date> [> table.txt]

For every <run>.txt log it prints, per phase (Insertions and each query distribution):
  (LevelDB/RocksDB also have a "compaction" phase: the full compaction before the queries;
   their insertion time for throughput is insert + compaction)
  time_s       phase time from the log ("End <phase>, time <ms>")
  read_MB      bytes read from the device in the phase
  write_MB     bytes written to the device in the phase
  reads/op     read requests per operation (BOA, STXXL: STXXL block reads;
               competitors: read syscalls from /proc/self/io)
  KB_read/op   bytes read per operation, in KiB
BOA phases use STXXL's counters: the "STXXL I/O statistics" block printed after "End <phase>"
(counters are reset per phase; this includes work done after the last progress line, e.g. the
online-suspended materialization). Competitor phases use the "Phase I/O" line (OS read_bytes/write_bytes from
/proc/self/io). Interleaved runs (--interleaved): one "insert (il)" and one "queries (il)" row, the sums over all
batches of insertions and of queries (the queries of a batch go to keys inserted so far).
Runs without an "End" line (timeout, out of memory) are listed as incomplete.
"""
import re, sys, pathlib

def parse(path):
    lines = path.read_text(errors="replace").splitlines()
    phases, last_details, ops = [], None, {}
    elements = next((int(l.split()[1]) for l in lines if l.startswith("Elements: ")), 0)
    for i, line in enumerate(lines):
        m = re.match(r"^(Insertions|Queries) (\d+), time .*details: (.*)", line)
        if m:
            last_details = m.group(3)
            ops["cur"] = int(m.group(2))
            continue
        m = re.match(r"^(?:End (Insertions|Queries (\S+))|(Compaction)), time ([-\d.e+]+)", line)
        if not m:
            continue
        name = "compaction" if m.group(3) else "insert" if m.group(1) == "Insertions" else m.group(2)
        row = {"phase": name, "time_s": float(m.group(4)) / 1000, "ops": ops.get("cur") or (elements if name in ("insert", "compaction") else 0)}
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        p = re.match(r"Phase I/O: OS bytes read (\d+), OS bytes written (\d+), read syscalls (\d+)", nxt)
        if p:
            row.update(read=int(p.group(1)), write=int(p.group(2)), reads=int(p.group(3)))
        elif nxt.startswith("STXXL I/O statistics"):
            block = "\n".join(lines[i + 2:i + 12])
            g = lambda k: int(re.search(k + r"\s*:\s*(\d+)", block).group(1))
            row.update(read=g("number of bytes read from disks"), write=g("number of bytes written to disks"),
                       reads=g("total number of reads"))
        elif last_details:
            d = dict((k.strip(), v) for k, v in
                     (kv.rsplit(" ", 1) for kv in last_details.split(", ") if " " in kv))
            try:
                row.update(read=int(d["bytes read"]), write=int(d["bytes written"]), reads=int(d["reads"]))
            except (KeyError, ValueError):
                pass
        phases.append(row)
        last_details, ops = None, {}
    complete = any(l.strip() == "End" for l in lines)
    return phases, complete

def io_fields(text):
    """bytes read, bytes written, read requests from a details text (BOA/STXXL or /proc/self/io style)."""
    def num(pattern):
        m = re.search(pattern, text)
        return int(m.group(1)) if m else None
    rd = num(r"OS bytes read (\d+)")
    if rd is not None:
        return rd, num(r"OS bytes written (\d+)"), num(r"read syscalls (\d+)")
    return num(r"bytes read (\d+)"), num(r"bytes written (\d+)"), num(r"(?:^|\[)reads (\d+)")

def parse_interleaved(lines):
    """Interleaved logs: sums over all "Batch N | ..." lines, as phases "insert (il)" and "queries (il)"."""
    ins = {"phase": "insert (il)", "time_s": 0.0, "ops": 0, "read": 0, "write": 0, "reads": 0}
    qry = {"phase": "queries (il)", "time_s": 0.0, "ops": 0, "read": 0, "write": 0, "reads": 0}
    batches = 0
    for line in lines:
        m = re.match(r"^Batch \d+ \| Insertions (\d+), Insertions time ([-\d.e+]+) ms, Queries (\d+), "
                     r"Queries time ([-\d.e+]+) ms.*insert details: \[([^\]]*)\], query details: \[([^\]]*)\]", line)
        if not m:
            continue
        batches += 1
        for row, n, t, det in ((ins, m.group(1), m.group(2), m.group(5)), (qry, m.group(3), m.group(4), m.group(6))):
            row["ops"] += int(n)
            row["time_s"] += float(t) / 1000
            for key, v in zip(("read", "write", "reads"), io_fields(det)):
                row[key] = None if v is None or row[key] is None else row[key] + v
    return [ins, qry] if batches else []

def main():
    out = pathlib.Path(sys.argv[1])
    order = [l.split()[0] for l in (out / "summary.txt").read_text().splitlines()[1:] if l.strip()] \
        if (out / "summary.txt").exists() else []
    files = sorted(out.glob("*.txt"), key=lambda f: (order.index(f.stem) if f.stem in order else 99, f.stem))
    print(f"{'run':22} {'phase':12} {'time_s':>9} {'read_MB':>10} {'write_MB':>9} {'reads/op':>9} {'KB_read/op':>10}")
    for f in files:
        if f.stem in ("summary", "params", "params_file", "environment", "table", "read"):
            continue
        phases, complete = parse(f)
        text = f.read_text(errors="replace")
        if text.startswith("Interleaved") or "\nBatch 1 | " in text:
            phases = parse_interleaved(text.splitlines())
        if not phases:
            print(f"{f.stem:22} (no completed phase)")
            continue
        for r in phases:
            ops = r["ops"] or 1
            rd, wr, n = r.get("read"), r.get("write"), r.get("reads")
            fmt = lambda v, s: f"{v / s:.1f}" if v is not None else "-"
            print(f"{f.stem:22} {r['phase']:12} {r['time_s']:9.1f} {fmt(rd, 2**20):>10} {fmt(wr, 2**20):>9} "
                  f"{(f'{n / ops:.2f}' if n is not None else '-'):>9} {fmt(rd, ops * 1024):>10}")
        if not complete:
            print(f"{f.stem:22} (incomplete: no final 'End')")

if __name__ == "__main__":
    main()
