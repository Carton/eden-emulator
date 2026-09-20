#!/usr/bin/env python3
"""Probe eden's window hierarchy and find a PrintWindow target that works.

Enumerates top-level AND child windows of the eden pid, tries
PrintWindow(PW_RENDERFULLCONTENT) (and |PW_CLIENTONLY) on each, saves the
result, and reports which captures are non-blank (pixel stddev).
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

import numpy as np
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
PW_CLIENTONLY = 0x1
PW_RENDERFULLCONTENT = 0x2

OUT = os.path.join(os.environ.get("EDEN_PROF_DATA", r"F:\prof"),
                   "shots", "_inspect", "probe")


def enum_children(parent):
    result = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        result.append(hwnd)
        return True

    user32.EnumChildWindows(parent, WNDENUMPROC(cb), 0)
    return result


def top_windows():
    result = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        result.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return result


def info(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    title = ctypes.create_unicode_buffer(128)
    user32.GetWindowTextW(hwnd, title, 128)
    cls = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(hwnd, cls, 128)
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return pid.value, title.value, cls.value, rect


def try_capture(hwnd, flags, w, h):
    hdc_window = user32.GetWindowDC(hwnd)
    if not hdc_window:
        return None
    try:
        hdc_mem = gdi32.CreateCompatibleDC(hdc_window)
        bmi = ctypes.create_string_buffer(40)
        st = ctypes.cast(bmi, ctypes.POINTER(ctypes.c_uint32))
        st[0], st[1], st[2], st[3], st[4] = 40, w, -h, 1, 32
        ppv = ctypes.c_void_p()
        hbm = gdi32.CreateDIBSection(hdc_mem, bmi, 0, ctypes.byref(ppv), None, 0)
        if not hbm or not ppv.value:
            gdi32.DeleteDC(hdc_mem)
            return None
        gdi32.SelectObject(hdc_mem, hbm)
        try:
            ok = user32.PrintWindow(hwnd, hdc_mem, flags)
            if not ok:
                return False  # PrintWindow itself failed
            raw = (ctypes.c_ubyte * (w * h * 4)).from_address(ppv.value)
            return Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1)
        finally:
            gdi32.DeleteObject(hbm)
            gdi32.DeleteDC(hdc_mem)
    finally:
        user32.ReleaseDC(hwnd, hdc_window)


def main():
    pid = int(sys.argv[1])
    os.makedirs(OUT, exist_ok=True)
    cands = []
    for hwnd in top_windows():
        p, *_ = info(hwnd)
        if p == pid:
            cands.append(("top", hwnd))
            for ch in enum_children(hwnd):
                cp, *_ = info(ch)
                if cp == pid:
                    cands.append(("child", ch))
    print(f"{len(cands)} windows of pid {pid}")
    for kind, hwnd in cands:
        p, title, cls, rect = info(hwnd)
        w, h = rect.right - rect.left, rect.bottom - rect.top
        vis = bool(user32.IsWindowVisible(hwnd))
        iconic = bool(user32.IsIconic(hwnd))
        if w <= 0 or h <= 0 or w > 4000 or h > 4000:
            print(f"  [{kind}] {cls!r} title={title!r} SKIP rect={w}x{h}")
            continue
        for fname, flags in (("full", PW_RENDERFULLCONTENT),
                             ("client", PW_RENDERFULLCONTENT | PW_CLIENTONLY)):
            img = try_capture(hwnd, flags, w, h)
            tag = f"{kind}_{cls[:24]}_{fname}"
            if img is False or img is None:
                print(f"  [{kind}] {cls!r} title={title!r} {w}x{h} vis={vis} "
                      f"min={iconic} {fname}: PrintWindow FAILED")
                continue
            arr = np.asarray(img.convert("L"), dtype=np.float32)
            path = os.path.join(OUT, tag + ".png")
            img.convert("RGB").save(path)
            print(f"  [{kind}] {cls!r} title={title!r} {w}x{h} vis={vis} "
                  f"min={iconic} {fname}: OK mean={arr.mean():.1f} std={arr.std():.1f} -> {tag}.png")


if __name__ == "__main__":
    main()
