"""Orchestrated uProf IBS capture over a rotation run (rot_capture.py).

Launches rot_capture.py as a child, waits for the first rotation window to
start, snapshots eden's thread names + per-thread CPU times (GetThreadDescription
via ctypes; uProf's own ProcessThread.threadName is empty for eden), attaches
AMDuProfCLI IBS sampling for the duration of the rotation windows, snapshots
thread times again at the end, and finally generates report.csv.

The thread-name snapshot is what makes per-thread function attribution from
cpu.db (DuckDB) actionable; without it TIDs cannot be mapped to CPUCore_*/GPU.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import subprocess
import sys
import time

from prof_common import HERE, data_dir, label_value, positive_int

UPROF = r"D:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe"
UPROF_ROOT = data_dir() / "uprof"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPTHREAD = 0x4
THREAD_QUERY_LIMITED_INFORMATION = 0x0800


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD),
        ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wt.DWORD),
    ]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Thread32First.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.Thread32Next.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetThreadDescription.restype = ctypes.HRESULT
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.GetThreadTimes.argtypes = [
    wt.HANDLE,
    ctypes.POINTER(wt.FILETIME),
    ctypes.POINTER(wt.FILETIME),
    ctypes.POINTER(wt.FILETIME),
    ctypes.POINTER(wt.FILETIME),
]
k32.LocalFree.argtypes = [ctypes.c_void_p]


def _filetime_seconds(ft):
    return (ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7


def snapshot_threads(pid):
    """Return {tid: {"name": str, "cpu_seconds": float}} for threads of pid."""
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snap == wt.HANDLE(-1).value or not snap:
        raise OSError("CreateToolhelp32Snapshot failed")
    out = {}
    try:
        te = THREADENTRY32()
        te.dwSize = ctypes.sizeof(THREADENTRY32)
        ok = k32.Thread32First(snap, ctypes.byref(te))
        while ok:
            if te.th32OwnerProcessID == pid:
                h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, te.th32ThreadID)
                if h:
                    try:
                        name = ""
                        buf = ctypes.c_wchar_p()
                        k32.GetThreadDescription(h, ctypes.byref(buf))
                        # HRESULT is not reliably 0 on this build (self-test:
                        # returns 0x10000000 yet fetches the string) - trust buf.
                        if buf.value:
                            name = buf.value
                            k32.LocalFree(ctypes.cast(buf, ctypes.c_void_p))
                        c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
                        cpu = 0.0
                        if k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
                            cpu = _filetime_seconds(k) + _filetime_seconds(u)
                        out[te.th32ThreadID] = {"name": name, "cpu_seconds": cpu}
                    finally:
                        k32.CloseHandle(h)
            ok = k32.Thread32Next(snap, ctypes.byref(te))
    finally:
        k32.CloseHandle(snap)
    return out


def find_eden_pid(timeout_s=300):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-Process -Name eden -ErrorAction SilentlyContinue).Id"],
            capture_output=True, text=True, timeout=30)
        ids = [int(x) for x in r.stdout.split() if x.isdigit()]
        if len(ids) == 1:
            return ids[0]
        if ids:
            raise RuntimeError(f"multiple eden processes running: {ids}")
        time.sleep(2)
    raise RuntimeError("eden did not start within timeout")


def wait_rotation_start(label, timeout_s=240):
    """Wait for rot_capture's windows.json to appear with its first window."""
    deadline = time.time() + timeout_s
    pattern = str(data_dir() / "runs" / f"{label}-*" / "windows.json")
    while time.time() < deadline:
        for path in sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True):
            try:
                with open(path, encoding="utf-8") as fh:
                    doc = json.load(fh)
            except (OSError, json.JSONDecodeError):
                continue
            if doc.get("windows"):
                return path
        time.sleep(1)
    raise RuntimeError("rotation windows.json never appeared")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--win", type=positive_int, default=20)
    parser.add_argument("--reps", type=positive_int, default=2)
    parser.add_argument("--settle", type=positive_int, default=30)
    args = parser.parse_args(argv)
    duration = args.win * 4 * args.reps

    out_dir = UPROF_ROOT / f"ibs-{args.label}"
    suffix = 2
    while out_dir.exists():
        out_dir = UPROF_ROOT / f"ibs-{args.label}-{suffix}"
        suffix += 1
    out_dir.parent.mkdir(parents=True, exist_ok=True)

    rot_log = open(out_dir.with_suffix(".rotlog"), "w", encoding="utf-8", errors="replace")
    rot = subprocess.Popen(
        [sys.executable, str(HERE / "rot_capture.py"), args.label,
         "--win", str(args.win), "--reps", str(args.reps), "--settle", str(args.settle)],
        cwd=str(HERE), stdout=rot_log, stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    print(f"rot_capture pid={rot.pid} log={rot_log.name}", flush=True)
    try:
        pid = find_eden_pid()
        print(f"eden pid={pid}", flush=True)
        markers = wait_rotation_start(args.label)
        run_dir = os.path.dirname(markers)
        print(f"rotation started; markers={markers}", flush=True)
        t0 = snapshot_threads(pid)
        with open(os.path.join(run_dir, "thread_snapshot_start.json"), "w", encoding="utf-8") as fh:
            json.dump(t0, fh, indent=1)

        collect_log = open(out_dir.with_suffix(".collectlog"), "w", encoding="utf-8", errors="replace")
        collect = subprocess.Popen(
            [UPROF, "collect", "--config", "ibs", "--pid", str(pid),
             "-d", str(duration), "-o", str(out_dir)],
            stdout=collect_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        rc = collect.wait(timeout=duration + 120)
        collect_log.close()
        print(f"uProf collect rc={rc}", flush=True)
        t1 = snapshot_threads(pid)
        with open(os.path.join(run_dir, "thread_snapshot_end.json"), "w", encoding="utf-8") as fh:
            json.dump(t1, fh, indent=1)
    except Exception as exc:
        print(f"FATAL during capture: {exc}", flush=True)
        if rot.poll() is None:
            rot.kill()
        rot_log.close()
        return 2

    rot_rc = rot.wait(timeout=duration + 300)
    rot_log.close()
    print(f"rot_capture rc={rot_rc}", flush=True)
    if rot_rc != 0:
        print("rotation run FAILED (see rotlog); IBS data still collected but unvalidated", flush=True)
        return 3

    report_log = open(out_dir.with_suffix(".reportlog"), "w", encoding="utf-8", errors="replace")
    report = subprocess.run(
        [UPROF, "report", "-i", str(out_dir), "--detail", "-s", "event=ibs-op"],
        stdout=report_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
        timeout=900)
    report_log.close()
    print(f"uProf report rc={report.returncode} csv={out_dir / 'report.csv'}", flush=True)
    print(f"run_dir={run_dir}", flush=True)
    return 0 if report.returncode == 0 else 4


if __name__ == "__main__":
    raise SystemExit(main())
