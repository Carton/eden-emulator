import ctypes as ct
import json
import os
import tempfile
import unittest
from ctypes import wintypes as wt
from pathlib import Path
from unittest.mock import patch

import memory_watch as watch


class MemoryWatchUnitTests(unittest.TestCase):
    def test_cli_persists_counters_identity_and_closes_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "test.exe"
            image.write_bytes(b"test image")
            output = root / "capture"
            target = unittest.mock.Mock()
            target.identity.return_value = {"image_path": str(image), "creation_filetime": 123}
            target.counters.return_value = {"private_bytes": 1024, "system_commit_bytes": 2048}
            target.regions.return_value = {"private_committed_virtual_bytes": 1024}
            with patch.object(watch, "WindowsProcess", return_value=target):
                self.assertEqual(
                    watch.main(["--pid", "42", "--seconds", "0.01", "--output", str(output)]), 0
                )
            target.close.assert_called_once()
            metadata = json.loads((output / "metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["status"], "duration_completed")
            self.assertEqual(metadata["creation_filetime"], 123)
            self.assertEqual(len(metadata["disk_image_sha256"]), 64)
            self.assertIn("private_bytes", (output / "memory.csv").read_text(encoding="utf-8"))
            self.assertTrue((output / "regions.jsonl").stat().st_size)
            with self.assertRaises(FileExistsError):
                watch.main(["--pid", "42", "--output", str(output)])

    def test_counter_failure_closes_target_and_records_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "test.exe"
            image.write_bytes(b"test image")
            output = root / "capture"
            target = unittest.mock.Mock()
            target.identity.return_value = {"image_path": str(image), "creation_filetime": 123}
            target.counters.side_effect = OSError("target no longer readable")
            with (
                patch.object(watch, "WindowsProcess", return_value=target),
                self.assertRaisesRegex(OSError, "target no longer readable"),
            ):
                watch.main(["--pid", "42", "--output", str(output)])
            target.close.assert_called_once()
            metadata = json.loads((output / "metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["status"], "failed")
            self.assertIn("target no longer readable", metadata["error"])
            self.assertIn("ended_utc", metadata)

    def test_daily_installation_cannot_be_an_output_directory(self):
        with self.assertRaisesRegex(ValueError, "Daily installation is read-only"):
            watch.main(["--pid", "42", "--output", "F:/Switch/Yuzu/memory-test"])


@unittest.skipUnless(
    os.name == "nt"
    and ct.sizeof(ct.c_void_p) == 8
    and os.environ.get("EDEN_PROF_WINDOWS_TESTS") == "1",
    "Opt-in Windows x64 API tests: set EDEN_PROF_WINDOWS_TESTS=1",
)
class MemoryWatchTests(unittest.TestCase):
    def test_commit_and_release_are_observed(self):
        process = watch.WindowsProcess(os.getpid())
        kernel = process.kernel
        kernel.VirtualAlloc.argtypes = [ct.c_void_p, ct.c_size_t, wt.DWORD, wt.DWORD]
        kernel.VirtualAlloc.restype = ct.c_void_p
        kernel.VirtualFree.argtypes = [ct.c_void_p, ct.c_size_t, wt.DWORD]
        kernel.VirtualFree.restype = wt.BOOL
        allocation = None
        try:
            before = process.counters()["private_bytes"]
            regions_before = process.regions()["private_committed_virtual_bytes"]
            allocation = kernel.VirtualAlloc(None, 64 * 1024**2, 0x3000, 0x04)
            self.assertTrue(allocation)
            during = process.counters()["private_bytes"]
            regions_during = process.regions()["private_committed_virtual_bytes"]
            self.assertGreaterEqual(during - before, 60 * 1024**2)
            self.assertGreaterEqual(regions_during - regions_before, 60 * 1024**2)
            self.assertTrue(kernel.VirtualFree(allocation, 0, 0x8000))
            allocation = None
            after = process.counters()["private_bytes"]
            self.assertGreaterEqual(during - after, 60 * 1024**2)
        finally:
            if allocation:
                kernel.VirtualFree(allocation, 0, 0x8000)
            process.close()

    def test_cli_writes_identity_counters_and_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            self.assertEqual(
                watch.main(
                    [
                        "--pid",
                        str(os.getpid()),
                        "--seconds",
                        "0.15",
                        "--interval",
                        "0.05",
                        "--output",
                        str(output),
                    ]
                ),
                0,
            )
            metadata = json.loads((output / "metadata.json").read_text())
            self.assertEqual(metadata["status"], "duration_completed")
            self.assertEqual(len(metadata["disk_image_sha256"]), 64)
            self.assertGreater(metadata["creation_filetime"], 0)
            self.assertIn("private_bytes", (output / "memory.csv").read_text())
            self.assertTrue((output / "regions.jsonl").stat().st_size)
            with self.assertRaises(FileExistsError):
                watch.main(["--pid", str(os.getpid()), "--output", str(output)])


if __name__ == "__main__":
    unittest.main()
