#!/usr/bin/env python3
"""Game-window capture for eden bench image QA.

The game renders in a separate top-level window titled 'Form'. Screen grabs
of a window rect capture whatever is on top when the window is occluded
(the IDE!), so the primary path is PrintWindow(PW_RENDERFULLCONTENT) into a
DIB section, which renders the window's own content regardless of
occlusion. There is NO screen-grab fallback: when PrintWindow fails or the
window is minimized the shot is skipped, never taken from the desktop.
"""
import ctypes
import ctypes.wintypes as wt
import os

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# GDI/HDC handles are pointer-sized; without these, ctypes truncates them to
# c_int and handles above 2^31 arrive sign-extended and INVALID. This single
# bug made every PrintWindow capture fail since day one (CreateDIBSection got
# a garbage DC and returned NULL), silently demoting every shot to the
# screen-grab fallback -- which captured the desktop whenever the game window
# was occluded (golden6, 2026-09-19).
user32.GetWindowDC.argtypes = [wt.HWND]
user32.GetWindowDC.restype = ctypes.c_void_p
user32.ReleaseDC.argtypes = [wt.HWND, ctypes.c_void_p]
user32.PrintWindow.argtypes = [wt.HWND, ctypes.c_void_p, wt.UINT]
user32.PrintWindow.restype = ctypes.c_bool
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.IsWindowVisible.restype = ctypes.c_bool
user32.IsIconic.argtypes = [wt.HWND]
user32.IsIconic.restype = ctypes.c_bool
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
gdi32.CreateDIBSection.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi32.CreateDIBSection.restype = ctypes.c_void_p
gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
gdi32.DeleteDC.argtypes = [ctypes.c_void_p]

PW_RENDERFULLCONTENT = 0x2


def _enum_windows():
    result = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def callback(hwnd, _lparam):
        result.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(callback), 0)
    return result


def _windows_of(pid):
    out = []
    pidbuf = wt.DWORD()
    title = ctypes.create_unicode_buffer(64)
    for hwnd in _enum_windows():
        if not user32.IsWindowVisible(hwnd):
            continue
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pidbuf))
        if pidbuf.value == pid:
            user32.GetWindowTextW(hwnd, title, 64)
            out.append((hwnd, title.value))
    return out


def find_render_window(pid):
    for hwnd, title in _windows_of(pid):
        if title == "Form":
            return hwnd
    return None


def find_hwnd(pid):
    wins = _windows_of(pid)
    return wins[0][0] if wins else None


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.UINT), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.UINT),
                ("biSizeImage", wt.UINT), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.UINT),
                ("biClrImportant", wt.UINT)]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 1)]


def _print_window_capture(hwnd, w, h):
    """PrintWindow into a DIB section; the section exposes its pixel buffer.

    Hard-won details (2026-09-19): GDI handles are sign-extended 64-bit
    values -- without the argtypes above, ctypes truncates them and every
    call downstream fails; and CreateDIBSection needs byref(BITMAPINFO),
    passing a raw buffer makes it return NULL with lasterr=0. Both bugs
    made this function return None since day one, silently demoting all
    captures to the (removed) screen-grab fallback.
    """
    hdc_window = user32.GetWindowDC(hwnd)
    if not hdc_window:
        return None
    try:
        hdc_mem = gdi32.CreateCompatibleDC(hdc_window)
        if not hdc_mem:
            return None
        bmi = _BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = w
        bmi.bmiHeader.biHeight = -h  # top-down
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        ppv = ctypes.c_void_p()
        hbm = gdi32.CreateDIBSection(hdc_mem, ctypes.byref(bmi), 0,
                                     ctypes.byref(ppv), None, 0)
        if not hbm or not ppv.value:
            gdi32.DeleteDC(hdc_mem)
            return None
        gdi32.SelectObject(hdc_mem, hbm)
        try:
            if not user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT):
                return None
            from PIL import Image
            raw = (ctypes.c_ubyte * (w * h * 4)).from_address(ppv.value)
            return Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1)
        finally:
            gdi32.DeleteObject(hbm)
            gdi32.DeleteDC(hdc_mem)
    finally:
        user32.ReleaseDC(hwnd, hdc_window)


def capture_window(pid):
    """Framebuffer content of the game render window; occlusion-tolerant.

    PrintWindow only. The old ImageGrab fallback captured the desktop
    (user's CAD/IDE windows) whenever the game was minimized or PrintWindow
    failed, poisoning image QA and luma stats (golden6, 2026-09-19) --
    a missing shot is strictly better than a wrong one.
    """
    hwnd = find_render_window(pid) or find_hwnd(pid)
    if not hwnd:
        return None
    if user32.IsIconic(hwnd):
        return None  # minimized: PrintWindow is unreliable, skip the shot
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None
    return _print_window_capture(hwnd, w, h)


def save_shot(pid, path):
    img = capture_window(pid)
    if img is None:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").save(path, "PNG")
    return True
