import ctypes, time, subprocess
user32 = ctypes.windll.user32
SW_MAXIMIZE = 3
for _ in range(120):
    time.sleep(1)
    out = subprocess.run(["tasklist", "/FO", "CSV"], capture_output=True, text=True).stdout
    if "eden.exe" not in out:
        continue
    hwnds = []
    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value.startswith("Qt") and user32.IsWindowVisible(h):
            hwnds.append(h)
        return True
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    if hwnds:
        user32.ShowWindow(hwnds[-1], SW_MAXIMIZE)
        print("maximized", [hex(h) for h in hwnds], flush=True)
        break
