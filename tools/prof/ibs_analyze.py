"""Per-thread x per-function IBS analysis from a uProf session's cpu.db.

cpu.db (DuckDB, auto-generated next to the rawdata) has UnifiedSampleSeries:
one row per sample with threadId/functionId/moduleId and per-sample event
columns. Event id -> metric mapping below was validated against report.csv
aggregates on ibs-rot5 (exact match on GPU-thread totals: ops 554816, BR
106035, LOAD 139053, STORE 100582, LD_MISS_LAT 1243551, TAG_TO_RETIRE
53389508). report.csv itself cannot break functions down per thread; this
script can.

Unresolved samples (isResolved=false, anonymous exec memory = dynarmic JIT
guest code) are reported as a separate '<JIT/anon>' bucket - for CPUCore
threads that bucket IS the guest-code share.

Usage: python ibs_analyze.py F:\\prof\\uprof\\ibs-<label> [--label rot6]
"""

import argparse
import glob
import json
import os

import duckdb

EV = {
    "ops": "event#0x100000000f100",
    "ttr": "event#0x100000000f101",
    "br": "event#0x100000000f103",
    "misp": "event#0x100000000f104",
    "load": "event#0x100000000f201",
    "store": "event#0x100000000f202",
    "ld_miss": "event#0x100000000f221",
    "ld_miss_lat": "event#0x100000000f219",
    "st_miss": "event#0x100000000f21e",
    "dtlb_lat": "event#0x100000000f225",
    "misp_ttr": "event#0x100000000f110",
}
SEL = ", ".join(f'sum("{v}") AS {k}' for k, v in EV.items())


def load_names(session_dir, label):
    names = {}
    if label:
        for path in sorted(glob.glob(os.path.join(session_dir, "..", "..", "runs",
                                                  f"{label}-*", "thread_snapshot_start.json")),
                           key=os.path.getmtime, reverse=True):
            with open(path, encoding="utf-8") as fh:
                for tid, info in json.load(fh).items():
                    names[int(tid)] = info.get("name") or ""
            break
    return names


def pct(x, total):
    return 100.0 * x / total if total else 0.0


def fmt_thread(label):
    return label


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session_dir")
    parser.add_argument("--label", help="bench label to find thread_snapshot_start.json")
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args(argv)
    names = load_names(args.session_dir, args.label)

    con = duckdb.connect(os.path.join(args.session_dir, "cpu.db"), read_only=True)
    opfilter = f'"{EV["ops"]}" != 0'

    print("=" * 30, "THREAD SUMMARY (op samples)", "=" * 30)
    print(f"{'thread':>28} {'ops':>9} {'share%':>6} {'jit%':>5} {'misp%':>6} {'ldmiss%':>7} "
          f"{'misslatcyc%':>11} {'dtlb%':>6} {'avgmisslat':>10}")
    threads = con.execute(f"""
        select threadId, {SEL.replace('sum(', 'sum(case when true then ', 1) if False else SEL}
        from UnifiedSampleSeries where {opfilter} group by threadId order by ops desc""").fetchall()
    jit = dict(con.execute(f"""
        select threadId, count(*) from UnifiedSampleSeries
        where {opfilter} and not isResolved group by threadId""").fetchall())
    total_ops = sum(t[1] for t in threads)
    interesting = []
    for row in threads:
        tid, ops, ttr, br, misp, ld, st, ldm, ldml, stm, dtlb, misp_ttr = row
        if ops < total_ops * 0.005:
            continue
        name = names.get(tid, "")
        label = f"{name}({tid})" if name else f"TID {tid}"
        if ops >= total_ops * 0.02 or "GPU" in name or "CPUCore" in name:
            interesting.append(tid)
        print(f"{label:>28} {ops:9.0f} {pct(ops, total_ops):6.2f} {pct(jit.get(tid, 0), ops):5.1f} "
              f"{pct(misp, br):6.2f} {pct(ldm, ld):7.2f} {pct(ldml, ttr):11.2f} {pct(dtlb, ttr):6.2f} "
              f"{ldml / ldm if ldm else 0:10.1f}")

    for tid in interesting[:8]:
        name = names.get(tid, "")
        label = f"{name}({tid})" if name else f"TID {tid}"
        tot = con.execute(f"""
            select {SEL} from UnifiedSampleSeries
            where threadId = {tid} and {opfilter}""").fetchone()
        ops_t = tot[0]
        print()
        print("=" * 25, f"TOP FUNCTIONS - {label} (jit/anon {pct(jit.get(tid, 0), ops_t):.1f}%)", "=" * 25)
        rows = con.execute(f"""
            select f.name, coalesce(m.path, '?'), {SEL}
            from UnifiedSampleSeries s
            join Function f on f.id = s.functionId
            left join Module m on m.id = f.moduleId
            where s.threadId = {tid} and {opfilter} and s.isResolved
            group by f.name, m.path order by ops desc limit {args.top}""").fetchall()
        print(f"{'function':<70} {'ops':>7} {'%thr':>5} {'misp%':>6} {'ldm%':>5} "
              f"{'mlc%':>5}  module")
        for r in rows:
            fname, mpath, ops, ttr, br, misp, ld, st, ldm, ldml, stm, dtlb, misp_ttr = r
            mod = os.path.basename(mpath or "?")
            short = (fname[:68] + "..") if len(fname) > 70 else fname
            print(f"{short:<70} {ops:7.0f} {pct(ops, ops_t):5.1f} {pct(misp, br) if br else 0:6.2f} "
                  f"{pct(ldm, ld) if ld else 0:5.1f} {pct(ldml, ttr):5.2f}  {mod}")

    print()
    print("=" * 25, "MODULE SPLIT per interesting thread (incl JIT/anon)", "=" * 25)
    for tid in interesting[:8]:
        rows = con.execute(f"""
            select coalesce(m.path, '<JIT/anon>'), sum("{EV['ops']}") ops
            from UnifiedSampleSeries s
            left join Function f on f.id = s.functionId
            left join Module m on m.id = f.moduleId
            where s.threadId = {tid} and {opfilter}
            group by m.path order by ops desc limit 7""").fetchall()
        name = names.get(tid, "")
        label = f"{name}({tid})" if name else f"TID {tid}"
        tot = sum(r[1] for r in rows) or 1
        print(f"{label}: " + " | ".join(
            f"{r[0] if r[0].startswith('<') else os.path.basename(r[0])} {100*r[1]/tot:.1f}%" for r in rows))

    print()
    print("=" * 25, "PROCESS-WIDE: cache-heaviest resolved functions", "=" * 25)
    rows = con.execute(f"""
        select f.name, m.path, {SEL}
        from UnifiedSampleSeries s
        join Function f on f.id = s.functionId
        join Module m on m.id = f.moduleId
        where {opfilter} and s.isResolved
        group by f.name, m.path order by ld_miss_lat desc limit {args.top}""").fetchall()
    proc = con.execute(
        f'select sum("{EV["ttr"]}") from UnifiedSampleSeries where {opfilter}').fetchone()[0]
    print(f"{'function':<70} {'ops':>7} {'ldmiss':>7} {'misslat':>9} {'avglat':>7} {'mlc%':>5}")
    for r in rows:
        fname, mpath, ops, ttr, br, misp, ld, st, ldm, ldml, stm, dtlb, misp_ttr = r
        short = (fname[:68] + "..") if len(fname) > 70 else fname
        print(f"{short:<70} {ops:7.0f} {ldm:7.0f} {ldml:9.0f} "
              f"{ldml/ldm if ldm else 0:7.1f} {pct(ldml, proc):5.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
