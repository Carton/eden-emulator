"""PrintWindow capture only; never substitute the desktop for the game."""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
from functools import cache
from pathlib import Path

from PIL import Image

from prof_common import writable_target


class BitmapHeader(ctypes.Structure):
    _fields_ = [
        ("size", wt.DWORD),
        ("width", wt.LONG),
        ("height", wt.LONG),
        ("planes", wt.WORD),
        ("bits", wt.WORD),
        ("compression", wt.DWORD),
        ("image_size", wt.DWORD),
        ("xppm", wt.LONG),
        ("yppm", wt.LONG),
        ("used", wt.DWORD),
        ("important", wt.DWORD),
    ]


class BitmapInfo(ctypes.Structure):
    _fields_ = [("header", BitmapHeader), ("colors", wt.DWORD * 1)]


@cache
def api():
    """Load Windows APIs only when used, with pointer-sized handles throughout."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    signatures = [
        (user, "EnumWindows", [callback, wt.LPARAM], wt.BOOL),
        (user, "EnumChildWindows", [wt.HWND, callback, wt.LPARAM], wt.BOOL),
        (user, "GetWindowDC", [wt.HWND], wt.HDC),
        (user, "ReleaseDC", [wt.HWND, wt.HDC], ctypes.c_int),
        (user, "PrintWindow", [wt.HWND, wt.HDC, wt.UINT], wt.BOOL),
        (user, "IsWindowVisible", [wt.HWND], wt.BOOL),
        (user, "IsIconic", [wt.HWND], wt.BOOL),
        (user, "GetWindowThreadProcessId", [wt.HWND, ctypes.POINTER(wt.DWORD)], wt.DWORD),
        (user, "GetWindowTextW", [wt.HWND, wt.LPWSTR, ctypes.c_int], ctypes.c_int),
        (user, "GetWindowRect", [wt.HWND, ctypes.POINTER(wt.RECT)], wt.BOOL),
        (user, "ShowWindow", [wt.HWND, ctypes.c_int], wt.BOOL),
        (
            user,
            "SetWindowPos",
            [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT],
            wt.BOOL,
        ),
        (gdi, "CreateCompatibleDC", [wt.HDC], wt.HDC),
        (
            gdi,
            "CreateDIBSection",
            [
                wt.HDC,
                ctypes.POINTER(BitmapInfo),
                wt.UINT,
                ctypes.POINTER(ctypes.c_void_p),
                wt.HANDLE,
                wt.DWORD,
            ],
            wt.HBITMAP,
        ),
        (gdi, "SelectObject", [wt.HDC, wt.HANDLE], wt.HANDLE),
        (gdi, "DeleteObject", [wt.HANDLE], wt.BOOL),
        (gdi, "DeleteDC", [wt.HDC], wt.BOOL),
    ]
    for library, name, argtypes, restype in signatures:
        method = getattr(library, name)
        method.argtypes, method.restype = argtypes, restype
    return user, gdi, callback


def windows(pid: int, *, children: bool = False) -> list[tuple[int, str]]:
    user, _, callback_type = api()
    handles = []

    def append(hwnd, lparam):
        handles.append(hwnd)
        return True

    callback = callback_type(append)
    if not user.EnumWindows(callback, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    if children:
        for parent in list(handles):
            user.EnumChildWindows(parent, callback, 0)
    result = []
    for hwnd in dict.fromkeys(handles):
        process = wt.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(process))
        if process.value != pid or not user.IsWindowVisible(hwnd):
            continue
        title = ctypes.create_unicode_buffer(256)
        user.GetWindowTextW(hwnd, title, len(title))
        result.append((hwnd, title.value))
    return result


def find_render_window(pid: int):
    return next((hwnd for hwnd, title in windows(pid) if title == "Form"), None)


def capture_hwnd(hwnd: int, flags: int = 2):
    user, gdi, _ = api()
    rect = wt.RECT()
    if user.IsIconic(hwnd) or not user.GetWindowRect(hwnd, ctypes.byref(rect)):
        return None
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if not 0 < width <= 8192 or not 0 < height <= 8192:
        return None
    window_dc = user.GetWindowDC(hwnd)
    if not window_dc:
        return None
    memory_dc = bitmap = previous = None
    try:
        memory_dc = gdi.CreateCompatibleDC(window_dc)
        if not memory_dc:
            return None
        info = BitmapInfo()
        info.header = BitmapHeader(ctypes.sizeof(BitmapHeader), width, -height, 1, 32)
        pixels = ctypes.c_void_p()
        bitmap = gdi.CreateDIBSection(
            memory_dc, ctypes.byref(info), 0, ctypes.byref(pixels), None, 0
        )
        if not bitmap or not pixels.value:
            return None
        previous = gdi.SelectObject(memory_dc, bitmap)
        if not previous or previous == ctypes.c_void_p(-1).value:
            previous = None
            return None
        if not user.PrintWindow(hwnd, memory_dc, flags):
            return None
        # Copy before releasing the DIB; no Pillow object may retain its backing memory.
        raw = ctypes.string_at(pixels.value, width * height * 4)
        return Image.frombytes("RGB", (width, height), raw, "raw", "BGRX")
    finally:
        if previous:
            gdi.SelectObject(memory_dc, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if memory_dc:
            gdi.DeleteDC(memory_dc)
        user.ReleaseDC(hwnd, window_dc)


def capture_window(pid: int):
    hwnd = find_render_window(pid)
    return capture_hwnd(hwnd) if hwnd else None


def save_shot(pid: int, path: str | Path) -> bool:
    image = capture_window(pid)
    if image is None:
        return False
    path = writable_target(Path(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, "PNG")
    return True
