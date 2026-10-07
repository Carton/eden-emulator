"""Read-only Windows process/commit sampling. Never launches or stops the target."""

import argparse
import csv
import ctypes as ct
import hashlib
import json
import os
import time
import uuid
from ctypes import wintypes as wt
from datetime import UTC, datetime
from pathlib import Path

from prof_common import data_dir, positive_int, writable_target


class ProcessCounters(ct.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [
        (name, ct.c_size_t)
        for name in (
            "PeakWorkingSetSize",
            "WorkingSetSize",
            "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage",
            "QuotaPeakNonPagedPoolUsage",
            "QuotaNonPagedPoolUsage",
            "PagefileUsage",
            "PeakPagefileUsage",
            "PrivateUsage",
        )
    ]


class PerformanceInfo(ct.Structure):
    _fields_ = (
        [("cb", wt.DWORD)]
        + [
            (name, ct.c_size_t)
            for name in (
                "CommitTotal",
                "CommitLimit",
                "CommitPeak",
                "PhysicalTotal",
                "PhysicalAvailable",
                "SystemCache",
                "KernelTotal",
                "KernelPaged",
                "KernelNonpaged",
                "PageSize",
            )
        ]
        + [(name, wt.DWORD) for name in ("HandleCount", "ProcessCount", "ThreadCount")]
    )


class MemoryRegion(ct.Structure):
    _fields_ = [
        ("BaseAddress", ct.c_void_p),
        ("AllocationBase", ct.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ct.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def utc_now():
    return datetime.now(UTC).isoformat()


class WindowsProcess:
    def __init__(self, pid):
        if os.name != "nt" or ct.sizeof(ct.c_void_p) != 8:
            raise RuntimeError("Use 64-bit Python on Windows")
        self.kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.psapi = ct.WinDLL("psapi", use_last_error=True)
        for dll, name, arguments, result in (
            (self.kernel, "OpenProcess", [wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
            (self.kernel, "CloseHandle", [wt.HANDLE], wt.BOOL),
            (self.kernel, "WaitForSingleObject", [wt.HANDLE, wt.DWORD], wt.DWORD),
            (
                self.kernel,
                "QueryFullProcessImageNameW",
                [wt.HANDLE, wt.DWORD, wt.LPWSTR, ct.POINTER(wt.DWORD)],
                wt.BOOL,
            ),
            (self.kernel, "GetProcessTimes", [wt.HANDLE] + [ct.POINTER(wt.FILETIME)] * 4, wt.BOOL),
            (
                self.kernel,
                "VirtualQueryEx",
                [wt.HANDLE, ct.c_void_p, ct.POINTER(MemoryRegion), ct.c_size_t],
                ct.c_size_t,
            ),
            (
                self.psapi,
                "GetProcessMemoryInfo",
                [wt.HANDLE, ct.POINTER(ProcessCounters), wt.DWORD],
                wt.BOOL,
            ),
            (self.psapi, "GetPerformanceInfo", [ct.POINTER(PerformanceInfo), wt.DWORD], wt.BOOL),
        ):
            function = getattr(dll, name)
            function.argtypes, function.restype = arguments, result
        self.handle = self.kernel.OpenProcess(0x100000 | 0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise ct.WinError(ct.get_last_error())

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def alive(self):
        status = self.kernel.WaitForSingleObject(self.handle, 0)
        if status == 0xFFFFFFFF:
            raise ct.WinError(ct.get_last_error())
        return status == 258

    def identity(self):
        buffer = ct.create_unicode_buffer(32768)
        length = wt.DWORD(len(buffer))
        self._check(
            self.kernel.QueryFullProcessImageNameW(self.handle, 0, buffer, ct.byref(length))
        )
        times = [wt.FILETIME() for _ in range(4)]
        self._check(self.kernel.GetProcessTimes(self.handle, *(ct.byref(t) for t in times)))
        creation = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return {"image_path": buffer.value, "creation_filetime": creation}

    @staticmethod
    def _check(result):
        if not result:
            raise ct.WinError(ct.get_last_error())

    def counters(self):
        process, system = ProcessCounters(), PerformanceInfo()
        process.cb, system.cb = ct.sizeof(process), ct.sizeof(system)
        self._check(self.psapi.GetProcessMemoryInfo(self.handle, ct.byref(process), process.cb))
        self._check(self.psapi.GetPerformanceInfo(ct.byref(system), system.cb))
        page = system.PageSize
        return {
            "private_bytes": process.PrivateUsage,
            "working_set_bytes": process.WorkingSetSize,
            "peak_private_bytes": process.PeakPagefileUsage,
            "system_commit_bytes": system.CommitTotal * page,
            "system_commit_limit_bytes": system.CommitLimit * page,
            "system_commit_available_bytes": max(0, system.CommitLimit - system.CommitTotal) * page,
            "physical_available_bytes": system.PhysicalAvailable * page,
        }

    def regions(self):
        result = {
            "private_committed_virtual_bytes": 0,
            "mapped_committed_virtual_bytes": 0,
            "image_committed_virtual_bytes": 0,
            "reserved_virtual_bytes": 0,
            "regions": 0,
        }
        address = 0
        while True:
            region = MemoryRegion()
            count = self.kernel.VirtualQueryEx(
                self.handle, address, ct.byref(region), ct.sizeof(region)
            )
            if not count:
                error = ct.get_last_error()
                if error == 87:  # End of accessible user address space.
                    break
                raise ct.WinError(error)
            if region.State == 0x1000:
                name = {0x20000: "private", 0x40000: "mapped", 0x1000000: "image"}.get(region.Type)
                if name:
                    result[name + "_committed_virtual_bytes"] += region.RegionSize
            elif region.State == 0x2000:
                result["reserved_virtual_bytes"] += region.RegionSize
            result["regions"] += 1
            next_address = (region.BaseAddress or 0) + region.RegionSize
            if next_address <= address:
                raise RuntimeError("VirtualQueryEx did not advance")
            address = next_address
        return result


def positive(value):
    value = float(value)
    if not 0 < value < float("inf"):
        raise argparse.ArgumentTypeError("Must be finite and positive")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=positive_int, required=True)
    parser.add_argument("--seconds", type=positive, default=3600)
    parser.add_argument("--interval", type=positive, default=5)
    parser.add_argument("--regions-every", type=positive, default=60)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    directory = writable_target(args.output or data_dir() / ("memory-" + uuid.uuid4().hex[:12]))
    directory.mkdir(parents=True, exist_ok=False)
    target = None
    metadata = {"pid": args.pid, "started_utc": utc_now(), "status": "sampling"}
    metadata_path = directory / "metadata.json"
    start = time.monotonic()
    try:
        target = WindowsProcess(args.pid)
        metadata.update(target.identity())
        image = Path(metadata["image_path"])
        with image.open("rb") as stream:
            metadata["disk_image_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        metadata["identity_note"] = (
            "Disk hash does not prove loaded image identity if exe was replaced while running."
        )
        metadata["regions_note"] = (
            "Virtual region sums are not unique physical/commit ownership; aliased views may count multiple times. Reserve is not commit."
        )
        metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
        print(directory, flush=True)
        with (
            (directory / "memory.csv").open("x", newline="", encoding="utf-8") as stream,
            (directory / "regions.jsonl").open("x", encoding="utf-8") as regions,
        ):
            writer = None
            next_regions = 0
            # Image hashing / metadata setup must not consume the sampling window.
            start = time.monotonic()
            metadata["sampling_started_utc"] = utc_now()
            while time.monotonic() - start < args.seconds:
                if not target.alive():
                    metadata["status"] = "target_exited"
                    break
                elapsed = time.monotonic() - start
                row = dict(utc=utc_now(), elapsed_seconds=round(elapsed, 3), **target.counters())
                if writer is None:
                    writer = csv.DictWriter(stream, fieldnames=list(row))
                    writer.writeheader()
                writer.writerow(row)
                stream.flush()
                if elapsed >= next_regions:
                    regions.write(
                        json.dumps(dict(utc=utc_now(), elapsed_seconds=elapsed, **target.regions()))
                        + "\n"
                    )
                    regions.flush()
                    next_regions = elapsed + args.regions_every
                time.sleep(min(args.interval, max(0, args.seconds - (time.monotonic() - start))))
            else:
                metadata["status"] = "duration_completed"
    except KeyboardInterrupt:
        metadata["status"] = "interrupted"
        return 130
    except Exception as error:
        metadata.update(status="failed", error=str(error))
        raise
    finally:
        if target is not None:
            target.close()
        metadata["ended_utc"] = utc_now()
        metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
