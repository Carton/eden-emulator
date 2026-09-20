import ctypes
from types import SimpleNamespace

from PIL import Image

import shot_capture
from shot_compare import compare


def test_natural_pairing_and_count_mismatch(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir()
    b.mkdir()
    for path, color in (
        (a / "t+9s.png", "red"),
        (a / "t+10s.png", "blue"),
        (b / "t+90s.png", "red"),
        (b / "t+100s.png", "blue"),
    ):
        Image.new("RGB", (20, 20), color).save(path)
    assert compare(a, b) == "PASS"
    (b / "t+100s.png").unlink()
    assert compare(a, b) == "FAIL"


def test_no_fallback_to_other_window(monkeypatch):
    monkeypatch.setattr(shot_capture, "windows", lambda pid: [(123, "Eden launcher")])
    assert shot_capture.capture_window(99) is None


def test_gdi_cleanup_and_pixel_copy(monkeypatch):
    calls = []
    pixels = ctypes.create_string_buffer(bytes([10, 20, 30, 0]))

    def rect(hwnd, pointer):
        pointer._obj.right, pointer._obj.bottom = 1, 1
        return True

    def dib(dc, info, colors, address, handle, offset):
        header = info._obj.header
        assert (header.planes, header.bits, header.height) == (1, 32, -1)
        address._obj.value = ctypes.addressof(pixels)
        return 400

    def select(dc, bitmap):
        calls.append(("select", bitmap))
        return 500

    def delete(bitmap):
        calls.append(("delete", bitmap))
        pixels[0] = b"\xff"

    user = SimpleNamespace(
        IsIconic=lambda h: False,
        GetWindowRect=rect,
        GetWindowDC=lambda h: 200,
        PrintWindow=lambda *args: True,
        ReleaseDC=lambda *args: calls.append(("release",)),
    )
    gdi = SimpleNamespace(
        CreateCompatibleDC=lambda dc: 300,
        CreateDIBSection=dib,
        SelectObject=select,
        DeleteObject=delete,
        DeleteDC=lambda dc: calls.append(("delete_dc",)),
    )
    monkeypatch.setattr(shot_capture, "api", lambda: (user, gdi, None))
    image = shot_capture.capture_hwnd(100)
    assert image.getpixel((0, 0)) == (30, 20, 10)
    assert calls == [
        ("select", 400),
        ("select", 500),
        ("delete", 400),
        ("delete_dc",),
        ("release",),
    ]


def test_failed_dib_releases_dc(monkeypatch):
    calls = []

    def rect(hwnd, pointer):
        pointer._obj.right, pointer._obj.bottom = 1, 1
        return True

    user = SimpleNamespace(
        IsIconic=lambda h: False,
        GetWindowRect=rect,
        GetWindowDC=lambda h: 200,
        ReleaseDC=lambda *args: calls.append("release"),
    )
    gdi = SimpleNamespace(
        CreateCompatibleDC=lambda dc: 300,
        CreateDIBSection=lambda *args: None,
        DeleteDC=lambda dc: calls.append("delete_dc"),
    )
    monkeypatch.setattr(shot_capture, "api", lambda: (user, gdi, None))
    assert shot_capture.capture_hwnd(100) is None
    assert calls == ["delete_dc", "release"]
