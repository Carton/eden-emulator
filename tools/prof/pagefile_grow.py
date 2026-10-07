"""Explicit admin-only live pagefile extension; never changes boot configuration.

Run only after user authorization. Active pagefiles cannot be shrunk by this tool;
the unchanged Windows boot configuration takes effect on the next normal reboot.
Native ABI reference: winsiderss/systeminformer phnt/include/ntmmapi.h.
"""

import argparse
import ctypes as ct
import os
import winreg
from pathlib import Path

from memory_watch import WindowsProcess, utc_now
from prof_common import positive_int, writable_target, write_json


class UnicodeString(ct.Structure):
    _fields_ = [("Length", ct.c_ushort), ("MaximumLength", ct.c_ushort), ("Buffer", ct.c_void_p)]


def paging_config():
    with winreg.OpenKey(
        winreg.HKEY_LOCAL_MACHINE,
        r"SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management",
    ) as key:
        return winreg.QueryValueEx(key, "PagingFiles")[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, required=True)
    parser.add_argument("--size-mib", type=positive_int, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory")
    args = parser.parse_args(argv)
    path = args.file.resolve()
    if os.name != "nt" or path.name.lower() != "pagefile.sys" or not path.is_absolute():
        parser.error("Expected an absolute path to an existing Windows pagefile.sys")
    if args.size_mib > 65536:
        parser.error("Size must be <=65536 MiB")
    directory = writable_target(args.output)
    directory.mkdir(parents=True, exist_ok=False)
    state = {
        "file": str(path),
        "size_mib": args.size_mib,
        "started_utc": utc_now(),
        "paging_files_before": paging_config(),
        "status": "pending",
    }
    write_json(directory / "result.json", state)
    process = WindowsProcess(os.getpid())
    native = ct.WinDLL("ntdll")
    native.RtlAdjustPrivilege.argtypes = [
        ct.c_uint32,
        ct.c_ubyte,
        ct.c_ubyte,
        ct.POINTER(ct.c_ubyte),
    ]
    native.RtlAdjustPrivilege.restype = ct.c_uint32
    native.NtCreatePagingFile.argtypes = [
        ct.POINTER(UnicodeString),
        ct.POINTER(ct.c_int64),
        ct.POINTER(ct.c_int64),
        ct.c_uint32,
    ]
    native.NtCreatePagingFile.restype = ct.c_uint32
    previous = ct.c_ubyte()
    enabled = False
    try:
        state["system_before"] = process.counters()
        status = native.RtlAdjustPrivilege(15, 1, 0, ct.byref(previous))
        if status:
            raise RuntimeError(
                f"SeCreatePagefilePrivilege failed: 0x{status:08X}; run as administrator"
            )
        enabled = True
        name = ct.create_unicode_buffer("\\??\\" + str(path))
        string = UnicodeString((len(name.value)) * 2, ct.sizeof(name), ct.cast(name, ct.c_void_p))
        size = ct.c_int64(args.size_mib * 1048576)
        status = native.NtCreatePagingFile(ct.byref(string), ct.byref(size), ct.byref(size), 0)
        state["ntstatus"] = f"0x{status:08X}"
        if status:
            raise RuntimeError(f"NtCreatePagingFile failed: 0x{status:08X}")
        state["system_after"] = process.counters()
        state["paging_files_after"] = paging_config()
        if state["paging_files_after"] != state["paging_files_before"]:
            raise RuntimeError("Persistent paging settings changed unexpectedly")
        if (
            state["system_after"]["system_commit_limit_bytes"]
            <= state["system_before"]["system_commit_limit_bytes"]
        ):
            raise RuntimeError("Commit limit did not increase")
        state["status"] = "extended_live"
        state["restore"] = (
            "Boot settings unchanged; current enlarged file persists until normal reboot. No reboot initiated."
        )
    except Exception as error:
        state.update(status="failed", error=str(error))
        raise
    finally:
        if enabled:
            native.RtlAdjustPrivilege(15, previous.value, 0, ct.byref(ct.c_ubyte()))
        process.close()
        state["ended_utc"] = utc_now()
        write_json(directory / "result.json", state)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
